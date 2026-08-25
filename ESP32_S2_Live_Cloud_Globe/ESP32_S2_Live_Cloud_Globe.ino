/*Buy me a coffee!
Bitcoin: 19H3zFF4W3zUZ3jAdjmiDNNLs8Ja46M6AD
ETH: 0xD656DB37b61ac30Fa1e16a3162719FE417b231C8
*/

#include <Arduino.h>
#include <new>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_heap_caps.h>

#include <PNGdec.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "wifi_config.h"
#include "ui_types.h"
#include "globe_screen_map_512x256.h"
#include "nasa_blue_marble_565.h"

// =============================================================================
// AUTONOMOUS TRUE-COLOUR EARTH + NSMC GLOBAL GEO IR CLOUDS
//
// Hardware:
//   Adafruit QtPy ESP32-S2
//   ZJY-IPS130-V2.0 / ST7789 240x240
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
// =============================================================================

#define TFT_CS   -1
#define TFT_DC   A1
#define TFT_RST  A0

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);


// =============================================================================
// Tiny ST7789 boot logger — u8log-like behaviour for startup only
// =============================================================================

class TFTLog {
public:
  static constexpr uint8_t MAX_LINES = 20;
  static constexpr uint8_t LINE_CHARS = 36;

  TFTLog()
  : _count(0)
  {
    clearLines();
  }

  void begin(const char *title) {
    clearLines();

    strncpy(
      _title,
      title ? title : "",
      sizeof(_title) - 1
    );

    _title[sizeof(_title) - 1] = '\0';
    redraw();
  }

  void println(const char *s) {
    pushLine(s ? s : "");
    redraw();
  }

  void printf(const char *fmt, ...) {
    char buf[LINE_CHARS];

    va_list args;
    va_start(args, fmt);

    vsnprintf(
      buf,
      sizeof(buf),
      fmt,
      args
    );

    va_end(args);

    println(buf);
  }

private:
  char _title[32] = {};
  char _lines[MAX_LINES][LINE_CHARS] = {};
  uint8_t _count;

  void clearLines() {
    _count = 0;

    for (uint8_t i = 0; i < MAX_LINES; ++i)
      _lines[i][0] = '\0';
  }

  void pushLine(const char *s) {
    if (_count < MAX_LINES) {
      strncpy(
        _lines[_count],
        s,
        LINE_CHARS - 1
      );

      _lines[_count][LINE_CHARS - 1] = '\0';
      ++_count;
      return;
    }

    for (uint8_t i = 1; i < MAX_LINES; ++i) {
      memcpy(
        _lines[i - 1],
        _lines[i],
        LINE_CHARS
      );
    }

    strncpy(
      _lines[MAX_LINES - 1],
      s,
      LINE_CHARS - 1
    );

    _lines[MAX_LINES - 1][LINE_CHARS - 1] = '\0';
  }

  void redraw() {
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextWrap(false);
    tft.setTextSize(1);

    tft.setTextColor(
      tft.color565(145, 190, 220)
    );

    tft.setCursor(6, 6);
    tft.println(_title);

    tft.drawFastHLine(
      6,
      17,
      228,
      tft.color565(45, 65, 80)
    );

    tft.setTextColor(
      tft.color565(215, 225, 230)
    );

    int16_t y = 23;

    for (uint8_t i = 0; i < _count; ++i) {
      tft.setCursor(6, y);
      tft.print(_lines[i]);
      y += 10;
    }
  }
};

static TFTLog bootLog;

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
static uint32_t decodeOpaquePixels = 0;

// -----------------------------------------------------------------------------
// Live product / SD archive / replay state
// -----------------------------------------------------------------------------

static bool haveIRClouds = false;
static char currentIRTime[13] = ""; // YYYYMMDDHHMM UTC

// The global mosaic can occasionally appear before it is fully settled.
// Even when NSMC's availability API lists a new dataset, stay one hour behind
// wall-clock UTC before allowing it to become the live display.
constexpr uint16_t NSMC_SOURCE_LAG_MINUTES = 60;

