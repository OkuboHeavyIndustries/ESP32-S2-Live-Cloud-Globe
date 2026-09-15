/*Buy me a coffee!
Bitcoin: 19H3zFF4W3zUZ3jAdjmiDNNLs8Ja46M6AD
ETH: 0xD656DB37b61ac30Fa1e16a3162719FE417b231C8
*/

#include <Arduino.h>
#include <new>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <stdarg.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_heap_caps.h>

// PNGdec 1.1.6 defaults to ((320*4+1)*2)=2562 bytes for its two
// scanline buffers. DecodePNG() positions the two 1281-byte RGBA+filter rows
// at offsets 15 and 1311, so bytes through offset 2591 are used: 2592 bytes
// total. The default therefore overruns into ucFileBuf by 30 bytes at 320px
// RGBA. build_opt.h supplies -DPNG_MAX_BUFFERED_PIXELS=2624 globally so BOTH
// this sketch and PNGdec.cpp are compiled with the same corrected layout.
#include <PNGdec.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ST7789.h>
#include <U8g2lib.h>
#include <AioP13.h>

static_assert(PNG_MAX_BUFFERED_PIXELS >= 2592,
              "PNGdec line buffer still 2562: compile the COMPLETE ZIP/folder with build_opt.h present");

#include "ui_types.h"
#include "globe_screen_map_512x256.h"
#include "nasa_blue_marble_565.h"

// =============================================================================
// CLOUD GLOBE V2
// =============================================================================
//
// A standalone 240x240 true-colour Earth display for the Adafruit QtPy
// ESP32-S2.  The globe combines NASA Blue Marble surface imagery with live
// global IR cloud observations from CMA/NSMC, local day/night shading and an
// independently propagated ISS overlay.
//
// Design notes:
//   * Large buffers live in PSRAM so Wi-Fi/mbedTLS retain scarce internal DRAM.
//   * The ST7789 and microSD use separate SPI buses because this TFT has no CS.
//   * Weather frames are archived to SD and can be replayed with one button.
//   * ISS propagation and pass prediction are local; only the TLE is downloaded.
//   * build_opt.h is REQUIRED.  It increases PNGdec's 320px RGBA scanline
//     buffer consistently in both this sketch and PNGdec.cpp.
//
// Hardware:
//   Adafruit QtPy ESP32-S2
//   ZJY-IPS130-V2.0 / ST7789 240x240
//   SSD1306 128x64 I2C OLED @ 0x3C
//   microSD breakout
//   momentary pushbutton (Cherry MX in the finished unit)
//
// Wiring used by the finished PCB:
//   ST7789:  SCK->SCK, MOSI->MO, RST->A0, DC->A1, VCC/BLK->3V, no CS
//   SSD1306: SDA->SDA, SCL->SCL, VCC->5V, GND->GND
//   microSD: SCK->A2, MOSI->A3, MISO->RX, CS->TX, VCC->3V
//   button:   QtPy MI/MISO pad -> switch -> GND (INPUT_PULLUP)
//
// Runtime configuration:
//   /config.ini on microSD stores Wi-Fi, timezone, observer position, visibility
//   thresholds and feature toggles.  Credentials are never compiled into the
//   sketch.  See config.ini.example in the release folder.
//
// Day/night:
//   Real AioP13 P13Sun Earth-fixed solar vector.
//   Geographic per-pixel shading uses the same lat/lon map as Earth/clouds.
//   Soft twilight and dimmed night hemisphere; historical replay uses its
//   archived timestamp so the terminator is historically correct.
//
// ISS:
//   CelesTrak GP endpoint, ISS (ZARYA) / NORAD 25544 only
//   24h successful-refresh limit, 2h failed-attempt retry limit
//   AioP13 local propagation: true-altitude ISS marker + +/-60 min 3D orbit
//   Past track solid amber; future track bright dashed cyan
//
// Live cloud source:
//   China Meteorological Administration
//   National Satellite Meteorological Center (NSMC)
//   GEO Satellite Global Image — IR 10.8 um
//
// Documented WMS layer:
//   GEOS_IRX
//
// The weather overlay is derived directly from the observed global IR image.
// It is NOT Cloud Fraction, NOT a MODIS swath mask, and NOT synthetic weather.
//
// IR image brightness is used only as a DISPLAY opacity mapping over the
// cloud-free NASA Blue Marble texture.
//
// Button controls:
//   1 click  = replay previous 24 hours
//   2 clicks = replay previous 7 days
//   3 clicks = replay previous 30 days
//   long     = return to live mode
//   any physical press also wakes the OLED forecast page immediately.
// =============================================================================

#define TFT_CS   -1
#define TFT_DC   A1
#define TFT_RST  A0

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);


// =============================================================================
// SSD1306 boot console — genuine U8G2LOG
// =============================================================================
//
// Hardware:
//   SSD1306 128x64 OLED
//   I2C address 0x3C
//   VCC -> 5V
//   GND -> GND
//   SDA -> SDA
//   SCL -> SCL
//
// The OLED serves two roles:
//   1. genuine U8G2LOG startup console
//   2. Okubo Heavy Industries-style ISS pass telemetry while the ISS is above
//      the observer's geometric horizon.
//
// Between passes it is cleared and placed into power-save mode.
// =============================================================================

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

constexpr uint8_t U8LOG_WIDTH = 25;
constexpr uint8_t U8LOG_HEIGHT = 8;

static uint8_t u8logBuffer[
  U8LOG_WIDTH * U8LOG_HEIGHT
];

static U8G2LOG bootLog;

static void beginBootLog() {
  oled.begin();

  // Fixed module address is the standard SSD1306 0x3C.
  // U8g2's SSD1306 HW-I2C constructor already defaults to this address.

  oled.setFont(
    u8g2_font_5x7_tr
  );

  bootLog.begin(
    oled,
    U8LOG_WIDTH,
    U8LOG_HEIGHT,
    u8logBuffer
  );

  bootLog.setLineHeightOffset(0);

  // U8G2LOG mode 0 refreshes on newline.  This is faster and cleaner on a
  // full-buffer U8g2 display than redrawing for every individual character.
  bootLog.setRedrawMode(0);

  bootLog.print(
    "CLOUD GLOBE V2\n"
  );
}

static void bootPrintln(
  const char *message
) {
  if (!message)
    message = "";

  Serial.println(message);

  bootLog.print(message);
  bootLog.print('\n');
}

static void bootPrintf(
  const char *format,
  ...
) {
  char buffer[48];

  va_list args;
  va_start(args, format);

  vsnprintf(
    buffer,
    sizeof(buffer),
    format,
    args
  );

  va_end(args);

  bootPrintln(buffer);
}

