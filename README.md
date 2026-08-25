/*Buy me a coffee!

Bitcoin: 19H3zFF4W3zUZ3jAdjmiDNNLs8Ja46M6AD

ETH: 0xD656DB37b61ac30Fa1e16a3162719FE417b231C8

*/


# ESP32-S2 Live Cloud Globe

A standalone **240×240 spinning Earth display with live global cloud imagery**, built around an **Adafruit QtPy ESP32-S2** and a **1.3-inch ST7789 IPS display**.

The globe uses a NASA Blue Marble Earth texture and overlays current global infrared cloud imagery from the **China Meteorological Administration / National Satellite Meteorological Center (NSMC)**.

Cloud images are archived automatically to a microSD card, allowing historical weather playback from a single pushbutton.

---

## Features

- Smooth 240×240 rotating Earth
- NASA Blue Marble true-colour surface texture
- Global cloud imagery from NSMC `GEOS_IRX`
- Live cloud image updated automatically
- Transparent clock/date overlay in JST
- Automatic microSD archive
- Official NSMC availability API used instead of guessing timestamps
- Automatic repair of missing archive frames
- Seven-day archive backfill window
- Historical weather playback:
  - single click → last 24 hours
  - double click → last 7 days
  - triple click → last 30 days
  - long press → return to live
- Replay timestamp shows the actual archived weather time
- Globe rotation pauses during replay and resumes from the same longitude afterward
- Cooperative HTTPS download handling to keep the single-core ESP32-S2 responsive
- Separate SPI buses for display and SD card

---

## Hardware

### Main controller

- **Adafruit QtPy ESP32-S2**
- PSRAM-capable version strongly recommended

The project uses PSRAM for the full-frame display buffer, downloaded PNG data and decoded cloud buffers.

### Display

- **ZJY-IPS130-V2.0**
- ST7789 controller
- 240×240 pixels
- SPI
- 1.3-inch IPS

### Storage

- microSD card reader
- Tested successfully with a ~64 GB SDXC card

### Input

- one momentary pushbutton

---

## Display wiring

The ST7789 is connected to the normal QtPy SPI pins.

| Display | QtPy ESP32-S2 |
|---|---|
| GND | GND |
| VCC | 3V |
| SCL / CLK | SCK |
| SDA / MOSI | MO |
| RES | A0 |
| DC | A1 |
| BLK | 3V |

The display used for this project has **no CS line**.

Known-good display initialization:

```cpp
tft.init(240, 240, SPI_MODE3);
tft.setRotation(2);
```

---

## SD card wiring

The SD card uses a **separate SPI bus** because the ST7789 has no chip-select line and is therefore permanently selected.

| microSD | QtPy ESP32-S2 |
|---|---|
| VCC | 3V |
| GND | GND |
| SCK / CLK | A2 |
| MOSI / DI | A3 |
| MISO / DO | RX |
| CS | TX |

The SD bus runs on `HSPI`.

```cpp
constexpr int SD_SCK  = A2;
constexpr int SD_MOSI = A3;
constexpr int SD_MISO = RX;
constexpr int SD_CS   = TX;
```

A conservative 4 MHz SD clock is currently used.

---

## Button wiring

Connect a momentary pushbutton between:

```text
QtPy MI pad → button → GND
```

The physical pad is labelled **MI**, but in the Arduino ESP32-S2 board definition it is referenced as:

```cpp
MISO
```

The firmware uses `INPUT_PULLUP`, so no external resistor is required.

---

## Button controls

The button is sampled by its own high-priority FreeRTOS task so Wi-Fi, SD access and PNG decoding do not cause missed clicks.

| Gesture | Action |
|---|---|
| 1 click | replay previous 24 hours |
| 2 clicks | replay previous 7 days |
| 3 clicks | replay previous 30 days |
| long press | stop replay / return to live |

The display gives immediate feedback while a click sequence is being entered:

```text
1x
2x
3x
```

Current timing:

```text
button polling:         5 ms
debounce:              25 ms
between-click window: 1.0 s
long press:            1.6 s
```

---

## Live time and date

Normal mode shows the current local time and date in JST:

```text
16:42
22 AUG
```

The text is rendered directly over the existing Earth/cloud framebuffer, so no opaque UI panel covers the globe.

During replay, the displayed timestamp becomes the **actual time of the archived cloud image being shown**.

Archive timestamps remain UTC internally.

---

# Weather data

## Cloud source

Cloud imagery comes from the:

**China Meteorological Administration / National Satellite Meteorological Center**

Product:

