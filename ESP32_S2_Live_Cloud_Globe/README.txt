ESP32-S2 NSMC WEATHER GLOBE — SD ARCHIVE + REPLAY
==================================================

HARDWARE
--------
Adafruit QtPy ESP32-S2 with PSRAM
ZJY-IPS130-V2.0 / ST7789 240x240

Display:
  GND -> GND
  VCC -> 3V
  SCL -> SCK
  SDA -> MO
  RES -> A0
  DC  -> A1
  BLK -> 3V

Known-good display setup:
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(2);

SD card — separate SPI:
  SCK  -> A2
  MOSI -> A3
  MISO -> RX
  CS   -> TX
  VCC  -> 3V
  GND  -> GND

Button:
  physical pad "MI" -> momentary pushbutton -> GND
  (Arduino symbol: MISO, GPIO 37)

The button uses INPUT_PULLUP.

VISUAL BASELINE
---------------
The user's current tuning is retained:

  CLOUD_OPACITY_GAIN_PERCENT = 350
  CLOUD_WHITENING            = 255

ARCHIVE
-------
Every accepted NSMC PNG is kept permanently.

UTC folder layout:

  /clouds/
    2026/
      08/
        22/
          0600.png
          0615.png
          ...

An index is appended at:

  /clouds/index.csv

Nothing is automatically deleted.

The device no longer guesses quarter-hour timestamps.

Every 15 minutes it asks NSMC's official GEOS_IRX availability endpoint for
the preceding 168 hours / 7 days, compares those officially listed timestamps with the
SD archive, and downloads only files that are genuinely missing.

If a listed hour fails because of weak Wi-Fi or a temporary WMS problem, it is
left missing and therefore remains in the repair queue on later passes.
Successful PNGs are never re-downloaded.

REPLAY BUTTON
-------------
Single click:
  previous 24 hours
  tries every 15-minute archive timestamp

Double click:
  previous 7 days
  samples one timestamp per hour

Triple click:
  previous 30 days
  samples one timestamp every 3 hours

Long press:
  stop playback / return to live

Missing archive timestamps are skipped automatically.

During playback the Earth viewpoint is held fixed so cloud movement is easier
to see. At the end, the exact cloud frame that was live before playback is
restored and normal rotation resumes.

TIME + DATE
-----------
Live:
  current JST, for example

    16:42
    22 AUG

Replay:
  JST time/date of the actual archived weather frame being displayed.

Archive timestamps and filenames remain UTC.

BACKGROUND UPDATES
------------------
The display is no longer cleared for updates.

A FreeRTOS weather task downloads and decodes into candidate buffers while the
currently accepted cloud image stays active. Because the ESP32-S2 is
single-core, Wi-Fi and PNG work can still cause a brief reduction in animation
frame rate, but the screen should remain on the globe.

The active/candidate cloud buffers are swapped only after a candidate passes
decode and image validation.

The availability API is now the primary source of valid timestamps. The live
display still requires a timestamp to be at least 60 minutes behind wall-clock
UTC before activating it, preserving the existing protection against a newly
assembled/transient composite.

BOOT LOG
--------
Startup now uses a small scrolling ST7789 text console for:
  memory
  SD
  Wi-Fi
  time
  NSMC
  archive
  ready

Once ready, the globe replaces the log.

LIBRARIES
---------
Adafruit GFX Library
Adafruit ST7735 and ST7789 Library
PNGdec by Larry Bank / BitBank Software

ESP32 built-ins:
  SPI
  SD
  WiFi
  HTTPClient
  FreeRTOS

FIRST TEST
----------
1. Copy your Wi-Fi credentials into wifi_config.h.
2. Compile/flash.
3. Confirm the live globe looks the same as the proven AlphaShade renderer.
4. Leave it running while the 24-hour archive fills in the background.
5. Single-click the MI-to-GND button to test 24-hour playback.
6. Long-press at any time to return to live.

7-day and 30-day modes replay whatever history is actually present on the
card; the archive grows naturally over time.


RESPONSIVE NETWORK REVISION
---------------------------
The initial NSMC request no longer blocks startup. The globe starts first,
then the weather task fetches current clouds and fills history.

HTTPS limits:
  TLS handshake  12 s
  HTTP connect   12 s
  HTTP I/O       20 s

Serial diagnostics now include:
  [HTTP] TLS/connect/request...
  [HTTP] GET returned <code> after <milliseconds> ms