static void finishBootLog() {
  // Clear U8G2LOG's text window, then clear the physical OLED.
  bootLog.print("\f\n");

  oled.clearBuffer();
  oled.sendBuffer();

  // The OLED will later be woken only when ISS information needs displaying.
  oled.setPowerSave(1);
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

constexpr uint16_t IR_W = 320;
constexpr uint16_t IR_H = 160;
constexpr uint32_t IR_PIXELS =
    (uint32_t)IR_W * (uint32_t)IR_H;

constexpr uint16_t EARTH_W = 512;
constexpr uint16_t EARTH_H = 256;

// -----------------------------------------------------------------------------
// Runtime memory
// -----------------------------------------------------------------------------

static uint8_t *irLuma = nullptr;
static uint8_t *candidateLuma = nullptr;
static uint8_t *irAlpha = nullptr;
static uint8_t *candidateAlpha = nullptr;

constexpr size_t DOWNLOAD_BUFFER_CAPACITY =
    512UL * 1024UL;

static uint8_t *downloadBuffer = nullptr;
static size_t downloadSize = 0;

static PNG *pngDecoder = nullptr;
static void *pngDecoderMemory = nullptr;

// Global decode state: deliberately simple so Arduino's .ino auto-prototyper
// cannot trip over custom types.
static uint8_t *decodeTarget = nullptr;
static uint8_t *decodeTargetAlpha = nullptr;
static bool decodeError = false;

// -----------------------------------------------------------------------------
// Live product / SD archive / replay state
// -----------------------------------------------------------------------------

static bool haveIRClouds = false;
static char currentIRTime[13] = ""; // YYYYMMDDHHMM UTC

// The official GEOS_IRX availability list is authoritative.
// Use the newest timestamp as soon as NSMC lists it; no deliberate source lag.

// Ask NSMC for its official GEOS_IRX availability list every 15 minutes.
// New imagery is currently hourly, while this shorter poll also gives missing
// hours another chance after a weak-Wi-Fi failure.
constexpr uint32_t UPDATE_INTERVAL_MS =
    15UL * 60UL * 1000UL;

// Ask for a full seven days so the SD archive can be populated/repaired after outages/reboots.
constexpr uint16_t NSMC_AVAILABILITY_HOURS = 168;
constexpr uint16_t NSMC_MAX_AVAILABLE_TIMES = 192;

static char (*nsmcAvailableTimes)[13] = nullptr;
static uint16_t nsmcAvailableCount = 0;

static uint32_t lastUpdateMillis = 0;

// Active display mapping.
static uint8_t *cloudOpacityLUT = nullptr;
static uint8_t globalCloudAlphaScale = 180;
static bool useAlphaPrimaryOpacity = false;

// Candidate mapping.  Downloads/PNG decode build these without touching the
// currently displayed weather.  commitCandidate() swaps them in atomically.
static uint8_t *candidateCloudOpacityLUT = nullptr;
static uint8_t candidateGlobalCloudAlphaScale = 180;
static bool candidateUseAlphaPrimaryOpacity = false;

// User-tuned visual settings.
constexpr uint16_t CLOUD_OPACITY_GAIN_PERCENT = 350;
constexpr uint8_t CLOUD_WHITENING = 255;

// -----------------------------------------------------------------------------
// SD card — separate SPI bus
// -----------------------------------------------------------------------------

constexpr int SD_SCK  = A2;
constexpr int SD_MOSI = A3;
constexpr int SD_MISO = RX;
constexpr int SD_CS   = TX;

constexpr uint32_t SD_SPI_HZ = 4000000;

SPIClass sdSPI(HSPI);
static bool sdReady = false;

// Button: one side to MI, the other side to GND.
// Physical QtPy pad labelled "MI".
// Arduino-ESP32 names that pin MISO (GPIO 37) in the QtPy ESP32-S2 variant.
constexpr int BUTTON_PIN = MISO;

// Archive is never automatically pruned.
static const char *ARCHIVE_ROOT = "/clouds";
static const char *ARCHIVE_INDEX = "/clouds/index.csv";

// -----------------------------------------------------------------------------
// ISS (ZARYA) TLE cache
// -----------------------------------------------------------------------------
//
// CelesTrak etiquette policy:
//   * request ONE object only: NORAD 25544 / ISS (ZARYA)
//   * after a successful refresh, do not request again for 24 hours
//   * if a refresh fails while the cached TLE is stale, wait at least 2 hours
//     before another attempt
//   * persist BOTH timestamps on SD so reboots cannot defeat the throttle
//
// If SD is unavailable we deliberately do NOT contact CelesTrak, because we
// could not persist the throttle across a reboot.

static const char *ISS_TLE_URL =
    "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE";

static const char *ISS_DIR = "/iss";
static const char *ISS_TLE_FILE = "/iss/iss.tle";
static const char *ISS_STATE_FILE = "/iss/fetch_state.txt";

constexpr uint32_t ISS_SUCCESS_INTERVAL_SEC =
    24UL * 60UL * 60UL;

constexpr uint32_t ISS_RETRY_INTERVAL_SEC =
    2UL * 60UL * 60UL;

// The local service check is deliberately much more frequent than either
// network limit; most checks therefore result in zero network traffic.
constexpr uint32_t ISS_SERVICE_INTERVAL_MS =
    5UL * 60UL * 1000UL;

static char issTLEName[32] = "";
static char issTLELine1[80] = "";
static char issTLELine2[80] = "";

static bool haveISSTLE = false;

static time_t issLastAttemptUTC = 0;
static time_t issLastSuccessUTC = 0;

static uint32_t lastISSServiceMillis = 0;

// -----------------------------------------------------------------------------
// ISS local propagation / live globe overlay
// -----------------------------------------------------------------------------
//
// No network traffic is required here.  AioP13 propagates the cached ISS TLE
// locally.  Current position and the +/-60 minute orbit are refreshed together
// every 7 seconds and double-buffered
// so the renderer never reads an array while it is being regenerated.

constexpr float ISS_EARTH_RADIUS_KM = 6378.137f;

// Keep the previous ~45-second sampling density while extending the track
// from +/-45 minutes to +/-60 minutes:
//
//   total span = 120 min = 7200 s
//   160 intervals -> 45 s per interval
//   161 stored points
constexpr uint16_t ISS_ORBIT_POINT_COUNT = 161;
constexpr uint16_t ISS_ORBIT_NOW_INDEX =
    (ISS_ORBIT_POINT_COUNT - 1) / 2;

constexpr int32_t ISS_ORBIT_HALF_WINDOW_SEC = 60 * 60;
constexpr int32_t ISS_ORBIT_STEP_SEC =
    (ISS_ORBIT_HALF_WINDOW_SEC * 2) /
    (ISS_ORBIT_POINT_COUNT - 1);

// At this 112 px Earth radius, one screen pixel corresponds to roughly
// 57 km at the globe surface.  The ISS travels about 7.6 km/s, so ~7 seconds
// is a good match for approximately one displayed pixel of orbital motion.
//
// Position and orbit are deliberately rebuilt on the SAME cadence so the ISS
// glyph and the past/future split always share essentially the same epoch.
constexpr uint32_t ISS_POSITION_INTERVAL_MS = 7000;
constexpr uint32_t ISS_ORBIT_REBUILD_INTERVAL_MS = 7000;

static ISSSpacePoint (*issOrbitBuffers)[ISS_ORBIT_POINT_COUNT] = nullptr;
static volatile uint8_t issOrbitActiveBuffer = 0;
static volatile bool issOrbitValid = false;

static volatile bool issPositionValid = false;
static volatile float issCurrentXER = 0.0f;
static volatile float issCurrentYER = 0.0f;
static volatile float issCurrentZER = 0.0f;

// Retain geographic position for the renderer and observer calculations.
static volatile float issCurrentLatDeg = 0.0f;
static volatile float issCurrentLonDeg = 0.0f;

static portMUX_TYPE issRenderMux =
    portMUX_INITIALIZER_UNLOCKED;

static uint32_t lastISSPropagationMillis = 0;

// -----------------------------------------------------------------------------
// Fixed observer / optical ISS visibility
// -----------------------------------------------------------------------------

static P13Observer *issObserver = nullptr;
static P13Sun issVisibilitySun;

static volatile bool issObserverStatusValid = false;
static volatile bool issAboveHorizon = false;
static volatile bool issOpticallyVisible = false;
static volatile bool issSunlit = false;

static volatile float issObserverElevationDeg = 0.0f;
static volatile float issObserverAzimuthDeg = 0.0f;
static volatile float issObserverRangeKm = 0.0f;
static volatile float issObserverSunElevationDeg = 0.0f;

// Predicted geometric horizon set time derived from the already-built future
// half of the +/-60 minute 3D orbit cache.
static volatile time_t issPredictedSetUTC = 0;

// Predicted culmination of the CURRENT geometric pass, derived from the
// existing +/-60 minute 3D orbit cache.  The elevation value is used for the
// high-quality visible-pass NeoPixel alert; the UTC time drives the live OLED
// MAX IN countdown.
static volatile float issCurrentPassMaxElevationDeg = 0.0f;
static volatile time_t issCurrentPassMaxUTC = 0;

// If the ISS is optically visible during the current pass, cache the next
// true->false visibility transition.  This lets the live OLED show VIS END
// after culmination without doing prediction work on every one-second redraw.
static volatile time_t issCurrentPassVisibleEndUTC = 0;

// Long-range local ISS forecast used by the 15-minute idle OLED screen.
//
// NEXT PASS    = next geometric horizon rise (EL crosses 0 degrees upward)
// NEXT VISIBLE = first future time satisfying the configured optical
//                visibility test.
//
// The scan uses its own AioP13 objects, so it never disturbs the live ISS
// predictor or the +/-60 minute renderer cache.
static volatile time_t issNextPassUTC = 0;
static volatile time_t issNextVisibleUTC = 0;

// Geometric peak elevation of the pass containing NEXT VISIBLE.
static volatile float issNextVisibleMaxElevationDeg = -1.0f;

static volatile time_t issUpcomingForecastComputedUTC = 0;
static volatile bool issUpcomingForecastValid = false;
static volatile bool issUpcomingForecastBusy = false;

static uint32_t lastISSUpcomingForecastServiceMs = 0;

constexpr uint32_t ISS_UPCOMING_FORECAST_SERVICE_MS =
    60UL * 1000UL;

// Scan far enough to survive gaps between naked-eye visibility seasons.
// One-minute coarse samples are refined to a few seconds at the first
// false->true transition.
constexpr int32_t ISS_UPCOMING_FORECAST_STEP_SEC = 60;
constexpr int32_t ISS_UPCOMING_FORECAST_MAX_SEC =
    14L * 24L * 60L * 60L;

static bool lastLoggedObserverValid = false;
static bool lastLoggedAboveHorizon = false;
static bool lastLoggedVisible = false;
static bool lastLoggedSunlit = false;
static uint32_t lastObserverStatusLogMs = 0;

constexpr double SUN_RADIUS_KM = 696340.0;
constexpr double SUN_MEAN_DISTANCE_KM = 149597870.7;

// No guessed 15-minute backfill is used anymore.  The official NSMC
// availability list is the source of truth for which timestamps should exist.

// -----------------------------------------------------------------------------
// Thread / buffer coordination
// -----------------------------------------------------------------------------

static SemaphoreHandle_t cloudMutex = nullptr;
static SemaphoreHandle_t workMutex = nullptr;
static TaskHandle_t weatherTaskHandle = nullptr;

// Background activity shown beside the live clock.
static volatile bool liveFetchBusy = false;
static volatile bool backfillActive = false;
static volatile uint16_t backfillDone = 0;
static volatile uint16_t backfillTotal = 0;

// Gap repair is intentionally incremental.  A pass walks the official NSMC
// availability list newest-first, but downloads at most ONE missing frame each
// time the weather worker comes around.  This prevents a large archive backlog
// from starving higher-priority ISS forecast work.
static bool gapRepairPassPending = false;
static int16_t gapRepairCursor = -1;
static uint16_t gapRepairSaved = 0;
static uint16_t gapRepairFailed = 0;

// Current HTTP body progress.  Used only for the small on-screen NET indicator.
static volatile uint32_t httpBodyBytes = 0;
static volatile int32_t httpBodyExpected = -1;

// Last NSMC transport-layer result. Negative HTTPClient codes mean the request
// never reached a normal HTTP response (DNS/TCP/TLS/connect path).
static int lastNSMCTransportCode = 0;

// configTime() starts the ESP32 SNTP client asynchronously. Keep it running
// instead of restarting it every time a short synchronous wait expires.
static bool ntpClientStarted = false;
static bool ntpTimeConfirmed = false;

// -----------------------------------------------------------------------------
// Replay state
// -----------------------------------------------------------------------------

static volatile bool replayActive = false;

// Replay requests remain pending until the first archived PNG has actually
// been opened and decoded.  The globe keeps spinning during this state.
static volatile bool replayStartPending = false;

static ReplayMode replayMode = ReplayMode::Live;

static time_t replayCursorUTC = 0;
static time_t replayEndUTC = 0;
static time_t replayDisplayUTC = 0;
static uint32_t replayStepSeconds = 900;
static uint32_t replayFrameIntervalMs = 110;
static uint32_t replayNextFrameMs = 0;

// Live globe rotation is accumulated rather than derived directly from
// absolute millis().  That allows replay to pause rotation and then resume
// from exactly the same longitude instead of jumping ahead by elapsed time.
static float liveLongitudeDeg = 0.0f;
static uint32_t liveSpinLastMs = 0;
static bool liveSpinInitialised = false;

static float replayFixedLongitudeDeg = 0.0f;
static char replaySavedLiveStamp[13] = "";

// -----------------------------------------------------------------------------
// Button state
// -----------------------------------------------------------------------------
//
// The button is deliberately handled by its own high-priority FreeRTOS task.
// That makes click recognition independent of globe rendering, SD PNG decode,
// Wi-Fi reconnects and archive repair work on the single-core ESP32-S2.

static TaskHandle_t buttonTaskHandle = nullptr;

// Atomic one-command mailbox between the dedicated button task and loop().
static portMUX_TYPE buttonCommandMux =
    portMUX_INITIALIZER_UNLOCKED;

static volatile uint8_t pendingButtonEvent =
    (uint8_t)ButtonEvent::None;

// Separate one-bit mailbox for the SSD1306 forecast page.
//
// This is deliberately triggered by the PHYSICAL debounced press rather than
// by the later Single/Double/Triple/LongPress gesture.  Therefore the OLED
// responds immediately while the cloud-playback gesture is still being built.
static volatile bool pendingISSOLEDManualForecast =
    false;

// Immediate visual acknowledgement while a click sequence is being assembled.
// 0 = no pending sequence; 1/2/3 = number of accepted short presses so far.
static volatile uint8_t buttonPreviewClicks = 0;

constexpr uint32_t BUTTON_POLL_MS = 5;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;

// Once a short click is released, wait this long for another press.
// The dedicated task means this can now be comfortable rather than "mouse fast".
constexpr uint32_t BUTTON_MULTI_CLICK_MS = 1000;

constexpr uint32_t BUTTON_LONG_PRESS_MS = 1600;

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

static void *allocPSRAMPreferred(size_t bytes) {
  void *p = nullptr;

  if (psramFound()) {
    p = heap_caps_malloc(
      bytes,
      MALLOC_CAP_SPIRAM |
      MALLOC_CAP_8BIT
    );
  }

  if (!p)
    p = malloc(bytes);

  return p;
}

static bool allocateRuntimeMemory() {
  // Preserve scarce internal DRAM for Wi-Fi/mbedTLS.  These retained tables
  // do not need DMA/internal memory and are ideal PSRAM residents.
  nsmcAvailableTimes =
      (char (*)[13])allocPSRAMPreferred(
        NSMC_MAX_AVAILABLE_TIMES * 13UL
      );

  issOrbitBuffers =
      (ISSSpacePoint (*)[ISS_ORBIT_POINT_COUNT])allocPSRAMPreferred(
        2UL * ISS_ORBIT_POINT_COUNT * sizeof(ISSSpacePoint)
      );

  cloudOpacityLUT =
      (uint8_t *)allocPSRAMPreferred(
        256
      );

  candidateCloudOpacityLUT =
      (uint8_t *)allocPSRAMPreferred(
        256
      );

  irLuma =
      (uint8_t *)allocPSRAMPreferred(
        IR_PIXELS
      );

  candidateLuma =
      (uint8_t *)allocPSRAMPreferred(
        IR_PIXELS
      );

  irAlpha =
      (uint8_t *)allocPSRAMPreferred(
        IR_PIXELS
      );

  candidateAlpha =
      (uint8_t *)allocPSRAMPreferred(
        IR_PIXELS
      );

  downloadBuffer =
      (uint8_t *)allocPSRAMPreferred(
        DOWNLOAD_BUFFER_CAPACITY
      );

  if (
    !nsmcAvailableTimes ||
    !issOrbitBuffers ||
    !cloudOpacityLUT ||
    !candidateCloudOpacityLUT ||
    !irLuma ||
    !candidateLuma ||
    !irAlpha ||
    !candidateAlpha ||
    !downloadBuffer
  ) {
    Serial.println(
      "[MEM] Runtime buffer allocation failed"
    );

    return false;
  }

  memset(
    nsmcAvailableTimes,
    0,
    NSMC_MAX_AVAILABLE_TIMES * 13UL
  );

  memset(
    issOrbitBuffers,
    0,
    2UL * ISS_ORBIT_POINT_COUNT * sizeof(ISSSpacePoint)
  );

  memset(cloudOpacityLUT, 0, 256);
  memset(candidateCloudOpacityLUT, 0, 256);

  memset(irLuma, 0, IR_PIXELS);
  memset(candidateLuma, 0, IR_PIXELS);
  memset(irAlpha, 0, IR_PIXELS);
  memset(candidateAlpha, 0, IR_PIXELS);

  return true;
}

static bool ensurePNGDecoder() {
  if (pngDecoder)
    return true;

  const size_t bytes =
      sizeof(PNG);

  // Prefer internal RAM after the HTTP object has released its TLS buffers.
  // PSRAM is a safe fallback; build_opt.h fixes the unrelated PNGdec scanline
  // buffer issue that previously corrupted 320x160 RGBA source images.
  pngDecoderMemory =
      heap_caps_malloc(
        bytes,
        MALLOC_CAP_INTERNAL |
        MALLOC_CAP_8BIT
      );

  if (!pngDecoderMemory) {
    pngDecoderMemory =
        allocPSRAMPreferred(
          bytes
        );
  }

  if (!pngDecoderMemory) {
    Serial.printf(
      "[PNG] Decoder allocation failed: %u bytes\n",
      (unsigned)bytes
    );

    return false;
  }

  pngDecoder =
      new (pngDecoderMemory) PNG();

  return true;
}

static void releasePNGDecoder() {
  if (pngDecoder) {
    pngDecoder->~PNG();
    pngDecoder = nullptr;
  }

  if (pngDecoderMemory) {
    free(pngDecoderMemory);
    pngDecoderMemory = nullptr;
  }
}

// -----------------------------------------------------------------------------
// HTTP RAM sink
// -----------------------------------------------------------------------------

class RAMWriteStream : public Stream {
public:
  RAMWriteStream(
    uint8_t *buffer,
    size_t capacity
  )
  : _buffer(buffer),
    _capacity(capacity),
    _size(0),
    _overflow(false)
  {
  }

  size_t write(uint8_t value) override {
    if (_size >= _capacity) {
      _overflow = true;
      return 0;
    }

    _buffer[_size++] = value;
    return 1;
  }

  size_t write(
    const uint8_t *buffer,
    size_t size
  ) override {
    if (!buffer || size == 0)
      return 0;

    const size_t room =
        (_size < _capacity) ?
        (_capacity - _size) :
        0;

    const size_t n =
        (size <= room) ?
        size :
        room;

    if (n > 0) {
      memcpy(
        _buffer + _size,
        buffer,
        n
      );

      _size += n;
    }

    if (n != size)
      _overflow = true;

    return n;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t size() const { return _size; }
  bool overflowed() const { return _overflow; }

private:
  uint8_t *_buffer;
  size_t _capacity;
  size_t _size;
  bool _overflow;
};

static bool hasPNGSignature(
  const uint8_t *data,
  size_t bytes
) {
  static const uint8_t sig[8] = {
    0x89, 'P', 'N', 'G',
    0x0D, 0x0A, 0x1A, 0x0A
  };

  return
      data &&
      bytes >= 8 &&
      memcmp(data, sig, 8) == 0;
}

static bool hasPNGIEND(
  const uint8_t *data,
  size_t bytes
) {
  static const uint8_t tail[12] = {
    0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4E, 0x44,
    0xAE, 0x42, 0x60, 0x82
  };

  return
      data &&
      bytes >= sizeof(tail) &&
      memcmp(
        data + bytes - sizeof(tail),
        tail,
        sizeof(tail)
      ) == 0;
}

// -----------------------------------------------------------------------------
// SD-card runtime configuration
// -----------------------------------------------------------------------------

static const char *RUNTIME_CONFIG_FILE =
    "/config.ini";

// Runtime configuration lives directly in the sketch so replacing only the
// .ino in an existing Arduino project does not require a matching new header.
struct AppRuntimeConfig {
  char wifiSSID[65];
  char wifiPassword[65];

  int16_t utcOffsetMinutes;
  char timezoneLabel[12];

  bool enableISS;
  bool enableClouds;
  bool enableDayNight;

  // Fixed observer position used for ISS azimuth/elevation and visibility.
  // Latitude: north positive, south negative.
  // Longitude: east positive, west negative.
  // Altitude: metres above sea level.
  double observerLatDeg;
  double observerLonDeg;
  double observerAltM;
  bool observerConfigured;

  // Practical visibility thresholds.
  double visibleMinElevationDeg;
  double visibleSunMaxElevationDeg;
};

static AppRuntimeConfig appConfig;

// -----------------------------------------------------------------------------
// Onboard QtPy NeoPixel — ISS observer status
// -----------------------------------------------------------------------------
//
// Plain uint8_t state constants are used deliberately. Arduino auto-generates
// function prototypes before much of the .ino is compiled; built-in types keep
// those generated prototypes independent of declaration order.
//
// OFF             = ISS below horizon / observer unavailable
// BLUE solid      = above horizon, eclipsed
// PURPLE solid    = above horizon, sunlit, not optically visible
// GREEN flashing  = optically visible

constexpr uint8_t ISS_NEO_STATE_OFF = 0;
constexpr uint8_t ISS_NEO_STATE_ABOVE_ECLIPSED = 1;
constexpr uint8_t ISS_NEO_STATE_ABOVE_SUNLIT_NOT_VISIBLE = 2;
constexpr uint8_t ISS_NEO_STATE_VISIBLE = 3;

static uint8_t lastISSNeoPixelState =
    ISS_NEO_STATE_OFF;

static bool lastISSNeoPixelFlashOn = false;
static bool lastISSNeoPixelGreatPass = false;

constexpr uint32_t ISS_VISIBLE_FLASH_HALF_PERIOD_MS = 700;

// "Really great" visible pass: geometric maximum elevation >= 60 degrees.
// 233 ms is approximately 3x the normal 700 ms half-period.
constexpr float ISS_GREAT_PASS_MIN_MAX_EL_DEG = 60.0f;
constexpr uint32_t ISS_VISIBLE_GREAT_FLASH_HALF_PERIOD_MS = 233;

constexpr uint8_t ISS_NEO_BLUE_B = 56;
constexpr uint8_t ISS_NEO_PURPLE_R = 48;
constexpr uint8_t ISS_NEO_PURPLE_B = 58;
constexpr uint8_t ISS_NEO_GREEN_G = 72;

#if defined(PIN_NEOPIXEL)
static Adafruit_NeoPixel issNeoPixel(
  1,
  PIN_NEOPIXEL,
  NEO_GRB + NEO_KHZ800
);
#endif

static void writeISSNeoPixel(
  uint8_t r,
  uint8_t g,
  uint8_t b
) {
#if defined(PIN_NEOPIXEL)
  issNeoPixel.setPixelColor(
    0,
    issNeoPixel.Color(
      r,
      g,
      b
    )
  );
  issNeoPixel.show();
#else
  (void)r;
  (void)g;
  (void)b;
#endif
}

static void beginISSNeoPixel() {
#if defined(NEOPIXEL_POWER)
  pinMode(
    NEOPIXEL_POWER,
    OUTPUT
  );

#if defined(NEOPIXEL_POWER_ON)
  digitalWrite(
    NEOPIXEL_POWER,
    NEOPIXEL_POWER_ON
  );
#else
  digitalWrite(
    NEOPIXEL_POWER,
    HIGH
  );
#endif
#endif

#if defined(PIN_NEOPIXEL)
  issNeoPixel.begin();
  issNeoPixel.setBrightness(
    255
  );
  issNeoPixel.clear();
  issNeoPixel.show();
#endif
}

static uint8_t determineISSNeoPixelState() {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured
  ) {
    return
        ISS_NEO_STATE_OFF;
  }

  bool statusValid = false;
  bool above = false;
  bool visible = false;
  bool sunlit = false;

  portENTER_CRITICAL(
    &issRenderMux
  );

  statusValid =
      issObserverStatusValid;

  above =
      issAboveHorizon;

  visible =
      issOpticallyVisible;

  sunlit =
      issSunlit;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (
    !statusValid ||
    !above
  ) {
    return
        ISS_NEO_STATE_OFF;
  }

  if (visible) {
    return
        ISS_NEO_STATE_VISIBLE;
  }

  if (sunlit) {
    return
        ISS_NEO_STATE_ABOVE_SUNLIT_NOT_VISIBLE;
  }

  return
      ISS_NEO_STATE_ABOVE_ECLIPSED;
}

static const char *issNeoPixelStateName(
  uint8_t state
) {
  switch (state) {
    case ISS_NEO_STATE_ABOVE_ECLIPSED:
      return "BLUE / ABOVE ECLIPSED";

    case ISS_NEO_STATE_ABOVE_SUNLIT_NOT_VISIBLE:
      return "PURPLE / ABOVE SUNLIT";

    case ISS_NEO_STATE_VISIBLE:
      return "GREEN FLASH / VISIBLE";

    case ISS_NEO_STATE_OFF:
    default:
      return "OFF";
  }
}

static void serviceISSNeoPixel() {
  const uint8_t state =
      determineISSNeoPixelState();

  const uint32_t nowMs =
      millis();

  float currentPassMaxEl = 0.0f;

  portENTER_CRITICAL(
    &issRenderMux
  );

  currentPassMaxEl =
      issCurrentPassMaxElevationDeg;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  const bool greatPass =
      state ==
        ISS_NEO_STATE_VISIBLE &&
      currentPassMaxEl >=
        ISS_GREAT_PASS_MIN_MAX_EL_DEG;

  const uint32_t visibleFlashHalfPeriodMs =
      greatPass ?
      ISS_VISIBLE_GREAT_FLASH_HALF_PERIOD_MS :
      ISS_VISIBLE_FLASH_HALF_PERIOD_MS;

  bool flashOn = true;

  if (
    state ==
    ISS_NEO_STATE_VISIBLE
  ) {
    flashOn =
        (
          nowMs /
          visibleFlashHalfPeriodMs
        ) &
        1u;
  }

  const bool stateChanged =
      state !=
      lastISSNeoPixelState;

  const bool flashChanged =
      state ==
        ISS_NEO_STATE_VISIBLE &&
      flashOn !=
        lastISSNeoPixelFlashOn;

  const bool greatPassChanged =
      state ==
        ISS_NEO_STATE_VISIBLE &&
      greatPass !=
        lastISSNeoPixelGreatPass;

  if (
    !stateChanged &&
    !flashChanged &&
    !greatPassChanged
  ) {
    return;
  }

  lastISSNeoPixelState =
      state;

  lastISSNeoPixelFlashOn =
      flashOn;

  lastISSNeoPixelGreatPass =
      greatPass;

  if (
    stateChanged ||
    greatPassChanged
  ) {
    if (
      state ==
        ISS_NEO_STATE_VISIBLE
    ) {
      Serial.printf(
        "[ISS NEO] %s MAXEL=%.1f FLASH=%lums\n",
        greatPass ?
          "GREEN FAST / GREAT VISIBLE" :
          "GREEN / VISIBLE",
        (double)currentPassMaxEl,
        (unsigned long)visibleFlashHalfPeriodMs
      );
    }
    else if (stateChanged) {
      Serial.printf(
        "[ISS NEO] %s\n",
        issNeoPixelStateName(
          state
        )
      );
    }
  }

  switch (state) {
    case ISS_NEO_STATE_ABOVE_ECLIPSED:
      writeISSNeoPixel(
        0,
        0,
        ISS_NEO_BLUE_B
      );
      break;

    case ISS_NEO_STATE_ABOVE_SUNLIT_NOT_VISIBLE:
      writeISSNeoPixel(
        ISS_NEO_PURPLE_R,
        0,
        ISS_NEO_PURPLE_B
      );
      break;

    case ISS_NEO_STATE_VISIBLE:
      if (flashOn) {
        writeISSNeoPixel(
          0,
          ISS_NEO_GREEN_G,
          0
        );
      }
      else {
        writeISSNeoPixel(
          0,
          0,
          0
        );
      }
      break;

    case ISS_NEO_STATE_OFF:
    default:
      writeISSNeoPixel(
        0,
        0,
        0
      );
      break;
  }
}


// -----------------------------------------------------------------------------
// SSD1306 ISS pass screen 
// -----------------------------------------------------------------------------
//
// 
//   128x64 outer frame
//   horizontal rules y=9, 27, 54
//   u8g2_font_u8glib_4_tr
//   compact telemetry
//   outlined/inverted status boxes
//   
//
// The OLED wakes at the geometric horizon (EL > 0), not the configurable
// visible-elevation threshold. This allows it to show the whole pass.

constexpr uint32_t ISS_OLED_REFRESH_MS = 1000;

// When the ISS is below the horizon, wake the OLED on local wall-clock quarter
// hours (:00, :15, :30, :45) and leave the forecast page visible for 30 seconds.
constexpr uint32_t ISS_OLED_FORECAST_INTERVAL_MS =
    15UL * 60UL * 1000UL;

constexpr uint32_t ISS_OLED_FORECAST_DURATION_MS =
    30UL * 1000UL;

constexpr uint8_t ISS_OLED_MODE_SLEEP = 0;
constexpr uint8_t ISS_OLED_MODE_PASS = 1;
constexpr uint8_t ISS_OLED_MODE_FORECAST = 2;

static bool issOLEDActive = false;
static uint8_t issOLEDMode =
    ISS_OLED_MODE_SLEEP;

static uint32_t lastISSOLEDRefreshMs = 0;

// Forecast page timing is aligned to local wall-clock quarter hours:
//
//   hh:00
//   hh:15
//   hh:30
//   hh:45
//
// Slot number is derived from configured local time rather than millis().
// Forecast start is additionally restricted to the first 30 seconds of a real
// quarter-hour, so NTP/time-sync corrections cannot masquerade as boundaries.
static bool issOLEDForecastClockInitialised = false;
static int64_t issOLEDForecastLastQuarterSlot = -1;

static uint32_t issOLEDForecastUntilMs = 0;

// Explicit button-triggered forecast window.  This is separate from the
// quarter-hour window so a button press can happen at any time and always gets
// a full 30 seconds from the most recent physical press.
static uint32_t issOLEDManualForecastUntilMs = 0;

// Tiny 8x8 ISS-ish glyph: body + solar-panel silhouette.
static const uint8_t PROGMEM ISS_OLED_GLYPH[] = {
  0x24,
  0x7E,
  0x3C,
  0xFF,
  0xFF,
  0x3C,
  0x7E,
  0x24
};

static void drawOHIStatusBox(
  uint8_t x,
  uint8_t y,
  uint8_t w,
  const char *label,
  bool active
) {
  constexpr uint8_t h = 11;

  if (active) {
    oled.setDrawColor(1);

    oled.drawBox(
      x,
      y,
      w,
      h
    );

    oled.setDrawColor(0);
  }
  else {
    oled.setDrawColor(1);

    oled.drawFrame(
      x,
      y,
      w,
      h
    );
  }

  const uint8_t textWidth =
      oled.getStrWidth(
        label
      );

  const uint8_t textX =
      x +
      (
        w >
          textWidth ?
        (
          w -
          textWidth
        ) /
        2 :
        1
      );

  oled.drawStr(
    textX,
    y + 8,
    label
  );

  oled.setDrawColor(1);
}

// U8g2's compact u8glib font does not reliably expose a UTF-8 degree glyph.
// Draw the telemetry text normally and add a tiny vector degree mark so the
// symbol is guaranteed to appear on every SSD1306 build.
static void drawISSOLEDTextWithDegree(
  int16_t x,
  int16_t baselineY,
  const char *text
) {
  if (!text)
    return;

  oled.drawStr(
    x,
    baselineY,
    text
  );

  const int16_t degreeX =
      x +
      (int16_t)oled.getStrWidth(
        text
      ) +
      2;

  oled.drawCircle(
    degreeX,
    baselineY - 5,
    1,
    U8G2_DRAW_ALL
  );
}

static void formatISSOLEDLocalTime(
  time_t utc,
  char *out,
  size_t outSize
) {
  if (
    !out ||
    outSize == 0 ||
    utc < 1700000000
  ) {
    if (
      out &&
      outSize > 0
    ) {
      strlcpy(
        out,
        "--:--",
        outSize
      );
    }

    return;
  }

  const time_t localTime =
      utc +
      (time_t)
      appConfig.utcOffsetMinutes *
      60;

  struct tm t = {};

  if (
    !gmtime_r(
      &localTime,
      &t
    )
  ) {
    strlcpy(
      out,
      "--:--",
      outSize
    );

    return;
  }

  snprintf(
    out,
    outSize,
    "%02d:%02d",
    t.tm_hour,
    t.tm_min
  );
}

static void formatISSOLEDLocalDateTime(
  time_t utc,
  char *out,
  size_t outSize
) {
  if (
    !out ||
    outSize == 0
  ) {
    return;
  }

  if (utc < 1700000000) {
    strlcpy(
      out,
      "-- --- --:--",
      outSize
    );

    return;
  }

  const time_t localTime =
      utc +
      (time_t)
      appConfig.utcOffsetMinutes *
      60;

  struct tm t = {};

  if (
    !gmtime_r(
      &localTime,
      &t
    )
  ) {
    strlcpy(
      out,
      "-- --- --:--",
      outSize
    );

    return;
  }

  static const char *MONTHS[12] = {
    "JAN", "FEB", "MAR", "APR",
    "MAY", "JUN", "JUL", "AUG",
    "SEP", "OCT", "NOV", "DEC"
  };

  snprintf(
    out,
    outSize,
    "%02d %s %02d:%02d",
    t.tm_mday,
    MONTHS[
      t.tm_mon
    ],
    t.tm_hour,
    t.tm_min
  );
}


static void sleepISSOLED() {
  if (!issOLEDActive)
    return;

  oled.clearBuffer();
  oled.sendBuffer();
  oled.setPowerSave(1);

  issOLEDActive = false;
  issOLEDMode =
      ISS_OLED_MODE_SLEEP;

  Serial.println(
    "[ISS OLED] Sleep"
  );
}

static void drawISSOLEDScreen() {
  bool statusValid = false;
  bool above = false;
  bool sunlit = false;
  bool visible = false;

  float elevation = 0.0f;
  float azimuth = 0.0f;
  float rangeKm = 0.0f;
  float sunElevation = 0.0f;

  time_t predictedSetUTC = 0;
  time_t predictedMaxUTC = 0;
  time_t predictedVisibleEndUTC = 0;

  portENTER_CRITICAL(
    &issRenderMux
  );

  statusValid =
      issObserverStatusValid;

  above =
      issAboveHorizon;

  sunlit =
      issSunlit;

  visible =
      issOpticallyVisible;

  elevation =
      issObserverElevationDeg;

  azimuth =
      issObserverAzimuthDeg;

  rangeKm =
      issObserverRangeKm;

  sunElevation =
      issObserverSunElevationDeg;

  predictedSetUTC =
      issPredictedSetUTC;

  predictedMaxUTC =
      issCurrentPassMaxUTC;

  predictedVisibleEndUTC =
      issCurrentPassVisibleEndUTC;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (
    !statusValid ||
    !above
  ) {
    sleepISSOLED();
    return;
  }

  if (
    !issOLEDActive ||
    issOLEDMode !=
      ISS_OLED_MODE_PASS
  ) {
    oled.setPowerSave(0);
    issOLEDActive = true;
    issOLEDMode =
        ISS_OLED_MODE_PASS;

    Serial.println(
      "[ISS OLED] Wake PASS"
    );
  }

  oled.clearBuffer();

  oled.setFontMode(1);
  oled.setDrawColor(1);
  oled.setFont(
    u8g2_font_u8glib_4_tr
  );

  // OHI frame and horizontal architecture.
  oled.drawFrame(
    0,
    0,
    128,
    64
  );

  oled.drawHLine(
    0,
    9,
    128
  );

  oled.drawHLine(
    0,
    27,
    128
  );

  oled.drawHLine(
    0,
    54,
    128
  );

  // Header.
  oled.drawXBMP(
    3,
    1,
    8,
    8,
    ISS_OLED_GLYPH
  );

  oled.drawStr(
    14,
    7,
    "ISS PASS"
  );

  char currentTime[8];

  formatISSOLEDLocalTime(
    time(nullptr),
    currentTime,
    sizeof(currentTime)
  );

  const uint8_t timeWidth =
      oled.getStrWidth(
        currentTime
      );

  oled.drawStr(
    124 -
      timeWidth,
    7,
    currentTime
  );

  // Primary telemetry.
  char leftLine[24];
  char rightLine[24];

  snprintf(
    leftLine,
    sizeof(leftLine),
    "EL:%+5.1f",
    (double)elevation
  );

  snprintf(
    rightLine,
    sizeof(rightLine),
    "AZ:%5.1f",
    (double)azimuth
  );

  drawISSOLEDTextWithDegree(
    4,
    17,
    leftLine
  );

  drawISSOLEDTextWithDegree(
    70,
    17,
    rightLine
  );

  snprintf(
    leftLine,
    sizeof(leftLine),
    "RNG:%4.0f km",
    (double)rangeKm
  );

  char setText[8];

  formatISSOLEDLocalTime(
    predictedSetUTC,
    setText,
    sizeof(setText)
  );

  snprintf(
    rightLine,
    sizeof(rightLine),
    "SET:%s",
    setText
  );

  oled.drawStr(
    4,
    25,
    leftLine
  );

  oled.drawStr(
    70,
    25,
    rightLine
  );

  // Three familiar OHI-style state boxes.
  drawOHIStatusBox(
    3,
    31,
    34,
    "ABOVE",
    above
  );

  drawOHIStatusBox(
    40,
    31,
    38,
    "SUNLIT",
    sunlit
  );

  drawOHIStatusBox(
    81,
    31,
    43,
    "VISIBLE",
    visible
  );

  // Local observing conditions plus one phase-aware current-pass countdown.
  snprintf(
    leftLine,
    sizeof(leftLine),
    "SUN:%+5.1f",
    (double)sunElevation
  );

  drawISSOLEDTextWithDegree(
    4,
    51,
    leftLine
  );

  const time_t nowUTC =
      time(nullptr);

  long remainingSec = 0;

  if (
    predictedMaxUTC >
      nowUTC
  ) {
    remainingSec =
        (long)(
          predictedMaxUTC -
          nowUTC
        );

    snprintf(
      rightLine,
      sizeof(rightLine),
      "MAX IN %ld s",
      remainingSec
    );
  }
  else if (
    visible &&
    predictedVisibleEndUTC >
      nowUTC
  ) {
    remainingSec =
        (long)(
          predictedVisibleEndUTC -
          nowUTC
        );

    snprintf(
      rightLine,
      sizeof(rightLine),
      "VIS END %ld s",
      remainingSec
    );
  }
  else if (
    predictedSetUTC >
      nowUTC
  ) {
    remainingSec =
        (long)(
          predictedSetUTC -
          nowUTC
        );

    snprintf(
      rightLine,
      sizeof(rightLine),
      "PASS END %ld s",
      remainingSec
    );
  }
  else {
    strlcpy(
      rightLine,
      "PASS END -- s",
      sizeof(rightLine)
    );
  }

  // Right-align the variable-length countdown so the final unit stays fixed.
  const uint8_t countdownWidth =
      oled.getStrWidth(
        rightLine
      );

  oled.drawStr(
    124 -
      countdownWidth,
    51,
    rightLine
  );

  // House footer.
  const char *footer =
      "OKUBO HEAVY INDUSTRIES";

  // OHI house footer: deliberately left-justified, close to but clear of the
  // outer frame.
  oled.drawStr(
    4,
    62,
    footer
  );

  oled.sendBuffer();
}

static void drawISSForecastOLEDScreen() {
  // Final semantic guard: never knowingly put a past event under a NEXT label.
  //
  // Normally serviceISSUpcomingForecast() has already retired it. This catches
  // the narrow race where a quarter-hour OLED wake occurs before the next
  // one-minute background forecast service.
  const time_t displayNowUTC =
      time(nullptr);

  bool needsImmediateForecastRefresh = false;

  if (
    displayNowUTC >= 1700000000 &&
    !issUpcomingForecastBusy
  ) {
    time_t cachedPassUTC = 0;
    time_t cachedVisibleUTC = 0;
    bool cachedValid = false;

    portENTER_CRITICAL(
      &issRenderMux
    );

    cachedValid =
        issUpcomingForecastValid;

    cachedPassUTC =
        issNextPassUTC;

    cachedVisibleUTC =
        issNextVisibleUTC;

    portEXIT_CRITICAL(
      &issRenderMux
    );

    if (
      cachedValid &&
      (
        (
          cachedPassUTC != 0 &&
          cachedPassUTC <=
            displayNowUTC
        ) ||
        (
          cachedVisibleUTC != 0 &&
          cachedVisibleUTC <=
            displayNowUTC
        )
      )
    ) {
      needsImmediateForecastRefresh =
          true;
    }
  }

  if (needsImmediateForecastRefresh) {
    Serial.println(
      "[ISS FORECAST] OLED found stale NEXT event; refreshing"
    );

    refreshISSUpcomingForecast();
  }

  bool forecastValid = false;
  bool forecastBusy = false;

  time_t nextPassUTC = 0;
  time_t nextVisibleUTC = 0;
  float nextVisibleMaxEl = -1.0f;

  portENTER_CRITICAL(
    &issRenderMux
  );

  forecastValid =
      issUpcomingForecastValid;

  forecastBusy =
      issUpcomingForecastBusy;

  nextPassUTC =
      issNextPassUTC;

  nextVisibleUTC =
      issNextVisibleUTC;

  nextVisibleMaxEl =
      issNextVisibleMaxElevationDeg;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (
    !issOLEDActive ||
    issOLEDMode !=
      ISS_OLED_MODE_FORECAST
  ) {
    oled.setPowerSave(0);
    issOLEDActive = true;
    issOLEDMode =
        ISS_OLED_MODE_FORECAST;

    Serial.println(
      "[ISS OLED] Wake FORECAST"
    );
  }

  oled.clearBuffer();
  oled.setFontMode(1);
  oled.setDrawColor(1);
  oled.setFont(
    u8g2_font_u8glib_4_tr
  );

  // OHI house frame.
  oled.drawFrame(
    0,
    0,
    128,
    64
  );

  oled.drawHLine(
    0,
    9,
    128
  );

  oled.drawHLine(
    0,
    31,
    128
  );

  oled.drawHLine(
    0,
    54,
    128
  );

  oled.drawXBMP(
    3,
    1,
    8,
    8,
    ISS_OLED_GLYPH
  );

  oled.drawStr(
    14,
    7,
    "ISS FORECAST"
  );

  char currentTime[8];

  formatISSOLEDLocalTime(
    time(nullptr),
    currentTime,
    sizeof(currentTime)
  );

  const uint8_t timeWidth =
      oled.getStrWidth(
        currentTime
      );

  oled.drawStr(
    124 -
      timeWidth,
    7,
    currentTime
  );

  char dateTimeText[20];

  oled.drawStr(
    4,
    17,
    "NEXT PASS"
  );

  if (
    forecastValid &&
    nextPassUTC != 0
  ) {
    formatISSOLEDLocalDateTime(
      nextPassUTC,
      dateTimeText,
      sizeof(dateTimeText)
    );
  }
  else if (forecastBusy) {
    strlcpy(
      dateTimeText,
      "CALCULATING",
      sizeof(dateTimeText)
    );
  }
  else {
    strlcpy(
      dateTimeText,
      "NOT FOUND <14D",
      sizeof(dateTimeText)
    );
  }

  oled.drawStr(
    4,
    27,
    dateTimeText
  );

  oled.drawStr(
    4,
    39,
    "NEXT VISIBLE"
  );

  if (
    forecastValid &&
    nextVisibleUTC != 0
  ) {
    formatISSOLEDLocalDateTime(
      nextVisibleUTC,
      dateTimeText,
      sizeof(dateTimeText)
    );
  }
  else if (forecastBusy) {
    strlcpy(
      dateTimeText,
      "CALCULATING",
      sizeof(dateTimeText)
    );
  }
  else {
    strlcpy(
      dateTimeText,
      "NOT FOUND <14D",
      sizeof(dateTimeText)
    );
  }

  oled.drawStr(
    4,
    50,
    dateTimeText
  );

  if (
    forecastValid &&
    nextVisibleUTC != 0 &&
    nextVisibleMaxEl >= 0.0f
  ) {
    char maxText[16];

    snprintf(
      maxText,
      sizeof(maxText),
      "MAX EL %.0f",
      (double)nextVisibleMaxEl
    );

    const uint8_t maxWidth =
        oled.getStrWidth(
          maxText
        );

    // Allow four pixels for the vector degree mark when right-aligning.
    drawISSOLEDTextWithDegree(
      120 -
        maxWidth,
      50,
      maxText
    );
  }

  const char *footer =
      "OKUBO HEAVY INDUSTRIES";

  // OHI house footer: deliberately left-justified, close to but clear of the
  // outer frame.
  oled.drawStr(
    4,
    62,
    footer
  );

  oled.sendBuffer();
}


static bool getLocalQuarterHourState(
  int64_t &slot,
  uint16_t &secondsIntoQuarter
) {
  const time_t nowUTC =
      time(nullptr);

  if (nowUTC < 1700000000)
    return false;

  const int64_t localEpoch =
      (int64_t)nowUTC +
      (
        (int64_t)
        appConfig.utcOffsetMinutes *
        60LL
      );

  constexpr int64_t QUARTER_SEC =
      15LL * 60LL;

  slot =
      localEpoch /
      QUARTER_SEC;

  int64_t remainder =
      localEpoch %
      QUARTER_SEC;

  if (remainder < 0)
    remainder += QUARTER_SEC;

  secondsIntoQuarter =
      (uint16_t)remainder;

  return true;
}

static bool takeISSOLEDManualForecastRequest() {
  bool requested = false;

  portENTER_CRITICAL(
    &buttonCommandMux
  );

  if (pendingISSOLEDManualForecast) {
    pendingISSOLEDManualForecast =
        false;

    requested =
        true;
  }

  portEXIT_CRITICAL(
    &buttonCommandMux
  );

  return requested;
}


static void serviceISSOLED() {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured
  ) {
    sleepISSOLED();
    return;
  }

  bool statusValid = false;
  bool above = false;

  portENTER_CRITICAL(
    &issRenderMux
  );

  statusValid =
      issObserverStatusValid;

  above =
      issAboveHorizon;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  const uint32_t nowMs =
      millis();

  // -------------------------------------------------------------------------
  // Explicit button request — highest OLED priority.
  // -------------------------------------------------------------------------
  //
  // Every debounced physical press starts/restarts a full 30-second forecast
  // window.  This is independent of whether that press eventually becomes a
  // 1-click, 2-click, 3-click or long-press cloud-playback gesture.
  if (
    takeISSOLEDManualForecastRequest()
  ) {
    issOLEDManualForecastUntilMs =
        nowMs +
        ISS_OLED_FORECAST_DURATION_MS;

    lastISSOLEDRefreshMs = 0;

    Serial.println(
      "[ISS OLED] Manual forecast 30s"
    );
  }

  const bool manualForecastActive =
      issOLEDManualForecastUntilMs != 0 &&
      (int32_t)(
        issOLEDManualForecastUntilMs -
        nowMs
      ) > 0;

  if (manualForecastActive) {
    // If a clock quarter-hour passes while a manual page is occupying the
    // OLED, consume that slot so we do not get a redundant one-second
    // periodic page immediately after the manual 30-second window expires.
    int64_t currentQuarterSlot = -1;
    uint16_t secondsIntoQuarter = 0;

    if (
      getLocalQuarterHourState(
        currentQuarterSlot,
        secondsIntoQuarter
      )
    ) {
      const bool inQuarterStartWindow =
          secondsIntoQuarter <
          (
            ISS_OLED_FORECAST_DURATION_MS /
            1000UL
          );

      if (inQuarterStartWindow) {
        issOLEDForecastClockInitialised =
            true;

        issOLEDForecastLastQuarterSlot =
            currentQuarterSlot;
      }
    }

    if (
      !issOLEDActive ||
      issOLEDMode !=
        ISS_OLED_MODE_FORECAST ||
      lastISSOLEDRefreshMs == 0 ||
      nowMs -
        lastISSOLEDRefreshMs >=
        ISS_OLED_REFRESH_MS
    ) {
      lastISSOLEDRefreshMs =
          nowMs;

      drawISSForecastOLEDScreen();
    }

    return;
  }

  // A manual window has just expired.  Clear its timer and release the OLED
  // back to normal priority.  If a real pass is still active, its live page
  // will be drawn immediately below.
  if (
    issOLEDManualForecastUntilMs != 0
  ) {
    issOLEDManualForecastUntilMs = 0;

    if (
      issOLEDActive &&
      issOLEDMode ==
        ISS_OLED_MODE_FORECAST
    ) {
      sleepISSOLED();
    }
  }

  // -------------------------------------------------------------------------
  // Live ISS pass — normal highest automatic priority.
  // -------------------------------------------------------------------------
  if (
    statusValid &&
    above
  ) {
    if (
      !issOLEDActive ||
      issOLEDMode !=
        ISS_OLED_MODE_PASS ||
      lastISSOLEDRefreshMs == 0 ||
      nowMs -
        lastISSOLEDRefreshMs >=
        ISS_OLED_REFRESH_MS
    ) {
      lastISSOLEDRefreshMs =
          nowMs;

      drawISSOLEDScreen();
    }

    return;
  }

  // If a pass ends, sleep immediately. Do NOT restart a relative 15-minute
  // countdown; the next automatic forecast remains clock aligned.
  if (
    issOLEDActive &&
    issOLEDMode ==
      ISS_OLED_MODE_PASS
  ) {
    sleepISSOLED();
  }

  // -------------------------------------------------------------------------
  // Existing automatic quarter-hour forecast page.
  // -------------------------------------------------------------------------
  if (
    issOLEDActive &&
    issOLEDMode ==
      ISS_OLED_MODE_FORECAST
  ) {
    if (
      (int32_t)(
        nowMs -
        issOLEDForecastUntilMs
      ) >= 0
    ) {
      sleepISSOLED();
      return;
    }

    if (
      lastISSOLEDRefreshMs == 0 ||
      nowMs -
        lastISSOLEDRefreshMs >=
        ISS_OLED_REFRESH_MS
    ) {
      lastISSOLEDRefreshMs =
          nowMs;

      drawISSForecastOLEDScreen();
    }

    return;
  }

  int64_t quarterSlot = -1;
  uint16_t secondsIntoQuarter = 0;

  if (
    !getLocalQuarterHourState(
      quarterSlot,
      secondsIntoQuarter
    )
  ) {
    return;
  }

  // Only the first 30 seconds of an actual wall-clock quarter-hour are an
  // eligible automatic forecast-start window.
  const bool inQuarterStartWindow =
      secondsIntoQuarter <
      (
        ISS_OLED_FORECAST_DURATION_MS /
        1000UL
      );

  if (
    !issOLEDForecastClockInitialised
  ) {
    issOLEDForecastClockInitialised =
        true;

    if (inQuarterStartWindow) {
      issOLEDForecastLastQuarterSlot =
          quarterSlot -
          1;
    }
    else {
      issOLEDForecastLastQuarterSlot =
          quarterSlot;
    }
  }

  if (
    inQuarterStartWindow &&
    quarterSlot !=
      issOLEDForecastLastQuarterSlot
  ) {
    issOLEDForecastLastQuarterSlot =
        quarterSlot;

    // End at the true +30 second wall-clock point for automatic pages.
    const uint32_t elapsedMs =
        (uint32_t)
        secondsIntoQuarter *
        1000UL;

    const uint32_t remainingMs =
        elapsedMs <
          ISS_OLED_FORECAST_DURATION_MS ?
        ISS_OLED_FORECAST_DURATION_MS -
          elapsedMs :
        1UL;

    issOLEDForecastUntilMs =
        nowMs +
        remainingMs;

    lastISSOLEDRefreshMs = 0;

    drawISSForecastOLEDScreen();
  }
}



