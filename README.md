# cloud-soundboard

A four-button soundboard. An ESP32-S3 plays WAV files streamed from the internet, and a small webapp chooses which sounds each button plays. I have to give credit to Claude Code for one-shotting the web app. Its firmware code was a bit more buggy though.

## Firmware (`main/`)

The ESP-IDF firmware in [main/main.c](main/main.c) connects to Wi-Fi and waits for a press on one of four buttons (red, blue, yellow and green). On a press it asks the webapp's `/api/random?color=<color>` for a random WAV URL from that button's playlist, then streams the file and plays it over I2S to an amplifier. A potentiometer sets the volume, and pressing another button while a sound plays skips to a sound from that button's playlist. The LED flashes quickly during startup, stays on while a sound downloads and flashes slowly while it plays.

WAVs must be uncompressed 16-bit PCM, mono or stereo, between 8 and 96 kHz.

**Pins**

| Function | GPIO |
| --- | --- |
| Buttons: red, blue, yellow, green | 10, 11, 12, 13 (pressed pulls to GND) |
| LED | 20 |
| I2S BCK / DIN / WS | 18 / 17 / 16 |
| Volume pot wiper | 2 |

**Build and flash**

This project was built using [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) and the ESP-IDF VSCode extension. Just remember to set the Wi-Fi SSID, password, and url in [main/Kconfig.projbuild](main/Kconfig.projbuild).

## Webapp (`webapp/`)

The webapp is a Cloudflare Pages site with a D1 database. The page lets me create playlists of WAV URLs and choose which playlist each button plays. The API is made of Pages Functions under `functions/api/`:

- `GET /api/random?color=red|blu|yel|grn` picks a random URL from that button's playlist. By default it redirects to the URL. Add `&format=text` or `&format=json` to get the URL in the response body instead.
- `GET/PUT /api/buttons[/:color]` reads or sets each button's playlist.
- `GET/POST/PUT/DELETE /api/playlists[/:id]` lists, creates, updates and deletes playlists.

**Run locally**

```sh
cd webapp
npm install
npm run dev        # applies migrations to a local D1 database and starts wrangler pages dev
```

**Deploy**

```sh
npx wrangler d1 create wav-playlists   # first time only; put the database_id in wrangler.toml
npm run db:migrate                     # apply migrations to the remote database
npm run deploy
```
