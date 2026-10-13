#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define BTN_PIN 13
#define LED_PIN 2

#define I2S_BCK_PIN 17
#define I2S_DIN_PIN 16
#define I2S_WCK_PIN 15

// Pot wiper; outer legs to 3.3V and GND
#define POT_PIN 4
#define POT_MAX 4095

// Button pulls BTN_PIN to GND, external 10k pull-up (R2) to 3.3V: pressed reads 0
#define BTN_PRESSED_LEVEL 0
#define BTN_DEBOUNCE_MS 20

#define DMA_DESC_NUM 6
#define DMA_FRAME_NUM 240

// I2S starts out in this format and is switched to each WAV's own format before it plays
#define DEFAULT_SAMPLE_RATE 44100
#define WAV_MIN_SAMPLE_RATE 8000
#define WAV_MAX_SAMPLE_RATE 96000

#define HTTP_TIMEOUT_MS 10000
#define HTTP_MAX_REDIRECTS 5
// ?format=text has the webapp return the WAV's URL as the body instead of redirecting to it,
// so the webapp and the WAV's host each keep a connection of their own between presses
#define API_URL CONFIG_SOUND_API_URL "?format=text"
// The webapp accepts URLs up to 2048 characters
#define WAV_URL_MAX 2048

// Audio downloaded ahead of playback (~370 ms of 44.1 kHz mono), so a slow patch of Wi-Fi
// doesn't cut out the sound. Playback waits for half of it to fill before starting.
#define AUDIO_BUF_SIZE (32 * 1024)
#define PREBUFFER_SIZE (AUDIO_BUF_SIZE / 2)

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "audio_output";

typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t bits_per_sample;
  size_t data_len;
} wav_info_t;

// Handed to fetch_task, which streams one WAV's sample data into audio_buf
typedef struct {
  esp_http_client_handle_t client;
  size_t data_len;
  size_t frame_size;
  volatile bool stop;
} fetch_ctx_t;

static TaskHandle_t play_task_handle;
static i2s_chan_handle_t tx_chan;
static uint32_t i2s_sample_rate;
static uint16_t i2s_channels;
static adc_oneshot_unit_handle_t adc_handle;
static adc_channel_t pot_channel;
static EventGroupHandle_t wifi_events;
static StreamBufferHandle_t audio_buf;
// Given by fetch_task when it has finished with wav_client
static SemaphoreHandle_t fetch_done;
// One long-lived client per server, so a press can reuse the connection from the last one
// instead of making new connections and checking certificates again
static esp_http_client_handle_t api_client;
static esp_http_client_handle_t wav_client;