static void resetRuntimeConfigDefaults() {
  appConfig.wifiSSID[0] = '\0';
  appConfig.wifiPassword[0] = '\0';

  // Sensible project default; change freely in /config.ini.
  appConfig.utcOffsetMinutes = 9 * 60;
  strlcpy(
    appConfig.timezoneLabel,
    "JST",
    sizeof(appConfig.timezoneLabel)
  );

  appConfig.enableISS = true;
  appConfig.enableClouds = true;
  appConfig.enableDayNight = true;

  // NaN means "not supplied"; unlike 0,0 it cannot accidentally create an
  // observer in the Gulf of Guinea when the config keys are missing.
  appConfig.observerLatDeg = NAN;
  appConfig.observerLonDeg = NAN;
  appConfig.observerAltM = NAN;
  appConfig.observerConfigured = false;

  // Geometric horizon by default. Users with terrain/buildings can raise this.
  appConfig.visibleMinElevationDeg = 0.0;

  // Observer Sun below civil twilight is a useful conservative definition of
  // a dark-enough sky for naked-eye ISS viewing.
  appConfig.visibleSunMaxElevationDeg = -6.0;
}

static bool parseConfigBool(
  String value,
  bool &out
) {
  value.trim();
  value.toLowerCase();

  if (
    value == "1" ||
    value == "true" ||
    value == "yes" ||
    value == "on"
  ) {
    out = true;
    return true;
  }

  if (
    value == "0" ||
    value == "false" ||
    value == "no" ||
    value == "off"
  ) {
    out = false;
    return true;
  }

  return false;
}

static bool parseConfigDouble(
  const String &value,
  double &out
) {
  String clean =
      value;

  clean.trim();

  if (clean.length() == 0)
    return false;

  char *end = nullptr;

  const double parsed =
      strtod(
        clean.c_str(),
        &end
      );

  if (
    end ==
      clean.c_str() ||
    !end ||
    *end != '\0' ||
    !isfinite(parsed)
  ) {
    return false;
  }

  out = parsed;
  return true;
}

static void stripOptionalQuotes(
  String &value
) {
  value.trim();

  if (
    value.length() >= 2 &&
    (
      (
        value[0] == '"' &&
        value[
          value.length() - 1
        ] == '"'
      ) ||
      (
        value[0] == '\'' &&
        value[
          value.length() - 1
        ] == '\''
      )
    )
  ) {
    value =
        value.substring(
          1,
          value.length() - 1
        );
  }
}

static void printTimezoneConfig() {
  const int offset =
      appConfig.utcOffsetMinutes;

  const char sign =
      offset < 0 ?
      '-' :
      '+';

  const int absMinutes =
      abs(offset);

  Serial.printf(
    "[CFG] TZ %s %c%02d:%02d\n",
    appConfig.timezoneLabel,
    sign,
    absMinutes / 60,
    absMinutes % 60
  );
}