// Ask NSMC for its official GEOS_IRX availability list every 15 minutes.
// New imagery is currently hourly, while this shorter poll also gives missing
// hours another chance after a weak-Wi-Fi failure.
constexpr uint32_t UPDATE_INTERVAL_MS =
    15UL * 60UL * 1000UL;

// Ask for a full seven days so the SD archive can be populated/repaired after outages/reboots.
constexpr uint16_t NSMC_AVAILABILITY_HOURS = 168;
constexpr uint16_t NSMC_MAX_AVAILABLE_TIMES = 192;

static char nsmcAvailableTimes[NSMC_MAX_AVAILABLE_TIMES][13] = {};
static uint16_t nsmcAvailableCount = 0;

static uint32_t lastUpdateMillis = 0;

// Active display mapping.
static uint8_t cloudOpacityLUT[256];
static uint8_t globalCloudAlphaScale = 180;
static bool useAlphaPrimaryOpacity = false;

// Candidate mapping.  Downloads/PNG decode build these without touching the
// currently displayed weather.  commitCandidate() swaps them in atomically.
static uint8_t candidateCloudOpacityLUT[256];
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

// Current HTTP body progress.  Used only for the small on-screen NET indicator.
static volatile uint32_t httpBodyBytes = 0;
static volatile int32_t httpBodyExpected = -1;

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

  memset(irLuma, 0, IR_PIXELS);
  memset(candidateLuma, 0, IR_PIXELS);
  memset(irAlpha, 0, IR_PIXELS);
  memset(candidateAlpha, 0, IR_PIXELS);

  Serial.printf(
    "[MEM] IR luma+alpha buffers=%lu bytes, download=%u bytes\n",
    (unsigned long)(IR_PIXELS * 4UL),
    (unsigned)DOWNLOAD_BUFFER_CAPACITY
  );

  return true;
}

static bool ensurePNGDecoder() {
  if (pngDecoder)
    return true;

  const size_t bytes =
      sizeof(PNG);

  pngDecoderMemory =
      allocPSRAMPreferred(
        bytes
      );

  if (!pngDecoderMemory) {
    Serial.printf(
      "[PNG] Decoder allocation failed: %u bytes\n",
      (unsigned)bytes
    );

    return false;
  }

  pngDecoder =
      new (pngDecoderMemory) PNG();

  Serial.printf(
    "[PNG] Decoder allocated: %u bytes\n",
    (unsigned)bytes
  );

  return true;
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
// Wi-Fi + UTC
// -----------------------------------------------------------------------------

static bool wifiConnect(
  uint32_t timeoutMs = 30000
) {
  if (WiFi.status() == WL_CONNECTED)
    return true;

  WiFi.mode(WIFI_STA);

  // Weak-signal installations benefit from disabling modem sleep during the
  // short weather download session.
  WiFi.setSleep(false);

  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    Serial.printf(
      "[WIFI] Connecting to %s attempt %u/2\n",
      WEATHER_WIFI_SSID,
      (unsigned)attempt
    );

    WiFi.begin(
      WEATHER_WIFI_SSID,
      WEATHER_WIFI_PASSWORD
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
        "[WIFI] Connected RSSI=%d\n",
        WiFi.RSSI()
      );

      return true;
    }

    WiFi.disconnect();
    delay(500);
  }

  Serial.println(
    "[WIFI] Connection failed"
  );

  return false;
}

static bool syncUTC(
  uint32_t timeoutMs = 15000
) {
  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  const uint32_t start =
      millis();

  while (
    time(nullptr) < 1700000000 &&
    millis() - start < timeoutMs
  ) {
    delay(250);
  }

  const time_t now =
      time(nullptr);

  if (now < 1700000000) {
    Serial.println("[TIME] NTP failed");
    return false;
  }

  struct tm t = {};
  gmtime_r(&now, &t);

  Serial.printf(
    "[TIME] UTC %04d-%02d-%02d %02d:%02d:%02d\n",
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec
  );

  return true;
}