Upper-left work status:
  NET       current weather request active
  23/93     initial 24 h archive scan progress
  blank     idle

Current weather always has priority over historical backfill.


COOPERATIVE SINGLE-CORE DOWNLOAD REVISION
-----------------------------------------
Live testing showed HTTP 200 returned quickly, but HTTPClient::writeToStream()
could then leave the globe apparently frozen while the PNG body was copied.

This revision:
  - removes writeToStream() for NSMC downloads
  - reads at most 2048 bytes per pass
  - yields 2 ms between passes
  - gives the weather task FreeRTOS priority 0
  - keeps the normal Arduino rendering loop at higher priority
  - shows received/expected KB beside the clock during a live download
  - aborts an idle body after 12 s
  - aborts an entire body transfer after 45 s

Expected live indicator example:
  17:24
  22 AUG   18/39K

The globe should continue rotating while this number advances.


NO-FLICKER OVERLAY REVISION
---------------------------
The earlier cooperative build drew the full globe framebuffer first and then
drew the clock/status rectangle directly onto the TFT. Because the next globe
frame immediately overwrote that rectangle, the overlay visibly flickered.

This revision draws the clock, date, NET progress, archive count and replay
mode into a small 91x20 off-screen GFXcanvas16, copies that canvas into the
main 240x240 framebuffer, and only then transfers the finished frame to the
ST7789.

There is now one complete screen image per frame:
  Earth + clouds + limb + clock/date/status -> one writePixels()

No display wiring, archive behaviour, replay controls, network timing or cloud
visual tuning changed.


TRANSPARENT CLOCK / STATUS OVERLAY
----------------------------------
The clock/date/status area no longer draws a dark rectangle over the Earth.

For each completed frame:
  1. the already-rendered Earth/cloud pixels beneath the text area are copied
     into the small off-screen canvas
  2. time/date/status characters are drawn transparently over those pixels
  3. the result is copied back into the main globe framebuffer
  4. the complete 240x240 frame is transferred to the TFT once

This preserves the no-flicker behaviour while leaving the globe visible behind
the clock, download count, network progress and replay mode.

No network, archive, replay, SD, button or cloud-rendering behaviour changed.

BUTTON TIMING FIX
-----------------
Multi-click recognition was made more forgiving and more robust:

  multi-click window: 360 ms -> 650 ms

The click sequence is also no longer finalized while the physical button input
is LOW, even if the debounced state has not yet caught up. This removes an edge
case where a second press near the end of the old window could be misread as a
single click.

Serial now reports the recognized gesture, for example:
  [BUTTON] 1 click
  [BUTTON] 2 clicks
  [BUTTON] 3 clicks

Replay mapping is unchanged:
  1 click  -> 24H
  2 clicks -> 7D
  3 clicks -> 30D
  long     -> live


RELAXED BUTTON TIMING
---------------------
Multi-click gap: 1200 ms
Long press:      1800 ms

This deliberately favors easy physical-button operation over mouse-style
double-click timing. Single-click replay therefore begins about 1.2 seconds
after release while the code waits to see whether a second click follows.


OFFICIAL AVAILABILITY + GAP REPAIR REVISION
-------------------------------------------
Primary timestamp source:
  https://data.nsmc.org.cn/nsmcapi/v1/nsmc/image/animation/datatime/mongodb

Product code:
  GEO_MULT_GBAL_L2_GGM_IRX_GLL_YYYYMMDD_HHmm_4000M.PNG

Requested history:
  168 hours / 7 days

Behaviour:
  1. Fetch official GEOS_IRX timestamp list.
  2. Ignore timestamps newer than the 60-minute safety cutoff.
  3. Load the newest listed frame from SD if it already exists.
  4. Otherwise download that newest listed frame for the live display.
  5. Compare all eligible listed timestamps with /clouds/YYYY/MM/DD/HHMM.png.
  6. Build a repair queue containing ONLY genuinely missing official frames.
  7. Download the repair queue newest-first.
  8. Any failure remains missing and is retried on a future 15-minute pass.

The old :15 / :30 / :45 probing has been removed.

If the availability API itself is temporarily unavailable, the device has an
emergency fallback which checks whole UTC hours only for the preceding six
hours.

The small on-screen n/total status now represents the CURRENT GAP-REPAIR
QUEUE, not 15-minute timestamps. For example:

  3/8

means three of eight missing officially listed frames have been attempted in
the current repair pass.

Replay timing is intentionally unchanged in this revision. Existing archived
quarter-hour files, if any, remain usable, but new collection follows the
official timestamp list.