static bool loadRuntimeConfig() {
  resetRuntimeConfigDefaults();

  if (
    !sdReady ||
    !SD.exists(
      RUNTIME_CONFIG_FILE
    )
  ) {
    Serial.println(
      "[CFG] /config.ini missing; using defaults"
    );

    printTimezoneConfig();

    Serial.printf(
      "[CFG] ISS=%u CLOUDS=%u DAY_NIGHT=%u\n",
      appConfig.enableISS ? 1u : 0u,
      appConfig.enableClouds ? 1u : 0u,
      appConfig.enableDayNight ? 1u : 0u
    );

    return false;
  }

  File f =
      SD.open(
        RUNTIME_CONFIG_FILE,
        FILE_READ
      );

  if (!f) {
    Serial.println(
      "[CFG] Could not open /config.ini"
    );

    return false;
  }

  uint16_t lineNumber = 0;

  while (f.available()) {
    String line =
        f.readStringUntil('\n');

    ++lineNumber;

    line.replace("\r", "");
    line.trim();

    if (
      line.length() == 0 ||
      line.startsWith("#") ||
      line.startsWith(";")
    ) {
      continue;
    }

    const int equals =
        line.indexOf('=');

    if (equals <= 0) {
      Serial.printf(
        "[CFG] Ignoring line %u\n",
        (unsigned)lineNumber
      );

      continue;
    }

    String key =
        line.substring(
          0,
          equals
        );

    String value =
        line.substring(
          equals + 1
        );

    key.trim();
    key.toUpperCase();

    stripOptionalQuotes(
      value
    );

    if (key == "WIFI_SSID") {
      strlcpy(
        appConfig.wifiSSID,
        value.c_str(),
        sizeof(appConfig.wifiSSID)
      );
    }
    else if (
      key == "WIFI_PASSWORD"
    ) {
      strlcpy(
        appConfig.wifiPassword,
        value.c_str(),
        sizeof(appConfig.wifiPassword)
      );
    }
    else if (
      key == "UTC_OFFSET_MINUTES"
    ) {
      const long minutes =
          value.toInt();

      if (
        minutes >= -720 &&
        minutes <= 840
      ) {
        appConfig.utcOffsetMinutes =
            (int16_t)minutes;
      }
      else {
        Serial.printf(
          "[CFG] Bad UTC offset on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (
      key == "TIMEZONE_LABEL"
    ) {
      if (value.length() > 0) {
        strlcpy(
          appConfig.timezoneLabel,
          value.c_str(),
          sizeof(appConfig.timezoneLabel)
        );
      }
    }
    else if (
      key == "OBSERVER_LAT"
    ) {
      double parsed;

      if (
        parseConfigDouble(
          value,
          parsed
        ) &&
        parsed >= -90.0 &&
        parsed <= 90.0
      ) {
        appConfig.observerLatDeg =
            parsed;
      }
      else {
        Serial.printf(
          "[CFG] Bad OBSERVER_LAT on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (
      key == "OBSERVER_LON"
    ) {
      double parsed;

      if (
        parseConfigDouble(
          value,
          parsed
        ) &&
        parsed >= -180.0 &&
        parsed <= 180.0
      ) {
        appConfig.observerLonDeg =
            parsed;
      }
      else {
        Serial.printf(
          "[CFG] Bad OBSERVER_LON on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (
      key == "OBSERVER_ALT_M"
    ) {
      double parsed;

      if (
        parseConfigDouble(
          value,
          parsed
        ) &&
        parsed >= -500.0 &&
        parsed <= 10000.0
      ) {
        appConfig.observerAltM =
            parsed;
      }
      else {
        Serial.printf(
          "[CFG] Bad OBSERVER_ALT_M on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (
      key == "VISIBLE_MIN_ELEVATION_DEG"
    ) {
      double parsed;

      if (
        parseConfigDouble(
          value,
          parsed
        ) &&
        parsed >= 0.0 &&
        parsed <= 45.0
      ) {
        appConfig.visibleMinElevationDeg =
            parsed;
      }
      else {
        Serial.printf(
          "[CFG] Bad VISIBLE_MIN_ELEVATION_DEG on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (
      key == "VISIBLE_SUN_MAX_ELEVATION_DEG"
    ) {
      double parsed;

      if (
        parseConfigDouble(
          value,
          parsed
        ) &&
        parsed >= -18.0 &&
        parsed <= 0.0
      ) {
        appConfig.visibleSunMaxElevationDeg =
            parsed;
      }
      else {
        Serial.printf(
          "[CFG] Bad VISIBLE_SUN_MAX_ELEVATION_DEG on line %u\n",
          (unsigned)lineNumber
        );
      }
    }
    else if (key == "ISS") {
      bool parsed;

      if (
        parseConfigBool(
          value,
          parsed
        )
      ) {
        appConfig.enableISS =
            parsed;
      }
    }
    else if (key == "CLOUDS") {
      bool parsed;

      if (
        parseConfigBool(
          value,
          parsed
        )
      ) {
        appConfig.enableClouds =
            parsed;
      }
    }
    else if (
      key == "DAY_NIGHT"
    ) {
      bool parsed;

      if (
        parseConfigBool(
          value,
          parsed
        )
      ) {
        appConfig.enableDayNight =
            parsed;
      }
    }
    else {
      Serial.printf(
        "[CFG] Unknown key on line %u\n",
        (unsigned)lineNumber
      );
    }
  }

  f.close();

  appConfig.observerConfigured =
      isfinite(
        appConfig.observerLatDeg
      ) &&
      isfinite(
        appConfig.observerLonDeg
      ) &&
      isfinite(
        appConfig.observerAltM
      );

  Serial.println(
    "[CFG] Loaded /config.ini"
  );

  printTimezoneConfig();

  Serial.printf(
    "[CFG] ISS=%u CLOUDS=%u DAY_NIGHT=%u\n",
    appConfig.enableISS ? 1u : 0u,
    appConfig.enableClouds ? 1u : 0u,
    appConfig.enableDayNight ? 1u : 0u
  );

  Serial.printf(
    "[CFG] WiFi credentials %s\n",
    appConfig.wifiSSID[0] ?
      "loaded" :
      "missing"
  );

  if (appConfig.observerConfigured) {
    Serial.printf(
      "[CFG] Observer configured; visible EL>=%.1f deg, Sun<=%.1f deg\n",
      appConfig.visibleMinElevationDeg,
      appConfig.visibleSunMaxElevationDeg
    );
  }
  else {
    Serial.println(
      "[CFG] Observer not configured"
    );
  }

  return true;
}

// -----------------------------------------------------------------------------
// Wi-Fi + UTC
// -----------------------------------------------------------------------------

// Connect only when a network operation needs Wi-Fi.  Automatic reconnect is
// disabled so a marginal RF link cannot enter a rapid driver reconnect loop;
// callers simply invoke wifiConnect() again when they next need the network.

static bool wifiConnect(
  uint32_t timeoutMs = 30000
) {
  if (WiFi.status() == WL_CONNECTED)
    return true;

  if (appConfig.wifiSSID[0] == '\0') {
    Serial.println(
      "[WIFI] No credentials in /config.ini"
    );

    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  // Keep the radio awake during weak-signal HTTPS transfers.
  WiFi.setSleep(false);

  // Use the full Arduino-ESP32 station transmit-power setting. The finished
  // unit can operate at weak RSSI, so modem sleep remains disabled while Wi-Fi
  // is in use. No regulatory country is hard-coded here; the release therefore
  // remains portable between regions. See README.md for an optional country-code
  // example if a particular installation requires one.
  delay(100);

  const bool txPowerOK =
      WiFi.setTxPower(
        WIFI_POWER_19_5dBm
      );

  if (!txPowerOK) {
    Serial.println(
      "[WIFI] Warning: could not set TX power"
    );
  }

  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    Serial.printf(
      "[WIFI] Connecting attempt %u/2\n",
      (unsigned)attempt
    );

    WiFi.begin(
      appConfig.wifiSSID,
      appConfig.wifiPassword
    );

    const uint32_t start =
        millis();

    while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < timeoutMs
    ) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf(
        "[WIFI] Connected RSSI=%d dBm channel=%d\n",
        WiFi.RSSI(),
        WiFi.channel()
      );

      return true;
    }

    WiFi.disconnect(false, false);
    delay(750);
  }

  Serial.printf(
    "[WIFI] Connection failed status=%d\n",
    (int)WiFi.status()
  );

  return false;
}

static void startNTPClientIfNeeded() {
  if (ntpClientStarted)
    return;

  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  ntpClientStarted = true;

  Serial.println(
    "[TIME] SNTP client started"
  );
}

static void printCurrentUTC(
  const char *prefix
) {
  const time_t now =
      time(nullptr);

  if (now < 1700000000)
    return;

  struct tm t = {};
  gmtime_r(&now, &t);

  Serial.printf(
    "%s %04d-%02d-%02d %02d:%02d:%02d\n",
    prefix,
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  );
}

static bool syncUTC(
  uint32_t timeoutMs = 15000
) {
  startNTPClientIfNeeded();

  const uint32_t start =
      millis();

  while (
    time(nullptr) < 1700000000 &&
    millis() - start < timeoutMs
  ) {
    delay(250);
  }

  if (
    time(nullptr) <
    1700000000
  ) {
    Serial.printf(
      "[TIME] NTP still pending after %lu ms; SNTP continues in background\n",
      (unsigned long)timeoutMs
    );

    return false;
  }

  ntpTimeConfirmed = true;

  printCurrentUTC(
    "[TIME] UTC"
  );

  return true;
}

static void serviceLateNTPSyncLog() {
  if (
    !ntpClientStarted ||
    ntpTimeConfirmed ||
    time(nullptr) <
      1700000000
  ) {
    return;
  }

  ntpTimeConfirmed = true;

  printCurrentUTC(
    "[TIME] Late SNTP sync UTC"
  );
}

static void makeNSMCTime(
  int hoursAgo,
  char out[13]
) {
  time_t now =
      time(nullptr);

  // Fallback only: the official availability API is the primary source.
  // Use the current UTC hour immediately; there is no deliberate source lag.
  // Round down to a whole UTC hour.
  now -=
      now %
      (60 * 60);

  now -=
      (time_t)hoursAgo *
      60 *
      60;

  struct tm t = {};
  gmtime_r(&now, &t);

  snprintf(
    out,
    13,
    "%04d%02d%02d%02d00",
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour
  );
}

// -----------------------------------------------------------------------------
// NSMC WMS
// -----------------------------------------------------------------------------

static const char *NSMC_WMS =
    "https://data.nsmc.org.cn/NSMCAPI/v1/nsmc/image/wms/compose";

static String makeNSMCURL(
  const char *datetimeUTC
) {
  String url = NSMC_WMS;

  // This follows NSMC's documented GetMap parameter form.
  url +=
      "?layers=GEOS_IRX"
      "&datetime=";

  url += datetimeUTC;

  url +=
      "&request=GetMap"
      "&bbox=-180,-90,180,90"
      "&width=320"
      "&height=160"
      "&version=1.1.0"
      "&format=png";

  return url;
}

static bool downloadNSMCPNG(
  const char *datetimeUTC
) {
  downloadSize = 0;
  lastNSMCTransportCode = 0;

  const String url =
      makeNSMCURL(
        datetimeUTC
      );

  WiFiClientSecure client;
  client.setInsecure();

  // WiFiClientSecure's TLS handshake has its own timeout.  The library
  // default is 120 s, far too long for an appliance display on weak Wi-Fi.
  client.setHandshakeTimeout(12);   // seconds
  client.setTimeout(20000);         // socket I/O, milliseconds

  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println(
      "[HTTP] begin failed"
    );

    return false;
  }

  // Identity body + connection close is simpler and has proved more reliable
  // on this weak Wi-Fi connection than chunked HTTP/1.1.
  http.useHTTP10(true);
  http.setConnectTimeout(12000);
  http.setTimeout(20000);

  Serial.printf(
    "[NSMC] GET %s GEOS_IRX\n",
    datetimeUTC
  );

  const int code =
      http.GET();

  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      lastNSMCTransportCode =
          code;

      const String errorText =
          http.errorToString(
            code
          );

      Serial.printf(
        "[HTTP] transport error=%d (%s)\n",
        code,
        errorText.c_str()
      );
    }
    else {
      Serial.printf(
        "[HTTP] status=%d\n",
        code
      );
    }

    http.end();
    return false;
  }

  // Read the response body cooperatively instead of calling
  // HTTPClient::writeToStream().  On the single-core ESP32-S2 the latter can
  // monopolise the Arduino task long enough to make the globe appear frozen.
  auto *stream =
      http.getStreamPtr();

  if (!stream) {
    Serial.println(
      "[HTTP] no response stream"
    );

    http.end();
    return false;
  }

  const int expected =
      http.getSize();

  httpBodyExpected =
      expected;

  httpBodyBytes = 0;
  downloadSize = 0;

  Serial.printf(
    "[HTTP] body expected=%d bytes\n",
    expected
  );

  const uint32_t bodyStartMs =
      millis();

  uint32_t lastDataMs =
      bodyStartMs;

  uint32_t lastProgressLogMs =
      bodyStartMs;

  constexpr uint32_t BODY_IDLE_TIMEOUT_MS =
      12000;

  constexpr uint32_t BODY_TOTAL_TIMEOUT_MS =
      45000;

  constexpr size_t BODY_CHUNK_MAX =
      2048;

  bool bodyOK = true;

  while (true) {
    const int availableBytes =
        stream->available();

    if (availableBytes > 0) {
      const size_t room =
          DOWNLOAD_BUFFER_CAPACITY -
          downloadSize;

      if (room == 0) {
        Serial.println(
          "[HTTP] download buffer overflow"
        );

        bodyOK = false;
        break;
      }

      size_t want =
          (size_t)availableBytes;

      if (
        want >
        BODY_CHUNK_MAX
      ) {
        want =
            BODY_CHUNK_MAX;
      }

      if (want > room)
        want = room;

      const int got =
          stream->read(
            downloadBuffer +
            downloadSize,
            want
          );

      if (got > 0) {
        downloadSize +=
            (size_t)got;

        httpBodyBytes =
            (uint32_t)
            downloadSize;

        lastDataMs =
            millis();

        if (
          millis() -
          lastProgressLogMs >=
            1000
        ) {
          Serial.printf(
            "[HTTP] body %u/%d bytes\n",
            (unsigned)downloadSize,
            expected
          );

          lastProgressLogMs =
              millis();
        }
      }
    }
    else {
      // If Content-Length is known, completion is unambiguous.
      if (
        expected >= 0 &&
        downloadSize >=
          (size_t)expected
      ) {
        break;
      }

      // If the server closed and there is no more buffered data, we're done.
      if (
        !stream->connected()
      ) {
        break;
      }

      if (
        millis() -
        lastDataMs >
          BODY_IDLE_TIMEOUT_MS
      ) {
        Serial.printf(
          "[HTTP] body idle timeout after %u bytes\n",
          (unsigned)downloadSize
        );

        bodyOK = false;
        break;
      }
    }

    if (
      millis() -
      bodyStartMs >
        BODY_TOTAL_TIMEOUT_MS
    ) {
      Serial.printf(
        "[HTTP] body total timeout after %u bytes\n",
        (unsigned)downloadSize
      );

      bodyOK = false;
      break;
    }

    // Critical on ESP32-S2: let the normal Arduino loop redraw the globe.
    vTaskDelay(
      pdMS_TO_TICKS(2)
    );
  }

  http.end();

  httpBodyBytes =
      (uint32_t)downloadSize;

  if (!bodyOK) {
    httpBodyExpected = -1;
    return false;
  }

  if (
    expected >= 0 &&
    downloadSize !=
      (size_t)expected
  ) {
    Serial.printf(
      "[HTTP] short body %u/%d bytes\n",
      (unsigned)downloadSize,
      expected
    );

    httpBodyExpected = -1;
    return false;
  }

  if (
    !hasPNGSignature(
      downloadBuffer,
      downloadSize
    ) ||
    !hasPNGIEND(
      downloadBuffer,
      downloadSize
    )
  ) {
    Serial.printf(
      "[HTTP] response not complete PNG bytes=%u\n",
      (unsigned)downloadSize
    );

    httpBodyExpected = -1;
    return false;
  }

  Serial.printf(
    "[NSMC] PNG received %u bytes\n",
    (unsigned)downloadSize
  );

  httpBodyExpected = -1;

  return true;
}

// -----------------------------------------------------------------------------
// PNG decode -> observed IR image luminance
// -----------------------------------------------------------------------------

static bool getPNGPixelRGBA(
  PNGDRAW *pDraw,
  int x,
  uint8_t &r,
  uint8_t &g,
  uint8_t &b,
  uint8_t &a
) {
  if (pDraw->iBpp != 8)
    return false;

  switch (pDraw->iPixelType) {
    case PNG_PIXEL_INDEXED: {
      const uint8_t index =
          pDraw->pPixels[x];

      const uint8_t *p =
          &pDraw->pPalette[
            index * 3
          ];

      r = p[0];
      g = p[1];
      b = p[2];

      a =
          pDraw->iHasAlpha ?
          pDraw->pPalette[
            768 + index
          ] :
          255;

      return true;
    }

    case PNG_PIXEL_TRUECOLOR: {
      const uint8_t *p =
          &pDraw->pPixels[
            x * 3
          ];

      r = p[0];
      g = p[1];
      b = p[2];
      a = 255;

      return true;
    }

    case PNG_PIXEL_TRUECOLOR_ALPHA: {
      const uint8_t *p =
          &pDraw->pPixels[
            x * 4
          ];

      r = p[0];
      g = p[1];
      b = p[2];
      a = p[3];

      return true;
    }

    default:
      return false;
  }
}

static int irPNGDraw(
  PNGDRAW *pDraw
) {
  if (
    !decodeTarget ||
    pDraw->y < 0 ||
    pDraw->y >= IR_H ||
    pDraw->iWidth != IR_W
  ) {
    decodeError = true;
    return 0;
  }

  const uint32_t rowBase =
      (uint32_t)pDraw->y *
      IR_W;


  for (int x = 0; x < pDraw->iWidth; ++x) {
    uint8_t r, g, b, a;

    if (
      !getPNGPixelRGBA(
        pDraw,
        x,
        r, g, b, a
      )
    ) {
      decodeError = true;
      return 0;
    }

    if (a == 0) {
      decodeTarget[
        rowBase +
        (uint32_t)x
      ] = 0;

      decodeTargetAlpha[
        rowBase +
        (uint32_t)x
      ] = 0;

      continue;
    }

    decodeTargetAlpha[
      rowBase +
      (uint32_t)x
    ] = a;


    // Perceptual luminance, integer approximation:
    //   0.2126 R + 0.7152 G + 0.0722 B
    const uint8_t luma =
        (uint8_t)(
          (
            54u  * (uint16_t)r +
            183u * (uint16_t)g +
            19u  * (uint16_t)b
          ) >> 8
        );


    decodeTarget[
      rowBase +
      (uint32_t)x
    ] = luma;
  }

  return 1;
}

static bool decodeNSMCIR() {
  // Decode the pristine NSMC PNG directly. No IDAT rewriting is needed once
  // PNGdec's 320px RGBA scanline-buffer overflow is corrected.
  uint8_t *decodeData = downloadBuffer;
  const size_t decodeBytes = downloadSize;

  if (!ensurePNGDecoder()) {
    return false;
  }

  memset(
    candidateLuma,
    0,
    IR_PIXELS
  );

  memset(
    candidateAlpha,
    0,
    IR_PIXELS
  );

  decodeTarget =
      candidateLuma;

  decodeTargetAlpha =
      candidateAlpha;

  decodeError = false;

  const int rcOpen =
      pngDecoder->openRAM(
        decodeData,
        (int)decodeBytes,
        irPNGDraw
      );

  if (rcOpen != PNG_SUCCESS) {
    Serial.printf(
      "[PNG] openRAM=%d\n",
      rcOpen
    );

    decodeTarget = nullptr;
    decodeTargetAlpha = nullptr;
    releasePNGDecoder();
    return false;
  }

  if (
    pngDecoder->getWidth() != IR_W ||
    pngDecoder->getHeight() != IR_H
  ) {
    pngDecoder->close();
    decodeTarget = nullptr;
    decodeTargetAlpha = nullptr;
    releasePNGDecoder();
    return false;
  }

  const int rc =
      pngDecoder->decode(
        nullptr,
        0
      );

  const int lastError =
      pngDecoder->getLastError();

  pngDecoder->close();
  decodeTarget = nullptr;
  decodeTargetAlpha = nullptr;
  releasePNGDecoder();

  if (
    rc != PNG_SUCCESS ||
    decodeError
  ) {
    Serial.printf(
      "[PNG] decode rc=%d lastError=%d callback=%d\n",
      rc,
      lastError,
      decodeError ? 1 : 0
    );

    return false;
  }


  return true;
}

// -----------------------------------------------------------------------------
// IR image validation + adaptive display mapping
// -----------------------------------------------------------------------------

static uint8_t percentileFromHistogram(
  const uint32_t hist[256],
  uint32_t total,
  float percentile
) {
  if (total == 0)
    return 0;

  const uint32_t target =
      (uint32_t)(
        percentile *
        (float)(total - 1)
      );

  uint32_t cumulative = 0;

  for (uint16_t v = 0; v < 256; ++v) {
    cumulative += hist[v];

    if (cumulative > target)
      return (uint8_t)v;
  }

  return 255;
}

static bool analyseIRAndBuildOpacity() {
  uint32_t histLuma[256] = {};
  uint32_t histAlpha[256] = {};

  uint32_t opaque = 0;
  uint64_t sumLuma = 0;
  uint64_t sum2Luma = 0;
  uint64_t sumAlpha = 0;

  for (
    uint32_t i = 0;
    i < IR_PIXELS;
    ++i
  ) {
    const uint8_t a =
        candidateAlpha[i];

    if (a == 0)
      continue;

    const uint8_t v =
        candidateLuma[i];

    ++opaque;
    ++histLuma[v];
    ++histAlpha[a];

    sumLuma += v;
    sum2Luma +=
        (uint32_t)v *
        (uint32_t)v;

    sumAlpha += a;
  }

  if (opaque < IR_PIXELS / 12u) {
    Serial.println(
      "[IR] Rejected: too little opaque image content"
    );

    return false;
  }

  const float meanLuma =
      (float)sumLuma /
      (float)opaque;

  const float varianceLuma =
      fmaxf(
        0.0f,
        (float)sum2Luma /
        (float)opaque -
        meanLuma * meanLuma
      );

  const float stddevLuma =
      sqrtf(
        varianceLuma
      );

  const uint8_t p10L =
      percentileFromHistogram(
        histLuma,
        opaque,
        0.10f
      );

  const uint8_t p95L =
      percentileFromHistogram(
        histLuma,
        opaque,
        0.95f
      );

  const uint8_t p10A =
      percentileFromHistogram(
        histAlpha,
        opaque,
        0.10f
      );

  const uint8_t p95A =
      percentileFromHistogram(
        histAlpha,
        opaque,
        0.95f
      );

  const float meanAlpha =
      (float)sumAlpha /
      (float)opaque;


  if (
    stddevLuma < 2.0f &&
    p95A <= p10A + 4
  ) {
    Serial.println(
      "[IR] Image rejected as blank/flat"
    );

    return false;
  }

  // If the luminance is effectively binary, let source alpha carry the depth.
  candidateUseAlphaPrimaryOpacity =
      (
        stddevLuma < 12.0f ||
        p95L <= p10L + 18
      );

  // How strongly the source alpha should be allowed to show through.
  {
    const float normalized =
        (meanAlpha - (float)p10A) /
        fmaxf(
          1.0f,
          (float)p95A - (float)p10A
        );

    const float clamped =
        fminf(
          1.0f,
          fmaxf(
            0.0f,
            normalized
          )
        );

    candidateGlobalCloudAlphaScale =
        (uint8_t)lroundf(
          96.0f + 128.0f * clamped
        );
  }

  const float low =
      (float)p10L;

  const float high =
      (float)max(
        (int)p95L,
        (int)p10L + 10
      );

  for (uint16_t v = 0; v < 256; ++v) {
    if (candidateUseAlphaPrimaryOpacity) {
      // In alpha-primary mode, luma only modulates slightly around the alpha.
      float t =
          (
            (float)v - low
          ) /
          (
            high - low
          );

      if (t < 0.0f)
        t = 0.0f;
      if (t > 1.0f)
        t = 1.0f;

      candidateCloudOpacityLUT[v] =
          (uint8_t)lroundf(
            150.0f + 105.0f * powf(t, 1.40f)
          );
    }
    else {
      float t =
          (
            (float)v - low
          ) /
          (
            high - low
          );

      if (t < 0.0f)
        t = 0.0f;
      if (t > 1.0f)
        t = 1.0f;

      t = powf(t, 1.55f);

      candidateCloudOpacityLUT[v] =
          (uint8_t)lroundf(
            24.0f + 231.0f * t
          );
    }
  }

  Serial.printf(
    "[IR] Cloud mapping: %s\n",
    candidateUseAlphaPrimaryOpacity ?
      "source alpha" :
      "alpha + luminance"
  );

  return true;
}

// -----------------------------------------------------------------------------
// UTC / archive helpers
// -----------------------------------------------------------------------------

static void formatUTCStamp(
  time_t utc,
  char out[13]
) {
  struct tm t = {};
  gmtime_r(&utc, &t);

  snprintf(
    out,
    13,
    "%04d%02d%02d%02d%02d",
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min
  );
}

static void archivePathForStamp(
  const char *stamp,
  char *out,
  size_t outBytes
) {
  snprintf(
    out,
    outBytes,
    "/clouds/%.4s/%.2s/%.2s/%.4s.png",
    stamp,
    stamp + 4,
    stamp + 6,
    stamp + 8
  );
}

static void ensureArchiveDirs(
  const char *stamp
) {
  if (!sdReady)
    return;

  char p[32];

  if (!SD.exists("/clouds"))
    SD.mkdir("/clouds");

  snprintf(
    p,
    sizeof(p),
    "/clouds/%.4s",
    stamp
  );

  if (!SD.exists(p))
    SD.mkdir(p);

  snprintf(
    p,
    sizeof(p),
    "/clouds/%.4s/%.2s",
    stamp,
    stamp + 4
  );

  if (!SD.exists(p))
    SD.mkdir(p);

  snprintf(
    p,
    sizeof(p),
    "/clouds/%.4s/%.2s/%.2s",
    stamp,
    stamp + 4,
    stamp + 6
  );

  if (!SD.exists(p))
    SD.mkdir(p);
}

static bool archiveExists(
  const char *stamp
) {
  if (!sdReady)
    return false;

  char path[64];

  archivePathForStamp(
    stamp,
    path,
    sizeof(path)
  );

  return SD.exists(path);
}

static void appendArchiveIndex(
  const char *stamp,
  size_t bytes
) {
  if (!sdReady)
    return;

  const bool newIndex =
      !SD.exists(
        ARCHIVE_INDEX
      );

  File f =
      SD.open(
        ARCHIVE_INDEX,
        FILE_APPEND
      );

  if (!f)
    return;

  if (newIndex) {
    f.println(
      "utc_yyyymmddhhmm,png_bytes"
    );
  }

  f.printf(
    "%s,%u\n",
    stamp,
    (unsigned)bytes
  );

  f.close();
}

static bool archiveDownloadedPNG(
  const char *stamp
) {
  if (
    !sdReady ||
    !downloadBuffer ||
    downloadSize == 0
  ) {
    return false;
  }

  char path[64];

  archivePathForStamp(
    stamp,
    path,
    sizeof(path)
  );

  if (SD.exists(path))
    return true;

  ensureArchiveDirs(stamp);

  File f =
      SD.open(
        path,
        FILE_WRITE
      );

  if (!f) {
    Serial.printf(
      "[SD] Could not create %s\n",
      path
    );

    return false;
  }

  const size_t written =
      f.write(
        downloadBuffer,
        downloadSize
      );

  f.flush();
  f.close();

  if (written != downloadSize) {
    Serial.printf(
      "[SD] Short write %s %u/%u\n",
      path,
      (unsigned)written,
      (unsigned)downloadSize
    );

    SD.remove(path);
    return false;
  }

  appendArchiveIndex(
    stamp,
    written
  );

  Serial.printf(
    "[SD] Archived %s (%u bytes)\n",
    stamp,
    (unsigned)written
  );

  return true;
}

static bool loadArchivedPNG(
  const char *stamp
) {
  if (!sdReady)
    return false;

  char path[64];

  archivePathForStamp(
    stamp,
    path,
    sizeof(path)
  );

  File f =
      SD.open(
        path,
        FILE_READ
      );

  if (!f)
    return false;

  const size_t bytes =
      (size_t)f.size();

  if (
    bytes == 0 ||
    bytes >
      DOWNLOAD_BUFFER_CAPACITY
  ) {
    f.close();
    return false;
  }

  const size_t got =
      f.read(
        downloadBuffer,
        bytes
      );

  f.close();

  if (got != bytes)
    return false;

  downloadSize = bytes;

  return
      hasPNGSignature(
        downloadBuffer,
        downloadSize
      ) &&
      hasPNGIEND(
        downloadBuffer,
        downloadSize
      );
}

// -----------------------------------------------------------------------------
// Candidate commit
// -----------------------------------------------------------------------------

static void commitCandidate(
  const char *stamp
) {
  if (cloudMutex)
    xSemaphoreTake(
      cloudMutex,
      portMAX_DELAY
    );

  uint8_t *tmp;

  tmp = irLuma;
  irLuma = candidateLuma;
  candidateLuma = tmp;

  tmp = irAlpha;
  irAlpha = candidateAlpha;
  candidateAlpha = tmp;

  memcpy(
    cloudOpacityLUT,
    candidateCloudOpacityLUT,
    256
  );

  globalCloudAlphaScale =
      candidateGlobalCloudAlphaScale;

  useAlphaPrimaryOpacity =
      candidateUseAlphaPrimaryOpacity;

  strncpy(
    currentIRTime,
    stamp,
    12
  );

  currentIRTime[12] = '\0';

  haveIRClouds = true;

  if (
    appConfig.enableClouds &&
    cloudMutex
  ) {
    xSemaphoreGive(
      cloudMutex
    );
  }
}

static bool processDownloadedFrame(
  const char *stamp,
  bool activate,
  bool archiveIt
) {
  if (!decodeNSMCIR())
    return false;

  if (!analyseIRAndBuildOpacity())
    return false;

  if (archiveIt)
    archiveDownloadedPNG(stamp);

  if (activate)
    commitCandidate(stamp);

  return true;
}

static bool fetchExactFrame(
  const char *stamp,
  bool activate,
  bool archiveIt
) {
  if (!downloadNSMCPNG(stamp))
    return false;

  return
      processDownloadedFrame(
        stamp,
        activate,
        archiveIt
      );
}

static bool loadArchivedFrame(
  const char *stamp,
  bool activate
) {
  if (!loadArchivedPNG(stamp))
    return false;

  return
      processDownloadedFrame(
        stamp,
        activate,
        false
      );
}

static bool activateNewestArchivedHourlyFrame() {
  if (
    !appConfig.enableClouds ||
    !sdReady
  ) {
    return false;
  }

  if (
    time(nullptr) <
    1700000000
  ) {
    return false;
  }

  // Search newest -> oldest through the same seven-day weather window used by
  // the archive repair system.  This is an SD-only fallback: no network
  // request is made here.
  constexpr int ARCHIVE_BOOTSTRAP_HOURS =
      7 * 24;

  for (
    int hoursAgo = 0;
    hoursAgo <
      ARCHIVE_BOOTSTRAP_HOURS;
    ++hoursAgo
  ) {
    char stamp[13];

    makeNSMCTime(
      hoursAgo,
      stamp
    );

    if (
      !archiveExists(
        stamp
      )
    ) {
      continue;
    }

    Serial.printf(
      "[LIVE] SD bootstrap trying %s UTC\n",
      stamp
    );

    if (
      loadArchivedFrame(
        stamp,
        true
      )
    ) {
      Serial.printf(
        "[LIVE] SD bootstrap active %s UTC\n",
        stamp
      );

      return true;
    }

    Serial.printf(
      "[LIVE] SD bootstrap rejected %s UTC; trying older frame\n",
      stamp
    );
  }

  Serial.println(
    "[LIVE] SD bootstrap found no usable archived frame"
  );

  return false;
}

// -----------------------------------------------------------------------------
// Official NSMC GEOS_IRX availability list
// -----------------------------------------------------------------------------

static const char *NSMC_AVAILABILITY_API =
    "https://data.nsmc.org.cn/nsmcapi/v1/nsmc/image/animation/datatime/mongodb";

static String makeNSMCAvailabilityURL() {
  String url =
      NSMC_AVAILABILITY_API;

  url +=
      "?dataCode="
      "GEO_MULT_GBAL_L2_GGM_IRX_GLL_YYYYMMDD_HHmm_4000M.PNG"
      "&hourRange=";

  url +=
      String(
        NSMC_AVAILABILITY_HOURS
      );

  return url;
}

static bool downloadNSMCAvailabilityJSON() {
  if (!wifiConnect())
    return false;

  downloadSize = 0;

  const String url =
      makeNSMCAvailabilityURL();

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12);
  client.setTimeout(20000);

  HTTPClient http;

  if (!http.begin(client, url)) {
    Serial.println(
      "[AVAIL] HTTP begin failed"
    );

    return false;
  }

  http.useHTTP10(true);
  http.setConnectTimeout(12000);
  http.setTimeout(20000);

  Serial.printf(
    "[AVAIL] GET official GEOS_IRX list, last %u h\n",
    (unsigned)NSMC_AVAILABILITY_HOURS
  );

  const int code =
      http.GET();

  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      lastNSMCTransportCode =
          code;

      const String errorText =
          http.errorToString(
            code
          );

      Serial.printf(
        "[AVAIL] transport error=%d (%s)\n",
        code,
        errorText.c_str()
      );

    }
    else {
      Serial.printf(
        "[AVAIL] HTTP status=%d\n",
        code
      );
    }

    http.end();
    return false;
  }

  auto *stream =
      http.getStreamPtr();

  if (!stream) {
    http.end();
    Serial.println(
      "[AVAIL] No response stream"
    );
    return false;
  }

  const int expected =
      http.getSize();

  if (
    expected >= 0 &&
    (size_t)expected >=
      DOWNLOAD_BUFFER_CAPACITY
  ) {
    http.end();
    Serial.println(
      "[AVAIL] JSON too large"
    );
    return false;
  }

  httpBodyExpected =
      expected;

  httpBodyBytes = 0;
  downloadSize = 0;

  const uint32_t bodyStartMs =
      millis();

  uint32_t lastDataMs =
      bodyStartMs;

  constexpr uint32_t BODY_IDLE_TIMEOUT_MS =
      10000;

  constexpr uint32_t BODY_TOTAL_TIMEOUT_MS =
      25000;

  constexpr size_t BODY_CHUNK_MAX =
      1024;

  bool bodyOK = true;

  while (true) {
    const int availableBytes =
        stream->available();

    if (availableBytes > 0) {
      size_t room =
          DOWNLOAD_BUFFER_CAPACITY -
          downloadSize -
          1;

      if (room == 0) {
        bodyOK = false;
        Serial.println(
          "[AVAIL] JSON buffer overflow"
        );
        break;
      }

      size_t want =
          (size_t)availableBytes;

      if (want > BODY_CHUNK_MAX)
        want = BODY_CHUNK_MAX;

      if (want > room)
        want = room;

      const int got =
          stream->read(
            downloadBuffer +
            downloadSize,
            want
          );

      if (got > 0) {
        downloadSize +=
            (size_t)got;

        httpBodyBytes =
            (uint32_t)downloadSize;

        lastDataMs =
            millis();
      }
    }
    else {
      if (
        expected >= 0 &&
        downloadSize >=
          (size_t)expected
      ) {
        break;
      }

      if (!stream->connected())
        break;

      if (
        millis() -
        lastDataMs >
          BODY_IDLE_TIMEOUT_MS
      ) {
        bodyOK = false;

        Serial.printf(
          "[AVAIL] Body idle timeout at %u bytes\n",
          (unsigned)downloadSize
        );

        break;
      }
    }

    if (
      millis() -
      bodyStartMs >
        BODY_TOTAL_TIMEOUT_MS
    ) {
      bodyOK = false;

      Serial.printf(
        "[AVAIL] Body total timeout at %u bytes\n",
        (unsigned)downloadSize
      );

      break;
    }

    // Keep the single-core S2 renderer responsive.
    vTaskDelay(
      pdMS_TO_TICKS(2)
    );
  }

  http.end();

  httpBodyExpected = -1;
  httpBodyBytes =
      (uint32_t)downloadSize;

  if (!bodyOK)
    return false;

  if (
    expected >= 0 &&
    downloadSize !=
      (size_t)expected
  ) {
    Serial.printf(
      "[AVAIL] Short JSON body %u/%d\n",
      (unsigned)downloadSize,
      expected
    );

    return false;
  }

  downloadBuffer[downloadSize] =
      '\0';

  return true;
}

static bool allDigits(
  const char *s,
  size_t count
) {
  if (!s)
    return false;

  for (
    size_t i = 0;
    i < count;
    ++i
  ) {
    if (
      s[i] < '0' ||
      s[i] > '9'
    ) {
      return false;
    }
  }

  return true;
}

static bool parseNSMCAvailabilityJSON() {
  nsmcAvailableCount = 0;

  if (
    !downloadBuffer ||
    downloadSize == 0
  ) {
    return false;
  }

  const char *json =
      (const char *)downloadBuffer;

  if (
    strstr(
      json,
      "\"returnCode\":0"
    ) == nullptr
  ) {
    Serial.println(
      "[AVAIL] API did not return success"
    );

    return false;
  }

  const char *p =
      json;

  while (
    nsmcAvailableCount <
      NSMC_MAX_AVAILABLE_TIMES
  ) {
    const char *dateKey =
        strstr(
          p,
          "\"dataDate\":\""
        );

    if (!dateKey)
      break;

    const char *date =
        dateKey +
        strlen(
          "\"dataDate\":\""
        );

    const char *timeKey =
        strstr(
          date + 8,
          "\"dataTime\":\""
        );

    if (!timeKey)
      break;

    const char *timeValue =
        timeKey +
        strlen(
          "\"dataTime\":\""
        );

    if (
      allDigits(date, 8) &&
      allDigits(timeValue, 4)
    ) {
      char stamp[13];

      memcpy(
        stamp,
        date,
        8
      );

      memcpy(
        stamp + 8,
        timeValue,
        4
      );

      stamp[12] =
          '\0';

      bool duplicate = false;

      for (
        uint16_t i = 0;
        i <
          nsmcAvailableCount;
        ++i
      ) {
        if (
          strcmp(
            nsmcAvailableTimes[i],
            stamp
          ) == 0
        ) {
          duplicate = true;
          break;
        }
      }

      if (!duplicate) {
        strncpy(
          nsmcAvailableTimes[
            nsmcAvailableCount
          ],
          stamp,
          13
        );

        ++nsmcAvailableCount;
      }
    }

    p =
        timeValue + 4;
  }

  // Sort oldest -> newest.  The service normally already does this, but the
  // device should not depend on that undocumented ordering detail.
  for (
    uint16_t i = 0;
    i <
      nsmcAvailableCount;
    ++i
  ) {
    for (
      uint16_t j = i + 1;
      j <
        nsmcAvailableCount;
      ++j
    ) {
      if (
        strcmp(
          nsmcAvailableTimes[i],
          nsmcAvailableTimes[j]
        ) > 0
      ) {
        char tmp[13];

        memcpy(
          tmp,
          nsmcAvailableTimes[i],
          13
        );

        memcpy(
          nsmcAvailableTimes[i],
          nsmcAvailableTimes[j],
          13
        );

        memcpy(
          nsmcAvailableTimes[j],
          tmp,
          13
        );
      }
    }
  }

  if (
    nsmcAvailableCount == 0
  ) {
    Serial.println(
      "[AVAIL] No GEOS_IRX timestamps parsed"
    );

    return false;
  }

  Serial.printf(
    "[AVAIL] %u valid timestamps: %s .. %s UTC\n",
    (unsigned)nsmcAvailableCount,
    nsmcAvailableTimes[0],
    nsmcAvailableTimes[
      nsmcAvailableCount - 1
    ]
  );

  return true;
}

static bool fetchNSMCAvailability() {
  // No UTC gate is required here: the official availability list itself is
  // authoritative. This allows cloud updates to proceed even while SNTP is
  // still synchronizing.
  if (!downloadNSMCAvailabilityJSON())
    return false;

  return
      parseNSMCAvailabilityJSON();
}

static int newestAvailabilityIndex() {
  if (nsmcAvailableCount == 0)
    return -1;

  return
      (int)nsmcAvailableCount - 1;
}

static bool removeArchivedStamp(
  const char *stamp
) {
  if (!sdReady)
    return false;

  char path[64];

  archivePathForStamp(
    stamp,
    path,
    sizeof(path)
  );

  if (!SD.exists(path))
    return true;

  Serial.printf(
    "[SD] Removing unreadable archive %s\n",
    stamp
  );

  return
      SD.remove(path);
}

static bool activateNewestAvailableFrame() {
  const int newest =
      newestAvailabilityIndex();

  if (newest < 0) {
    Serial.println(
      "[LIVE] No NSMC timestamp listed"
    );

    return false;
  }

  const char *target =
      nsmcAvailableTimes[
        newest
      ];

  if (
    haveIRClouds &&
    strcmp(
      currentIRTime,
      target
    ) == 0
  ) {
    Serial.printf(
      "[LIVE] Already displaying %s UTC\n",
      target
    );

    return true;
  }

  // Best case: the current image is already on SD, so startup needs no WMS
  // image download at all.
  if (
    sdReady &&
    archiveExists(target)
  ) {
    Serial.printf(
      "[LIVE] Loading %s UTC from SD\n",
      target
    );

    if (
      loadArchivedFrame(
        target,
        true
      )
    ) {
      return true;
    }

    // A corrupt/incomplete local file must not permanently block repair.
    removeArchivedStamp(
      target
    );
  }

  if (!wifiConnect())
    return false;

  Serial.printf(
    "[LIVE] Downloading newest listed frame %s UTC\n",
    target
  );

  if (
    fetchExactFrame(
      target,
      true,
      true
    )
  ) {
    Serial.printf(
      "[LIVE] Accepted %s UTC\n",
      target
    );

    return true;
  }

  Serial.printf(
    "[LIVE] %s failed; keeping previous live frame\n",
    target
  );

  // If this is first boot and there is still no cloud frame, work backwards
  // through already archived official timestamps before giving up.
  if (
    !haveIRClouds &&
    sdReady
  ) {
    for (
      int i = newest - 1;
      i >= 0;
      --i
    ) {
      if (
        archiveExists(
          nsmcAvailableTimes[i]
        )
      ) {
        Serial.printf(
          "[LIVE] Falling back to archived %s UTC\n",
          nsmcAvailableTimes[i]
        );

        if (
          loadArchivedFrame(
            nsmcAvailableTimes[i],
            true
          )
        ) {
          return true;
        }
      }
    }
  }

  return false;
}

static uint16_t countMissingAvailableFrames() {
  if (
    !sdReady ||
    nsmcAvailableCount == 0
  ) {
    return 0;
  }

  uint16_t missing = 0;

  for (
    uint16_t i = 0;
    i <
      nsmcAvailableCount;
    ++i
  ) {
    if (
      !archiveExists(
        nsmcAvailableTimes[i]
      )
    ) {
      ++missing;
    }
  }

  return missing;
}

static void finishArchiveGapRepairPass() {
  const uint16_t remaining =
      countMissingAvailableFrames();

  Serial.printf(
    "[GAP] Pass done: saved=%u failed=%u remaining=%u\n",
    (unsigned)gapRepairSaved,
    (unsigned)gapRepairFailed,
    (unsigned)remaining
  );

  gapRepairPassPending = false;
  gapRepairCursor = -1;
  backfillActive = false;
}

static void startArchiveGapRepairPass() {
  if (
    !sdReady ||
    nsmcAvailableCount == 0
  ) {
    gapRepairPassPending = false;
    gapRepairCursor = -1;
    backfillActive = false;
    backfillDone = 0;
    backfillTotal = 0;
    return;
  }

  const uint16_t missing =
      countMissingAvailableFrames();

  backfillDone = 0;
  backfillTotal = missing;
  gapRepairSaved = 0;
  gapRepairFailed = 0;

  if (missing == 0) {
    gapRepairPassPending = false;
    gapRepairCursor = -1;
    backfillActive = false;

    Serial.println(
      "[GAP] Official 7 day window complete - no missing frames"
    );

    return;
  }

  gapRepairPassPending = true;
  gapRepairCursor =
      (int16_t)nsmcAvailableCount - 1;
  backfillActive = true;

  Serial.printf(
    "[GAP] Repair queue: %u missing official frames; incremental mode\n",
    (unsigned)missing
  );
}

static void repairOneArchiveGapFromAvailability() {
  if (
    !gapRepairPassPending ||
    !sdReady ||
    nsmcAvailableCount == 0
  ) {
    return;
  }

  if (
    replayActive ||
    replayStartPending ||
    pendingButtonEvent !=
      (uint8_t)ButtonEvent::None
  ) {
    return;
  }

  // Find the next missing official frame, newest first.  Existing frames are
  // skipped without consuming a network turn.
  while (gapRepairCursor >= 0) {
    const char *candidate =
        nsmcAvailableTimes[gapRepairCursor];

    --gapRepairCursor;

    if (!archiveExists(candidate)) {
      char stamp[13];
      memcpy(stamp, candidate, sizeof(stamp));
      stamp[12] = '\0';

      Serial.printf(
        "[GAP] Missing %s UTC\n",
        stamp
      );

      bool saved = false;
      bool transportUnavailable = false;

      if (
        workMutex &&
        xSemaphoreTake(
          workMutex,
          portMAX_DELAY
        ) == pdTRUE
      ) {
        // Re-check user/replay state after obtaining the shared work lock.
        if (
          replayActive ||
          replayStartPending ||
          pendingButtonEvent !=
            (uint8_t)ButtonEvent::None
        ) {
          xSemaphoreGive(
            workMutex
          );

          // Put this candidate back at the head of the remaining pass so it is
          // not accidentally skipped merely because the user pressed a button.
          ++gapRepairCursor;
          return;
        }

        if (
          WiFi.status() !=
            WL_CONNECTED &&
          !wifiConnect()
        ) {
          Serial.println(
            "[GAP] Wi-Fi unavailable; pausing repair until next availability pass"
          );

          transportUnavailable = true;
        }
        else {
          saved =
              fetchExactFrame(
                stamp,
                false,
                true
              );

          if (
            !saved &&
            lastNSMCTransportCode < 0
          ) {
            transportUnavailable = true;
          }
        }

        xSemaphoreGive(
          workMutex
        );
      }

      if (saved) {
        ++gapRepairSaved;
      }
      else {
        ++gapRepairFailed;

        Serial.printf(
          "[GAP] %s still missing; will retry on a later pass\n",
          stamp
        );
      }

      if (
        backfillDone <
          backfillTotal
      ) {
        ++backfillDone;
      }

      // A transport outage will almost certainly make every subsequent frame
      // fail too.  End this pass rather than hammering the same unavailable
      // host; the next 15-minute availability refresh starts a fresh pass.
      if (transportUnavailable) {
        finishArchiveGapRepairPass();
      }

      // Crucially: ONE network/archive attempt per worker turn.  Return now so
      // ISS forecast and live-display services get CPU time before the next gap.
      return;
    }
  }

  // Cursor exhausted: this incremental pass is complete.
  finishArchiveGapRepairPass();
}

// -----------------------------------------------------------------------------
// Fetch newest available NSMC global IR composite
// -----------------------------------------------------------------------------

static bool fetchLatestNSMCIRFallbackHourly() {
  if (!wifiConnect())
    return false;

  if (
    time(nullptr) <
    1700000000
  ) {
    if (!syncUTC())
      return false;
  }

  Serial.println(
    "[LIVE] Availability API unavailable; using hourly fallback"
  );

  // Emergency fallback only.  Try six complete UTC hours, never :15/:30/:45.
  for (
    int hour = 0;
    hour < 6;
    ++hour
  ) {
    char dt[13];

    makeNSMCTime(
      hour,
      dt
    );

    if (
      sdReady &&
      archiveExists(dt)
    ) {
      if (
        loadArchivedFrame(
          dt,
          true
        )
      ) {
        Serial.printf(
          "[LIVE] Fallback loaded %s UTC from SD\n",
          dt
        );

        return true;
      }
    }

    if (
      fetchExactFrame(
        dt,
        true,
        true
      )
    ) {
      Serial.printf(
        "[LIVE] Fallback accepted %s UTC\n",
        dt
      );

      return true;
    }

    if (
      lastNSMCTransportCode <
      0
    ) {
      Serial.printf(
        "[LIVE] Transport unavailable (%d); aborting hourly retry batch\n",
        lastNSMCTransportCode
      );

      break;
    }
  }

  Serial.println(
    "[LIVE] Hourly fallback found no usable network image"
  );

  if (
    !haveIRClouds &&
    sdReady
  ) {
    Serial.println(
      "[LIVE] No live cloud map in RAM; trying newest SD archive"
    );

    if (
      activateNewestArchivedHourlyFrame()
    ) {
      return true;
    }
  }

  Serial.println(
    "[LIVE] Keeping previous map"
  );

  return false;
}

// -----------------------------------------------------------------------------
// SD init
// -----------------------------------------------------------------------------

static bool initSDCard() {
  sdSPI.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  pinMode(
    SD_CS,
    OUTPUT
  );

  digitalWrite(
    SD_CS,
    HIGH
  );

  if (
    !SD.begin(
      SD_CS,
      sdSPI,
      SD_SPI_HZ
    )
  ) {
    Serial.println(
      "[SD] Mount failed"
    );

    sdReady = false;
    return false;
  }

  if (
    SD.cardType() ==
    CARD_NONE
  ) {
    Serial.println(
      "[SD] No card"
    );

    sdReady = false;
    return false;
  }

  sdReady = true;

  Serial.printf(
    "[SD] Ready %llu MB\n",
    SD.cardSize() /
    (1024ULL * 1024ULL)
  );

  return true;
}


// -----------------------------------------------------------------------------
// ISS (ZARYA) TLE cache / CelesTrak throttle
// -----------------------------------------------------------------------------

static void ensureISSDir() {
  if (
    sdReady &&
    !SD.exists(ISS_DIR)
  ) {
    SD.mkdir(ISS_DIR);
  }
}

static bool writeTextFileAtomic(
  const char *finalPath,
  const char *tempPath,
  const char *contents
) {
  if (
    !sdReady ||
    !finalPath ||
    !tempPath ||
    !contents
  ) {
    return false;
  }

  SD.remove(tempPath);

  File f =
      SD.open(
        tempPath,
        FILE_WRITE
      );

  if (!f)
    return false;

  const size_t expected =
      strlen(contents);

  const size_t written =
      f.write(
        (const uint8_t *)contents,
        expected
      );

  f.flush();
  f.close();

  if (written != expected) {
    SD.remove(tempPath);
    return false;
  }

  SD.remove(finalPath);

  if (
    !SD.rename(
      tempPath,
      finalPath
    )
  ) {
    SD.remove(tempPath);
    return false;
  }

  return true;
}

static bool saveISSFetchState() {
  if (!sdReady)
    return false;

  ensureISSDir();

  char state[96];

  snprintf(
    state,
    sizeof(state),
    "last_attempt=%lld\n"
    "last_success=%lld\n",
    (long long)issLastAttemptUTC,
    (long long)issLastSuccessUTC
  );

  const bool ok =
      writeTextFileAtomic(
        ISS_STATE_FILE,
        "/iss/fetch_state.tmp",
        state
      );

  if (!ok) {
    Serial.println(
      "[ISS] Could not save fetch state"
    );
  }

  return ok;
}

static void loadISSFetchState() {
  issLastAttemptUTC = 0;
  issLastSuccessUTC = 0;

  if (
    !sdReady ||
    !SD.exists(ISS_STATE_FILE)
  ) {
    return;
  }

  File f =
      SD.open(
        ISS_STATE_FILE,
        FILE_READ
      );

  if (!f)
    return;

  while (f.available()) {
    String line =
        f.readStringUntil('\n');

    line.trim();

    if (
      line.startsWith(
        "last_attempt="
      )
    ) {
      issLastAttemptUTC =
          (time_t)strtoll(
            line.c_str() + 13,
            nullptr,
            10
          );
    }
    else if (
      line.startsWith(
        "last_success="
      )
    ) {
      issLastSuccessUTC =
          (time_t)strtoll(
            line.c_str() + 13,
            nullptr,
            10
          );
    }
  }

  f.close();

  Serial.printf(
    "[ISS] State attempt=%lld success=%lld\n",
    (long long)issLastAttemptUTC,
    (long long)issLastSuccessUTC
  );
}

static bool tleChecksumValid(
  const char *line
) {
  if (!line)
    return false;

  const size_t len =
      strlen(line);

  if (len < 69)
    return false;

  if (
    line[68] < '0' ||
    line[68] > '9'
  ) {
    return false;
  }

  uint16_t sum = 0;

  for (size_t i = 0; i < 68; ++i) {
    const char c =
        line[i];

    if (
      c >= '0' &&
      c <= '9'
    ) {
      sum +=
          (uint16_t)(
            c - '0'
          );
    }
    else if (c == '-') {
      sum += 1;
    }
  }

  return
      (sum % 10) ==
      (uint16_t)(
        line[68] - '0'
      );
}

static bool validateISSTLE(
  const char *name,
  const char *line1,
  const char *line2
) {
  if (
    !name ||
    !line1 ||
    !line2
  ) {
    return false;
  }

  if (
    strcmp(
      name,
      "ISS (ZARYA)"
    ) != 0
  ) {
    Serial.printf(
      "[ISS] Wrong object name: %s\n",
      name
    );

    return false;
  }

  if (
    strncmp(
      line1,
      "1 25544",
      7
    ) != 0 ||
    strncmp(
      line2,
      "2 25544",
      7
    ) != 0
  ) {
    Serial.println(
      "[ISS] Wrong NORAD catalog number"
    );

    return false;
  }

  if (
    !tleChecksumValid(line1) ||
    !tleChecksumValid(line2)
  ) {
    Serial.println(
      "[ISS] TLE checksum failed"
    );

    return false;
  }

  return true;
}

static bool saveISSTLE(
  const char *name,
  const char *line1,
  const char *line2
) {
  if (!sdReady)
    return false;

  ensureISSDir();

  char contents[224];

  snprintf(
    contents,
    sizeof(contents),
    "%s\n%s\n%s\n",
    name,
    line1,
    line2
  );

  if (
    !writeTextFileAtomic(
      ISS_TLE_FILE,
      "/iss/iss.tmp",
      contents
    )
  ) {
    Serial.println(
      "[ISS] Could not save TLE"
    );

    return false;
  }

  return true;
}

static bool loadISSTLECache() {
  haveISSTLE = false;

  issTLEName[0] = '\0';
  issTLELine1[0] = '\0';
  issTLELine2[0] = '\0';

  if (
    !sdReady ||
    !SD.exists(ISS_TLE_FILE)
  ) {
    Serial.println(
      "[ISS] No cached TLE"
    );

    return false;
  }

  File f =
      SD.open(
        ISS_TLE_FILE,
        FILE_READ
      );

  if (!f)
    return false;

  String name =
      f.readStringUntil('\n');

  String line1 =
      f.readStringUntil('\n');

  String line2 =
      f.readStringUntil('\n');

  f.close();

  name.trim();
  line1.trim();
  line2.trim();

  if (
    !validateISSTLE(
      name.c_str(),
      line1.c_str(),
      line2.c_str()
    )
  ) {
    Serial.println(
      "[ISS] Cached TLE invalid"
    );

    return false;
  }

  strlcpy(
    issTLEName,
    name.c_str(),
    sizeof(issTLEName)
  );

  strlcpy(
    issTLELine1,
    line1.c_str(),
    sizeof(issTLELine1)
  );

  strlcpy(
    issTLELine2,
    line2.c_str(),
    sizeof(issTLELine2)
  );

  haveISSTLE = true;

  Serial.println(
    "[ISS] Cached ISS (ZARYA) TLE loaded"
  );

  return true;
}

static bool fetchISSTLEFromCelesTrak() {
  if (
    !sdReady ||
    WiFi.status() != WL_CONNECTED
  ) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12);
  client.setTimeout(15000);

  HTTPClient http;

  if (
    !http.begin(
      client,
      ISS_TLE_URL
    )
  ) {
    Serial.println(
      "[ISS] HTTP begin failed"
    );

    return false;
  }

  http.useHTTP10(true);
  http.setConnectTimeout(12000);
  http.setTimeout(15000);

  http.addHeader(
    "Accept",
    "text/plain"
  );

  http.addHeader(
    "User-Agent",
    "ESP32-S2-Live-Cloud-Globe/2.0"
  );

  Serial.println(
    "[ISS] GET CelesTrak CATNR=25544 only"
  );

  const uint32_t startMs =
      millis();

  const int code =
      http.GET();

  Serial.printf(
    "[ISS] GET returned %d after %lu ms\n",
    code,
    (unsigned long)(
      millis() -
      startMs
    )
  );

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String body =
      http.getString();

  http.end();

  body.replace("\r", "");

  const int firstNL =
      body.indexOf('\n');

  const int secondNL =
      (
        firstNL >= 0
      ) ?
      body.indexOf(
        '\n',
        firstNL + 1
      ) :
      -1;

  if (
    firstNL < 0 ||
    secondNL < 0
  ) {
    Serial.println(
      "[ISS] Response did not contain 3 TLE lines"
    );

    return false;
  }

  String name =
      body.substring(
        0,
        firstNL
      );

  String line1 =
      body.substring(
        firstNL + 1,
        secondNL
      );

  int thirdNL =
      body.indexOf(
        '\n',
        secondNL + 1
      );

  if (thirdNL < 0)
    thirdNL = body.length();

  String line2 =
      body.substring(
        secondNL + 1,
        thirdNL
      );

  name.trim();
  line1.trim();
  line2.trim();

  if (
    !validateISSTLE(
      name.c_str(),
      line1.c_str(),
      line2.c_str()
    )
  ) {
    Serial.println(
      "[ISS] CelesTrak response rejected"
    );

    return false;
  }

  if (
    !saveISSTLE(
      name.c_str(),
      line1.c_str(),
      line2.c_str()
    )
  ) {
    // Do not mark this as a successful refresh unless it is safely cached.
    return false;
  }

  strlcpy(
    issTLEName,
    name.c_str(),
    sizeof(issTLEName)
  );

  strlcpy(
    issTLELine1,
    line1.c_str(),
    sizeof(issTLELine1)
  );

  strlcpy(
    issTLELine2,
    line2.c_str(),
    sizeof(issTLELine2)
  );

  haveISSTLE = true;

  Serial.println(
    "[ISS] ISS (ZARYA) TLE cached"
  );

  return true;
}

static bool issRefreshDue(
  time_t now
) {
  if (
    !haveISSTLE ||
    issLastSuccessUTC <= 0
  ) {
    return true;
  }

  if (now <= issLastSuccessUTC)
    return false;

  return
      (uint64_t)(
        now -
        issLastSuccessUTC
      ) >=
      ISS_SUCCESS_INTERVAL_SEC;
}

static bool issRetryAllowed(
  time_t now
) {
  if (issLastAttemptUTC <= 0)
    return true;

  if (now <= issLastAttemptUTC)
    return false;

  return
      (uint64_t)(
        now -
        issLastAttemptUTC
      ) >=
      ISS_RETRY_INTERVAL_SEC;
}

static bool serviceISSTLE(
  bool bootMessage
) {
  if (!appConfig.enableISS)
    return false;

  if (!sdReady) {
    if (bootMessage) {
      bootPrintln(
        "[ISS] No SD - no fetch"
      );
    }

    return false;
  }

  const time_t now =
      time(nullptr);

  if (now < 1700000000) {
    if (bootMessage) {
      bootPrintln(
        "[ISS] Waiting for UTC"
      );
    }

    return false;
  }

  if (
    !issRefreshDue(now)
  ) {
    if (bootMessage) {
      uint32_t ageHours = 0;

      if (
        issLastSuccessUTC > 0 &&
        now > issLastSuccessUTC
      ) {
        ageHours =
            (uint32_t)(
              (
                now -
                issLastSuccessUTC
              ) /
              3600
            );
      }

      bootPrintf(
        "[ISS] Cached %luh",
        (unsigned long)ageHours
      );
    }

    return true;
  }

  if (
    !issRetryAllowed(now)
  ) {
    if (bootMessage) {
      uint32_t waitMinutes = 0;

      const time_t nextTry =
          issLastAttemptUTC +
          (time_t)ISS_RETRY_INTERVAL_SEC;

      if (nextTry > now) {
        waitMinutes =
            (uint32_t)(
              (
                nextTry -
                now +
                59
              ) /
              60
            );
      }

      bootPrintf(
        "[ISS] Retry in %lum",
        (unsigned long)waitMinutes
      );
    }

    return haveISSTLE;
  }

  if (
    WiFi.status() != WL_CONNECTED &&
    !wifiConnect(12000)
  ) {
    if (bootMessage) {
      bootPrintln(
        "[ISS] WiFi unavailable"
      );
    }

    // A Wi-Fi connection failure is intentionally NOT recorded as a
    // CelesTrak attempt because no request reached CelesTrak.
    return haveISSTLE;
  }

  // Persist the attempt BEFORE opening the HTTPS request.  If power is lost
  // mid-request, the reboot still honours the 2-hour retry throttle.
  issLastAttemptUTC =
      now;

  if (!saveISSFetchState()) {
    Serial.println(
      "[ISS] State persistence failed; request cancelled"
    );

    if (bootMessage) {
      bootPrintln(
        "[ISS] State save failed"
      );
    }

    return haveISSTLE;
  }

  if (bootMessage) {
    bootPrintln(
      "[ISS] Refreshing TLE..."
    );
  }

  if (
    !fetchISSTLEFromCelesTrak()
  ) {
    if (bootMessage) {
      bootPrintln(
        "[ISS] Fetch failed"
      );
    }

    return haveISSTLE;
  }

  // A valid response safely written to SD counts as one successful refresh.
  // Even if CelesTrak happens to return the same orbital element set, do not
  // request it again for another 24 hours.
  issLastSuccessUTC =
      now;

  saveISSFetchState();

  if (bootMessage) {
    bootPrintln(
      "[ISS] TLE OK / 24h"
    );
  }

  return true;
}


// -----------------------------------------------------------------------------
// ISS local propagation
// -----------------------------------------------------------------------------

static P13Satellite *issPredictor = nullptr;

static char issPredictorLine1[80] = "";
static char issPredictorLine2[80] = "";

static uint32_t issLastOrbitBuildMillis = 0;
static bool issFirstOrbitLogDone = false;

static bool makeP13UTC(
  time_t utc,
  P13DateTime &dt
) {
  if (utc < 1700000000)
    return false;

  struct tm tmUTC;

  if (
    !gmtime_r(
      &utc,
      &tmUTC
    )
  ) {
    return false;
  }

  dt.settime(
    tmUTC.tm_year + 1900,
    tmUTC.tm_mon + 1,
    tmUTC.tm_mday,
    tmUTC.tm_hour,
    tmUTC.tm_min,
    tmUTC.tm_sec
  );

  return true;
}

static bool ensureISSPredictor() {
  if (!haveISSTLE)
    return false;

  const bool changed =
      !issPredictor ||
      strcmp(
        issPredictorLine1,
        issTLELine1
      ) != 0 ||
      strcmp(
        issPredictorLine2,
        issTLELine2
      ) != 0;

  if (!changed)
    return true;

  if (!issPredictor) {
    issPredictor =
        new (std::nothrow)
        P13Satellite(
          issTLEName,
          issTLELine1,
          issTLELine2
        );

    if (!issPredictor) {
      Serial.println(
        "[ISS] Propagator allocation failed"
      );

      return false;
    }
  }
  else {
    issPredictor->tle(
      issTLEName,
      issTLELine1,
      issTLELine2
    );
  }

  strlcpy(
    issPredictorLine1,
    issTLELine1,
    sizeof(issPredictorLine1)
  );

  strlcpy(
    issPredictorLine2,
    issTLELine2,
    sizeof(issPredictorLine2)
  );

  // Force fresh renderer and long-range forecast caches immediately after
  // any TLE change.
  issLastOrbitBuildMillis = 0;
  issOrbitValid = false;
  issUpcomingForecastValid = false;

  Serial.println(
    "[ISS] AioP13 propagator ready"
  );

  return true;
}

static bool readISSSpacePoint(
  ISSSpacePoint &point
) {
  if (!issPredictor)
    return false;

  const double xKm =
      issPredictor->c_vecS[0];

  const double yKm =
      issPredictor->c_vecS[1];

  const double zKm =
      issPredictor->c_vecS[2];

  if (
    !isfinite(xKm) ||
    !isfinite(yKm) ||
    !isfinite(zKm)
  ) {
    return false;
  }

  point.xER =
      (float)(
        xKm /
        ISS_EARTH_RADIUS_KM
      );

  point.yER =
      (float)(
        yKm /
        ISS_EARTH_RADIUS_KM
      );

  point.zER =
      (float)(
        zKm /
        ISS_EARTH_RADIUS_KM
      );

  return true;
}

static bool propagateISSAt(
  time_t utc,
  float &latDeg,
  float &lonDeg,
  ISSSpacePoint *spacePoint = nullptr
) {
  if (
    !ensureISSPredictor()
  ) {
    return false;
  }

  P13DateTime dt;

  if (
    !makeP13UTC(
      utc,
      dt
    )
  ) {
    return false;
  }

  double lat = 0.0;
  double lon = 0.0;

  issPredictor->predict(dt);
  issPredictor->latlon(
    lat,
    lon
  );

  if (
    !isfinite(lat) ||
    !isfinite(lon)
  ) {
    return false;
  }

  if (
    spacePoint &&
    !readISSSpacePoint(
      *spacePoint
    )
  ) {
    return false;
  }

  latDeg =
      (float)lat;

  lonDeg =
      (float)lon;

  return true;
}

static bool rebuildISSOrbit(
  time_t nowUTC
) {
  if (
    !ensureISSPredictor()
  ) {
    return false;
  }

  P13DateTime baseTime;

  if (
    !makeP13UTC(
      nowUTC,
      baseTime
    )
  ) {
    return false;
  }

  const uint8_t inactive =
      (uint8_t)(
        1u -
        issOrbitActiveBuffer
      );

  ISSSpacePoint *dst =
      issOrbitBuffers[
        inactive
      ];

  for (
    uint16_t i = 0;
    i < ISS_ORBIT_POINT_COUNT;
    ++i
  ) {
    const int32_t offsetSec =
        -ISS_ORBIT_HALF_WINDOW_SEC +
        (int32_t)i *
        ISS_ORBIT_STEP_SEC;

    P13DateTime sampleTime(
      baseTime
    );

    sampleTime.add(
      (double)offsetSec /
      86400.0
    );

    issPredictor->predict(
      sampleTime
    );

    if (
      !readISSSpacePoint(
        dst[i]
      )
    ) {
      return false;
    }
  }

  portENTER_CRITICAL(
    &issRenderMux
  );

  issOrbitActiveBuffer =
      inactive;

  issOrbitValid =
      true;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (!issFirstOrbitLogDone) {
    issFirstOrbitLogDone = true;

    Serial.printf(
      "[ISS] 3D orbit cache ready: %u points, %+ld..%+ld min\n",
      (unsigned)ISS_ORBIT_POINT_COUNT,
      (long)(
        -ISS_ORBIT_HALF_WINDOW_SEC /
        60
      ),
      (long)(
        ISS_ORBIT_HALF_WINDOW_SEC /
        60
      )
    );
  }

  return true;
}

static bool ensureISSObserver() {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured
  ) {
    return false;
  }

  if (issObserver)
    return true;

  issObserver =
      new (std::nothrow)
      P13Observer(
        "GLOBE",
        appConfig.observerLatDeg,
        appConfig.observerLonDeg,
        appConfig.observerAltM
      );

  if (!issObserver) {
    Serial.println(
      "[ISS OBS] Observer allocation failed"
    );

    return false;
  }

  Serial.println(
    "[ISS OBS] Observer ready"
  );

  return true;
}