```text
GEO Satellite Global Image — IR 10.8 µm
```

WMS layer:

```text
GEOS_IRX
```

WMS endpoint:

```text
https://data.nsmc.org.cn/NSMCAPI/v1/nsmc/image/wms/compose
```

Typical request:

```text
?layers=GEOS_IRX
&datetime=YYYYMMDDHHMM
&request=GetMap
&bbox=-180,-90,180,90
&width=320
&height=160
&version=1.1.0
&format=png
```

---

## Official availability API

The firmware does **not** blindly probe every 15-minute timestamp.

Instead, it asks NSMC which `GEOS_IRX` datasets actually exist.

Availability endpoint:

```text
https://data.nsmc.org.cn/nsmcapi/v1/nsmc/image/animation/datatime/mongodb
```

Product code:

```text
GEO_MULT_GBAL_L2_GGM_IRX_GLL_YYYYMMDD_HHmm_4000M.PNG
```

Current archive repair window:

```text
168 hours / 7 days
```

The returned data currently shows `GEOS_IRX` as an **hourly product**.

That means the firmware no longer wastes requests on `:15`, `:30` or `:45` timestamps that do not contain usable global composites.

---

## Cloud processing

The downloaded PNG contains real NSMC infrared imagery.

The display treatment is derived locally:

- source alpha is preserved
- source luminance is analysed
- cloud opacity is derived from the image
- clouds are blended onto the visible Earth texture
- cloud geography remains aligned with the Blue Marble projection

The current preferred visual tuning is:

```cpp
constexpr uint16_t CLOUD_OPACITY_GAIN_PERCENT = 350;
constexpr uint8_t CLOUD_WHITENING = 255;
```

These values intentionally produce strong white cloud tops while preserving weaker cloud structure through alpha transparency.

This is a **display treatment**, not a calibrated meteorological optical-thickness product.

---

# Earth texture

The underlying Earth texture is based on:

**NASA Blue Marble: Next Generation — August 2004**

The image is converted to a 512×256 RGB565 texture and stored in flash.

A precomputed reverse-projection map converts each visible globe pixel to its corresponding latitude/longitude texture coordinate.

Current globe geometry:

```text
display:          240 × 240
globe centre:     120,120
globe radius:     112 px
view latitude:    +10°
screen tilt:      0°
spin direction:   west → east
spin rate:        15°/second
```

---

# Rendering architecture

The project uses a full 240×240 RGB565 framebuffer:

```text
240 × 240 × 2 bytes = 115,200 bytes
```

It is allocated in PSRAM where available.

Each frame is composed completely before being sent to the display:

```text
Earth
  ↓
cloud layer
  ↓
globe limb
  ↓
time/date/status text
  ↓
single full-screen TFT transfer
```

This avoids visible tearing and overlay flicker.

The resulting animation is significantly smoother than drawing individual globe elements directly to the TFT.

---

# Network handling

The QtPy ESP32-S2 can operate at fairly weak Wi-Fi signal levels, but this project was tested with RSSI values around:

```text
-80 to -87 dBm
```

To prevent long apparent freezes, explicit HTTPS timeouts are used:

```text
TLS handshake: 12 s
HTTP connect:  12 s
HTTP I/O:      20 s
```

The HTTP response body is read cooperatively in small chunks rather than using a long blocking `writeToStream()` operation.

This allows the single-core ESP32-S2 to continue servicing the globe renderer while network work is happening.

---

# SD archive

Accepted cloud PNGs are stored permanently.

Directory structure:

```text
/clouds/
  2026/
    08/
      23/
        0000.png
        0100.png
        0200.png
        ...
```

Filenames and directory timestamps are UTC.

A small archive index is also maintained:

```text
/clouds/index.csv
```

No automatic deletion or pruning is currently performed.

A 64 GB SD card is therefore effectively enormous for this project.

At approximately 40 KB per hourly cloud image:

```text
1 day     ≈   1 MB
1 week    ≈   7 MB
1 month   ≈  30 MB
1 year    ≈ 350 MB
```

Actual file sizes vary.

---

# Automatic gap repair

Every availability refresh:

1. the firmware downloads the official NSMC `GEOS_IRX` timestamp list
2. it applies the live-image safety cutoff
3. it compares every listed timestamp with the SD archive
4. existing PNGs are skipped
5. missing official frames are added to the repair queue
6. missing frames are downloaded newest-first
7. failed downloads remain missing
8. missing frames are retried during later passes

This is important on weak Wi-Fi because a failed hourly download is not silently forgotten.

---