SEVEN-DAY COLLECTION REVISION
-----------------------------
NSMC_AVAILABILITY_HOURS is now 168.

On startup and every availability refresh, the unit compares the official
GEOS_IRX timestamps from the preceding seven days with the SD archive.
Existing PNG files are skipped. Only genuinely missing officially-listed
timestamps are placed in the repair queue.

This means a new/empty card can acquire approximately a week's history
automatically. With an existing card, only gaps are requested.

ROBUST BUTTON REVISION
----------------------
The button is no longer sampled by the main rendering loop.

A dedicated FreeRTOS task:
  priority: 2
  sample interval: 5 ms
  debounce: 25 ms
  between-click window: 1000 ms
  long press: 1600 ms

Because this task runs above both the normal Arduino rendering loop and the
priority-0 weather/archive worker, Wi-Fi, PNG decode and SD activity should no
longer cause short button presses to be missed.

Immediate visual feedback:
  first accepted release  -> 1x
  second accepted release -> 2x
  third accepted release  -> 3x, launches immediately

After 1 second with no further press:
  1x -> 24H replay
  2x -> 7D replay

Three accepted clicks launch 30D immediately after the third release.

Long press returns to live mode.

Serial diagnostics:
  [BUTTON] registered click 1
  [BUTTON] registered click 2
  [BUTTON] gesture = 2 clicks


BUTTON COMMAND HANDOFF FIX
--------------------------
Gesture recognition and replay command delivery are now separated cleanly.

The button task still performs the reliable 5 ms sampling/debounce and shows
1x / 2x / 3x immediate feedback.

The FreeRTOS event queue has been removed. A tiny atomic command mailbox now
passes the completed gesture to loop().

Serial diagnostics now distinguish the two stages:

  [BUTTON] gesture = 2 clicks
  [BUTTON] main received event=2
  [REPLAY] 7D start

Event numbers:
  1 = 24H
  2 = 7D
  3 = 30D
  4 = long press / live

Replay gestures are now honoured even if a replay is already active:
  24H -> 7D
  7D  -> 30D
  etc.

When switching replay mode, the original live cloud timestamp is preserved so
long-press/end-of-replay still restores the actual live frame.

The seven-day archive repair loop also stops when a button command is pending,
so it releases the SD/PNG work mutex promptly for replay.


REPLAY START / MUTEX FIX
------------------------
This fixes the regression where [REPLAY] 24H start immediately stopped the
globe but no archive frames appeared.

Replay is now transactional:

  button gesture
       |
       v
  replayStartPending = true
       |
       |  globe still rotates
       |  status shows WAIT
       v
  acquire SD/PNG work lock
       |
       v
  find + decode first real archive PNG
       |
       v
  replayActive = true
       |
       v
  fixed-view replay begins

If no archived frame exists in the requested period, replay is cancelled and
the globe simply remains live and rotating.

The seven-day gap repair no longer holds the shared work mutex for the whole
repair queue. It locks one missing image at a time and releases between files,
allowing replay to pre-empt promptly.

24H playback now uses the confirmed hourly GEOS_IRX cadence:
  24 frames maximum
  500 ms/frame
  about 12 seconds for a complete day

7D remains hourly at 90 ms/frame (about 15 seconds for 168 frames).
30D remains 3-hour sampling.

Expected Serial for a successful 24H request:

  [REPLAY] 24H requested; finding first archive frame
  [REPLAY] Loading 202608221000 UTC from SD
  [PNG] ...
  [REPLAY] 24H started at 202608221000 UTC


RESUME-FROM-STOPPED-LONGITUDE REVISION
--------------------------------------
The live globe no longer derives longitude directly from absolute millis().

Previously:
  replay held the Earth still
  millis() continued advancing
  replay ended
  live formula jumped to the longitude Earth would have reached if it had
  continued spinning invisibly

Now:
  live longitude is accumulated frame by frame
  WAIT still allows the globe to rotate
  when the first replay frame is ready, replay freezes the CURRENT longitude
  during replay, the live rotation phase is paused
  when replay ends or is stopped, live longitude is explicitly set to the
  replay viewpoint
  rotation then continues smoothly from that exact position

Expected Serial:
  [REPLAY] 24H started at 202608231000 UTC, lon=-123.45
  ...
  [REPLAY] Return to live at lon=-123.45

No weather, SD, archive, button, availability, replay cadence or cloud-render
settings changed.