// True while the ISS is outside Earth's full umbra.
//
// This uses a finite-Sun conical umbra rather than an infinite cylindrical
// shadow. Penumbra is intentionally treated as sunlit; for LEO the partial
// phase is brief and this keeps the visible/not-visible state useful and
// stable on the 7-second display cadence.
static bool isISSXYZSunlit(
  double px,
  double py,
  double pz,
  double sx,
  double sy,
  double sz
) {
  const double axialDot =
      px * sx +
      py * sy +
      pz * sz;

  // Sunward side of Earth cannot be in Earth's umbra.
  if (axialDot >= 0.0)
    return true;

  const double behindEarthKm =
      -axialDot;

  const double umbraLengthKm =
      SUN_MEAN_DISTANCE_KM *
      ISS_EARTH_RADIUS_KM /
      (
        SUN_RADIUS_KM -
        ISS_EARTH_RADIUS_KM
      );

  if (
    behindEarthKm >=
    umbraLengthKm
  ) {
    return true;
  }

  const double radius2 =
      px * px +
      py * py +
      pz * pz;

  double perpendicular2 =
      radius2 -
      axialDot * axialDot;

  if (perpendicular2 < 0.0)
    perpendicular2 = 0.0;

  const double umbraRadiusKm =
      ISS_EARTH_RADIUS_KM *
      (
        1.0 -
        behindEarthKm /
        umbraLengthKm
      );

  return
      perpendicular2 >=
      umbraRadiusKm *
      umbraRadiusKm;
}

static bool currentISSIsSunlit() {
  if (!issPredictor)
    return false;

  return
      isISSXYZSunlit(
        issPredictor->c_vecS[0],
        issPredictor->c_vecS[1],
        issPredictor->c_vecS[2],
        issVisibilitySun.c_vecH[0],
        issVisibilitySun.c_vecH[1],
        issVisibilitySun.c_vecH[2]
      );
}

static void updateISSObserverStatus(
  time_t nowUTC
) {
  if (
    !ensureISSObserver() ||
    !issPredictor
  ) {
    portENTER_CRITICAL(
      &issRenderMux
    );

    issObserverStatusValid =
        false;

    portEXIT_CRITICAL(
      &issRenderMux
    );

    return;
  }

  // IMPORTANT: caller invokes this immediately after propagateISSAt(nowUTC),
  // before rebuildISSOrbit() advances the shared predictor through +/-60 min.
  double elevationDeg = 0.0;
  double azimuthDeg = 0.0;

  issPredictor->elaz(
    *issObserver,
    elevationDeg,
    azimuthDeg
  );

  const double dx =
      issPredictor->c_vecS[0] -
      issObserver->c_vecO[0];

  const double dy =
      issPredictor->c_vecS[1] -
      issObserver->c_vecO[1];

  const double dz =
      issPredictor->c_vecS[2] -
      issObserver->c_vecO[2];

  const double rangeKm =
      sqrt(
        dx * dx +
        dy * dy +
        dz * dz
      );

  P13DateTime sunTime;

  if (
    !makeP13UTC(
      nowUTC,
      sunTime
    )
  ) {
    return;
  }

  issVisibilitySun.predict(
    sunTime
  );

  double observerSunElevationDeg = 0.0;
  double observerSunAzimuthDeg = 0.0;

  issVisibilitySun.elaz(
    *issObserver,
    observerSunElevationDeg,
    observerSunAzimuthDeg
  );

  const bool sunlit =
      currentISSIsSunlit();

  const bool above =
      elevationDeg > 0.0;

  const bool visible =
      elevationDeg >=
        appConfig.visibleMinElevationDeg &&
      observerSunElevationDeg <=
        appConfig.visibleSunMaxElevationDeg &&
      sunlit;

  portENTER_CRITICAL(
    &issRenderMux
  );

  issObserverElevationDeg =
      (float)elevationDeg;

  issObserverAzimuthDeg =
      (float)azimuthDeg;

  issObserverRangeKm =
      (float)rangeKm;

  issObserverSunElevationDeg =
      (float)observerSunElevationDeg;

  issAboveHorizon =
      above;

  issSunlit =
      sunlit;

  issOpticallyVisible =
      visible;

  issObserverStatusValid =
      true;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  // Log state changes immediately, plus a 30-second heartbeat while above the
  // horizon so hardware testing is easy without flooding Serial all day.
  const uint32_t nowMs =
      millis();

  const bool changed =
      !lastLoggedObserverValid ||
      above !=
        lastLoggedAboveHorizon ||
      visible !=
        lastLoggedVisible ||
      sunlit !=
        lastLoggedSunlit;

  const bool heartbeat =
      above &&
      (
        nowMs -
        lastObserverStatusLogMs >=
        30000
      );

  if (
    changed ||
    heartbeat
  ) {
    lastLoggedObserverValid =
        true;

    lastLoggedAboveHorizon =
        above;

    lastLoggedVisible =
        visible;

    lastLoggedSunlit =
        sunlit;

    lastObserverStatusLogMs =
        nowMs;

    Serial.printf(
      "[ISS OBS] EL=%.1f AZ=%.1f RANGE=%.0fkm SUN=%.1f ISS_SUN=%u ABOVE=%u VISIBLE=%u\n",
      elevationDeg,
      azimuthDeg,
      rangeKm,
      observerSunElevationDeg,
      sunlit ? 1u : 0u,
      above ? 1u : 0u,
      visible ? 1u : 0u
    );
  }
}

static bool evaluateISSForecastState(
  P13Satellite &satellite,
  P13Observer &observer,
  P13Sun &sun,
  time_t utc,
  double &elevationDeg,
  bool &visible
) {
  P13DateTime dt;

  if (
    !makeP13UTC(
      utc,
      dt
    )
  ) {
    return false;
  }

  satellite.predict(
    dt
  );

  double azimuthDeg = 0.0;

  satellite.elaz(
    observer,
    elevationDeg,
    azimuthDeg
  );

  sun.predict(
    dt
  );

  double sunElevationDeg = 0.0;
  double sunAzimuthDeg = 0.0;

  sun.elaz(
    observer,
    sunElevationDeg,
    sunAzimuthDeg
  );

  const bool sunlit =
      isISSXYZSunlit(
        satellite.c_vecS[0],
        satellite.c_vecS[1],
        satellite.c_vecS[2],
        sun.c_vecH[0],
        sun.c_vecH[1],
        sun.c_vecH[2]
      );

  visible =
      elevationDeg >=
        appConfig.visibleMinElevationDeg &&
      sunElevationDeg <=
        appConfig.visibleSunMaxElevationDeg &&
      sunlit;

  return
      isfinite(
        elevationDeg
      ) &&
      isfinite(
        sunElevationDeg
      );
}

static time_t refineISSForecastTransition(
  P13Satellite &satellite,
  P13Observer &observer,
  P13Sun &sun,
  time_t lowUTC,
  time_t highUTC,
  bool visibleTransition
) {
  // Caller guarantees:
  //   low  = predicate false
  //   high = predicate true
  //
  // Six bisections turn a 60-second coarse interval into ~1-second timing.
  for (
    uint8_t i = 0;
    i < 6;
    ++i
  ) {
    if (
      highUTC -
      lowUTC <= 1
    ) {
      break;
    }

    const time_t midUTC =
        lowUTC +
        (
          highUTC -
          lowUTC
        ) /
        2;

    double elevationDeg = -90.0;
    bool visible = false;

    if (
      !evaluateISSForecastState(
        satellite,
        observer,
        sun,
        midUTC,
        elevationDeg,
        visible
      )
    ) {
      break;
    }

    const bool predicate =
        visibleTransition ?
        visible :
        elevationDeg > 0.0;

    if (predicate) {
      highUTC =
          midUTC;
    }
    else {
      lowUTC =
          midUTC;
    }
  }

  return
      highUTC;
}