static uint16_t read_u16(const uint8_t *p)
{
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static uint32_t read_u32(const uint8_t *p)
{
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// Reads exactly len bytes of the response body; false if it ends or fails first
static bool http_read_exact(esp_http_client_handle_t client, void *buf, size_t len)
{
  return esp_http_client_read_response(client, buf, len) == (int)len;
}

static bool http_skip(esp_http_client_handle_t client, size_t len)
{
  uint8_t scratch[64];
  while (len > 0) {
    size_t n = len < sizeof(scratch) ? len : sizeof(scratch);
    if (!http_read_exact(client, scratch, n)) {
      return false;
    }
    len -= n;
  }
  return true;
}

// Walks the RIFF chunks in the response body up to "data", leaving the client positioned at
// the first sample. Only uncompressed 16-bit PCM is accepted.
static bool wav_read_header(esp_http_client_handle_t client, wav_info_t *out)
{
  uint8_t riff[12];
  if (!http_read_exact(client, riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool have_fmt = false;
  while (1) {
    uint8_t chunk[8];
    if (!http_read_exact(client, chunk, sizeof(chunk))) {
      return false;
    }
    uint32_t chunk_len = read_u32(chunk + 4);

    if (memcmp(chunk, "data", 4) == 0) {
      if (!have_fmt) {
        return false;
      }
      // Whole frames only, so the player never gets out of step with the samples
      size_t frame_size = out->channels * sizeof(int16_t);
      out->data_len = chunk_len - chunk_len % frame_size;
      return true;
    }

    size_t skip = chunk_len + (chunk_len & 1);
    if (memcmp(chunk, "fmt ", 4) == 0 && chunk_len >= 16) {
      uint8_t body[16];
      if (!http_read_exact(client, body, sizeof(body))) {
        return false;
      }
      uint16_t format = read_u16(body);
      out->channels = read_u16(body + 2);
      out->sample_rate = read_u32(body + 4);
      out->bits_per_sample = read_u16(body + 14);
      if (format != 1 || out->bits_per_sample != 16 || out->channels < 1 || out->channels > 2 ||
          out->sample_rate < WAV_MIN_SAMPLE_RATE || out->sample_rate > WAV_MAX_SAMPLE_RATE) {
        return false;
      }
      have_fmt = true;
      skip -= sizeof(body);
    }
    if (!http_skip(client, skip)) {
      return false;
    }
  }
}

static esp_http_client_handle_t http_client_new(const char *url)
{
  esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = HTTP_TIMEOUT_MS,
    .crt_bundle_attach = esp_crt_bundle_attach,
    // Servers drop idle connections after a while. When the client reconnects, a saved session
    // lets the server skip the certificate and key exchange, if it still accepts the session.
    .save_client_session = true,
    // TCP keep-alive probes notice an idle connection that died without the server closing it
    // (across a Wi-Fi drop, say), so the next request fails fast instead of waiting out timeout_ms
    .keep_alive_enable = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  configASSERT(client);
  return client;
}

// Sends a GET for the client's URL and reads the response headers. The server may have closed
// a kept connection while it sat idle, which only shows up now, so a failure gets one more
// try on a new connection.
static bool http_get(esp_http_client_handle_t client)
{
  for (int attempt = 0; attempt < 2; attempt++) {
    // Body bytes that arrive along with the headers are cached in the client, and neither
    // closing the connection nor starting a new request throws away any that weren't read.
    // Left there, they'd be handed out as the start of this response's body, or trip an
    // assert in esp_http_client_fetch_headers if some of them had been read.
    esp_http_client_clear_response_buffer(client);
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
      return true;
    }
    esp_http_client_close(client);
  }
  ESP_LOGE(TAG, "Request failed");
  return false;
}

// Reads off whatever is left of the response body, so the connection can carry the next
// request, unless the server said it's closing it. This doesn't use
// esp_http_client_flush_response: it leaves the count of bytes it flushed recorded as cached
// body data, and that many junk bytes then come out ahead of the next response's body.
static void http_finish(esp_http_client_handle_t client)
{
  char scratch[128];
  while (esp_http_client_read(client, scratch, sizeof(scratch)) > 0) {
  }
  if (!esp_http_client_is_complete_data_received(client) ||
      !esp_http_client_is_persistent_connection(client)) {
    esp_http_client_close(client);
  }
}

// Asks the webapp for the URL of a random WAV from the selected playlist
static bool get_random_url(char *url, size_t size)
{
  if (!http_get(api_client)) {
    return false;
  }
  int status = esp_http_client_get_status_code(api_client);
  int len = esp_http_client_read_response(api_client, url, size - 1);
  http_finish(api_client);
  url[len > 0 ? len : 0] = '\0';

  // Errors come back as a JSON message, which is worth logging as-is
  if (status != 200 || len <= 0) {
    ESP_LOGE(TAG, "HTTP %d from the webapp: %s", status, url);
    return false;
  }
  return true;
}

// GETs the WAV and reads the response headers, leaving wav_client ready to read the body.
// esp_http_client only follows redirects by itself in esp_http_client_perform, which hands
// the body to a callback instead of letting it be read as a stream, so they're followed here.
static bool open_wav(const char *url)
{
  // Keeps the connection if the WAV is on the same host as the last one
  if (esp_http_client_set_url(wav_client, url) != ESP_OK) {
    ESP_LOGE(TAG, "Bad URL: %s", url);
    return false;
  }

  for (int i = 0; i <= HTTP_MAX_REDIRECTS; i++) {
    if (!http_get(wav_client)) {
      return false;
    }
    int status = esp_http_client_get_status_code(wav_client);
    if (status == 200) {
      return true;
    }
    http_finish(wav_client);
    // set_redirection fails if there's no Location header to follow
    if (status / 100 != 3 || esp_http_client_set_redirection(wav_client) != ESP_OK) {
      ESP_LOGE(TAG, "HTTP %d for %s", status, url);
      return false;
    }
  }
  return false;
}

// Copies the WAV's sample data from the response body into audio_buf until it's all there
// or play_task asks it to stop, then gives fetch_done.
static void fetch_task(void *arg)
{
  fetch_ctx_t *ctx = arg;
  // Only one fetch runs at a time
  static uint8_t buf[2048];
  size_t have = 0;
  size_t remaining = ctx->data_len;

  while (remaining > 0 && !ctx->stop) {
    size_t want = sizeof(buf) - have;
    if (want > remaining) {
      want = remaining;
    }
    int n = esp_http_client_read(ctx->client, (char *)buf + have, want);
    if (n <= 0) {
      ESP_LOGW(TAG, "Download ended %u bytes early", (unsigned)remaining);
      break;
    }
    have += n;
    remaining -= n;

    // Every send and receive on audio_buf is a whole number of frames (and so is its size),
    // which keeps the player's 16-bit samples and left/right channels lined up
    size_t whole = have - have % ctx->frame_size;
    size_t sent = 0;
    while (sent < whole && !ctx->stop) {
      sent += xStreamBufferSend(audio_buf, buf + sent, whole - sent, pdMS_TO_TICKS(100));
    }
    memmove(buf, buf + whole, have - whole);
    have -= whole;
  }

  // Abandoning the body partway leaves the rest of it in the way of the next response, so
  // only a complete download keeps the connection
  if (remaining == 0) {
    http_finish(ctx->client);
  } else {
    esp_http_client_close(ctx->client);
  }
  xSemaphoreGive(fetch_done);
  vTaskDelete(NULL);
}

static i2s_std_slot_config_t i2s_slot_config(uint16_t channels)
{
  i2s_slot_mode_t slot_mode = channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode);
  // Mono: put the sample in both left and right slots, whichever channel the amp listens to
  slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  return slot_cfg;
}

static void i2s_init(void)
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_DESC_NUM;
  chan_cfg.dma_frame_num = DMA_FRAME_NUM;
  // Send silence whenever there is nothing queued, so the amp stays quiet between plays
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

  i2s_sample_rate = DEFAULT_SAMPLE_RATE;
  i2s_channels = 1;
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2s_sample_rate),
    .slot_cfg = i2s_slot_config(i2s_channels),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = I2S_BCK_PIN,
      .ws = I2S_WCK_PIN,
      .dout = I2S_DIN_PIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

// Each WAV can have its own sample rate and channel count. The channel has to be disabled to
// change them, so that's only done when they differ from the last sound's.
static void i2s_set_format(const wav_info_t *wav)
{
  if (wav->sample_rate == i2s_sample_rate && wav->channels == i2s_channels) {
    return;
  }

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wav->sample_rate);
  i2s_std_slot_config_t slot_cfg = i2s_slot_config(wav->channels);
  ESP_ERROR_CHECK(i2s_channel_disable(tx_chan));
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg));
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
  i2s_sample_rate = wav->sample_rate;
  i2s_channels = wav->channels;
}

