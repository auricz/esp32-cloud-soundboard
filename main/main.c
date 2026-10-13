#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"

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

static const char *TAG = "audio_output";

// Embedded by EMBED_FILES in main/CMakeLists.txt
extern const uint8_t wav_start[] asm("_binary_Demoman_laughlong02_wav_start");
extern const uint8_t wav_end[] asm("_binary_Demoman_laughlong02_wav_end");

typedef struct {
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t bits_per_sample;
  const uint8_t *data;
  size_t data_len;
} wav_info_t;

static TaskHandle_t play_task_handle;
static i2s_chan_handle_t tx_chan;
static adc_oneshot_unit_handle_t adc_handle;
static adc_channel_t pot_channel;

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

// Walks the RIFF chunks to find "fmt " and "data"; only uncompressed 16-bit PCM is accepted
static bool wav_parse(const uint8_t *buf, size_t size, wav_info_t *out)
{
  if (size < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool have_fmt = false;
  bool have_data = false;
  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t *chunk = buf + pos;
    uint32_t chunk_len = read_u32(chunk + 4);
    const uint8_t *body = chunk + 8;
    if (chunk_len > size - pos - 8) {
      return false;
    }

    if (memcmp(chunk, "fmt ", 4) == 0 && chunk_len >= 16) {
      uint16_t format = read_u16(body);
      out->channels = read_u16(body + 2);
      out->sample_rate = read_u32(body + 4);
      out->bits_per_sample = read_u16(body + 14);
      if (format != 1 || out->bits_per_sample != 16 || out->channels < 1 || out->channels > 2) {
        return false;
      }
      have_fmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      out->data = body;
      out->data_len = chunk_len;
      have_data = true;
    }

    pos += 8 + chunk_len + (chunk_len & 1);
  }

  return have_fmt && have_data;
}

static void i2s_init(const wav_info_t *wav)
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_DESC_NUM;
  chan_cfg.dma_frame_num = DMA_FRAME_NUM;
  // Send silence whenever there is nothing queued, so the amp stays quiet between plays
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

  i2s_slot_mode_t slot_mode = wav->channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wav->sample_rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
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
  // Mono: put the sample in both left and right slots, whichever channel the amp listens to
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
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

static void play_wav(const wav_info_t *wav)
{
  static const uint8_t silence[DMA_FRAME_NUM * sizeof(int16_t) * 2];
  static int16_t chunk[DMA_FRAME_NUM * 2];
  size_t written;
  bool press_pending = false;
  TickType_t press_tick = 0;

  // The WAV is read-only in flash, so scale it a chunk at a time into RAM. Reading the pot
  // per chunk (~11 ms of mono audio at 44.1 kHz) lets the volume follow the knob while the sound plays.
  for (size_t pos = 0; pos < wav->data_len; pos += sizeof(chunk)) {
    // A press while playing restarts from the beginning. Each edge restarts the debounce
    // window, and the press only counts if the button is still held once it settles, so
    // bounce from releasing the button doesn't trigger a restart.
    if (ulTaskNotifyTake(pdTRUE, 0)) {
      press_pending = true;
      press_tick = xTaskGetTickCount();
    }
    if (press_pending && xTaskGetTickCount() - press_tick >= pdMS_TO_TICKS(BTN_DEBOUNCE_MS)) {
      press_pending = false;
      if (gpio_get_level(BTN_PIN) == BTN_PRESSED_LEVEL) {
        pos = 0;
      }
    }

    size_t len = wav->data_len - pos;
    if (len > sizeof(chunk)) {
      len = sizeof(chunk);
    }
    memcpy(chunk, wav->data + pos, len);

    int32_t volume = read_volume();
    for (size_t i = 0; i < len / sizeof(int16_t); i++) {
      chunk[i] = (int16_t)((chunk[i] * volume) >> 15);
    }

    ESP_ERROR_CHECK(i2s_channel_write(tx_chan, chunk, len, &written, portMAX_DELAY));
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
}

static void play_task(void *arg)
{
  const wav_info_t *wav = arg;

  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Debounce: ignore edges where the button isn't still held after the contacts settle
    vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
    if (gpio_get_level(BTN_PIN) != BTN_PRESSED_LEVEL) {
      continue;
    }

    gpio_set_level(LED_PIN, 1);
    play_wav(wav);
    gpio_set_level(LED_PIN, 0);
  }
}

static void btn_isr_handler(void *arg)
{
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(play_task_handle, &woken);
  portYIELD_FROM_ISR(woken);
}

void app_main(void)
{
  static wav_info_t wav;
  if (!wav_parse(wav_start, wav_end - wav_start, &wav)) {
    ESP_LOGE(TAG, "Embedded WAV is not 16-bit PCM mono/stereo");
    return;
  }
  ESP_LOGI(TAG, "WAV: %lu Hz, %u ch, %u bytes", (unsigned long)wav.sample_rate, wav.channels,
           (unsigned)wav.data_len);

  gpio_reset_pin(LED_PIN);
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_PIN, 0);

  pot_init();
  i2s_init(&wav);

  xTaskCreate(play_task, "play_task", 4096, &wav, 5, &play_task_handle);

  gpio_reset_pin(BTN_PIN);
  gpio_set_direction(BTN_PIN, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BTN_PIN, GPIO_FLOATING);
  gpio_set_intr_type(BTN_PIN, GPIO_INTR_NEGEDGE);

  gpio_install_isr_service(0);
  gpio_isr_handler_add(BTN_PIN, btn_isr_handler, NULL);
}