static float computeISSPassMaxElevationAroundTime(
  P13Satellite &satellite,
  P13Observer &observer,
  time_t eventUTC
) {
  if (eventUTC < 1700000000)
    return -1.0f;

  // ISS horizon-to-horizon passes are much shorter than this window and
  // successive passes are ~90 minutes apart, so +/-20 minutes safely captures
  // the complete pass containing the visible event.
  constexpr int32_t SPAN_SEC =
      20 * 60;

  constexpr int32_t COARSE_STEP_SEC =
      10;

  float bestElevation =
      -90.0f;

  time_t bestUTC = 0;

  for (
    int32_t offset =
        -SPAN_SEC;
    offset <=
        SPAN_SEC;
    offset +=
        COARSE_STEP_SEC
  ) {
    const time_t sampleUTC =
        eventUTC +
        offset;

    P13DateTime dt;

    if (
      !makeP13UTC(
        sampleUTC,
        dt
      )
    ) {
      continue;
    }

    satellite.predict(
      dt
    );

    double elevationDeg = -90.0;
    double azimuthDeg = 0.0;

    satellite.elaz(
      observer,
      elevationDeg,
      azimuthDeg
    );

    if (
      isfinite(
        elevationDeg
      ) &&
      elevationDeg >
        bestElevation
    ) {
      bestElevation =
          (float)elevationDeg;

      bestUTC =
          sampleUTC;
    }
  }

  if (bestUTC == 0)
    return -1.0f;

  // Refine +/-10 seconds around the best coarse sample at one-second spacing.
  for (
    int32_t offset = -10;
    offset <= 10;
    ++offset
  ) {
    const time_t sampleUTC =
        bestUTC +
        offset;

    P13DateTime dt;

    if (
      !makeP13UTC(
        sampleUTC,
        dt
      )
    ) {
      continue;
    }

    satellite.predict(
      dt
    );

    double elevationDeg = -90.0;
    double azimuthDeg = 0.0;

    satellite.elaz(
      observer,
      elevationDeg,
      azimuthDeg
    );

    if (
      isfinite(
        elevationDeg
      ) &&
      elevationDeg >
        bestElevation
    ) {
      bestElevation =
          (float)elevationDeg;
    }
  }

  if (bestElevation > 90.0f)
    bestElevation = 90.0f;

  return
      bestElevation;
}


static bool refreshISSUpcomingForecast() {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured ||
    !haveISSTLE
  ) {
    return false;
  }

  const time_t nowUTC =
      time(nullptr);

  if (nowUTC < 1700000000)
    return false;

  issUpcomingForecastBusy =
      true;

  Serial.println(
    "[ISS FORECAST] Calculating..."
  );

  // Independent objects: this long scan cannot move the live ISS glyph,
  // observer state, day/night state, or the +/-60 minute orbit cache.
  P13Satellite forecastSatellite(
    issTLEName,
    issTLELine1,
    issTLELine2
  );

  P13Observer forecastObserver(
    "FORECAST",
    appConfig.observerLatDeg,
    appConfig.observerLonDeg,
    appConfig.observerAltM
  );

  P13Sun forecastSun;

  double previousElevationDeg = -90.0;
  bool previousVisible = false;

  if (
    !evaluateISSForecastState(
      forecastSatellite,
      forecastObserver,
      forecastSun,
      nowUTC,
      previousElevationDeg,
      previousVisible
    )
  ) {
    issUpcomingForecastBusy =
        false;

    return false;
  }

  bool previousAbove =
      previousElevationDeg > 0.0;

  // If calculation begins during an existing pass/visible interval, skip that
  // current event and report the NEXT one.
  bool passArmed =
      !previousAbove;

  bool visibleArmed =
      !previousVisible;

  time_t nextPassUTC = 0;
  time_t nextVisibleUTC = 0;

  time_t previousUTC =
      nowUTC;

  uint32_t sampleCount = 0;

  for (
    int32_t offsetSec =
        ISS_UPCOMING_FORECAST_STEP_SEC;
    offsetSec <=
        ISS_UPCOMING_FORECAST_MAX_SEC;
    offsetSec +=
        ISS_UPCOMING_FORECAST_STEP_SEC
  ) {
    const time_t sampleUTC =
        nowUTC +
        offsetSec;

    double elevationDeg = -90.0;
    bool visible = false;

    if (
      !evaluateISSForecastState(
        forecastSatellite,
        forecastObserver,
        forecastSun,
        sampleUTC,
        elevationDeg,
        visible
      )
    ) {
      previousUTC =
          sampleUTC;

      continue;
    }

    const bool above =
        elevationDeg > 0.0;

    if (!passArmed) {
      if (!above) {
        passArmed =
            true;
      }
    }
    else if (
      nextPassUTC == 0 &&
      !previousAbove &&
      above
    ) {
      nextPassUTC =
          refineISSForecastTransition(
            forecastSatellite,
            forecastObserver,
            forecastSun,
            previousUTC,
            sampleUTC,
            false
          );

      // Publish the ordinary geometric pass immediately.  Finding the next
      // naked-eye-visible pass can require scanning days farther into the
      // future; the OLED should not hide a pass we already know about while
      // that longer search continues.  Busy intentionally remains true, so
      // NEXT VISIBLE can continue to display CALCULATING.
      if (nextPassUTC != 0) {
        portENTER_CRITICAL(
          &issRenderMux
        );

        issNextPassUTC =
            nextPassUTC;

        issNextVisibleUTC = 0;
        issNextVisibleMaxElevationDeg = -1.0f;

        issUpcomingForecastComputedUTC =
            nowUTC;

        issUpcomingForecastValid =
            true;

        portEXIT_CRITICAL(
          &issRenderMux
        );

        Serial.printf(
          "[ISS FORECAST] NEXT PASS=%lld published; continuing visible search\n",
          (long long)nextPassUTC
        );
      }
    }

    if (!visibleArmed) {
      if (!visible) {
        visibleArmed =
            true;
      }
    }
    else if (
      nextVisibleUTC == 0 &&
      !previousVisible &&
      visible
    ) {
      nextVisibleUTC =
          refineISSForecastTransition(
            forecastSatellite,
            forecastObserver,
            forecastSun,
            previousUTC,
            sampleUTC,
            true
          );
    }

    previousAbove =
        above;

    previousVisible =
        visible;

    previousElevationDeg =
        elevationDeg;

    previousUTC =
        sampleUTC;

    ++sampleCount;

    // This is a background calculation on a single-core ESP32-S2.  Yield every
    // 32 coarse samples so rendering and network housekeeping stay responsive
    // without unnecessarily stretching the forecast calculation.
    if (
      (
        sampleCount &
        0x1Fu
      ) == 0
    ) {
      vTaskDelay(
        pdMS_TO_TICKS(1)
      );
    }

    if (
      nextPassUTC != 0 &&
      nextVisibleUTC != 0
    ) {
      break;
    }
  }

  float nextVisibleMaxEl = -1.0f;

  if (
    nextVisibleUTC != 0
  ) {
    nextVisibleMaxEl =
        computeISSPassMaxElevationAroundTime(
          forecastSatellite,
          forecastObserver,
          nextVisibleUTC
        );
  }

  portENTER_CRITICAL(
    &issRenderMux
  );

  issNextPassUTC =
      nextPassUTC;

  issNextVisibleUTC =
      nextVisibleUTC;

  issNextVisibleMaxElevationDeg =
      nextVisibleMaxEl;

  issUpcomingForecastComputedUTC =
      nowUTC;

  issUpcomingForecastValid =
      true;

  issUpcomingForecastBusy =
      false;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  Serial.printf(
    "[ISS FORECAST] PASS=%lld VISIBLE=%lld VMAX=%.1f samples=%u\n",
    (long long)nextPassUTC,
    (long long)nextVisibleUTC,
    (double)nextVisibleMaxEl,
    (unsigned)sampleCount
  );

  return true;
}

static void serviceISSUpcomingForecast() {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured ||
    !haveISSTLE
  ) {
    return;
  }

  const time_t nowUTC =
      time(nullptr);

  if (nowUTC < 1700000000)
    return;

  bool valid = false;
  time_t nextPassUTC = 0;
  time_t nextVisibleUTC = 0;
  time_t computedUTC = 0;

  portENTER_CRITICAL(
    &issRenderMux
  );

  valid =
      issUpcomingForecastValid;

  nextPassUTC =
      issNextPassUTC;

  nextVisibleUTC =
      issNextVisibleUTC;

  computedUTC =
      issUpcomingForecastComputedUTC;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  bool refresh =
      !valid;

  // "NEXT" means strictly future.  As soon as a predicted event starts, retire
  // it and calculate the following event.  The old code retained an event for
  // 20 minutes after its onset, which could make a 22:45 forecast still show
  // a 22:40 pass.
  //
  // A small 5-second tolerance avoids needless recalculation from one-second
  // forecast-refinement jitter around the transition itself.
  constexpr time_t FORECAST_RETIRE_TOLERANCE_SEC =
      5;

  if (
    nextPassUTC != 0 &&
    nowUTC >=
      nextPassUTC +
      FORECAST_RETIRE_TOLERANCE_SEC
  ) {
    refresh =
        true;
  }

  if (
    nextVisibleUTC != 0 &&
    nowUTC >=
      nextVisibleUTC +
      FORECAST_RETIRE_TOLERANCE_SEC
  ) {
    refresh =
        true;
  }

  // If no visible event was found inside the 14-day search horizon, or simply
  // to keep an old forecast current, retry every six hours.
  if (
    computedUTC == 0 ||
    nowUTC -
      computedUTC >=
      6 * 60 * 60
  ) {
    refresh =
        true;
  }

  if (refresh) {
    refreshISSUpcomingForecast();
  }
}


static float observerElevationForER(
  float xER,
  float yER,
  float zER
) {
  if (!issObserver)
    return -90.0f;

  const double xKm =
      (double)xER *
      ISS_EARTH_RADIUS_KM;

  const double yKm =
      (double)yER *
      ISS_EARTH_RADIUS_KM;

  const double zKm =
      (double)zER *
      ISS_EARTH_RADIUS_KM;

  double rx =
      xKm -
      issObserver->c_vecO[0];

  double ry =
      yKm -
      issObserver->c_vecO[1];

  double rz =
      zKm -
      issObserver->c_vecO[2];

  const double rangeKm =
      sqrt(
        rx * rx +
        ry * ry +
        rz * rz
      );

  if (
    !isfinite(rangeKm) ||
    rangeKm <= 0.0
  ) {
    return -90.0f;
  }

  rx /= rangeKm;
  ry /= rangeKm;
  rz /= rangeKm;

  double up =
      rx *
      issObserver->c_vecU[0] +
      ry *
      issObserver->c_vecU[1] +
      rz *
      issObserver->c_vecU[2];

  if (up > 1.0)
    up = 1.0;

  if (up < -1.0)
    up = -1.0;

  return
      (float)degrees(
        asin(up)
      );
}

// Refine a current-pass optical visibility loss.  The caller supplies a
// low endpoint that is visible and a high endpoint that is not visible.
static time_t refineISSVisibilityEndTransition(
  P13Satellite &satellite,
  P13Observer &observer,
  P13Sun &sun,
  time_t lowUTC,
  time_t highUTC
) {
  for (
    uint8_t i = 0;
    i < 6;
    ++i
  ) {
    if (
      highUTC -
      lowUTC <= 1
    ) {
      break;
    }

    const time_t midUTC =
        lowUTC +
        (
          highUTC -
          lowUTC
        ) /
        2;

    double elevationDeg = -90.0;
    bool visible = false;

    if (
      !evaluateISSForecastState(
        satellite,
        observer,
        sun,
        midUTC,
        elevationDeg,
        visible
      )
    ) {
      break;
    }

    if (visible) {
      lowUTC =
          midUTC;
    }
    else {
      highUTC =
          midUTC;
    }
  }

  return
      highUTC;
}


// Predict the end of the CURRENT optical visibility interval.  This uses
// independent AioP13 objects so it cannot disturb the live globe propagator.
// It is called only when a new visible interval needs a cached end time.
static time_t predictISSCurrentVisibleEndUTC(
  time_t nowUTC,
  time_t predictedSetUTC
) {
  if (
    predictedSetUTC <=
      nowUTC ||
    !haveISSTLE ||
    !appConfig.observerConfigured
  ) {
    return 0;
  }

  P13Satellite satellite(
    issTLEName,
    issTLELine1,
    issTLELine2
  );

  P13Observer observer(
    "PASS END",
    appConfig.observerLatDeg,
    appConfig.observerLonDeg,
    appConfig.observerAltM
  );

  P13Sun sun;

  double elevationDeg = -90.0;
  bool visible = false;

  if (
    !evaluateISSForecastState(
      satellite,
      observer,
      sun,
      nowUTC,
      elevationDeg,
      visible
    ) ||
    !visible
  ) {
    return 0;
  }

  constexpr int32_t VISIBLE_END_STEP_SEC = 5;

  time_t previousUTC =
      nowUTC;

  // Look slightly beyond the interpolated geometric set so an interval that
  // remains visible right to the horizon still produces a true->false sample.
  const time_t scanEndUTC =
      predictedSetUTC +
      30;

  for (
    time_t sampleUTC =
        nowUTC +
        VISIBLE_END_STEP_SEC;
    sampleUTC <=
      scanEndUTC;
    sampleUTC +=
      VISIBLE_END_STEP_SEC
  ) {
    if (
      !evaluateISSForecastState(
        satellite,
        observer,
        sun,
        sampleUTC,
        elevationDeg,
        visible
      )
    ) {
      // Keep previousUTC at the last known-visible sample so the refinement
      // bracket remains valid if a later sample succeeds.
      continue;
    }

    if (!visible) {
      return
          refineISSVisibilityEndTransition(
            satellite,
            observer,
            sun,
            previousUTC,
            sampleUTC
          );
    }

    previousUTC =
        sampleUTC;
  }

  return
      predictedSetUTC;
}


static void updateISSPredictedSet(
  time_t nowUTC
) {
  if (
    !appConfig.enableISS ||
    !appConfig.observerConfigured ||
    !issObserver
  ) {
    portENTER_CRITICAL(
      &issRenderMux
    );

    issPredictedSetUTC = 0;
    issCurrentPassMaxElevationDeg = 0.0f;
    issCurrentPassMaxUTC = 0;
    issCurrentPassVisibleEndUTC = 0;

    portEXIT_CRITICAL(
      &issRenderMux
    );

    return;
  }

  bool orbitOK = false;
  bool aboveNow = false;
  bool visibleNow = false;
  float currentEl = -90.0f;
  uint8_t active = 0;
  time_t cachedVisibleEndUTC = 0;

  portENTER_CRITICAL(
    &issRenderMux
  );

  orbitOK =
      issOrbitValid;

  aboveNow =
      issAboveHorizon;

  visibleNow =
      issOpticallyVisible;

  currentEl =
      issObserverElevationDeg;

  active =
      issOrbitActiveBuffer;

  cachedVisibleEndUTC =
      issCurrentPassVisibleEndUTC;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (
    !orbitOK ||
    !aboveNow ||
    currentEl <= 0.0f
  ) {
    portENTER_CRITICAL(
      &issRenderMux
    );

    issPredictedSetUTC = 0;
    issCurrentPassMaxElevationDeg = 0.0f;
    issCurrentPassMaxUTC = 0;
    issCurrentPassVisibleEndUTC = 0;

    portEXIT_CRITICAL(
      &issRenderMux
    );

    return;
  }

  // Snapshot the full +/-60 minute cache.  This lets us inspect both the past
  // and future portions of the CURRENT pass and therefore know its full peak
  // elevation even after culmination.
  ISSSpacePoint orbit[
    ISS_ORBIT_POINT_COUNT
  ];

  portENTER_CRITICAL(
    &issRenderMux
  );

  for (
    uint16_t i = 0;
    i < ISS_ORBIT_POINT_COUNT;
    ++i
  ) {
    orbit[i] =
        issOrbitBuffers[
          active
        ][
          i
        ];
  }

  portEXIT_CRITICAL(
    &issRenderMux
  );

  float elevations[
    ISS_ORBIT_POINT_COUNT
  ];

  for (
    uint16_t i = 0;
    i < ISS_ORBIT_POINT_COUNT;
    ++i
  ) {
    elevations[i] =
        observerElevationForER(
          orbit[i].xER,
          orbit[i].yER,
          orbit[i].zER
        );
  }

  uint16_t passStart =
      ISS_ORBIT_NOW_INDEX;

  while (
    passStart > 0 &&
    elevations[
      passStart - 1
    ] > 0.0f
  ) {
    --passStart;
  }

  uint16_t passEnd =
      ISS_ORBIT_NOW_INDEX;

  while (
    passEnd + 1 <
      ISS_ORBIT_POINT_COUNT &&
    elevations[
      passEnd + 1
    ] > 0.0f
  ) {
    ++passEnd;
  }

  float predictedMaxEl =
      currentEl;

  uint16_t maxIndex =
      ISS_ORBIT_NOW_INDEX;

  for (
    uint16_t i = passStart;
    i <= passEnd;
    ++i
  ) {
    if (
      elevations[i] >
      predictedMaxEl
    ) {
      predictedMaxEl =
          elevations[i];

      maxIndex =
          i;
    }
  }

  // Fractional offset from the best coarse sample to the parabolic vertex.
  // This gives us both a better peak elevation and a useful MAX IN timestamp.
  float peakFraction = 0.0f;

  // Refine the coarse 45-second peak with a simple parabolic interpolation
  // through the neighbouring elevations. This is particularly useful for
  // deciding whether a high pass crosses the 60-degree "great pass" threshold.
  if (
    maxIndex > passStart &&
    maxIndex < passEnd
  ) {
    const float a =
        elevations[
          maxIndex - 1
        ];

    const float b =
        elevations[
          maxIndex
        ];

    const float c =
        elevations[
          maxIndex + 1
        ];

    const float curvature =
        a -
        2.0f * b +
        c;

    if (
      curvature <
      -0.0001f
    ) {
      const float vertexFraction =
          (a - c) /
          (
            2.0f *
            curvature
          );

      const float interpolatedPeak =
          b -
          (
            (a - c) *
            (a - c)
          ) /
          (
            8.0f *
            curvature
          );

      if (
        isfinite(
          vertexFraction
        ) &&
        vertexFraction >=
          -1.0f &&
        vertexFraction <=
          1.0f
      ) {
        peakFraction =
            vertexFraction;
      }

      if (
        isfinite(
          interpolatedPeak
        ) &&
        interpolatedPeak >
          predictedMaxEl &&
        interpolatedPeak <=
          90.0f
      ) {
        predictedMaxEl =
            interpolatedPeak;
      }
    }
  }

  const double maxOffsetSec =
      (
        (double)(
          (int32_t)maxIndex -
          (int32_t)ISS_ORBIT_NOW_INDEX
        ) +
        (double)peakFraction
      ) *
      (double)ISS_ORBIT_STEP_SEC;

  const time_t predictedMaxUTC =
      nowUTC +
      (time_t)lround(
        maxOffsetSec
      );

  // Find the future EL=0 crossing for the SET field.
  float previousEl =
      currentEl;

  int32_t previousOffsetSec =
      0;

  time_t predictedSet = 0;

  for (
    uint16_t i =
        ISS_ORBIT_NOW_INDEX + 1;
    i < ISS_ORBIT_POINT_COUNT;
    ++i
  ) {
    const int32_t offsetSec =
        (
          (int32_t)i -
          (int32_t)
          ISS_ORBIT_NOW_INDEX
        ) *
        ISS_ORBIT_STEP_SEC;

    const float elevation =
        elevations[i];

    if (
      previousEl > 0.0f &&
      elevation <= 0.0f
    ) {
      float fraction = 0.0f;

      const float denominator =
          previousEl -
          elevation;

      if (
        fabsf(denominator) >
        0.0001f
      ) {
        fraction =
            previousEl /
            denominator;
      }

      if (fraction < 0.0f)
        fraction = 0.0f;

      if (fraction > 1.0f)
        fraction = 1.0f;

      const double interpolatedOffset =
          (double)previousOffsetSec +
          (
            (double)(
              offsetSec -
              previousOffsetSec
            ) *
            fraction
          );

      predictedSet =
          nowUTC +
          (time_t)lround(
            interpolatedOffset
          );

      break;
    }

    previousEl =
        elevation;

    previousOffsetSec =
        offsetSec;
  }

  time_t predictedVisibleEndUTC =
      cachedVisibleEndUTC;

  if (visibleNow) {
    const bool needVisibleEndPrediction =
        predictedVisibleEndUTC <=
          nowUTC ||
        (
          predictedSet != 0 &&
          predictedVisibleEndUTC >
            predictedSet +
            30
        );

    if (needVisibleEndPrediction) {
      predictedVisibleEndUTC =
          predictISSCurrentVisibleEndUTC(
            nowUTC,
            predictedSet
          );
    }
  }
  else if (
    predictedVisibleEndUTC <=
      nowUTC
  ) {
    predictedVisibleEndUTC = 0;
  }

  portENTER_CRITICAL(
    &issRenderMux
  );

  issPredictedSetUTC =
      predictedSet;

  issCurrentPassMaxElevationDeg =
      predictedMaxEl;

  issCurrentPassMaxUTC =
      predictedMaxUTC;

  issCurrentPassVisibleEndUTC =
      predictedVisibleEndUTC;

  portEXIT_CRITICAL(
    &issRenderMux
  );
}


static void serviceISSPropagation(
  bool forceOrbitBuild
) {
  if (
    !appConfig.enableISS ||
    !haveISSTLE
  ) {
    return;
  }

  const time_t nowUTC =
      time(nullptr);

  if (nowUTC < 1700000000)
    return;

  float lat = 0.0f;
  float lon = 0.0f;

  ISSSpacePoint currentPoint;

  if (
    propagateISSAt(
      nowUTC,
      lat,
      lon,
      &currentPoint
    )
  ) {
    portENTER_CRITICAL(
      &issRenderMux
    );

    issCurrentXER =
        currentPoint.xER;

    issCurrentYER =
        currentPoint.yER;

    issCurrentZER =
        currentPoint.zER;

    issCurrentLatDeg =
        lat;

    issCurrentLonDeg =
        lon;

    issPositionValid =
        true;

    portEXIT_CRITICAL(
      &issRenderMux
    );

    updateISSObserverStatus(
      nowUTC
    );
  }

  const uint32_t nowMs =
      millis();

  if (
    forceOrbitBuild ||
    !issOrbitValid ||
    issLastOrbitBuildMillis == 0 ||
    nowMs -
      issLastOrbitBuildMillis >=
      ISS_ORBIT_REBUILD_INTERVAL_MS
  ) {
    if (
      rebuildISSOrbit(
        nowUTC
      )
    ) {
      issLastOrbitBuildMillis =
          nowMs;

      updateISSPredictedSet(
        nowUTC
      );

      Serial.printf(
        "[ISS] Position lat=%.3f lon=%.3f\n",
        (double)lat,
        (double)lon
      );
    }
  }
}

// -----------------------------------------------------------------------------
// Button / replay controls
// -----------------------------------------------------------------------------

static void publishButtonEvent(
  ButtonEvent event
) {
  if (
    event ==
    ButtonEvent::None
  ) {
    return;
  }

  portENTER_CRITICAL(
    &buttonCommandMux
  );

  // Gestures are rare. If a previous command somehow has not yet been
  // consumed, the newest deliberate gesture wins.
  pendingButtonEvent =
      (uint8_t)event;

  portEXIT_CRITICAL(
    &buttonCommandMux
  );
}

static ButtonEvent takeButtonEvent() {
  uint8_t raw =
      (uint8_t)
      ButtonEvent::None;

  portENTER_CRITICAL(
    &buttonCommandMux
  );

  raw =
      pendingButtonEvent;

  pendingButtonEvent =
      (uint8_t)
      ButtonEvent::None;

  portEXIT_CRITICAL(
    &buttonCommandMux
  );

  return
      (ButtonEvent)raw;
}

static void buttonTask(
  void *parameter
) {
  (void)parameter;

  bool rawLast =
      digitalRead(
        BUTTON_PIN
      ) == LOW;

  bool stablePressed =
      rawLast;

  uint32_t rawChangedMs =
      millis();

  uint32_t pressStartedMs =
      stablePressed ?
      millis() :
      0;

  uint32_t lastReleaseMs = 0;

  uint8_t clickCount = 0;
  bool longReported = false;

  while (true) {
    const uint32_t now =
        millis();

    const bool rawPressed =
        digitalRead(
          BUTTON_PIN
        ) == LOW;

    // Raw edge seen: restart debounce timer.
    if (
      rawPressed !=
      rawLast
    ) {
      rawLast =
          rawPressed;

      rawChangedMs =
          now;
    }

    // Accept a state transition only after the raw input has remained stable.
    if (
      rawPressed !=
        stablePressed &&
      now -
      rawChangedMs >=
        BUTTON_DEBOUNCE_MS
    ) {
      stablePressed =
          rawPressed;

      if (stablePressed) {
        pressStartedMs =
            now;

        longReported =
            false;

        // Every real button press also requests 30 seconds of the ISS forecast
        // page.  Cloud replay gesture recognition continues independently.
        portENTER_CRITICAL(
          &buttonCommandMux
        );

        pendingISSOLEDManualForecast =
            true;

        portEXIT_CRITICAL(
          &buttonCommandMux
        );

        Serial.println(
          "[BUTTON] ISS forecast OLED requested"
        );
      }
      else {
        // A released short press becomes one click.
        if (!longReported) {
          if (clickCount < 3)
            ++clickCount;

          buttonPreviewClicks =
              clickCount;

          lastReleaseMs =
              now;

          Serial.printf(
            "[BUTTON] registered click %u\n",
            (unsigned)clickCount
          );

          // Three is the maximum gesture, so there is no reason to wait for
          // another timeout after the third accepted release.
          if (clickCount >= 3) {
            publishButtonEvent(
              ButtonEvent::Triple
            );

            Serial.println(
              "[BUTTON] gesture = 3 clicks"
            );

            clickCount = 0;
            buttonPreviewClicks = 0;
          }
        }
      }
    }

    // Long press is generated while held, independently of any prior clicks.
    if (
      stablePressed &&
      !longReported &&
      now -
      pressStartedMs >=
        BUTTON_LONG_PRESS_MS
    ) {
      longReported = true;
      clickCount = 0;
      buttonPreviewClicks = 0;

      Serial.println(
        "[BUTTON] gesture = long press"
      );

      publishButtonEvent(
        ButtonEvent::LongPress
      );
    }

    // Finalise a 1- or 2-click sequence only while the physical input is
    // released. This prevents a second press from being misread as a completed
    // single-click during its debounce interval.
    if (
      !rawPressed &&
      !stablePressed &&
      clickCount > 0 &&
      now -
      lastReleaseMs >=
        BUTTON_MULTI_CLICK_MS
    ) {
      ButtonEvent event =
          ButtonEvent::Single;

      if (clickCount == 2)
        event = ButtonEvent::Double;

      Serial.printf(
        "[BUTTON] gesture = %u click%s\n",
        (unsigned)clickCount,
        clickCount == 1 ?
          "" :
          "s"
      );

      publishButtonEvent(
        event
      );

      clickCount = 0;
      buttonPreviewClicks = 0;
    }

    vTaskDelay(
      pdMS_TO_TICKS(
        BUTTON_POLL_MS
      )
    );
  }
}

static const char *replayModeName(
  ReplayMode mode
) {
  switch (mode) {
    case ReplayMode::Hours24:
      return "24H";

    case ReplayMode::Days7:
      return "7D";

    case ReplayMode::Days30:
      return "30D";

    default:
      return "LIVE";
  }
}