static void pot_init(void)
{
  adc_unit_t unit;
  ESP_ERROR_CHECK(adc_oneshot_io_to_channel(POT_PIN, &unit, &pot_channel));

  adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = unit,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

  // 12 dB attenuation covers the full 0-3.3V swing of the wiper
  adc_oneshot_chan_cfg_t chan_cfg = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, pot_channel, &chan_cfg));
}

// Volume as a Q15 factor (0 = silent, 32767 ~= full). Squaring the pot position gives a
// roughly logarithmic feel, so the knob doesn't do all its work in the first few degrees.
static int32_t read_volume(void)
{
  int raw = 0;
  if (adc_oneshot_read(adc_handle, pot_channel, &raw) != ESP_OK) {
    return 0;
  }
  if (raw > POT_MAX) {
    raw = POT_MAX;
  }
  return ((int64_t)raw * raw * 32767) / ((int64_t)POT_MAX * POT_MAX);
}

// Downloads and plays one random sound from the webapp. Returns true if the button was pressed
// again while it played, which cuts it short so the caller can play another.
static bool play_random(void)
{
  static const uint8_t silence[DMA_FRAME_NUM * sizeof(int16_t) * 2];
  static int16_t chunk[DMA_FRAME_NUM * 2];
  static fetch_ctx_t fetch_ctx;
  static char url[WAV_URL_MAX + 1];
  size_t written;
  wav_info_t wav;
  TickType_t start_tick = xTaskGetTickCount();

  if (!get_random_url(url, sizeof(url)) || !open_wav(url)) {
    return false;
  }
  if (!wav_read_header(wav_client, &wav)) {
    ESP_LOGE(TAG, "Not a 16-bit PCM mono/stereo WAV: %s", url);
    esp_http_client_close(wav_client);
    return false;
  }

  ESP_LOGI(TAG, "Playing %s: %lu Hz, %u ch, %u bytes, connected in %lu ms", url,
           (unsigned long)wav.sample_rate, wav.channels, (unsigned)wav.data_len,
           (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start_tick));

  i2s_set_format(&wav);

  xStreamBufferReset(audio_buf);
  fetch_ctx = (fetch_ctx_t){
    .client = wav_client,
    .data_len = wav.data_len,
    .frame_size = wav.channels * sizeof(int16_t),
  };
  if (xTaskCreate(fetch_task, "fetch_task", 6144, &fetch_ctx, 5, NULL) != pdPASS) {
    esp_http_client_close(wav_client);
    return false;
  }

  // Presses while connecting were most likely impatience, not a request to skip this sound
  ulTaskNotifyTake(pdTRUE, 0);

  bool fetch_finished = false;
  while (!fetch_finished && xStreamBufferBytesAvailable(audio_buf) < PREBUFFER_SIZE) {
    fetch_finished = xSemaphoreTake(fetch_done, pdMS_TO_TICKS(10));
  }

  bool press_pending = false;
  bool pressed_again = false;
  TickType_t press_tick = 0;
  while (1) {
    // A press while playing skips to another sound. Each edge restarts the debounce window,
    // and the press only counts if the button is still held once it settles, so bounce from
    // releasing the button doesn't trigger a skip.
    if (ulTaskNotifyTake(pdTRUE, 0)) {
      press_pending = true;
      press_tick = xTaskGetTickCount();
    }
    if (press_pending && xTaskGetTickCount() - press_tick >= pdMS_TO_TICKS(BTN_DEBOUNCE_MS)) {
      press_pending = false;
      if (gpio_get_level(BTN_PIN) == BTN_PRESSED_LEVEL) {
        pressed_again = true;
        break;
      }
    }

    // Times out on an empty buffer so presses are still checked while the download catches
    // up; auto_clear keeps the amp fed with silence meanwhile
    size_t len = xStreamBufferReceive(audio_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
    if (len == 0) {
      // Nothing more can arrive once fetch_task is done, so empty then means finished
      if (fetch_finished) {
        break;
      }
      fetch_finished = xSemaphoreTake(fetch_done, 0);
      continue;
    }

    // Reading the pot per chunk (~11 ms of mono audio at 44.1 kHz) lets the volume follow
    // the knob while the sound plays
    int32_t volume = read_volume();
    for (size_t i = 0; i < len / sizeof(int16_t); i++) {
      chunk[i] = (int16_t)((chunk[i] * volume) >> 15);
    }

    ESP_ERROR_CHECK(i2s_channel_write(tx_chan, chunk, len, &written, portMAX_DELAY));
  }

  if (!fetch_finished) {
    fetch_ctx.stop = true;
    xSemaphoreTake(fetch_done, portMAX_DELAY);
  }

  // A press in the last few ms hasn't been confirmed yet; hand it back to play_task
  if (press_pending) {
    xTaskNotifyGive(xTaskGetCurrentTaskHandle());
  }

  // i2s_channel_write returns once data is queued in DMA, not played. Pushing a full
  // set of DMA buffers of silence behind it means the audio has gone out when this returns.
  for (int i = 0; i <= DMA_DESC_NUM; i++) {
    ESP_ERROR_CHECK(i2s_channel_write(tx_chan, silence, sizeof(silence), &written, portMAX_DELAY));
  }

  return pressed_again;
}

static void play_task(void *arg)
{
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Debounce: ignore edges where the button isn't still held after the contacts settle
    vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
    if (gpio_get_level(BTN_PIN) != BTN_PRESSED_LEVEL) {
      continue;
    }

    if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
      ESP_LOGW(TAG, "Not connected to Wi-Fi yet");
      continue;
    }

    // The LED stays on while the sound downloads too, to show the press was taken
    gpio_set_level(LED_PIN, 1);
    while (play_random()) {
    }
    gpio_set_level(LED_PIN, 0);
  }
}

