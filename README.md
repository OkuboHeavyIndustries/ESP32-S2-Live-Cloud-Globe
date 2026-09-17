/*Buy me a coffee!
Bitcoin: 19H3zFF4W3zUZ3jAdjmiDNNLs8Ja46M6AD
ETH: 0xD656DB37b61ac30Fa1e16a3162719FE417b231C8
*/

# Cloud Globe V2

A standalone 240 × 240 true-colour Earth display based on an **Adafruit QtPy ESP32-S2**. It renders NASA Blue Marble surface imagery, current CMA/NSMC global IR cloud observations, a real day/night terminator, and a locally propagated ISS orbit. A 128 × 64 OLED provides startup status and ISS pass/visibility information, while a microSD card stores configuration, TLE state and a replayable weather archive.


<img src="/cloud_globe.jpg" alt="Cloud Globe" width="500">

## Hardware

- Adafruit QtPy ESP32-S2
- ZJY-IPS130-V2.0 / ST7789 240 × 240 SPI TFT
- SSD1306 128 × 64 I²C OLED at `0x3C`
- microSD breakout
- momentary pushbutton (Cherry MX used in the finished unit)

### Wiring

| Module | Signal | QtPy ESP32-S2 |
|---|---|---|
| ST7789 | SCK / CLK | SCK |
| ST7789 | MOSI / SDA | MO |
| ST7789 | RST / RES | A0 |
| ST7789 | DC | A1 |
| ST7789 | VCC | 3V |
| ST7789 | BLK | 3V |
| SSD1306 | SDA | SDA |
| SSD1306 | SCL | SCL |
| SSD1306 | VCC | 5V |
| microSD | SCK | A2 |
| microSD | MOSI | A3 |
| microSD | MISO | RX |
| microSD | CS | TX |
| Button | switch input | MI / MISO pad |
| Button | other contact | GND |

The ST7789 used here has **no CS pin**, so the SD card deliberately uses a separate SPI bus.

## Required Arduino libraries

- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library
- Adafruit NeoPixel
- U8g2
- PNGdec
- AioP13

The ESP32 Arduino core supplies WiFi, HTTPClient, WiFiClientSecure, SD, SPI and Wire.

## Important: `build_opt.h`

Keep `build_opt.h` in the same sketch folder when compiling.

PNGdec 1.1.6's default buffered-pixel allocation is slightly too small for two aligned 320-pixel RGBA scanlines. That can overwrite its adjacent input buffer and corrupt the lower portion of some NSMC images. This release uses:

```text
-DPNG_MAX_BUFFERED_PIXELS=2624
```

as a sketch-wide compiler option so **both** the sketch and `PNGdec.cpp` see the same `PNG` class layout. Do not copy only the `.ino` into a different folder without also copying `build_opt.h`.

## SD card configuration

Copy `config.ini.example` to the root of the SD card as:

```text
/config.ini
```

Then set your Wi-Fi credentials, UTC offset/timezone label and observer location.

The observer coordinates are used for ISS azimuth/elevation, pass prediction and optical-visibility calculations. `VISIBLE_MIN_ELEVATION_DEG` can be raised if terrain or buildings block the local horizon. `VISIBLE_SUN_MAX_ELEVATION_DEG=-6` corresponds to civil twilight.

## Controls

- **1 click:** replay previous 24 hours
- **2 clicks:** replay previous 7 days
- **3 clicks:** replay previous 30 days
- **long press:** stop replay and return to live mode
- **any physical press:** immediately wakes the OLED forecast page while the multi-click gesture is still being resolved

The live globe rotation pauses during replay and resumes from the same longitude afterward. Replay time/date and the day/night terminator use the archived frame timestamp rather than current time.

## Weather data

Clouds come from the China Meteorological Administration / National Satellite Meteorological Center global GEO IR product, WMS layer `GEOS_IRX`.

The official NSMC availability API is treated as the source of truth. The firmware polls it every 15 minutes, displays the newest listed frame, archives successful frames to microSD and incrementally repairs gaps from the preceding seven days without monopolising the background worker.

The original PNG is decoded directly. There is no synthetic weather fallback.

## ISS tracking

The firmware fetches only **ISS (ZARYA), NORAD 25544** from CelesTrak, caches the TLE on SD and propagates it locally with AioP13. Successful TLE refreshes are limited to once per 24 hours; failed stale-TLE retries are limited to once per two hours and the timestamps persist across reboots.

The globe shows a ±60 minute 3D orbit track and the current ISS position. The OLED shows live observer geometry during a pass and forecast information when requested.

To experiment with another ordinary TLE satellite, change the `CATNR` in `ISS_TLE_URL` and delete `/iss/iss.tle` plus `/iss/fetch_state.txt` from the SD card before first boot so the old ISS cache cannot be reused.

## Wi-Fi behaviour

The release firmware is geographically neutral: it does **not** hard-code a Wi-Fi regulatory country. It disables modem sleep during network activity and requests the 19.5 dBm Arduino-ESP32 transmit-power setting. Automatic driver reconnect is disabled; network operations retry explicitly through `wifiConnect()` instead. This avoids rapid reconnect loops on a marginal RF link.

If an installation needs an explicit regulatory domain, add the appropriate ESP-IDF country setting for that location. For example, a Japan installation can use:

```cpp
#include <esp_wifi.h>

// Optional installation-specific setting — use the correct code for your country.
esp_wifi_set_country_code("JP", false);
```

Place the call after `WiFi.mode(WIFI_STA)` and before `WiFi.begin(...)`. Do not copy `"JP"` blindly for installations in other countries.

## Main source files

- `ESP32_S2_Live_Cloud_Globe_V2_OpenSource.ino` — application
- `build_opt.h` — required PNGdec compile option
- `ui_types.h` — small shared UI types
- `nasa_blue_marble_565.h` — RGB565 Earth texture
- `globe_screen_map_512x256.h` — globe projection lookup data
- `config.ini.example` — SD-card configuration template

## Notes for contributors

The code intentionally keeps several internal names prefixed `ISS` rather than adding a generic satellite abstraction. The shipping firmware tracks the ISS only; changing the CelesTrak catalogue number is deliberately a source-level modification.

Large retained buffers are preferentially allocated in PSRAM so internal DRAM remains available for Wi-Fi/mbedTLS. The temporary PNG decoder is created only after HTTP/TLS work has released its memory.