static void configureReplay(
  ReplayMode mode
) {
  const time_t now =
      time(nullptr);

  replayMode = mode;
  replayEndUTC = now;

  uint32_t windowSeconds;

  switch (mode) {
    case ReplayMode::Hours24:
      windowSeconds =
          24UL * 60UL * 60UL;

      // GEOS_IRX official availability is hourly.
      replayStepSeconds =
          60UL * 60UL;

      // About 24 real frames -> roughly 12 seconds for a full day.
      replayFrameIntervalMs =
          500;
      break;

    case ReplayMode::Days7:
      windowSeconds =
          7UL * 24UL * 60UL * 60UL;

      replayStepSeconds =
          60UL * 60UL;

      replayFrameIntervalMs =
          90;
      break;

    case ReplayMode::Days30:
      windowSeconds =
          30UL * 24UL * 60UL * 60UL;

      replayStepSeconds =
          3UL * 60UL * 60UL;

      replayFrameIntervalMs =
          70;
      break;

    default:
      return;
  }

  replayCursorUTC =
      now -
      (time_t)windowSeconds;

  replayCursorUTC -=
      replayCursorUTC %
      (time_t)replayStepSeconds;

  replayDisplayUTC =
      replayCursorUTC;

  replayNextFrameMs =
      0;

  const bool wasReplayActive =
      replayActive;

  // Capture the genuine live frame only when entering replay from live mode.
  // A pending start is still live because no archive frame has replaced it.
  if (
    !wasReplayActive &&
    !replayStartPending
  ) {
    if (cloudMutex)
      xSemaphoreTake(
        cloudMutex,
        portMAX_DELAY
      );

    strncpy(
      replaySavedLiveStamp,
      currentIRTime,
      12
    );

    replaySavedLiveStamp[12] =
        '\0';

    if (cloudMutex)
      xSemaphoreGive(
        cloudMutex
      );
  }

  replayStartPending = true;

  Serial.printf(
    "[REPLAY] %s requested; finding first archive frame\n",
    replayModeName(mode)
  );
}

static void finishReplay() {
  if (
    !replayActive &&
    !replayStartPending
  ) {
    return;
  }

  if (
    replayStartPending &&
    !replayActive
  ) {
    replayStartPending = false;
    replayMode = ReplayMode::Live;

    Serial.println(
      "[REPLAY] Pending replay cancelled"
    );

    return;
  }

  // Keep replayActive true while restoring so the background weather task
  // cannot race us for the candidate buffers / PNG decoder.
  if (
    replaySavedLiveStamp[0] !=
    '\0'
  ) {
    if (
      workMutex &&
      xSemaphoreTake(
        workMutex,
        pdMS_TO_TICKS(4000)
      ) == pdTRUE
    ) {
      loadArchivedFrame(
        replaySavedLiveStamp,
        true
      );

      xSemaphoreGive(
        workMutex
      );
    }
  }

  // Resume normal rotation from the exact longitude that replay was
  // displaying.  Reset the integration timestamp so replay duration is not
  // accidentally added as a large delta on the next loop.
  liveLongitudeDeg =
      replayFixedLongitudeDeg;

  liveSpinLastMs =
      millis();

  liveSpinInitialised =
      true;

  replayMode = ReplayMode::Live;
  replayStartPending = false;
  replayActive = false;

  Serial.printf(
    "[REPLAY] Return to live at lon=%.2f\n",
    (double)liveLongitudeDeg
  );
}

static void serviceReplay() {
  if (
    !replayActive &&
    !replayStartPending
  ) {
    return;
  }

  const uint32_t nowMs =
      millis();

  if (
    replayActive &&
    replayNextFrameMs != 0 &&
    (int32_t)(
      nowMs -
      replayNextFrameMs
    ) < 0
  ) {
    return;
  }

  // Never block the renderer.  While start is pending, the globe continues
  // rotating until the archive/PNG lock is actually free.
  if (
    workMutex &&
    xSemaphoreTake(
      workMutex,
      0
    ) != pdTRUE
  ) {
    return;
  }

  bool loaded = false;
  char loadedStamp[13] = "";

  // Search the requested period for the next file actually present on SD.
  // 192 attempts is enough for the full 7-day hourly replay in one pass.
  for (
    uint16_t tries = 0;
    tries < 192 &&
    replayCursorUTC <=
      replayEndUTC;
    ++tries
  ) {
    char stamp[13];

    formatUTCStamp(
      replayCursorUTC,
      stamp
    );

    const time_t frameTime =
        replayCursorUTC;

    replayCursorUTC +=
        (time_t)
        replayStepSeconds;

    if (
      !archiveExists(
        stamp
      )
    ) {
      continue;
    }

    Serial.printf(
      "[REPLAY] Loading %s UTC from SD\n",
      stamp
    );

    if (
      loadArchivedFrame(
        stamp,
        true
      )
    ) {
      replayDisplayUTC =
          frameTime;

      strncpy(
        loadedStamp,
        stamp,
        12
      );

      loadedStamp[12] =
          '\0';

      loaded = true;
      break;
    }

    Serial.printf(
      "[REPLAY] Failed to decode %s; skipping\n",
      stamp
    );
  }

  if (workMutex)
    xSemaphoreGive(
      workMutex
    );

  if (loaded) {
    // Only now does fixed-view replay begin.
    if (replayStartPending) {
      // Freeze exactly where the live globe is currently being viewed.
      // This avoids a small jump back to the longitude from the button press
      // if archive acquisition spent some time in WAIT.
      replayFixedLongitudeDeg =
          liveLongitudeDeg;

      replayStartPending = false;
      replayActive = true;

      Serial.printf(
        "[REPLAY] %s started at %s UTC, lon=%.2f\n",
        replayModeName(
          replayMode
        ),
        loadedStamp,
        (double)replayFixedLongitudeDeg
      );
    }

    replayNextFrameMs =
        millis() +
        replayFrameIntervalMs;

    return;
  }

  if (
    replayCursorUTC >
      replayEndUTC
  ) {
    if (replayStartPending) {
      replayStartPending = false;
      replayMode = ReplayMode::Live;

      Serial.println(
        "[REPLAY] No archived frames found; staying live"
      );
    }
    else {
      finishReplay();
    }

    return;
  }

  replayNextFrameMs = 0;
}

// -----------------------------------------------------------------------------
// Background weather archive task
// -----------------------------------------------------------------------------

static void weatherTask(void *parameter) {
  (void)parameter;

  // Force an immediate official availability refresh after the globe appears.
  lastUpdateMillis =
      millis() -
      UPDATE_INTERVAL_MS;

  lastISSServiceMillis =
      millis();

  lastISSPropagationMillis =
      millis() -
      ISS_POSITION_INTERVAL_MS;

  // Do not make the first upcoming-pass forecast wait for the normal 60-second
  // service cadence.  As soon as UTC/TLE/observer data are valid, the loop
  // below will calculate it before any archive gap repair work.
  lastISSUpcomingForecastServiceMs = 0;

  // Build the initial current position + orbit as soon as the background task
  // starts. No network access occurs here.
  if (appConfig.enableISS) {
    serviceISSPropagation(
      true
    );
  }

  while (true) {
    if (
      replayActive ||
      replayStartPending
    ) {
      vTaskDelay(
        pdMS_TO_TICKS(50)
      );

      continue;
    }

    const uint32_t nowMs =
        millis();

    // Highest-priority background job: publish the first NEXT ISS forecast as
    // soon as UTC is valid, then service forecast retirement/refresh at the
    // normal cadence.  A missing forecast must never sit behind SD gap repair.
    if (
      appConfig.enableISS &&
      appConfig.observerConfigured &&
      haveISSTLE &&
      time(nullptr) >= 1700000000 &&
      (
        !issUpcomingForecastValid ||
        nowMs -
          lastISSUpcomingForecastServiceMs >=
          ISS_UPCOMING_FORECAST_SERVICE_MS
      )
    ) {
      lastISSUpcomingForecastServiceMs =
          nowMs;

      serviceISSUpcomingForecast();
    }

    if (
      appConfig.enableISS &&
      nowMs -
      lastISSServiceMillis >=
        ISS_SERVICE_INTERVAL_MS
    ) {
      lastISSServiceMillis =
          nowMs;

      if (
        !replayActive &&
        !replayStartPending &&
        workMutex &&
        xSemaphoreTake(
          workMutex,
          portMAX_DELAY
        ) == pdTRUE
      ) {
        serviceISSTLE(false);

        xSemaphoreGive(
          workMutex
        );
      }
    }

    if (
      appConfig.enableISS &&
      nowMs -
      lastISSPropagationMillis >=
        ISS_POSITION_INTERVAL_MS
    ) {
      lastISSPropagationMillis =
          nowMs;

      serviceISSPropagation(
        false
      );
    }

    if (
      appConfig.enableClouds &&
      nowMs -
      lastUpdateMillis >=
        UPDATE_INTERVAL_MS
    ) {
      lastUpdateMillis =
          nowMs;

      bool availabilityOK = false;

      // Phase 1: tiny official availability request + newest live frame.
      liveFetchBusy = true;
      httpBodyBytes = 0;
      httpBodyExpected = -1;

      if (
        workMutex &&
        xSemaphoreTake(
          workMutex,
          portMAX_DELAY
        ) == pdTRUE
      ) {
        // Try the network before allocating/using the PNG decoder.  TLS on the
        // ESP32-S2 is constrained by internal DRAM; the SD bootstrap remains the
        // fallback, but should not consume resources before the first handshake.
        availabilityOK =
            fetchNSMCAvailability();

        if (availabilityOK) {
          activateNewestAvailableFrame();
        }
        else {
          fetchLatestNSMCIRFallbackHourly();
        }

        xSemaphoreGive(
          workMutex
        );
      }

      liveFetchBusy = false;
      httpBodyBytes = 0;
      httpBodyExpected = -1;

      // Phase 2 is now only QUEUED here.  The worker repairs one missing
      // frame per later turn through the loop, with ISS forecast checks between
      // every frame, so a large SD backlog can never monopolize this task.
      if (
        availabilityOK &&
        sdReady &&
        !replayActive &&
        !replayStartPending
      ) {
        startArchiveGapRepairPass();
      }

      vTaskDelay(
        pdMS_TO_TICKS(250)
      );

      continue;
    }

    // Lowest-priority background work: repair at most one historical frame on
    // this loop turn.  The next turn begins again with the ISS forecast check.
    if (
      gapRepairPassPending &&
      !replayActive &&
      !replayStartPending
    ) {
      repairOneArchiveGapFromAvailability();
    }

    vTaskDelay(
      pdMS_TO_TICKS(250)
    );
  }
}

// =============================================================================
// Globe renderer
// =============================================================================

// The time/date/status overlay is composed into the same framebuffer as the
// Earth.  This avoids the visible erase/redraw flicker caused by drawing text
// directly to the TFT after each full-screen frame transfer.
static void composeCornerOverlay(
  uint16_t *frameBuffer
);

namespace Globe {

constexpr int16_t SCREEN_W = 240;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t CX = 120;
constexpr int16_t CY = 120;
constexpr int16_t RADIUS = 112;
constexpr int16_t DIAMETER = RADIUS * 2;
constexpr float SPIN_DEG_PER_SEC = 15.0f;

constexpr uint8_t LIMB_R = 82;
constexpr uint8_t LIMB_G = 135;
constexpr uint8_t LIMB_B = 170;

static uint16_t *frameBuffer = nullptr;

constexpr size_t FRAME_PIXELS =
    (size_t)SCREEN_W *
    (size_t)SCREEN_H;

constexpr size_t FRAME_BYTES =
    FRAME_PIXELS *
    sizeof(uint16_t);

static uint8_t *shadeLUT = nullptr;

// Geographic surface-normal lookup tables in Q14 fixed point.
//
// Each displayed globe pixel already has an exact geographic latitude row and
// longitude column from the same reverse-projection map used for Earth/clouds.
// Use that geography directly for solar illumination.
static int16_t latitudeSinQ14[EARTH_H];
static int16_t latitudeCosQ14[EARTH_H];
static int16_t longitudeSinQ14[EARTH_W];
static int16_t longitudeCosQ14[EARTH_W];

constexpr int32_t NORMAL_ONE_Q14 = 16384;

// Stronger night level so the day/night split is unmistakable on hardware.
constexpr uint8_t NIGHT_MIN_BRIGHTNESS = 36;

// Soft civil-twilight-like transition.
constexpr int16_t TWILIGHT_NIGHT_Q14 = -1713; // sin(-6 deg) * 16384
constexpr int16_t TWILIGHT_DAY_Q14 = 0;

static P13Sun dayNightSun;
static time_t cachedSunKeyUTC = 0;
static bool cachedSunValid = false;

static double cachedSunEarthX = 0.0;
static double cachedSunEarthY = 0.0;
static double cachedSunEarthZ = 0.0;
static bool dayNightFirstLogDone = false;

static bool updateDayNightSun(
  time_t displayUTC
) {
  if (
    !appConfig.enableDayNight ||
    displayUTC < 1700000000
  ) {
    return false;
  }

  // Live view only needs a new solar solution every 30 seconds. During replay,
  // use the exact archived weather timestamp so the terminator travels through
  // the historical sequence correctly.
  const time_t keyUTC =
      replayActive ?
      displayUTC :
      (
        displayUTC -
        displayUTC % 30
      );

  if (
    cachedSunValid &&
    keyUTC ==
      cachedSunKeyUTC
  ) {
    return true;
  }

  struct tm t = {};

  if (
    !gmtime_r(
      &keyUTC,
      &t
    )
  ) {
    return false;
  }

  P13DateTime dt(
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  );

  dayNightSun.predict(
    dt
  );

  cachedSunEarthX =
      dayNightSun.c_vecH[0];

  cachedSunEarthY =
      dayNightSun.c_vecH[1];

  cachedSunEarthZ =
      dayNightSun.c_vecH[2];

  cachedSunKeyUTC =
      keyUTC;

  cachedSunValid =
      true;

  if (!dayNightFirstLogDone) {
    dayNightFirstLogDone = true;

    double sunLat = 0.0;
    double sunLon = 0.0;

    dayNightSun.latlon(
      sunLat,
      sunLon
    );

    Serial.printf(
      "[DAY/NIGHT] Sun lat=%.2f lon=%.2f UTC=%lld\n",
      sunLat,
      sunLon,
      (long long)keyUTC
    );
  }

  return true;
}

static bool getSunEarthQ14(
  int16_t &sunXQ14,
  int16_t &sunYQ14,
  int16_t &sunZQ14
) {
  const time_t displayUTC =
      replayActive ?
      replayDisplayUTC :
      time(nullptr);

  if (
    !updateDayNightSun(
      displayUTC
    )
  ) {
    return false;
  }

  sunXQ14 =
      (int16_t)lround(
        cachedSunEarthX *
        NORMAL_ONE_Q14
      );

  sunYQ14 =
      (int16_t)lround(
        cachedSunEarthY *
        NORMAL_ONE_Q14
      );

  sunZQ14 =
      (int16_t)lround(
        cachedSunEarthZ *
        NORMAL_ONE_Q14
      );

  return true;
}

static uint8_t dayNightBrightness(
  uint16_t row,
  uint16_t col,
  int16_t sunXQ14,
  int16_t sunYQ14,
  int16_t sunZQ14
) {
  const int16_t sinLat =
      latitudeSinQ14[row];

  const int16_t cosLat =
      latitudeCosQ14[row];

  const int16_t sinLon =
      longitudeSinQ14[col];

  const int16_t cosLon =
      longitudeCosQ14[col];

  const int16_t nx =
      (int16_t)(
        (
          (int32_t)cosLat *
          cosLon
        ) >> 14
      );

  const int16_t ny =
      (int16_t)(
        (
          (int32_t)cosLat *
          sinLon
        ) >> 14
      );

  const int16_t nz =
      sinLat;

  const int32_t dotQ28 =
      (int32_t)nx *
      (int32_t)sunXQ14 +
      (int32_t)ny *
      (int32_t)sunYQ14 +
      (int32_t)nz *
      (int32_t)sunZQ14;

  const int16_t dotQ14 =
      (int16_t)(
        dotQ28 >> 14
      );

  if (dotQ14 >= TWILIGHT_DAY_Q14)
    return 255;

  if (dotQ14 <= TWILIGHT_NIGHT_Q14)
    return NIGHT_MIN_BRIGHTNESS;

  const int32_t numerator =
      (
        (int32_t)dotQ14 -
        TWILIGHT_NIGHT_Q14
      ) *
      (
        255 -
        NIGHT_MIN_BRIGHTNESS
      );

  const int32_t denominator =
      TWILIGHT_DAY_Q14 -
      TWILIGHT_NIGHT_Q14;

  return
      (uint8_t)(
        NIGHT_MIN_BRIGHTNESS +
        numerator /
        denominator
      );
}

static inline uint16_t rgb565(
  uint8_t r,
  uint8_t g,
  uint8_t b
) {
  return
      ((uint16_t)(r & 0xF8) << 8) |
      ((uint16_t)(g & 0xFC) << 3) |
      ((uint16_t)b >> 3);
}

static inline void unpack565(
  uint16_t c,
  uint8_t &r,
  uint8_t &g,
  uint8_t &b
) {
  r =
      (uint8_t)(
        ((c >> 11) & 0x1F) *
        255 / 31
      );

  g =
      (uint8_t)(
        ((c >> 5) & 0x3F) *
        255 / 63
      );

  b =
      (uint8_t)(
        (c & 0x1F) *
        255 / 31
      );
}

static inline uint8_t shade8(
  uint8_t component,
  uint8_t brightness
) {
  return
      (uint8_t)(
        ((uint16_t)component *
         (uint16_t)brightness) >>
        8
      );
}

static inline uint8_t blend8(
  uint8_t base,
  uint8_t cloud,
  uint8_t alpha
) {
  const int16_t delta =
      (int16_t)cloud -
      (int16_t)base;

  return
      (uint8_t)(
        (int16_t)base +
        ((delta *
          (int16_t)alpha) >>
         8)
      );
}

static inline void makeCloudTintFromBase(
  uint8_t baseR,
  uint8_t baseG,
  uint8_t baseB,
  uint8_t &cloudR,
  uint8_t &cloudG,
  uint8_t &cloudB
) {
  // CLOUD_WHITENING controls how far the cloud target colour moves from the
  // shaded land/sea pixel underneath toward white.
  //
  // 0   = cloud target stays the underlying surface colour
  // 255 = cloud target is essentially white
  //
  // Opacity/transparency structure still comes from the real NSMC alpha/luma
  // data; this parameter only controls how bright/white dense cloud becomes.
  cloudR =
      (uint8_t)(
        baseR +
        (((uint16_t)(252 - baseR) *
          CLOUD_WHITENING) >> 8)
      );

  cloudG =
      (uint8_t)(
        baseG +
        (((uint16_t)(253 - baseG) *
          CLOUD_WHITENING) >> 8)
      );

  cloudB =
      (uint8_t)(
        baseB +
        (((uint16_t)(255 - baseB) *
          CLOUD_WHITENING) >> 8)
      );
}

static inline void fbPixel(
  int16_t x,
  int16_t y,
  uint16_t colour
) {
  if (
    (uint16_t)x >= SCREEN_W ||
    (uint16_t)y >= SCREEN_H
  ) {
    return;
  }

  frameBuffer[
    (size_t)y *
    SCREEN_W +
    (size_t)x
  ] = colour;
}

static void fbCircle(
  int16_t cx,
  int16_t cy,
  int16_t radius,
  uint16_t colour
) {
  int16_t x = radius;
  int16_t y = 0;
  int16_t err = 1 - radius;

  while (x >= y) {
    fbPixel(cx + x, cy + y, colour);
    fbPixel(cx + y, cy + x, colour);
    fbPixel(cx - y, cy + x, colour);
    fbPixel(cx - x, cy + y, colour);
    fbPixel(cx - x, cy - y, colour);
    fbPixel(cx - y, cy - x, colour);
    fbPixel(cx + y, cy - x, colour);
    fbPixel(cx + x, cy - y, colour);

    ++y;

    if (err < 0) {
      err +=
          2 * y + 1;
    }
    else {
      --x;

      err +=
          2 * (y - x + 1);
    }
  }
}


static void fbLinePattern(
  int16_t x0,
  int16_t y0,
  int16_t x1,
  int16_t y1,
  uint16_t colour,
  bool dashed
) {
  int16_t dx =
      abs(
        x1 - x0
      );

  int16_t sx =
      x0 < x1 ?
      1 :
      -1;

  int16_t dy =
      -abs(
        y1 - y0
      );

  int16_t sy =
      y0 < y1 ?
      1 :
      -1;

  int16_t err =
      dx + dy;

  uint16_t step = 0;

  while (true) {
    // Dashed track uses a chunky 6-on / 3-off pattern so the visible
    // segments remain easy to follow through bright cloud imagery.
    const bool drawPixel =
        !dashed ||
        (
          step %
          9u
        ) < 6u;

    if (drawPixel) {
      fbPixel(
        x0,
        y0,
        colour
      );
    }

    if (
      x0 == x1 &&
      y0 == y1
    ) {
      break;
    }

    const int16_t e2 =
        2 * err;

    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }

    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }

    ++step;
  }
}

struct OrbitProjection {
  float screenX;
  float screenY;

  // Camera-space distance toward the viewer, in Earth-radius units.
  float cameraZ;

  // Squared projected radius from globe centre, in Earth-radius units.
  float projectedR2;

  bool visible;
};

static OrbitProjection projectISSSpace(
  const ISSSpacePoint &point,
  float centreLonDeg
) {
  constexpr float VIEW_LAT_DEG =
      10.0f;

  const float centreLon =
      radians(
        centreLonDeg
      );

  const float sinCentre =
      sinf(
        centreLon
      );

  const float cosCentre =
      cosf(
        centreLon
      );

  // Rotate the Earth-fixed point so centreLonDeg is at the middle of the
  // display.  This is the 3D equivalent of lon - centreLonDeg.
  const float equatorialFront =
      point.xER *
      cosCentre +
      point.yER *
      sinCentre;

  const float cameraX =
      point.yER *
      cosCentre -
      point.xER *
      sinCentre;

  const float viewLat =
      radians(
        VIEW_LAT_DEG
      );

  const float sinView =
      sinf(
        viewLat
      );

  const float cosView =
      cosf(
        viewLat
      );

  const float cameraY =
      cosView *
      point.zER -
      sinView *
      equatorialFront;

  const float cameraZ =
      sinView *
      point.zER +
      cosView *
      equatorialFront;

  const float projectedR2 =
      cameraX *
      cameraX +
      cameraY *
      cameraY;

  OrbitProjection p;

  p.screenX =
      (float)CX +
      (float)RADIUS *
      cameraX;

  p.screenY =
      (float)CY -
      (float)RADIUS *
      cameraY;

  p.cameraZ =
      cameraZ;

  p.projectedR2 =
      projectedR2;

  // Orthographic occultation by the Earth:
  //
  //   front of Earth -> visible
  //   behind Earth but projected outside the Earth disc -> visible
  //   behind Earth and projected onto the Earth disc -> hidden
  //
  // This is what allows the real ISS orbit to remain visible just outside
  // the limb even while the spacecraft is geometrically behind the globe.
  p.visible =
      cameraZ >= 0.0f ||
      projectedR2 > 1.0f;

  return p;
}

static ISSSpacePoint interpolateSpace(
  const ISSSpacePoint &a,
  const ISSSpacePoint &b,
  float t
) {
  ISSSpacePoint out;

  out.xER =
      a.xER +
      (
        b.xER -
        a.xER
      ) *
      t;

  out.yER =
      a.yER +
      (
        b.yER -
        a.yER
      ) *
      t;

  out.zER =
      a.zER +
      (
        b.zER -
        a.zER
      ) *
      t;

  return out;
}

static ISSSpacePoint findOccultationBoundary(
  const ISSSpacePoint &visiblePoint,
  const ISSSpacePoint &hiddenPoint,
  float centreLonDeg
) {
  ISSSpacePoint lo =
      visiblePoint;

  ISSSpacePoint hi =
      hiddenPoint;

  // Ten bisections locate the Earth-occultation boundary far more precisely
  // than one display pixel at 240x240.
  for (uint8_t i = 0; i < 10; ++i) {
    const ISSSpacePoint mid =
        interpolateSpace(
          lo,
          hi,
          0.5f
        );

    const OrbitProjection p =
        projectISSSpace(
          mid,
          centreLonDeg
        );

    if (p.visible)
      lo = mid;
    else
      hi = mid;
  }

  return lo;
}

static void drawISSOrbit(
  float centreLonDeg
) {
  if (
    !appConfig.enableISS ||
    replayActive ||
    !issOrbitValid
  ) {
    return;
  }

  uint8_t activeBuffer;

  portENTER_CRITICAL(
    &issRenderMux
  );

  activeBuffer =
      issOrbitActiveBuffer;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  const ISSSpacePoint *points =
      issOrbitBuffers[
        activeBuffer
      ];

  // Past: warm amber/orange, solid.
  // Using a different hue rather than merely a brightness change makes
  // past/future immediately distinguishable even over cloud imagery.
  const uint16_t pastColour =
      rgb565(
        255,
        150,
        35
      );

  // Future: bright cyan, dashed.
  const uint16_t futureColour =
      rgb565(
        75,
        235,
        255
      );

  for (
    uint16_t i = 1;
    i < ISS_ORBIT_POINT_COUNT;
    ++i
  ) {
    ISSSpacePoint a =
        points[i - 1];

    ISSSpacePoint b =
        points[i];

    OrbitProjection pa =
        projectISSSpace(
          a,
          centreLonDeg
        );

    OrbitProjection pb =
        projectISSSpace(
          b,
          centreLonDeg
        );

    if (
      !pa.visible &&
      !pb.visible
    ) {
      continue;
    }

    if (
      pa.visible !=
      pb.visible
    ) {
      if (pa.visible) {
        b =
            findOccultationBoundary(
              a,
              b,
              centreLonDeg
            );

        pb =
            projectISSSpace(
              b,
              centreLonDeg
            );
      }
      else {
        a =
            findOccultationBoundary(
              b,
              a,
              centreLonDeg
            );

        pa =
            projectISSSpace(
              a,
              centreLonDeg
            );
      }
    }

    // Segment i joins points i-1 -> i.
    // The central point is "now":
    //
    //   segments ending at/before NOW_INDEX = past, solid
    //   segments after NOW_INDEX            = future, dashed
    const bool past =
        i <=
        ISS_ORBIT_NOW_INDEX;

    fbLinePattern(
      (int16_t)lroundf(
        pa.screenX
      ),
      (int16_t)lroundf(
        pa.screenY
      ),
      (int16_t)lroundf(
        pb.screenX
      ),
      (int16_t)lroundf(
        pb.screenY
      ),
      past ?
        pastColour :
        futureColour,
      !past
    );
  }
}