# Live cloud selection

The live display intentionally avoids the very newest possible composite.

A safety delay is applied:

```cpp
constexpr uint16_t NSMC_SOURCE_LAG_MINUTES = 60;
```

This reduces the chance of displaying a transient or partially assembled global composite.

If the newest suitable image already exists on SD, it is loaded directly without downloading it again.

---

# Replay behaviour

Replay uses archived PNG files from the SD card.

## 24-hour replay

`GEOS_IRX` has been confirmed to be hourly, so 24H playback uses hourly frames:

```text
maximum frames: 24
frame interval: 500 ms
total playback: ≈12 seconds
```

## 7-day replay

```text
sampling: hourly
maximum frames: 168
```

## 30-day replay

The longer replay mode samples less frequently to keep playback duration practical.

---

## Replay startup

A replay request does not immediately freeze the globe.

Instead:

```text
button gesture
      ↓
replay requested
      ↓
WAIT
      ↓
archive/PNG lock becomes available
      ↓
first archived PNG successfully loads
      ↓
replay begins
```

The Earth continues rotating during `WAIT`.

This prevents the display from appearing frozen if SD or archive work is temporarily busy.

---

## Resume position after replay

Live rotation is accumulated rather than calculated directly from absolute `millis()`.

That means:

```text
live globe spinning
      ↓
replay starts
      ↓
current longitude is frozen
      ↓
replay runs
      ↓
replay ends / long press
      ↓
normal rotation resumes from exactly the same longitude
```

The globe therefore no longer jumps to a seemingly random position after replay.

---

# Boot display

Startup uses a compact ST7789 text log similar in spirit to `u8log`.

It reports stages such as:

```text
[MEM] OK
[SD] Ready
[WIFI] Connected
[TIME] OK
[NSMC] Availability in BG
[ARCHIVE] 7d gap repair
[BUTTON] Ready
[READY] Starting globe
```

After startup, the boot log disappears and the globe takes over the display.

---

# Required Arduino libraries

Install:

- **Adafruit GFX Library**
- **Adafruit ST7735 and ST7789 Library**
- **PNGdec** by Larry Bank / BitBank Software

The ESP32 Arduino core supplies:

- `WiFi`
- `WiFiClientSecure`
- `HTTPClient`
- `SPI`
- `SD`
- FreeRTOS support

The project has been tested with the Arduino ESP32 core in the 3.x series.

---

# Wi-Fi configuration

Edit:

```text
wifi_config.h
```

Example:

```cpp
#pragma once

#define WEATHER_WIFI_SSID      "YOUR_WIFI_NAME"
#define WEATHER_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
```

Do not commit real Wi-Fi credentials to a public repository.

---

# Repository files

Typical project structure:

```text
ESP32_S2_Live_Cloud_Globe/
├── ESP32_S2_Live_Cloud_Globe.ino
├── nasa_blue_marble_565.h
├── globe_screen_map_512x256.h
├── ui_types.h
├── wifi_config.h
├── DATA_PROVENANCE.txt
└── README.md
```

---

# Notes

### Why ESP32-S2?

The project currently benefits more from the available PSRAM on the chosen QtPy ESP32-S2 than it would from moving to a no-PSRAM ESP32-S3.

Large buffers include:

- 115 KB full-screen RGB565 framebuffer
- decoded cloud luminance buffer
- decoded cloud alpha buffer
- candidate cloud buffers
- HTTP PNG download buffer
- PNG decoder working memory

### Why separate SPI for the SD card?

The ST7789 module used here has no chip-select line.

Sharing its SPI bus with another device would therefore cause SD traffic to be interpreted by the display.

The SD card is placed on a second SPI peripheral instead.

---

# Status

Current hardware/software baseline:

- live globe rendering working
- NSMC cloud overlay working
- SD archive working
- official availability-driven collection working
- seven-day gap repair working
- 24H / 7D / 30D replay working
- robust pushbutton detection working
- transparent clock/date overlay working
- resume-from-replay longitude behaviour working

The project is currently being left running for longer-term archive and reliability testing.

---

# Data sources

### NASA

NASA Blue Marble: Next Generation  
https://visibleearth.nasa.gov/collection/1484/blue-marble

### NSMC

National Satellite Meteorological Center  
https://www.nsmc.org.cn/

NSMC WMS documentation:  
https://www.nsmc.org.cn/nsmc/cn/image/wms.html

---

## License

No project license has been selected yet.

If publishing this repository publicly, add a suitable software license and retain the appropriate attribution for NASA and NSMC source data.