static void btn_isr_handler(void *arg)
{
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(play_task_handle, &woken);
  portYIELD_FROM_ISR(woken);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "Wi-Fi connected");
    xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
  }
}

// Starts connecting to the network set in menuconfig; WIFI_CONNECTED_BIT tracks when it's up
static void wifi_init(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      wifi_event_handler, NULL, NULL));

  wifi_config_t wifi_cfg = {0};
  strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_SOUND_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
  strlcpy((char *)wifi_cfg.sta.password, CONFIG_SOUND_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Modem sleep adds latency spikes that the audio buffer would have to ride out
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
  gpio_reset_pin(LED_PIN);
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_PIN, 0);

  audio_buf = xStreamBufferCreate(AUDIO_BUF_SIZE, 1);
  fetch_done = xSemaphoreCreateBinary();
  configASSERT(audio_buf && fetch_done);

  wifi_init();
  api_client = http_client_new(API_URL);
  // Pointed at each WAV's URL as it's played
  wav_client = http_client_new(API_URL);
  pot_init();
  i2s_init();

  // TLS handshakes run on this task's stack
  xTaskCreate(play_task, "play_task", 8192, NULL, 5, &play_task_handle);

  gpio_reset_pin(BTN_PIN);
  gpio_set_direction(BTN_PIN, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BTN_PIN, GPIO_FLOATING);
  gpio_set_intr_type(BTN_PIN, GPIO_INTR_NEGEDGE);

  gpio_install_isr_service(0);
  gpio_isr_handler_add(BTN_PIN, btn_isr_handler, NULL);
}