static void drawISSMarker(
  float centreLonDeg
) {
  if (
    !appConfig.enableISS ||
    replayActive
  ) {
    return;
  }

  bool valid;
  ISSSpacePoint current;

  portENTER_CRITICAL(
    &issRenderMux
  );

  valid =
      issPositionValid;

  current.xER =
      issCurrentXER;

  current.yER =
      issCurrentYER;

  current.zER =
      issCurrentZER;

  portEXIT_CRITICAL(
    &issRenderMux
  );

  if (!valid)
    return;

  const OrbitProjection p =
      projectISSSpace(
        current,
        centreLonDeg
      );

  if (!p.visible)
    return;

  const int16_t x =
      (int16_t)lroundf(
        p.screenX
      );

  const int16_t y =
      (int16_t)lroundf(
        p.screenY
      );

  const uint16_t outer =
      rgb565(
        255,
        190,
        32
      );

  const uint16_t centre =
      rgb565(
        255,
        255,
        255
      );

  // Current ISS marker is drawn at its true scaled orbital altitude.
  // One step larger than the previous version, but still compact relative
  // to the 240x240 globe.
  for (int8_t d = -5; d <= 5; ++d) {
    fbPixel(x + d, y,     outer);
    fbPixel(x,     y + d, outer);
  }

  // Diagonal shoulders make it read as a compact spacecraft-like glyph.
  fbPixel(x - 3, y - 3, outer);
  fbPixel(x + 3, y - 3, outer);
  fbPixel(x - 3, y + 3, outer);
  fbPixel(x + 3, y + 3, outer);

  // Bright 3x3 centre.
  for (int8_t yy = -1; yy <= 1; ++yy) {
    for (int8_t xx = -1; xx <= 1; ++xx) {
      fbPixel(
        x + xx,
        y + yy,
        centre
      );
    }
  }
}

static void buildShadeLUT() {
  if (!shadeLUT) {
    shadeLUT =
        (uint8_t *)allocPSRAMPreferred(
          256
        );

    if (!shadeLUT) {
      Serial.println(
        "[MEM] shade LUT allocation failed"
      );
      return;
    }
  }

  // Existing globe limb shading.
  for (uint16_t i = 0; i < 256; ++i) {
    const float r2 =
        (float)i /
        255.0f;

    const float z =
        sqrtf(
          fmaxf(
            0.0f,
            1.0f - r2
          )
        );

    const float brightness =
        0.72f +
        0.28f * z;

    shadeLUT[i] =
        (uint8_t)lroundf(
          brightness *
          255.0f
        );
  }

  // Geographic latitude row lookup.
  // Row 0 is near +90 deg; row 128 is near the equator.
  for (
    uint16_t row = 0;
    row < EARTH_H;
    ++row
  ) {
    const float latDeg =
        90.0f -
        (
          (
            (float)row +
            0.5f
          ) *
          180.0f /
          (float)EARTH_H
        );

    const float latRad =
        radians(latDeg);

    latitudeSinQ14[row] =
        (int16_t)lroundf(
          sinf(latRad) *
          NORMAL_ONE_Q14
        );

    latitudeCosQ14[row] =
        (int16_t)lroundf(
          cosf(latRad) *
          NORMAL_ONE_Q14
        );
  }

  // Geographic longitude column lookup.
  // The reverse map puts 0 deg longitude at column 256.
  for (
    uint16_t col = 0;
    col < EARTH_W;
    ++col
  ) {
    const float lonDeg =
        (
          (
            (float)col +
            0.5f
          ) -
          (float)EARTH_W /
          2.0f
        ) *
        360.0f /
        (float)EARTH_W;

    const float lonRad =
        radians(lonDeg);

    longitudeSinQ14[col] =
        (int16_t)lroundf(
          sinf(lonRad) *
          NORMAL_ONE_Q14
        );

    longitudeCosQ14[col] =
        (int16_t)lroundf(
          cosf(lonRad) *
          NORMAL_ONE_Q14
        );
  }
}

// Bilinear sample of the 320x160 real IR image using a 512x256 geographic
// coordinate.  Both Earth and clouds therefore share the SAME longitude.
static uint8_t sampleIRLuma(
  uint16_t earthRow,
  uint16_t earthCol
) {
  if (!haveIRClouds)
    return 0;

  const uint32_t fx =
      (
        (uint32_t)earthCol *
        (uint32_t)(IR_W - 1)
        << 8
      ) /
      (uint32_t)(EARTH_W - 1);

  const uint32_t fy =
      (
        (uint32_t)earthRow *
        (uint32_t)(IR_H - 1)
        << 8
      ) /
      (uint32_t)(EARTH_H - 1);

  const uint16_t x0 =
      (uint16_t)(fx >> 8);

  const uint16_t y0 =
      (uint16_t)(fy >> 8);

  const uint16_t x1 =
      (x0 + 1 < IR_W) ?
      x0 + 1 :
      x0;

  const uint16_t y1 =
      (y0 + 1 < IR_H) ?
      y0 + 1 :
      y0;

  const uint16_t wx =
      (uint16_t)(fx & 0xFFu);

  const uint16_t wy =
      (uint16_t)(fy & 0xFFu);

  const uint8_t p00 =
      irLuma[
        (uint32_t)y0 *
        IR_W +
        x0
      ];

  const uint8_t p10 =
      irLuma[
        (uint32_t)y0 *
        IR_W +
        x1
      ];

  const uint8_t p01 =
      irLuma[
        (uint32_t)y1 *
        IR_W +
        x0
      ];

  const uint8_t p11 =
      irLuma[
        (uint32_t)y1 *
        IR_W +
        x1
      ];

  const uint16_t top =
      (
        (uint16_t)p00 *
        (256u - wx) +
        (uint16_t)p10 *
        wx
      ) >> 8;

  const uint16_t bottom =
      (
        (uint16_t)p01 *
        (256u - wx) +
        (uint16_t)p11 *
        wx
      ) >> 8;

  return
      (uint8_t)(
        (
          top *
          (256u - wy) +
          bottom *
          wy
        ) >> 8
      );
}


static uint8_t sampleIRAlpha(
  uint16_t earthRow,
  uint16_t earthCol
) {
  if (!haveIRClouds)
    return 0;

  const uint32_t fx =
      (
        (uint32_t)earthCol *
        (uint32_t)(IR_W - 1)
        << 8
      ) /
      (uint32_t)(EARTH_W - 1);

  const uint32_t fy =
      (
        (uint32_t)earthRow *
        (uint32_t)(IR_H - 1)
        << 8
      ) /
      (uint32_t)(EARTH_H - 1);

  const uint16_t x0 =
      (uint16_t)(fx >> 8);

  const uint16_t y0 =
      (uint16_t)(fy >> 8);

  const uint16_t x1 =
      (x0 + 1 < IR_W) ?
      x0 + 1 :
      x0;

  const uint16_t y1 =
      (y0 + 1 < IR_H) ?
      y0 + 1 :
      y0;

  const uint16_t wx =
      (uint16_t)(fx & 0xFFu);

  const uint16_t wy =
      (uint16_t)(fy & 0xFFu);

  const uint8_t p00 =
      irAlpha[
        (uint32_t)y0 * IR_W + x0
      ];

  const uint8_t p10 =
      irAlpha[
        (uint32_t)y0 * IR_W + x1
      ];

  const uint8_t p01 =
      irAlpha[
        (uint32_t)y1 * IR_W + x0
      ];

  const uint8_t p11 =
      irAlpha[
        (uint32_t)y1 * IR_W + x1
      ];

  const uint16_t top =
      (
        (uint16_t)p00 * (256u - wx) +
        (uint16_t)p10 * wx
      ) >> 8;

  const uint16_t bottom =
      (
        (uint16_t)p01 * (256u - wx) +
        (uint16_t)p11 * wx
      ) >> 8;

  return
      (uint8_t)(
        (
          top * (256u - wy) +
          bottom * wy
        ) >> 8
      );
}

static void drawEarthAndClouds(
  float centreLonDeg
) {
  if (
    appConfig.enableClouds &&
    cloudMutex
  ) {
    xSemaphoreTake(
      cloudMutex,
      portMAX_DELAY
    );
  }

  int16_t sunXQ14 = 0;
  int16_t sunYQ14 = 0;
  int16_t sunZQ14 = 0;

  const bool haveDayNight =
      appConfig.enableDayNight &&
      getSunEarthQ14(
        sunXQ14,
        sunYQ14,
        sunZQ14
      );

  int32_t lonShift =
      (int32_t)lroundf(
        centreLonDeg *
        ((float)EARTH_W /
         360.0f)
      );

  lonShift &=
      (EARTH_W - 1);

  const int16_t globeLeft =
      CX - RADIUS;

  const int16_t globeTop =
      CY - RADIUS;

  const int32_t RR =
      (int32_t)RADIUS *
      (int32_t)RADIUS;

  for (
    int16_t y = 0;
    y < SCREEN_H;
    ++y
  ) {
    uint16_t *dst =
        frameBuffer +
        (size_t)y *
        SCREEN_W;

    for (
      int16_t x = 0;
      x < SCREEN_W;
      ++x
    ) {
      uint16_t pixel =
          ST77XX_BLACK;

      const int16_t gx =
          x - globeLeft;

      const int16_t gy =
          y - globeTop;

      if (
        gx >= 0 &&
        gx < DIAMETER &&
        gy >= 0 &&
        gy < DIAMETER
      ) {
        const int16_t dx =
            x - CX;

        const int16_t dy =
            y - CY;

        const int32_t r2 =
            (int32_t)dx * dx +
            (int32_t)dy * dy;

        if (r2 <= RR) {
          const uint32_t mapIndex =
              (uint32_t)gy *
              (uint32_t)DIAMETER +
              (uint32_t)gx;

          const uint32_t packed =
              globeScreenCoordAt(
                mapIndex
              );

          const uint16_t row =
              (uint16_t)(
                (packed >> 9) &
                0x00FFu
              );

          const uint16_t relativeCol =
              (uint16_t)(
                packed &
                0x01FFu
              );

          const uint16_t col =
              (relativeCol +
               lonShift) &
              0x01FFu;

          uint8_t r;
          uint8_t g;
          uint8_t b;

          unpack565(
            earthTex565At(
              row,
              col
            ),
            r, g, b
          );

          uint16_t shadeIndex =
              (uint16_t)(
                (r2 * 255L) /
                RR
              );

          if (shadeIndex > 255)
            shadeIndex = 255;

          const uint8_t brightness =
              shadeLUT[
                shadeIndex
              ];

          r = shade8(r, brightness);
          g = shade8(g, brightness);
          b = shade8(b, brightness);

          if (appConfig.enableClouds) {
            const uint8_t ir =
                sampleIRLuma(
                  row,
                  col
                );

            const uint8_t cloudA =
                sampleIRAlpha(
                  row,
                  col
                );

            uint8_t alpha = 0;

            if (cloudA != 0) {
              const uint16_t alphaBase =
                  (uint16_t)cloudA *
                  (uint16_t)globalCloudAlphaScale;

              const uint8_t lumaAlpha =
                  cloudOpacityLUT[
                    ir
                  ];

              alpha =
                  (uint8_t)(
                    (
                      alphaBase *
                      (uint16_t)lumaAlpha
                    ) >> 16
                  );
            }

            if (alpha != 0) {
              const uint16_t boosted =
                  (
                    (uint16_t)alpha *
                    CLOUD_OPACITY_GAIN_PERCENT
                  ) / 100u;

              alpha =
                  (uint8_t)(
                    boosted > 255u ?
                    255u :
                    boosted
                  );

              uint8_t cr, cg, cb;

              makeCloudTintFromBase(
                r, g, b,
                cr, cg, cb
              );

              r = blend8(r, cr, alpha);
              g = blend8(g, cg, alpha);
              b = blend8(b, cb, alpha);
            }
          }

          // Apply solar illumination AFTER clouds so the entire visible
          // atmosphere/terrain darkens together on the night hemisphere.
          if (haveDayNight) {
            const uint8_t solarBrightness =
                dayNightBrightness(
                  row,
                  col,
                  sunXQ14,
                  sunYQ14,
                  sunZQ14
                );

            r = shade8(
              r,
              solarBrightness
            );

            g = shade8(
              g,
              solarBrightness
            );

            b = shade8(
              b,
              solarBrightness
            );
          }

          pixel =
              rgb565(
                r, g, b
              );
        }
      }

      dst[x] =
          pixel;
    }
  }

  if (cloudMutex)
    xSemaphoreGive(
      cloudMutex
    );
}

static void drawLimb() {
  fbCircle(
    CX,
    CY,
    RADIUS,
    rgb565(
      LIMB_R,
      LIMB_G,
      LIMB_B
    )
  );
}

static void pushFrame() {
  tft.startWrite();

  tft.setAddrWindow(
    0, 0,
    SCREEN_W,
    SCREEN_H
  );

  tft.writePixels(
    frameBuffer,
    FRAME_PIXELS
  );

  tft.endWrite();
}

static bool begin() {
  buildShadeLUT();

  frameBuffer =
      (uint16_t *)
      allocPSRAMPreferred(
        FRAME_BYTES
      );

  if (!frameBuffer) {
    Serial.println(
      "[GLOBE] Framebuffer allocation failed"
    );

    return false;
  }

  return true;
}

static void draw(
  float centreLonDeg
) {
  drawEarthAndClouds(
    centreLonDeg
  );

  drawISSOrbit(
    centreLonDeg
  );

  drawLimb();

  drawISSMarker(
    centreLonDeg
  );

  // Clock/date/download state is now part of the completed framebuffer.
  composeCornerOverlay(
    frameBuffer
  );

  pushFrame();
}

} // namespace Globe


// -----------------------------------------------------------------------------
// Live globe rotation phase
// -----------------------------------------------------------------------------
//
// This must be defined AFTER namespace Globe because it uses
// Globe::SPIN_DEG_PER_SEC.

static float updateLiveLongitude() {
  const uint32_t nowMs =
      millis();

  if (!liveSpinInitialised) {
    // Preserve the same initial phase used by the previous absolute-millis
    // renderer so this revision does not arbitrarily change startup view.
    const float seconds =
        nowMs *
        0.001f;

    liveLongitudeDeg =
        -fmodf(
          seconds *
          Globe::SPIN_DEG_PER_SEC,
          360.0f
        );

    liveSpinLastMs =
        nowMs;

    liveSpinInitialised =
        true;

    return
        liveLongitudeDeg;
  }

  const uint32_t elapsedMs =
      nowMs -
      liveSpinLastMs;

  liveSpinLastMs =
      nowMs;

  // During an active replay the live globe's phase is deliberately paused.
  // replayStartPending is NOT paused: while WAIT is visible, Earth continues
  // to rotate until the first archive frame is ready.
  if (!replayActive) {
    liveLongitudeDeg -=
        (
          (float)elapsedMs *
          0.001f
        ) *
        Globe::SPIN_DEG_PER_SEC;

    liveLongitudeDeg =
        fmodf(
          liveLongitudeDeg,
          360.0f
        );
  }

  return
      liveLongitudeDeg;
}



// -----------------------------------------------------------------------------
// Corner time/date — runtime-configured local zone.
// Replay uses the archived weather-frame UTC converted by the same offset.
// -----------------------------------------------------------------------------

static void formatLocalCorner(
  time_t utc,
  char timeText[6],
  char dateText[7]
) {
  static const char *MONTHS[12] = {
    "JAN", "FEB", "MAR", "APR",
    "MAY", "JUN", "JUL", "AUG",
    "SEP", "OCT", "NOV", "DEC"
  };

  const time_t localTime =
      utc +
      (time_t)
      appConfig.utcOffsetMinutes *
      60;

  struct tm t = {};
  gmtime_r(
    &localTime,
    &t
  );

  snprintf(
    timeText,
    6,
    "%02d:%02d",
    t.tm_hour,
    t.tm_min
  );

  snprintf(
    dateText,
    7,
    "%02d %s",
    t.tm_mday,
    MONTHS[
      t.tm_mon
    ]
  );
}

static void composeCornerOverlay(
  uint16_t *frameBuffer
) {
  if (!frameBuffer)
    return;

  const time_t displayUTC =
      replayActive ?
      replayDisplayUTC :
      time(nullptr);

  if (
    displayUTC <
    1700000000
  ) {
    return;
  }

  char timeText[6];
  char dateText[7];

  formatLocalCorner(
    displayUTC,
    timeText,
    dateText
  );

  // The globe is centred at (120,120), radius 112.  Two 16-pixel-high text
  // blocks fit in the extreme top corners without covering the Earth.
  constexpr int16_t BLOCK_W = 74;
  constexpr int16_t BLOCK_H = 18;
  constexpr int16_t TOP_Y = 1;
  constexpr int16_t DATE_X = 2;
  constexpr int16_t TIME_X =
      Globe::SCREEN_W -
      BLOCK_W -
      2;
  constexpr int16_t STATUS_X =
      (
        Globe::SCREEN_W -
        BLOCK_W
      ) /
      2;

  // Reuse one transparent off-screen canvas for all three top regions.  At
  // 74x18 this is smaller than the previous 91x20 corner canvas.
  static GFXcanvas16 topCanvas(
    BLOCK_W,
    BLOCK_H
  );

  uint16_t *canvasPixels =
      topCanvas.getBuffer();

  if (!canvasPixels)
    return;

  auto copyFrameToCanvas =
      [&](int16_t screenX, int16_t screenY) {
        for (
          int16_t y = 0;
          y < BLOCK_H;
          ++y
        ) {
          memcpy(
            canvasPixels +
              y * BLOCK_W,
            frameBuffer +
              (
                screenY + y
              ) *
              Globe::SCREEN_W +
              screenX,
            BLOCK_W *
              sizeof(uint16_t)
          );
        }
      };

  auto copyCanvasToFrame =
      [&](int16_t screenX, int16_t screenY) {
        for (
          int16_t y = 0;
          y < BLOCK_H;
          ++y
        ) {
          memcpy(
            frameBuffer +
              (
                screenY + y
              ) *
              Globe::SCREEN_W +
              screenX,
            canvasPixels +
              y * BLOCK_W,
            BLOCK_W *
              sizeof(uint16_t)
          );
        }
      };

  topCanvas.setTextWrap(false);

  // Large date at top left.
  copyFrameToCanvas(
    DATE_X,
    TOP_Y
  );

  topCanvas.setTextSize(2);
  topCanvas.setTextColor(
    tft.color565(
      190, 202, 208
    )
  );
  topCanvas.setCursor(0, 0);
  topCanvas.print(dateText);

  copyCanvasToFrame(
    DATE_X,
    TOP_Y
  );

  // Large time at top right.
  copyFrameToCanvas(
    TIME_X,
    TOP_Y
  );

  topCanvas.setTextSize(2);
  topCanvas.setTextColor(
    tft.color565(
      240, 245, 247
    )
  );

  int16_t boundsX = 0;
  int16_t boundsY = 0;
  uint16_t timeWidth = 0;
  uint16_t timeHeight = 0;

  topCanvas.getTextBounds(
    timeText,
    0,
    0,
    &boundsX,
    &boundsY,
    &timeWidth,
    &timeHeight
  );

  topCanvas.setCursor(
    max(
      0,
      BLOCK_W -
      (int16_t)timeWidth
    ),
    0
  );
  topCanvas.print(timeText);

  copyCanvasToFrame(
    TIME_X,
    TOP_Y
  );

  // Preserve replay/download/button feedback in the centre strip.  The 1x
  // font occupies rows 0-7, immediately above the circular globe.
  char statusText[16] = "";
  uint16_t statusColor =
      tft.color565(
        205, 215, 220
      );

  if (replayActive) {
    strlcpy(
      statusText,
      replayModeName(
        replayMode
      ),
      sizeof(statusText)
    );
  }
  else if (replayStartPending) {
    strlcpy(
      statusText,
      "WAIT",
      sizeof(statusText)
    );
  }
  else if (
    buttonPreviewClicks > 0
  ) {
    snprintf(
      statusText,
      sizeof(statusText),
      "%ux",
      (unsigned)buttonPreviewClicks
    );

    statusColor =
        tft.color565(
          240, 245, 245
        );
  }
  else if (liveFetchBusy) {
    if (
      httpBodyExpected > 0
    ) {
      snprintf(
        statusText,
        sizeof(statusText),
        "%lu/%ldK",
        (unsigned long)(
          httpBodyBytes /
          1024UL
        ),
        (long)(
          httpBodyExpected /
          1024L
        )
      );
    }
    else {
      strlcpy(
        statusText,
        "NET",
        sizeof(statusText)
      );
    }
  }
  else if (
    backfillActive &&
    backfillTotal > 0
  ) {
    snprintf(
      statusText,
      sizeof(statusText),
      "%u/%u",
      (unsigned)backfillDone,
      (unsigned)backfillTotal
    );

    statusColor =
        tft.color565(
          165, 180, 188
        );
  }

  if (statusText[0] != '\0') {
    copyFrameToCanvas(
      STATUS_X,
      0
    );

    topCanvas.setTextSize(1);
    topCanvas.setTextColor(
      statusColor
    );

    int16_t statusBoundsX = 0;
    int16_t statusBoundsY = 0;
    uint16_t statusWidth = 0;
    uint16_t statusHeight = 0;

    topCanvas.getTextBounds(
      statusText,
      0,
      0,
      &statusBoundsX,
      &statusBoundsY,
      &statusWidth,
      &statusHeight
    );

    topCanvas.setCursor(
      max(
        0,
        (
          BLOCK_W -
          (int16_t)statusWidth
        ) /
        2
      ),
      0
    );
    topCanvas.print(
      statusText
    );

    copyCanvasToFrame(
      STATUS_X,
      0
    );
  }
}

// -----------------------------------------------------------------------------
// Display status
// -----------------------------------------------------------------------------

static void showMessage(
  const char *a,
  const char *b = nullptr,
  const char *c = nullptr
) {
  tft.fillScreen(
    ST77XX_BLACK
  );

  tft.setTextColor(
    ST77XX_WHITE
  );

  tft.setTextSize(2);
  tft.setCursor(10, 72);

  if (a) tft.println(a);
  if (b) tft.println(b);
  if (c) tft.println(c);
}

// =============================================================================
// Arduino
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  // Arduino-ESP32's mbedTLS allocator uses internal DRAM by default.  Preserve
  // that scarce pool by making ordinary sketch malloc/new prefer PSRAM.  Calls
  // that explicitly require internal/DMA memory are unaffected.
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(0);

    Serial.printf(
      "[MEM] PSRAM policy enabled: ordinary malloc prefers external RAM\n"
    );
  }

  tft.init(
    240,
    240,
    SPI_MODE3
  );

  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);

  beginBootLog();

  bootPrintln(
    "[MEM] Allocating PSRAM..."
  );

  if (
    !allocateRuntimeMemory() ||
    !Globe::begin()
  ) {
    bootPrintln(
      "[FAIL] Memory"
    );

    while (true)
      delay(1000);
  }

  bootPrintln(
    "[MEM] OK"
  );

  cloudMutex =
      xSemaphoreCreateMutex();

  workMutex =
      xSemaphoreCreateMutex();

  if (
    !cloudMutex ||
    !workMutex
  ) {
    bootPrintln(
      "[FAIL] Mutex"
    );

    while (true)
      delay(1000);
  }

  pinMode(
    BUTTON_PIN,
    INPUT_PULLUP
  );

  // Higher priority than the Arduino loop and weather task, but it sleeps
  // almost all the time. This guarantees that brief button edges are sampled
  // reliably even during network/SD activity.
  if (
    xTaskCreate(
      buttonTask,
      "button",
      2048,
      nullptr,
      2,
      &buttonTaskHandle
    ) != pdPASS
  ) {
    bootPrintln(
      "[FAIL] Button task"
    );

    while (true)
      delay(1000);
  }

  bootPrintln(
    "[BUTTON] Ready"
  );

  beginISSNeoPixel();

  bootPrintln(
    "[NEO] ISS status ready"
  );

  bootPrintln(
    "[SD] Mounting..."
  );

  if (initSDCard()) {
    bootPrintf(
      "[SD] %llu MB ready",
      SD.cardSize() /
      (1024ULL * 1024ULL)
    );

    loadRuntimeConfig();

    bootPrintf(
      "[CFG] ISS%d CLD%d DN%d",
      appConfig.enableISS ? 1 : 0,
      appConfig.enableClouds ? 1 : 0,
      appConfig.enableDayNight ? 1 : 0
    );

    if (appConfig.enableISS) {
      ensureISSDir();
      loadISSFetchState();
      loadISSTLECache();

      bootPrintln(
        appConfig.observerConfigured ?
          "[OBS] Coordinates OK" :
          "[OBS] Add LAT/LON/ALT"
      );
    }
    else {
      bootPrintln(
        "[ISS] Disabled"
      );
    }
  }
  else {
    resetRuntimeConfigDefaults();

    bootPrintln(
      "[SD] FAILED - no config"
    );
  }

  bootPrintln(
    "[WIFI] Connecting..."
  );

  if (!wifiConnect()) {
    bootPrintln(
      "[WIFI] FAILED"
    );
  }
  else {
    bootPrintf(
      "[WIFI] RSSI %d dBm",
      WiFi.RSSI()
    );
  }

  bootPrintln(
    "[TIME] Sync UTC..."
  );

  if (syncUTC()) {
    char tzStatus[32];

    const int offset =
        appConfig.utcOffsetMinutes;

    const char sign =
        offset < 0 ?
        '-' :
        '+';

    const int absMinutes =
        abs(offset);

    snprintf(
      tzStatus,
      sizeof(tzStatus),
      "[TIME] OK %s %c%02d:%02d",
      appConfig.timezoneLabel,
      sign,
      absMinutes / 60,
      absMinutes % 60
    );

    bootPrintln(
      tzStatus
    );

    if (appConfig.enableISS) {
      serviceISSTLE(true);
    }
  }
  else {
    bootPrintln(
      "[TIME] FAILED"
    );

    if (appConfig.enableISS) {
      bootPrintln(
        "[ISS] No UTC - cache only"
      );
    }
  }

  if (appConfig.enableClouds) {
    bootPrintln(
      "[NSMC] Availability in BG"
    );

    if (sdReady) {
      bootPrintln(
        "[ARCHIVE] 7d gap repair"
      );
    }
  }
  else {
    bootPrintln(
      "[CLOUDS] Disabled"
    );
  }

  bootPrintln(
    appConfig.enableDayNight ?
      "[DAY/NIGHT] Enabled" :
      "[DAY/NIGHT] Disabled"
  );

  bootPrintln(
    "[READY] Starting globe"
  );

  delay(350);

  // Startup status is complete. The SSD1306 now sleeps between passes
  // and is woken automatically by serviceISSOLED() at the geometric horizon.
  finishBootLog();

  // Priority 0 is intentional on the single-core ESP32-S2.
  // The renderer/Arduino loop stays responsive; network/archive work runs
  // whenever the loop yields.
  xTaskCreate(
    weatherTask,
    "weather",
    8192,
    nullptr,
    0,
    &weatherTaskHandle
  );
}

void loop() {
  serviceLateNTPSyncLog();

  // Cloud-playback gesture events arrive here.  The SSD1306 forecast request
  // is intentionally handled separately and begins on the physical press,
  // before this gesture has necessarily been finalised.
  const ButtonEvent event =
      takeButtonEvent();

  if (
    event !=
    ButtonEvent::None
  ) {
    Serial.printf(
      "[BUTTON] main received event=%u\n",
      (unsigned)(
        (uint8_t)event
      )
    );
  }

  updateLiveLongitude();

  if (
    event ==
    ButtonEvent::LongPress
  ) {
    finishReplay();
  }
  else if (
    sdReady &&
    appConfig.enableClouds
  ) {
    if (
      event ==
      ButtonEvent::Single
    ) {
      configureReplay(
        ReplayMode::Hours24
      );
    }
    else if (
      event ==
      ButtonEvent::Double
    ) {
      configureReplay(
        ReplayMode::Days7
      );
    }
    else if (
      event ==
      ButtonEvent::Triple
    ) {
      configureReplay(
        ReplayMode::Days30
      );
    }
  }

  serviceReplay();

  // Smooth NeoPixel animation is intentionally independent of the 7-second
  // ISS propagation cadence.
  serviceISSNeoPixel();

  // OHI-style pass telemetry wakes only while the ISS is above the horizon.
  serviceISSOLED();

  const float displayLongitudeDeg =
      replayActive ?
      replayFixedLongitudeDeg :
      liveLongitudeDeg;

  Globe::draw(
    displayLongitudeDeg
  );

  // Yield so the single-core ESP32-S2 can service Wi-Fi/SD/background work.
  delay(1);
}