static void makeNSMCTime(
  int hoursAgo,
  char out[13]
) {
  time_t now =
      time(nullptr);

  now -=
      (time_t)NSMC_SOURCE_LAG_MINUTES *
      60;

  // Fallback only: the official availability API is the primary source.
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

  const uint32_t getStartMs =
      millis();

  Serial.println(
    "[HTTP] TLS/connect/request..."
  );

  const int code =
      http.GET();

  Serial.printf(
    "[HTTP] GET returned %d after %lu ms\n",
    code,
    (unsigned long)(
      millis() -
      getStartMs
    )
  );

  if (code != HTTP_CODE_OK) {
    Serial.printf(
      "[HTTP] status=%d\n",
      code
    );

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
    "[HTTP] PNG received %u bytes in %lu ms\n",
    (unsigned)downloadSize,
    (unsigned long)(
      millis() -
      bodyStartMs
    )
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

    ++decodeOpaquePixels;

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
  if (!ensurePNGDecoder())
    return false;

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
  decodeOpaquePixels = 0;

  const int rcOpen =
      pngDecoder->openRAM(
        downloadBuffer,
        (int)downloadSize,
        irPNGDraw
      );

  if (rcOpen != PNG_SUCCESS) {
    Serial.printf(
      "[PNG] openRAM=%d\n",
      rcOpen
    );

    decodeTarget = nullptr;
    decodeTargetAlpha = nullptr;
    return false;
  }

  Serial.printf(
    "[PNG] specs %d x %d bpp=%d type=%d alpha=%d\n",
    pngDecoder->getWidth(),
    pngDecoder->getHeight(),
    pngDecoder->getBpp(),
    pngDecoder->getPixelType(),
    pngDecoder->hasAlpha()
  );

  if (
    pngDecoder->getWidth() != IR_W ||
    pngDecoder->getHeight() != IR_H
  ) {
    pngDecoder->close();
    decodeTarget = nullptr;
    decodeTargetAlpha = nullptr;
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

  Serial.printf(
    "[PNG] decoded opaque pixels=%lu/%lu\n",
    (unsigned long)decodeOpaquePixels,
    (unsigned long)IR_PIXELS
  );

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

  const uint8_t p50L =
      percentileFromHistogram(
        histLuma,
        opaque,
        0.50f
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

  const uint8_t p50A =
      percentileFromHistogram(
        histAlpha,
        opaque,
        0.50f
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

  Serial.printf(
    "[IR] opaque=%.1f%%  luma mean=%.1f sd=%.1f p10=%u p50=%u p95=%u | alpha mean=%.1f p10=%u p50=%u p95=%u\n",
    100.0f * (float)opaque / (float)IR_PIXELS,
    meanLuma,
    stddevLuma,
    p10L,
    p50L,
    p95L,
    meanAlpha,
    p10A,
    p50A,
    p95A
  );

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
    "[IR] opacity mode=%s alphaScale=%u lumaLow=%.0f lumaHigh=%.0f\n",
    candidateUseAlphaPrimaryOpacity ?
      "alpha-primary" :
      "alpha*luma",
    (unsigned)candidateGlobalCloudAlphaScale,
    low,
    high
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
    sizeof(cloudOpacityLUT)
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

  if (cloudMutex)
    xSemaphoreGive(
      cloudMutex
    );
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

  const uint32_t getStartMs =
      millis();

  const int code =
      http.GET();

  Serial.printf(
    "[AVAIL] GET returned %d after %lu ms\n",
    code,
    (unsigned long)(
      millis() -
      getStartMs
    )
  );

  if (code != HTTP_CODE_OK) {
    http.end();

    Serial.printf(
      "[AVAIL] HTTP status=%d\n",
      code
    );

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

  Serial.printf(
    "[AVAIL] JSON received %u bytes\n",
    (unsigned)downloadSize
  );

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
  // The availability list itself does not need local time, but selecting the
  // safe newest frame does. Recover automatically if boot-time NTP failed.
  if (
    time(nullptr) <
    1700000000
  ) {
    if (!wifiConnect())
      return false;

    Serial.println(
      "[AVAIL] UTC invalid; retrying NTP"
    );

    if (!syncUTC())
      return false;
  }

  if (!downloadNSMCAvailabilityJSON())
    return false;

  return
      parseNSMCAvailabilityJSON();
}

static void makeAvailabilityCutoff(
  char out[13]
) {
  const time_t cutoff =
      time(nullptr) -
      (time_t)
      NSMC_SOURCE_LAG_MINUTES *
      60;

  formatUTCStamp(
    cutoff,
    out
  );
}

static bool stampEligibleForUse(
  const char *stamp,
  const char *cutoff
) {
  return
      strcmp(
        stamp,
        cutoff
      ) <= 0;
}

static int newestEligibleAvailabilityIndex() {
  if (
    nsmcAvailableCount == 0
  ) {
    return -1;
  }

  char cutoff[13];

  makeAvailabilityCutoff(
    cutoff
  );

  for (
    int i =
        (int)nsmcAvailableCount - 1;
    i >= 0;
    --i
  ) {
    if (
      stampEligibleForUse(
        nsmcAvailableTimes[i],
        cutoff
      )
    ) {
      return i;
    }
  }

  return -1;
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
      newestEligibleAvailabilityIndex();

  if (newest < 0) {
    Serial.println(
      "[LIVE] No eligible NSMC timestamp"
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

  char cutoff[13];

  makeAvailabilityCutoff(
    cutoff
  );

  uint16_t missing = 0;

  for (
    uint16_t i = 0;
    i <
      nsmcAvailableCount;
    ++i
  ) {
    if (
      !stampEligibleForUse(
        nsmcAvailableTimes[i],
        cutoff
      )
    ) {
      continue;
    }

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

static void repairArchiveGapsFromAvailability() {
  if (
    !sdReady ||
    nsmcAvailableCount == 0
  ) {
    backfillActive = false;
    return;
  }

  const uint16_t missingAtStart =
      countMissingAvailableFrames();

  backfillDone = 0;
  backfillTotal =
      missingAtStart;

  if (
    missingAtStart == 0
  ) {
    backfillActive = false;

    Serial.println(
      "[GAP] Official 7 day window complete - no missing frames"
    );

    return;
  }

  backfillActive = true;

  Serial.printf(
    "[GAP] Repair queue: %u missing official frames\n",
    (unsigned)missingAtStart
  );

  char cutoff[13];

  makeAvailabilityCutoff(
    cutoff
  );

  uint16_t saved = 0;
  uint16_t failed = 0;

  // Newest first: recent weather is more useful than an older repaired gap.
  for (
    int i =
        (int)nsmcAvailableCount - 1;
    i >= 0;
    --i
  ) {
    if (
      replayActive ||
      replayStartPending ||
      pendingButtonEvent !=
        (uint8_t)ButtonEvent::None
    ) {
      break;
    }

    const char *stamp =
        nsmcAvailableTimes[i];

    if (
      !stampEligibleForUse(
        stamp,
        cutoff
      )
    ) {
      continue;
    }

    if (archiveExists(stamp))
      continue;

    Serial.printf(
      "[GAP] Missing %s UTC\n",
      stamp
    );

    if (
      workMutex &&
      xSemaphoreTake(
        workMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {
      // Re-check after obtaining the shared PNG/download lock.
      if (
        replayActive ||
        replayStartPending ||
        pendingButtonEvent !=
          (uint8_t)ButtonEvent::None
      ) {
        xSemaphoreGive(
          workMutex
        );

        break;
      }

      if (
        WiFi.status() !=
          WL_CONNECTED &&
        !wifiConnect()
      ) {
        Serial.println(
          "[GAP] Wi-Fi unavailable; pausing repair until next pass"
        );

        ++failed;

        xSemaphoreGive(
          workMutex
        );

        break;
      }

      if (
        fetchExactFrame(
          stamp,
          false,
          true
        )
      ) {
        ++saved;
      }
      else {
        ++failed;

        Serial.printf(
          "[GAP] %s still missing; will retry on a later pass\n",
          stamp
        );
      }

      xSemaphoreGive(
        workMutex
      );
    }

    if (
      backfillDone <
        backfillTotal
    ) {
      ++backfillDone;
    }

    vTaskDelay(
      pdMS_TO_TICKS(250)
    );
  }

  backfillActive = false;

  const uint16_t remaining =
      countMissingAvailableFrames();

  Serial.printf(
    "[GAP] Pass done: saved=%u failed=%u remaining=%u\n",
    (unsigned)saved,
    (unsigned)failed,
    (unsigned)remaining
  );
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
  }

  Serial.println(
    "[LIVE] Hourly fallback found no usable image; keeping previous map"
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

    if (
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

      // Phase 2: compare every officially listed hour with the SD archive.
      // Failed hours remain absent, therefore remain in the repair queue next
      // time rather than being silently forgotten.
      if (
        availabilityOK &&
        sdReady &&
        !replayActive &&
        !replayStartPending
      ) {
        // This function now locks one missing frame at a time, so replay can
        // pre-empt between downloads.
        repairArchiveGapsFromAvailability();
      }

      vTaskDelay(
        pdMS_TO_TICKS(250)
      );

      continue;
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

static uint8_t shadeLUT[256];

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

static void buildShadeLUT() {
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
  if (cloudMutex)
    xSemaphoreTake(
      cloudMutex,
      portMAX_DELAY
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

            if (useAlphaPrimaryOpacity) {
              alpha =
                  (uint8_t)(
                    (
                      alphaBase *
                      (uint16_t)lumaAlpha
                    ) >> 16
                  );
            }
            else {
              alpha =
                  (uint8_t)(
                    (
                      alphaBase *
                      (uint16_t)lumaAlpha
                    ) >> 16
                  );
            }
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

  drawLimb();

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
// Corner time/date — live JST, replay uses archived weather-frame JST
// -----------------------------------------------------------------------------

static void formatJSTCorner(
  time_t utc,
  char timeText[6],
  char dateText[7]
) {
  static const char *MONTHS[12] = {
    "JAN", "FEB", "MAR", "APR",
    "MAY", "JUN", "JUL", "AUG",
    "SEP", "OCT", "NOV", "DEC"
  };

  const time_t jst =
      utc +
      9 * 60 * 60;

  struct tm t = {};
  gmtime_r(&jst, &t);

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

  formatJSTCorner(
    displayUTC,
    timeText,
    dateText
  );

  constexpr int16_t OVERLAY_X = 3;
  constexpr int16_t OVERLAY_Y = 3;
  constexpr int16_t OVERLAY_W = 91;
  constexpr int16_t OVERLAY_H = 20;

  // Small off-screen Adafruit_GFX canvas. Start by copying the already
  // rendered Earth/cloud pixels beneath this area into the canvas. Text is
  // then drawn transparently on top and the result is copied back into the
  // main framebuffer. No solid rectangle obscures the globe.
  static GFXcanvas16 cornerCanvas(
    OVERLAY_W,
    OVERLAY_H
  );

  uint16_t *canvasPixels =
      cornerCanvas.getBuffer();

  if (!canvasPixels)
    return;

  for (
    int16_t y = 0;
    y < OVERLAY_H;
    ++y
  ) {
    memcpy(
      canvasPixels +
      y * OVERLAY_W,
      frameBuffer +
      (
        OVERLAY_Y + y
      ) *
      Globe::SCREEN_W +
      OVERLAY_X,
      OVERLAY_W *
      sizeof(uint16_t)
    );
  }

  cornerCanvas.setTextWrap(false);
  cornerCanvas.setTextSize(1);

  cornerCanvas.setTextColor(
    tft.color565(
      235, 240, 242
    )
  );

  cornerCanvas.setCursor(4, 2);
  cornerCanvas.print(timeText);

  cornerCanvas.setTextColor(
    tft.color565(
      180, 192, 198
    )
  );

  cornerCanvas.setCursor(4, 10);
  cornerCanvas.print(dateText);

  if (replayActive) {
    cornerCanvas.setTextColor(
      tft.color565(
        205, 215, 220
      )
    );

    cornerCanvas.setCursor(47, 6);
    cornerCanvas.print(
      replayModeName(
        replayMode
      )
    );
  }
  else if (replayStartPending) {
    cornerCanvas.setTextColor(
      tft.color565(
        205, 215, 220
      )
    );

    cornerCanvas.setCursor(46, 6);
    cornerCanvas.print("WAIT");
  }
  else if (
    buttonPreviewClicks > 0
  ) {
    char clickPreview[5];

    snprintf(
      clickPreview,
      sizeof(clickPreview),
      "%ux",
      (unsigned)buttonPreviewClicks
    );

    cornerCanvas.setTextColor(
      tft.color565(
        240, 245, 245
      )
    );

    cornerCanvas.setCursor(50, 6);
    cornerCanvas.print(
      clickPreview
    );
  }
  else if (liveFetchBusy) {
    cornerCanvas.setTextColor(
      tft.color565(
        205, 215, 220
      )
    );

    cornerCanvas.setCursor(43, 6);

    if (
      httpBodyExpected > 0
    ) {
      char netProgress[12];

      snprintf(
        netProgress,
        sizeof(netProgress),
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

      cornerCanvas.print(
        netProgress
      );
    }
    else {
      cornerCanvas.print(
        "NET"
      );
    }
  }
  else if (
    backfillActive &&
    backfillTotal > 0
  ) {
    char progress[12];

    snprintf(
      progress,
      sizeof(progress),
      "%u/%u",
      (unsigned)backfillDone,
      (unsigned)backfillTotal
    );

    cornerCanvas.setTextColor(
      tft.color565(
        165, 180, 188
      )
    );

    cornerCanvas.setCursor(45, 6);
    cornerCanvas.print(
      progress
    );
  }

  const uint16_t *src =
      canvasPixels;

  for (
    int16_t y = 0;
    y < OVERLAY_H;
    ++y
  ) {
    memcpy(
      frameBuffer +
      (
        OVERLAY_Y + y
      ) *
      Globe::SCREEN_W +
      OVERLAY_X,
      src +
      y *
      OVERLAY_W,
      OVERLAY_W *
      sizeof(uint16_t)
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

  tft.init(
    240,
    240,
    SPI_MODE3
  );

  tft.setRotation(2);

  bootLog.begin(
    "GLOBAL WEATHER GLOBE"
  );

  bootLog.println(
    "[MEM] Allocating PSRAM..."
  );

  if (
    !allocateRuntimeMemory() ||
    !Globe::begin()
  ) {
    bootLog.println(
      "[FAIL] Memory"
    );

    while (true)
      delay(1000);
  }

  bootLog.println(
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
    bootLog.println(
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
      3072,
      nullptr,
      2,
      &buttonTaskHandle
    ) != pdPASS
  ) {
    bootLog.println(
      "[FAIL] Button task"
    );

    while (true)
      delay(1000);
  }

  bootLog.println(
    "[BUTTON] Ready"
  );

  bootLog.println(
    "[SD] Mounting..."
  );

  if (initSDCard()) {
    bootLog.printf(
      "[SD] %llu MB ready",
      SD.cardSize() /
      (1024ULL * 1024ULL)
    );
  }
  else {
    bootLog.println(
      "[SD] FAILED - live only"
    );
  }

  bootLog.println(
    "[WIFI] Connecting..."
  );

  if (!wifiConnect()) {
    bootLog.println(
      "[WIFI] FAILED"
    );
  }
  else {
    bootLog.printf(
      "[WIFI] RSSI %d dBm",
      WiFi.RSSI()
    );
  }

  bootLog.println(
    "[TIME] Sync UTC..."
  );

  if (syncUTC()) {
    bootLog.println(
      "[TIME] OK / JST +9"
    );
  }
  else {
    bootLog.println(
      "[TIME] FAILED"
    );
  }

  bootLog.println(
    "[NSMC] Availability in BG"
  );

  if (sdReady) {
    bootLog.println(
      "[ARCHIVE] 7d gap repair"
    );
  }

  bootLog.println(
    "[READY] Starting globe"
  );

  delay(350);

  // Priority 0 is intentional on the single-core ESP32-S2.
  // The renderer/Arduino loop stays responsive; network/archive work runs
  // whenever the loop yields.
  xTaskCreate(
    weatherTask,
    "weather",
    12288,
    nullptr,
    0,
    &weatherTaskHandle
  );
}

void loop() {
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
  else if (sdReady) {
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
