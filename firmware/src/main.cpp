// Stage-8 (spec 008): everything from stage-7 + Sheets sync.
//
// Boot lifecycle:
//   1. OLED init (Wire 21/22) + WiFiManager auto-connect.
//   2. If no creds saved: AP mode with dual-QR setup screen.
//   3. On STA join: marquee "WiFi:<ssid>  IP:<ip>", PN532 SPI init, NTP
//      sync, LittleFS mount, sync queue drain loop arms.
//   4. On card tap: enqueue { card_uid, captured_at, event_id } to
//      LittleFS, drain loop POSTs to the Sheets Web App receiver with
//      Authorization: Bearer <token>. On success, OLED shows name +
//      hours-today (registered) or "บัตรไม่ได้ลงทะเบียน" (unregistered).
//      On auth failure, OLED shows "ตรวจสอบ token".

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <qrcode.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <HTTPUpdate.h>

// ---- Display / pin constants ----------------------------------------------

static constexpr uint8_t  OLED_ADDR        = 0x3C;
static constexpr uint8_t  OLED_W           = 128;
static constexpr uint8_t  OLED_H           = 64;
static constexpr int8_t   OLED_RESET       = -1;
// Adafruit_GFX renders text at 6 px per glyph cell (5 px glyph + 1 px gap)
// at text size 1. Used to compute the marquee width in pixels.
static constexpr uint8_t  GLYPH_PX_W       = 6;
// SSD1306 contrast register: 0xFF disables PWM dimming so camera-captured
// frames stop flickering (otherwise QR scans fail).
static constexpr uint8_t  OLED_CONTRAST_MAX = 0xFF;
// SSD1306 clock-divider register: high nibble = oscillator freq, low =
// divider. 0xD0 (freq=13, divider=÷1) lands at ~127 Hz refresh — prime,
// avoids beat-frequency aliasing under 30/60 FPS phone cameras.
static constexpr uint8_t  OLED_CLOCKDIV_127HZ = 0xD0;
static constexpr uint8_t  LED_PIN          = 2;
static constexpr int      PN532_SS        = 5;
static constexpr int      PN532_SCK       = 18;
static constexpr int      PN532_MISO      = 19;
static constexpr int      PN532_MOSI      = 23;

static constexpr uint16_t WIFI_CONNECT_TIMEOUT_S = 15;

// Marquee
static constexpr uint32_t MARQUEE_FRAME_MS         = 60;
static constexpr int16_t  MARQUEE_PX_PER_FRAME     = 2;
static constexpr int16_t  MARQUEE_Y                = 8;  // bottom half of yellow
static constexpr uint8_t  MARQUEE_TRAILING_SPACES  = 4;

// AP mode dual-QR
static constexpr uint32_t AP_SCREEN_SWITCH_MS      = 8000;
static constexpr uint8_t  QR_VERSION               = 3;
static constexpr uint8_t  QR_SIZE_MODULES          = 29;
static constexpr uint8_t  QR_PIXELS_PER_MODULE     = 1;
static constexpr int16_t  BLUE_ZONE_Y_START        = 16;
static constexpr uint16_t QR_BUFFER_BYTES =
    (static_cast<uint16_t>(QR_SIZE_MODULES) * QR_SIZE_MODULES + 7) / 8;

// Card-tap behaviour
static constexpr uint32_t CARD_DISPLAY_MS          = 5000;  // hold UID screen 5 s
static constexpr uint32_t CARD_POLL_MS             = 250;
static constexpr uint16_t CARD_READ_TIMEOUT_MS     = 50;    // non-blocking poll
static constexpr uint32_t CARD_DEDUP_MS            = 1500;

// ---- Globals --------------------------------------------------------------

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RESET);
// U8g2-for-Adafruit-GFX overlay — Unicode-aware text rendering on top of the
// existing SSD1306 framebuffer. Use `oled.*` for graphics primitives
// (lines, rects, drawBitmap) and `u8gThai.*` for any text that may contain
// Thai characters. The two share the same framebuffer; oled.display()
// commits the frame.
U8G2_FOR_ADAFRUIT_GFX u8gThai;
Adafruit_PN532   nfc(PN532_SS, &SPI);
WebServer        server(80);
WiFiManager      wm;

static String   apSsid;
static String   staIp;
static bool     inApMode      = false;
static bool     wmHttpStarted = false;
static bool     pn532Ready    = false;

// Marquee state
static String   marqueeText;
static int16_t  marqueeX      = 0;
static uint16_t marqueePixelW = 0;
static uint32_t lastFrameMs   = 0;
static uint32_t lastSerialMs  = 0;

// AP-mode alternation
static String   apQrPayload;
static const char AP_URL_PAYLOAD[] = "http://192.168.4.1";
static uint8_t  apScreen        = 0;
static uint32_t lastApSwitchMs  = 0;
static constexpr uint8_t AP_SCREEN_COUNT = 2;

// Card-tap state
static uint32_t lastCardPollMs = 0;
static uint32_t cardScreenUntilMs = 0;     // 0 = marquee mode
static uint8_t  lastUid[10] = {0};
static uint8_t  lastUidLen  = 0;
static uint32_t lastUidSeenMs = 0;

// ---- Spec 008 sync configuration -----------------------------------------

// NVS namespace + keys (Preferences API). Survives reboot. The bearer token
// MUST be configured by the operator before the device can talk to the
// Sheets receiver — see the device's web admin page.
static constexpr const char* PREFS_NS         = "clocksync";
static constexpr const char* PREFS_K_BEARER   = "bearer_token";
static constexpr const char* PREFS_K_URL      = "receiver_url";
static constexpr const char* PREFS_K_SEQ      = "event_seq";

// LittleFS layout for the offline event queue. One file per pending event;
// drain order = oldest mtime first. The directory is created on first boot.
static constexpr const char* QUEUE_DIR        = "/queue";
static constexpr uint32_t    QUEUE_MAX_FILES  = 1000;

// HTTP timings. The Sheets web app typically responds in 300-1500ms;
// 10 s is a generous ceiling.
static constexpr uint32_t HTTPS_TIMEOUT_MS    = 10000;
static constexpr uint32_t DRAIN_TICK_MS       = 250;   // how often loop() considers draining
static constexpr uint32_t BACKOFF_INIT_MS     = 1000;
static constexpr uint32_t BACKOFF_MAX_MS      = 60000;

// Card-feedback screen variants — extends the spec-007 5-second UID hold.
enum class CardFeedback : uint8_t {
  None,           // no card screen up; marquee active
  Syncing,        // waiting for receiver response (transient)
  Registered,     // success with name + hours_today
  Unregistered,   // success with name=null
  AuthFail,       // receiver rejected the bearer token
  TransportFail,  // could not reach receiver (WiFi/DNS/TLS)
  TimeWaiting,    // NTP not yet synced — event queued but not delivered
  RegPromptTap,   // registration mode: name selected, waiting for tap
  RegHasTap,      // registration mode: name + UID both captured
  OtaChecking,    // GET /latest.txt in flight
  OtaDownloading, // newer tag found, streaming firmware.bin (blocks loop)
  OtaFailed,      // most-recent OTA attempt didn't complete
};

// ---- Spec 008 sync state -------------------------------------------------

Preferences      prefs;
static uint32_t  lastOtaCheckMs   = 0;
// Newest tag the device has seen on the manifest, displayed on the OLED
// during OtaDownloading / OtaFailed states.
static char      otaVersionTag[16] = {0};
// 0..100 percent of the current binary download (HTTPUpdate progress
// callback). -1 when not actively downloading.
static int       otaProgressPct    = -1;
static String    bearerToken;
static String    receiverUrl;
static bool      littleFsReady    = false;
static bool      timeSynced       = false;
static uint32_t  lastDrainTickMs  = 0;
static uint32_t  nextDrainAtMs    = 0;
static uint32_t  drainBackoffMs   = BACKOFF_INIT_MS;
static uint32_t  queueDepth       = 0;     // monotonic refresh on each enqueue/drain
static uint32_t  eventsDeliveredToday = 0;
static uint32_t  lastSyncOkAtEpoch = 0;
static const char* lastFailureReason = nullptr;  // points to a string literal

// Most-recent /tap response cached for OLED render.
static CardFeedback currentFeedback = CardFeedback::None;
static char         feedbackName[64] = {0};
static float        feedbackHours    = 0.0f;
static char         feedbackUidHex[24] = {0};

// ---- Spec 008 registration mode (US3) ------------------------------------

// While a browser is polling /api/latest-tap, taps go to the buffer
// instead of the sync queue. The mutex resets after a successful register
// OR after REG_WATCHDOG_MS of no polling (browser closed).
static constexpr uint32_t REG_WATCHDOG_MS = 60000;
static constexpr uint32_t REG_TAP_STALE_MS = 30000;
static constexpr uint32_t REG_SESSION_MS = 60000;  // auto-clear name after 60s idle

// ---- OTA auto-update (GitHub) --------------------------------------------
//
// Build-time version stamp. Operator bumps this when cutting a new release.
// At runtime the device asks GitHub for the *latest* release tag via
// OTA_LATEST_URL (returns 302 → Location: .../releases/tag/<tag>). The tag
// is parsed from Location and compared to FIRMWARE_VERSION; if newer the
// device downloads firmware.bin from the matching Release asset and
// self-flashes via HTTPUpdate.
//
// Why not raw.githubusercontent.com/.../latest.txt? That endpoint is CDN-
// cached for 5 min, so newly-cut releases take up to 5 min to propagate.
// /releases/latest carries `cache-control: no-cache` → instant pickup.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v0.0.1"
#endif
static constexpr const char* OTA_LATEST_URL =
    "https://github.com/dutch3883/smart_card_firmware/releases/latest";
// {VERSION} is replaced with the manifest's tag value at runtime.
static constexpr const char* OTA_BIN_URL_TEMPLATE =
    "https://github.com/dutch3883/smart_card_firmware/releases/download/{VERSION}/firmware.bin";
static constexpr uint32_t OTA_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour
static bool      inRegistrationMode      = false;
static uint32_t  lastRegPollMs           = 0;
static char      latestTapUid[24]        = {0};
static uint32_t  latestTapScannedAtMs    = 0;
// Name currently picked in the browser's dropdown. Passed in on every
// /api/latest-tap poll so the device's OLED can show "Register: <name>".
static char      regSelectedEmployee[64] = {0};
// millis() at the moment regSelectedEmployee transitioned from empty to
// non-empty (or from one name to a different name). 0 when no name is set.
// Drives the 60s session countdown.
static uint32_t  regNameSelectedAtMs     = 0;

// ---- Sync helpers (spec 008) ---------------------------------------------

static void prefsLoad_() {
  prefs.begin(PREFS_NS, /* readOnly */ true);
  bearerToken = prefs.getString(PREFS_K_BEARER, "");
  receiverUrl = prefs.getString(PREFS_K_URL, "");
  prefs.end();
}

static void prefsSaveBearer_(const String& v) {
  prefs.begin(PREFS_NS, false);
  prefs.putString(PREFS_K_BEARER, v);
  prefs.end();
  bearerToken = v;
}

static void prefsSaveUrl_(const String& v) {
  prefs.begin(PREFS_NS, false);
  prefs.putString(PREFS_K_URL, v);
  prefs.end();
  receiverUrl = v;
}

// Monotonic event-id sequence — persisted across reboots so we never reuse
// a value within the same device's lifetime (which would defeat the
// receiver's idempotency ledger).
static uint32_t nextEventSeq_() {
  prefs.begin(PREFS_NS, false);
  uint32_t seq = prefs.getUInt(PREFS_K_SEQ, 0);
  ++seq;
  prefs.putUInt(PREFS_K_SEQ, seq);
  prefs.end();
  return seq;
}

// Build a unique-enough event id: <mac_suffix>-<seq>. The seq is global per
// device, so even across reboots two ids never collide. <mac_suffix> makes
// ids self-identifying when the receiver looks at them in the audit log.
static void buildEventId_(char* out, size_t outLen) {
  const uint64_t mac = ESP.getEfuseMac();
  const uint16_t macSuffix = static_cast<uint16_t>((mac >> 32) & 0xFFFF);
  const uint32_t seq = nextEventSeq_();
  snprintf(out, outLen, "%04X-%lu", macSuffix, (unsigned long)seq);
}

// Minimal URL encoder for query-string values. Encodes anything that
// isn't unreserved (RFC 3986: A-Z a-z 0-9 -_.~). Used to shove the
// Bearer token into the query string safely — base64 tokens contain
// `+` and `/` which must be percent-encoded or Google's frontend
// returns 400 Bad Request before reaching our script.
static String urlEncode_(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); ++i) {
    const unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Format a Unix timestamp into the ISO 8601 the receiver expects.
// Asia/Bangkok is hardcoded — the venue is fixed; spec 008 §Assumptions.
static void formatIso8601Bangkok_(time_t epoch, char* out, size_t outLen) {
  struct tm tm;
  // gmtime, then add the +07:00 offset manually so the output string
  // carries the right TZ marker (configTzTime sets local timezone but
  // gmtime gives us a known base).
  gmtime_r(&epoch, &tm);
  // Asia/Bangkok = UTC+7
  time_t bangkok = epoch + 7 * 3600;
  gmtime_r(&bangkok, &tm);
  snprintf(out, outLen,
           "%04d-%02d-%02dT%02d:%02d:%02d+07:00",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Encode the binary card UID into uppercase hex.
static void formatUidHex_(const uint8_t* uid, uint8_t uidLen, char* out, size_t outLen) {
  size_t p = 0;
  for (uint8_t i = 0; i < uidLen && p + 2 < outLen; ++i) {
    p += snprintf(out + p, outLen - p, "%02X", uid[i]);
  }
  out[p] = '\0';
}

// Initialise LittleFS and ensure the queue directory exists.
static bool littleFsBoot_() {
  if (!LittleFS.begin(/* formatOnFail */ true)) {
    Serial.println(F("[sync] LittleFS mount failed"));
    return false;
  }
  if (!LittleFS.exists(QUEUE_DIR)) {
    LittleFS.mkdir(QUEUE_DIR);
  }
  return true;
}

// Count the number of pending events on disk.
static uint32_t queueCount_() {
  if (!littleFsReady) return 0;
  File dir = LittleFS.open(QUEUE_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  uint32_t n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) ++n;
    f.close();
  }
  dir.close();
  return n;
}

// Find the oldest queue file by name (event ids are monotonic so name sort
// equals chronological order, simpler and cheaper than stat-ing every file).
// Returns "" if the queue is empty.
static String queueOldestFile_() {
  if (!littleFsReady) return "";
  File dir = LittleFS.open(QUEUE_DIR);
  if (!dir || !dir.isDirectory()) return "";
  String oldest;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const String name = String(QUEUE_DIR) + "/" + f.name();
      if (oldest.length() == 0 || name < oldest) oldest = name;
    }
    f.close();
  }
  dir.close();
  return oldest;
}

// Evict the oldest file to make room — used when the queue hits the cap.
static void queueEvictOldest_() {
  const String path = queueOldestFile_();
  if (path.length() == 0) return;
  LittleFS.remove(path);
  Serial.printf("[sync] evicted %s (queue full)\n", path.c_str());
  lastFailureReason = "events_dropped";
}

// Persist one event to the queue. Writes the whole JSON body to a single
// file so a power loss mid-write at worst loses one event.
static bool queueEnqueue_(const char* eventId, const char* uidHex, const char* iso) {
  if (!littleFsReady) return false;
  if (queueCount_() >= QUEUE_MAX_FILES) queueEvictOldest_();
  String path = String(QUEUE_DIR) + "/" + eventId + ".json";
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[sync] failed to open %s for write\n", path.c_str());
    return false;
  }
  JsonDocument doc;
  doc["event_id"]    = eventId;
  doc["card_uid"]    = uidHex;
  doc["captured_at"] = iso;
  serializeJson(doc, f);
  f.close();
  queueDepth = queueCount_();
  return true;
}

// Forward decl — feedback rendering lives next to the other OLED frames.
static void renderCardFeedback_();

// HTTPS POST one queued event. Returns true and fills outResp on success
// (including idempotency-hit success); returns false on any failure and
// sets currentFeedback + lastFailureReason appropriately.
static bool sendTap_(const String& path) {
  if (bearerToken.length() < 8 || receiverUrl.length() < 16) {
    lastFailureReason = "receiver_misconfig";
    currentFeedback = CardFeedback::AuthFail;
    return false;
  }
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    lastFailureReason = "queue_read";
    return false;
  }
  String body = f.readString();
  f.close();

  WiFiClientSecure client;
  client.setInsecure();              // research R5 — LAN-trust model
  HTTPClient http;
  http.setTimeout(HTTPS_TIMEOUT_MS);
  // Apps Script /exec returns 302 to script.googleusercontent.com/macros/echo
  // where the actual response JSON is staged. Built-in HTTPClient redirect
  // handling doesn't reliably downgrade POST → GET on Apps Script's specific
  // redirect shape, so we follow manually.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* collectHeaders[] = {"Location"};
  http.collectHeaders(collectHeaders, 1);
  String url = receiverUrl;
  if (url.indexOf("?route=tap") < 0) url += (url.indexOf('?') < 0 ? "?route=tap" : "&route=tap");
  url += "&token=" + urlEncode_(bearerToken);
  if (!http.begin(client, url)) {
    lastFailureReason = "transport";
    currentFeedback = CardFeedback::TransportFail;
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + bearerToken);

  Serial.printf("[sync] POST body: %s\n", body.c_str());
  int httpCode = http.POST(body);
  String resp = http.getString();
  if (httpCode == 302) {
    const String location = http.header("Location");
    http.end();
    if (location.length() == 0) {
      lastFailureReason = "transport";
      currentFeedback = CardFeedback::TransportFail;
      return false;
    }
    WiFiClientSecure client2;
    client2.setInsecure();
    HTTPClient http2;
    http2.setTimeout(HTTPS_TIMEOUT_MS);
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http2.begin(client2, location)) {
      lastFailureReason = "transport";
      currentFeedback = CardFeedback::TransportFail;
      return false;
    }
    httpCode = http2.GET();
    resp = http2.getString();
    http2.end();
  } else {
    http.end();
  }

  if (httpCode <= 0) {
    Serial.printf("[sync] POST failed: %d\n", httpCode);
    lastFailureReason = "transport";
    currentFeedback = CardFeedback::TransportFail;
    return false;
  }

  Serial.printf("[sync] HTTP %d, body len=%u\n", httpCode, (unsigned)resp.length());
  if (resp.length() < 400) {
    Serial.printf("[sync] body: %s\n", resp.c_str());
  } else {
    Serial.printf("[sync] body[0..200]: %.200s\n", resp.c_str());
  }
  JsonDocument respDoc;
  const DeserializationError err = deserializeJson(respDoc, resp);
  if (err) {
    Serial.printf("[sync] response parse error: %s\n", err.c_str());
    lastFailureReason = "transport";
    currentFeedback = CardFeedback::TransportFail;
    return false;
  }
  const char* status = respDoc["status"] | "";
  if (strcmp(status, "ok") == 0) {
    const char* name = respDoc["name"] | nullptr;
    const float hours = respDoc["hours_today"] | 0.0f;
    if (name && strlen(name) > 0) {
      strlcpy(feedbackName, name, sizeof(feedbackName));
      feedbackHours = hours;
      currentFeedback = CardFeedback::Registered;
    } else {
      currentFeedback = CardFeedback::Unregistered;
    }
    cardScreenUntilMs = millis() + CARD_DISPLAY_MS;
    lastFailureReason = nullptr;
    lastSyncOkAtEpoch = time(nullptr);
    ++eventsDeliveredToday;
    return true;
  }
  const char* errCat = respDoc["error"] | "";
  if (strcmp(errCat, "unauthenticated") == 0 ||
      strcmp(errCat, "receiver_misconfig") == 0) {
    lastFailureReason = errCat;
    currentFeedback = CardFeedback::AuthFail;
    cardScreenUntilMs = millis() + CARD_DISPLAY_MS;
    return false;
  }
  // bad_request / schema_mismatch / anything else: treat as transport-ish
  // so the drain loop backs off but keeps the event queued.
  Serial.printf("[sync] receiver responded %s / %s\n", status, errCat);
  lastFailureReason = errCat[0] ? errCat : "transport";
  currentFeedback = CardFeedback::TransportFail;
  return false;
}

// ---- OTA: poll latest.txt, compare, self-flash if newer ------------------
//
// Manifest format on GitHub (single line, no whitespace beyond a trailing
// newline): `v1.2.3`. Compared lexicographically against FIRMWARE_VERSION.
// On a newer tag, the device downloads
//   https://github.com/.../releases/download/<tag>/firmware.bin
// via HTTPUpdate which streams it into the OTA partition, verifies, and
// reboots. No-op if running version >= manifest version.
//
// Returns true if an update was initiated (caller should expect reboot).
// Force-render the OTA frame synchronously. The single-render-controller
// in loop() can't service us here because checkOtaOnce_ blocks for tens of
// seconds inside HTTPUpdate.update(). So we paint directly.
static void otaFlash_(CardFeedback fb) {
  currentFeedback = fb;
  cardScreenUntilMs = millis() + 30UL * 1000UL; // suppress marquee return
  renderCardFeedback_();
}

// HTTPUpdate progress callback: throttle redraws to every 5% so we don't
// spam the I²C bus mid-download.
static void otaOnProgress_(int cur, int total) {
  if (total <= 0) return;
  const int pct = (int)((int64_t)cur * 100 / total);
  if (pct == otaProgressPct) return;
  if (pct < 100 && pct - otaProgressPct < 5 && otaProgressPct >= 0) return;
  otaProgressPct = pct;
  renderCardFeedback_();
}

static bool checkOtaOnce_() {
  otaFlash_(CardFeedback::OtaChecking);

  // GET /releases/latest — DON'T follow the redirect; we want the Location
  // header itself, which carries the latest tag. cache-control: no-cache, so
  // the tag we see here is always fresh (no 5-min CDN delay).
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTPS_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* collectHeaders[] = {"Location"};
  http.collectHeaders(collectHeaders, 1);
  if (!http.begin(client, OTA_LATEST_URL)) {
    Serial.println(F("[ota] latest http.begin failed"));
    return false;
  }
  const int code = http.GET();
  const String location = http.header("Location");
  http.end();
  if (code != 302 || location.length() == 0) {
    Serial.printf("[ota] /releases/latest → HTTP %d (no Location)\n", code);
    cardScreenUntilMs = 0;
    currentFeedback = CardFeedback::None;
    return false;
  }
  // Location looks like: https://github.com/<owner>/<repo>/releases/tag/v0.2.4
  const int tagIdx = location.lastIndexOf("/tag/");
  if (tagIdx < 0) {
    Serial.printf("[ota] unexpected Location: %s\n", location.c_str());
    cardScreenUntilMs = 0;
    currentFeedback = CardFeedback::None;
    return false;
  }
  String latest = location.substring(tagIdx + 5);  // 5 = strlen("/tag/")
  latest.trim();
  if (latest.length() == 0 || latest.length() > 32) {
    Serial.println(F("[ota] parsed tag empty or oversized"));
    cardScreenUntilMs = 0;
    currentFeedback = CardFeedback::None;
    return false;
  }

  Serial.printf("[ota] running=%s latest=%s\n", FIRMWARE_VERSION, latest.c_str());
  // Lexicographic — works as long as you keep the tag scheme zero-padded
  // (v1.02.03) OR you commit to single-digit segments. For most projects
  // semantic-ish tags (v0.0.1 → v0.0.2 → v0.1.0) sort correctly without
  // padding. If you ever cross v0.9.x → v0.10.x, switch to a parser.
  if (!(String(FIRMWARE_VERSION) < latest)) {
    Serial.println(F("[ota] already up to date"));
    cardScreenUntilMs = 0;
    currentFeedback = CardFeedback::None;
    return false;
  }

  String binUrl = String(OTA_BIN_URL_TEMPLATE);
  binUrl.replace("{VERSION}", latest);
  Serial.printf("[ota] downloading %s\n", binUrl.c_str());

  // Announce the download visually before we block.
  strlcpy(otaVersionTag, latest.c_str(), sizeof(otaVersionTag));
  otaProgressPct = -1;
  otaFlash_(CardFeedback::OtaDownloading);

  // httpUpdate downloads, verifies, swaps partitions, and reboots on
  // success. On failure it returns and we keep running the old image.
  httpUpdate.setLedPin(LED_PIN, LOW);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.onProgress(otaOnProgress_);
  // GitHub release URLs 302 to objects.githubusercontent.com — must follow.
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  const t_httpUpdate_return ret = httpUpdate.update(client, binUrl);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[ota] FAILED (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      otaFlash_(CardFeedback::OtaFailed);
      // Keep failure on screen ~5 s so the operator notices.
      cardScreenUntilMs = millis() + 5000;
      return false;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println(F("[ota] no updates (HTTPUpdate)"));
      cardScreenUntilMs = 0;
      currentFeedback = CardFeedback::None;
      return false;
    case HTTP_UPDATE_OK:
      // Should not reach here — rebootOnUpdate(true) means the device
      // resets inside .update().
      Serial.println(F("[ota] OK — rebooting"));
      return true;
  }
  return false;
}

// Drain at most one event per call. Caller handles cadence (loop()).
static void drainOnce_() {
  if (!timeSynced) return;
  if (WiFi.status() != WL_CONNECTED) return;
  const String path = queueOldestFile_();
  if (path.length() == 0) return;
  if (sendTap_(path)) {
    LittleFS.remove(path);
    queueDepth = queueCount_();
    drainBackoffMs = BACKOFF_INIT_MS;
    nextDrainAtMs = millis();    // drain next event immediately
  } else {
    // Hold the file; back off.
    drainBackoffMs = min(drainBackoffMs * 2, BACKOFF_MAX_MS);
    nextDrainAtMs = millis() + drainBackoffMs;
  }
}

// ---- Helpers --------------------------------------------------------------

static String buildApSsid() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t suffix = static_cast<uint16_t>((mac >> 32) & 0xffff);
  char buf[24];
  snprintf(buf, sizeof(buf), "ClockIn-Setup-%04X", suffix);
  return String(buf);
}

static void oledShowConnecting() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.println(F("IdeaSpark Clock-In"));
  oled.drawLine(0, 10, OLED_W - 1, 10, SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(0, 22); oled.println(F("Connecting"));
  oled.setTextSize(1); oled.setCursor(0, 56);
  oled.printf("t=%lus", static_cast<unsigned long>(millis() / 1000));
  oled.display();
}

static void oledShowApMode_text() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(true);
  oled.setCursor(0, 0);  oled.println(F("Setup mode (AP)"));
  oled.drawLine(0, 10, OLED_W - 1, 10, SSD1306_WHITE);
  oled.setCursor(0, 14); oled.println(F("Join WiFi:"));
  oled.setCursor(0, 26); oled.println(apSsid);
  oled.setCursor(0, 40); oled.println(F("Open:"));
  oled.setCursor(0, 52); oled.println(F("http://192.168.4.1"));
  oled.display();
}

static void drawQrAt(QRCode* qrcode, int16_t x, int16_t y) {
  for (uint8_t qy = 0; qy < qrcode->size; ++qy) {
    for (uint8_t qx = 0; qx < qrcode->size; ++qx) {
      if (qrcode_getModule(qrcode, qx, qy)) {
        oled.fillRect(x + qx * QR_PIXELS_PER_MODULE,
                      y + qy * QR_PIXELS_PER_MODULE,
                      QR_PIXELS_PER_MODULE, QR_PIXELS_PER_MODULE,
                      SSD1306_WHITE);
      }
    }
  }
}

static void oledShowApMode_dualQr() {
  static QRCode  qrcodeJoin;
  static uint8_t qrcodeJoinData[QR_BUFFER_BYTES];
  qrcode_initText(&qrcodeJoin, qrcodeJoinData, QR_VERSION, ECC_LOW,
                  apQrPayload.c_str());
  static QRCode  qrcodeUrl;
  static uint8_t qrcodeUrlData[QR_BUFFER_BYTES];
  qrcode_initText(&qrcodeUrl, qrcodeUrlData, QR_VERSION, ECC_LOW,
                  AP_URL_PAYLOAD);

  const int16_t qrJoinX = 29, qrUrlX = 70, qrY = BLUE_ZONE_Y_START;
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  oled.setCursor(qrJoinX + 2, 0); oled.print(F("JOIN"));
  oled.setCursor(qrUrlX  + 2, 0); oled.print(F("OPEN"));
  oled.setCursor(qrJoinX + 2, 8); oled.print(F("WiFi"));
  oled.setCursor(qrUrlX  + 2, 8); oled.print(F("page"));
  drawQrAt(&qrcodeJoin, qrJoinX, qrY);
  drawQrAt(&qrcodeUrl,  qrUrlX,  qrY);
  oled.display();
}

static void rebuildMarqueeText() {
  marqueeText = "WiFi: " + WiFi.SSID() + "   IP: " + staIp;
  for (uint8_t i = 0; i < MARQUEE_TRAILING_SPACES; ++i) marqueeText += " ";
  marqueePixelW = static_cast<uint16_t>(marqueeText.length()) * GLYPH_PX_W;
  marqueeX = 0;
}

static void renderStaFrame() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);

  // Yellow zone, row 0: pinned firmware version (right-aligned so SSID
  // length in the marquee below doesn't crowd it).
  {
    const char* fw = FIRMWARE_VERSION;
    const int16_t fwPx = (int16_t)strlen(fw) * GLYPH_PX_W;
    oled.setCursor(OLED_W - fwPx, 0);
    oled.print(fw);
  }

  // Yellow zone, row 8: marquee
  oled.setCursor(marqueeX, MARQUEE_Y);
  oled.print(marqueeText);
  oled.setCursor(marqueeX + static_cast<int16_t>(marqueePixelW), MARQUEE_Y);
  oled.print(marqueeText);

  // Blue zone: status block
  oled.setCursor(0, 18); oled.println(F("IdeaSpark Clock-In"));
  oled.drawLine(0, 28, OLED_W - 1, 28, SSD1306_WHITE);

  // Spec 008 US5 — second status line: today's delivered count + queue
  // depth + last failure reason if any. Three pieces of info on one row.
  oled.setCursor(0, 36);
  oled.printf("OK %lu  q%lu",
              (unsigned long)eventsDeliveredToday,
              (unsigned long)queueDepth);
  if (lastFailureReason) {
    oled.setCursor(0, 46);
    oled.printf("err %s", lastFailureReason);
  } else if (inRegistrationMode) {
    oled.setCursor(0, 46);
    oled.print(F("registration mode"));
  }

  oled.setCursor(0, 56);
  if (pn532Ready) {
    oled.print(F("Tap card  "));
  } else {
    oled.print(F("RFID off  "));
  }
  oled.printf("RSSI%d", WiFi.RSSI());
  oled.display();
}

// Thai combining-mark detector for printThai_(). Above/below marks have
// zero visual advance — they overlay on the previous base consonant —
// but the bitmap font draws them as normal width-advancing glyphs.
// printThai_() backs the cursor up after each combining mark so the next
// glyph lands at the same X as the base.
static inline bool isThaiCombiningMark_(uint32_t cp) {
  return (cp == 0x0E31)
      || (cp >= 0x0E34 && cp <= 0x0E3A)   // sara i..uu, phinthu
      || (cp >= 0x0E47 && cp <= 0x0E4E);  // mai taikhu..yamakkan
}

// Render a UTF-8 string with proper Thai combining-mark stacking.
//
// For each glyph:
//   - Base consonant/vowel: draw at the current cursor position; cursor
//     advances by the glyph's width. Remember that width as `lastBaseW`.
//   - Combining mark (upper vowel, tone mark, etc.): retreat the cursor
//     by `lastBaseW` so the mark renders OVER the previous base, then
//     restore the cursor to where the base left it. The mark thus has
//     zero visual advance — the next glyph lands after the base, not
//     after the mark.
static void printThai_(const char* s) {
  int16_t lastBaseW = 0;
  while (*s) {
    uint32_t cp;
    int len;
    if ((uint8_t)*s < 0x80) {
      cp = (uint8_t)*s; len = 1;
    } else if (((uint8_t)*s & 0xE0) == 0xC0) {
      cp = ((uint32_t)((uint8_t)s[0] & 0x1F) << 6)
         |  ((uint32_t)((uint8_t)s[1] & 0x3F));
      len = 2;
    } else if (((uint8_t)*s & 0xF0) == 0xE0) {
      cp = ((uint32_t)((uint8_t)s[0] & 0x0F) << 12)
         | ((uint32_t)((uint8_t)s[1] & 0x3F) << 6)
         |  ((uint32_t)((uint8_t)s[2] & 0x3F));
      len = 3;
    } else {
      cp = '?'; len = 1;
    }

    char buf[5] = {0};
    memcpy(buf, s, len);
    s += len;

    const int16_t y = u8gThai.getCursorY();
    if (isThaiCombiningMark_(cp) && lastBaseW > 0) {
      // Retreat by the previous base's width, draw the mark there, then
      // restore the cursor to where the base advanced it.
      const int16_t curX = u8gThai.getCursorX();
      u8gThai.setCursor(curX - lastBaseW, y);
      u8gThai.print(buf);
      u8gThai.setCursor(curX, y);
      // lastBaseW unchanged — the next mark (if any) should still
      // overlay on the SAME base.
    } else {
      const int16_t xBefore = u8gThai.getCursorX();
      u8gThai.print(buf);
      lastBaseW = u8gThai.getCursorX() - xBefore;
    }
  }
}

// Spec 008 card-feedback frames. The variant currently set on
// `currentFeedback` determines what's drawn; one render function so all
// the layout decisions are co-located.
//
// Thai strings: SSD1306 + Adafruit_GFX default font doesn't render Thai
// glyphs (it's an 8-bit code page). For now we render Thai labels as
// transliterated English ("Bat Mai Long Tabian" → "Unregistered card")
// to avoid garbled output. A future task may swap the font for a
// Unicode-capable one if Thai-script rendering is required.
static void renderCardFeedback_() {
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  oled.setCursor(0, 0);
  oled.println(F("Clocking System"));
  oled.drawLine(0, 10, OLED_W - 1, 10, SSD1306_WHITE);

  switch (currentFeedback) {
    case CardFeedback::Syncing:
      oled.setTextSize(2);
      oled.setCursor(0, 22); oled.println(F("Syncing..."));
      oled.setTextSize(1);
      oled.setCursor(0, OLED_H - 8); oled.print(feedbackUidHex);
      break;
    case CardFeedback::Registered:
      oled.setTextSize(2);
      oled.setCursor(0, 16); oled.println(feedbackName);
      oled.setTextSize(1);
      oled.setCursor(0, 40); oled.printf("%.1f hr today", feedbackHours);
      oled.setCursor(0, OLED_H - 8); oled.print(feedbackUidHex);
      break;
    case CardFeedback::Unregistered:
      oled.setTextSize(2);
      oled.setCursor(0, 18); oled.println(F("Unregistered"));
      oled.setTextSize(1);
      oled.setCursor(0, 40); oled.println(F("card"));
      oled.setCursor(0, OLED_H - 8); oled.print(feedbackUidHex);
      break;
    case CardFeedback::AuthFail:
      oled.setTextSize(2);
      oled.setCursor(0, 18); oled.println(F("Check"));
      oled.setCursor(0, 38); oled.println(F("token"));
      break;
    case CardFeedback::TransportFail:
      oled.setTextSize(2);
      oled.setCursor(0, 18); oled.println(F("Sync"));
      oled.setCursor(0, 38); oled.println(F("offline"));
      oled.setTextSize(1);
      oled.setCursor(0, OLED_H - 8);
      oled.printf("q=%lu", (unsigned long)queueDepth);
      break;
    case CardFeedback::TimeWaiting:
      oled.setTextSize(1);
      oled.setCursor(0, 18); oled.println(F("Waiting for time"));
      oled.setCursor(0, 28); oled.println(F("sync (NTP)..."));
      break;
    case CardFeedback::OtaChecking:
      oled.setTextSize(1);
      oled.setCursor(0, 0);  oled.println(F("Firmware update"));
      oled.drawLine(0, 12, OLED_W - 1, 12, SSD1306_WHITE);
      oled.setTextSize(2);
      oled.setCursor(0, 22); oled.println(F("Checking"));
      oled.setTextSize(1);
      oled.setCursor(0, OLED_H - 8);
      oled.printf("now: %s", FIRMWARE_VERSION);
      break;
    case CardFeedback::OtaDownloading:
      oled.setTextSize(1);
      oled.setCursor(0, 0);  oled.println(F("Firmware update"));
      oled.drawLine(0, 12, OLED_W - 1, 12, SSD1306_WHITE);
      oled.setCursor(0, 16); oled.println(F("Downloading"));
      oled.setTextSize(2);
      oled.setCursor(0, 26); oled.println(otaVersionTag);
      // Progress bar across the bottom: 1px frame, fill scales 0..100.
      if (otaProgressPct >= 0) {
        const int barY = OLED_H - 10;
        oled.drawRect(0, barY, OLED_W, 8, SSD1306_WHITE);
        const int fill = (OLED_W - 2) * otaProgressPct / 100;
        if (fill > 0) oled.fillRect(1, barY + 1, fill, 6, SSD1306_WHITE);
      } else {
        oled.setTextSize(1);
        oled.setCursor(0, OLED_H - 8); oled.println(F("starting..."));
      }
      break;
    case CardFeedback::OtaFailed:
      oled.setTextSize(1);
      oled.setCursor(0, 0);  oled.println(F("Firmware update"));
      oled.drawLine(0, 12, OLED_W - 1, 12, SSD1306_WHITE);
      oled.setTextSize(2);
      oled.setCursor(0, 22); oled.println(F("FAILED"));
      oled.setTextSize(1);
      oled.setCursor(0, OLED_H - 8);
      if (otaVersionTag[0]) oled.printf("retry %s later", otaVersionTag);
      else oled.print(F("check manifest"));
      break;
    case CardFeedback::RegPromptTap: {
      // Yellow zone (rows 0-15): only the "Clocking System" header.
      // Blue zone (rows 16-63): label + Thai employee name + countdown
      // + prompt.
      oled.setTextSize(1);
      oled.setCursor(0, 0);  oled.println(F("Clocking System"));
      oled.drawLine(0, 12, OLED_W - 1, 12, SSD1306_WHITE);
      oled.setCursor(0, 18); oled.println(F("Register card for:"));
      u8gThai.setFont(u8g2_font_etl14thai_t);
      u8gThai.setCursor(0, 42);     // baseline-Y; top of glyph ≈ row 30 (blue)
      printThai_(regSelectedEmployee);
      // Countdown — appended in ASCII (GFX font) for clean digits.
      if (regNameSelectedAtMs > 0) {
        const uint32_t elapsed = millis() - regNameSelectedAtMs;
        const int sec = (elapsed >= REG_SESSION_MS)
                          ? 0
                          : (int)((REG_SESSION_MS - elapsed + 999) / 1000);
        const int16_t cx = u8gThai.getCursorX();
        oled.setCursor(cx + 2, 36);     // top-left Y matching the Thai baseline
        oled.printf(" %ds", sec);
      }
      oled.setCursor(0, OLED_H - 8); oled.println(F("Tap card now..."));
      break;
    }
    case CardFeedback::RegHasTap: {
      oled.setTextSize(1);
      oled.setCursor(0, 0);  oled.println(F("Clocking System"));
      oled.drawLine(0, 12, OLED_W - 1, 12, SSD1306_WHITE);
      oled.setCursor(0, 18); oled.println(F("Register card for:"));
      u8gThai.setFont(u8g2_font_etl14thai_t);
      u8gThai.setCursor(0, 42);
      printThai_(regSelectedEmployee);
      if (regNameSelectedAtMs > 0) {
        const uint32_t elapsed = millis() - regNameSelectedAtMs;
        const int sec = (elapsed >= REG_SESSION_MS)
                          ? 0
                          : (int)((REG_SESSION_MS - elapsed + 999) / 1000);
        const int16_t cx = u8gThai.getCursorX();
        oled.setCursor(cx + 2, 36);
        oled.printf(" %ds", sec);
      }
      oled.setCursor(0, OLED_H - 8);
      oled.print(F("UID ")); oled.print(latestTapUid);
      break;
    }
    default:
      // None — should not be drawn; keep the screen blank rather than
      // crash on an unexpected state.
      break;
  }
  oled.display();
}

// One non-blocking card-poll iteration. Sets cardScreenUntilMs if a new card
// is detected.
static void pollPn532() {
  if (!pn532Ready) return;
  if (millis() - lastCardPollMs < CARD_POLL_MS) return;
  lastCardPollMs = millis();

  uint8_t uid[10] = {0};
  uint8_t uidLen  = 0;
  const bool ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen,
                                          CARD_READ_TIMEOUT_MS);
  if (!ok || uidLen == 0) return;

  const bool same = (uidLen == lastUidLen) && memcmp(uid, lastUid, uidLen) == 0;
  if (same && millis() - lastUidSeenMs < CARD_DEDUP_MS) {
    lastUidSeenMs = millis();
    return;
  }
  memcpy(lastUid, uid, uidLen);
  lastUidLen   = uidLen;
  lastUidSeenMs = millis();

  Serial.print(F("UID: "));
  for (uint8_t i = 0; i < uidLen; ++i) Serial.printf("%02X", uid[i]);
  Serial.printf("  (len=%u)\n", uidLen);

  formatUidHex_(uid, uidLen, feedbackUidHex, sizeof(feedbackUidHex));

  // Spec 008 US3: if a registration page is open, route the tap to the
  // latest-tap buffer instead of the sync queue. This prevents the
  // operator's "test tap" during enrollment from accidentally recording
  // a clock-in.
  if (inRegistrationMode) {
    strlcpy(latestTapUid, feedbackUidHex, sizeof(latestTapUid));
    latestTapScannedAtMs = millis();
    Serial.printf("[reg] buffered UID %s for browser\n", feedbackUidHex);
    if (regSelectedEmployee[0]) {
      currentFeedback = CardFeedback::RegHasTap;
      cardScreenUntilMs = millis() + 60000;
    }
    return;
  }

  // Spec 008: enqueue for sync. The OLED feedback (name + hours, or the
  // unregistered banner) lands when the receiver responds via the drain
  // loop. Show "Syncing..." in the meantime.
  if (!timeSynced) {
    currentFeedback = CardFeedback::TimeWaiting;
  } else {
    currentFeedback = CardFeedback::Syncing;
  }
  cardScreenUntilMs = millis() + CARD_DISPLAY_MS;

  char eventId[32];
  buildEventId_(eventId, sizeof(eventId));
  char iso[40];
  formatIso8601Bangkok_(time(nullptr), iso, sizeof(iso));
  if (queueEnqueue_(eventId, feedbackUidHex, iso)) {
    Serial.printf("[sync] queued event_id=%s\n", eventId);
    nextDrainAtMs = millis(); // try to drain immediately
  } else {
    Serial.println(F("[sync] enqueue failed"));
    lastFailureReason = "queue_write";
  }
}

// ---- HTTP handlers (STA mode only) ----------------------------------------

static void handleRoot() {
  String html;
  html.reserve(512);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  html += F("<title>IdeaSpark Clock-In</title>");
  html += F("<style>body{font-family:system-ui;margin:2em;max-width:32em}code{background:#eee;padding:.2em .4em;border-radius:3px}</style>");
  html += F("</head><body><h1>IdeaSpark Clock-In</h1>");
  html += F("<p><strong>Status:</strong> online &middot; joined ");
  html += WiFi.SSID();
  html += F("</p><p><strong>IP:</strong> <code>");
  html += staIp;
  html += F("</code></p><p><strong>RFID:</strong> ");
  html += pn532Ready ? F("ready") : F("not detected");
  html += F("</p><p><strong>Uptime:</strong> ");
  html += String(millis() / 1000);
  html += F(" s</p><hr><p><a href='/reset'>Forget WiFi & enter setup mode</a></p>");
  // Spec 008 sync-health section.
  html += F("<hr><h2>Sync</h2>");
  html += F("<p><strong>Last sync:</strong> ");
  if (lastSyncOkAtEpoch > 0) {
    char buf[32];
    time_t t = lastSyncOkAtEpoch + 7 * 3600;
    struct tm tm; gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    html += buf;
  } else {
    html += F("(never)");
  }
  html += F("</p><p><strong>Delivered today:</strong> ");
  html += String(eventsDeliveredToday);
  html += F("</p><p><strong>Queue depth:</strong> ");
  html += String(queueDepth);
  html += F("</p>");
  if (lastFailureReason) {
    html += F("<p><strong>Last error:</strong> <code>");
    html += lastFailureReason;
    html += F("</code></p>");
  }
  html += F("<p><a href='/admin'>Sync admin (token + receiver URL)</a> · <a href='/register'>Register a card</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

static void handleReset() {
  server.send(200, "text/html",
              F("<html><body><h2>Forgetting WiFi…</h2></body></html>"));
  delay(500);
  // Re-use the global WiFiManager — instantiating a local one here just to
  // clear settings burns ~kB of stack inside the HTTP handler.
  wm.resetSettings();
  ESP.restart();
}

static void handleNotFound() { server.send(404, "text/plain", "not found"); }

// ---- Admin-page PIN auth (spec 008 + operator request) -------------------
//
// Cookie-session login with a single 6-digit PIN. The operator opens any
// protected page → 302 redirect to /login → enters the PIN → device sets
// a session cookie → subsequent requests pass the cookie and skip the
// login screen. XHR endpoints (/api/*) return 401 with a JSON body so the
// page JS can handle session expiry without a redirect mid-fetch.
//
// Sessions live in a small in-memory ring (cleared on reboot). One slot
// per browser; the operator can have a couple of tabs/devices open at once.
static constexpr const char* ADMIN_PIN = "908724";
static constexpr int  SESSION_RING_SZ  = 4;
static String         sessionTokens[SESSION_RING_SZ];
static int            sessionNext      = 0;

static String readCookie_(const char* name) {
  if (!server.hasHeader("Cookie")) return String();
  String raw = server.header("Cookie");
  String key = String(name) + "=";
  int start = raw.indexOf(key);
  if (start < 0) return String();
  // Match either at the start of the string OR after a "; " separator,
  // never as a suffix of a different cookie name.
  if (start > 0 && raw[start - 1] != ' ' && raw[start - 1] != ';') return String();
  start += key.length();
  int end = raw.indexOf(';', start);
  if (end < 0) end = raw.length();
  return raw.substring(start, end);
}

static bool sessionValid_(const String& token) {
  if (token.length() < 16) return false;
  for (int i = 0; i < SESSION_RING_SZ; ++i) {
    if (sessionTokens[i] == token) return true;
  }
  return false;
}

static String sessionCreate_() {
  static const char hex[] = "0123456789abcdef";
  char buf[33] = {0};
  for (int i = 0; i < 32; ++i) {
    buf[i] = hex[esp_random() & 0x0F];
  }
  String token(buf);
  sessionTokens[sessionNext] = token;
  sessionNext = (sessionNext + 1) % SESSION_RING_SZ;
  return token;
}

// Returns true if the request carries a valid session cookie. On false,
// already wrote the appropriate response (302 for HTML clients, 401 JSON
// for /api/*) — caller must just `return`.
static bool requirePin_() {
  const String token = readCookie_("sid");
  if (sessionValid_(token)) return true;

  // Distinguish "browser navigation" from "XHR call". /api/* is always JSON.
  const String uri = server.uri();
  if (uri.startsWith("/api/")) {
    server.send(401, "application/json",
                "{\"status\":\"error\",\"error\":\"unauthenticated\"}");
    return false;
  }
  // Build "next" so post-login redirect returns to original URL.
  String next = uri;
  if (server.args() > 0) {
    next += "?";
    for (int i = 0; i < server.args(); ++i) {
      if (i > 0) next += "&";
      next += server.argName(i);
      next += "=";
      next += server.arg(i);
    }
  }
  // Manual minimal URL-encode for the redirect query (just `=`, `&`, `?`).
  String nextEnc;
  for (size_t i = 0; i < next.length(); ++i) {
    char c = next[i];
    if (c == '&') nextEnc += "%26";
    else if (c == '?') nextEnc += "%3F";
    else if (c == '=') nextEnc += "%3D";
    else if (c == ' ') nextEnc += "%20";
    else nextEnc += c;
  }
  server.sendHeader("Location", String("/login?next=") + nextEnc);
  server.send(302, "text/plain", "auth required");
  return false;
}

static void renderLoginPage_(const char* errMsg) {
  const String next = server.hasArg("next") ? server.arg("next") : String("/");
  String html;
  html.reserve(1024);
  html += F("<!doctype html><html><head><meta charset='utf-8'>");
  html += F("<meta name=viewport content='width=device-width,initial-scale=1'>");
  html += F("<title>Sign in — IdeaSpark</title>");
  html += F("<style>body{font-family:system-ui;background:#f8f8f8;display:flex;");
  html += F("align-items:center;justify-content:center;min-height:90vh;margin:0}");
  html += F("form{background:#fff;padding:2em;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.1);min-width:18em}");
  html += F("h1{margin:0 0 .8em;font-size:1.1em}");
  html += F("input{width:100%;padding:.8em;font-size:1.4em;text-align:center;");
  html += F("letter-spacing:.4em;font-family:monospace;border:1px solid #bbb;border-radius:6px;box-sizing:border-box}");
  html += F("button{margin-top:1em;width:100%;padding:.7em;font-size:1em;border:none;");
  html += F("background:#2563eb;color:#fff;border-radius:6px;cursor:pointer}");
  html += F(".err{color:#b91c1c;margin:.5em 0 0;font-size:.9em}</style></head><body>");
  html += F("<form method=post action='/login'>");
  html += F("<h1>ใส่รหัส PIN 6 หลัก</h1>");
  html += F("<input type='password' name='pin' inputmode='numeric' pattern='[0-9]{6}' maxlength='6' autocomplete='off' autofocus required>");
  html += F("<input type='hidden' name='next' value='"); html += next; html += F("'>");
  html += F("<button>เข้าสู่ระบบ</button>");
  if (errMsg && *errMsg) {
    html += F("<p class=err>"); html += errMsg; html += F("</p>");
  }
  html += F("</form></body></html>");
  server.send(200, "text/html", html);
}

static void handleLoginGet() { renderLoginPage_(nullptr); }

static void handleLoginPost() {
  const String pin = server.hasArg("pin") ? server.arg("pin") : String();
  // Constant-time-ish PIN compare.
  const size_t pinLen = strlen(ADMIN_PIN);
  bool ok = (pin.length() == pinLen);
  if (ok) {
    uint8_t mismatch = 0;
    for (size_t i = 0; i < pinLen; ++i) mismatch |= pin[i] ^ ADMIN_PIN[i];
    ok = (mismatch == 0);
  }
  if (!ok) {
    renderLoginPage_("รหัส PIN ไม่ถูกต้อง");
    return;
  }
  const String token = sessionCreate_();
  // Path=/ so the cookie applies to all routes. Max-Age = 1 day. HttpOnly
  // so JS can't read it. SameSite=Lax tolerates the post→redirect flow.
  server.sendHeader("Set-Cookie",
                    String("sid=") + token +
                    "; Path=/; Max-Age=86400; HttpOnly; SameSite=Lax");
  const String next = server.hasArg("next") ? server.arg("next") : String("/");
  server.sendHeader("Location", next.length() > 0 ? next : String("/"));
  server.send(302, "text/plain", "ok");
}

static void handleLogout() {
  const String token = readCookie_("sid");
  if (token.length() > 0) {
    for (int i = 0; i < SESSION_RING_SZ; ++i) {
      if (sessionTokens[i] == token) sessionTokens[i] = String();
    }
  }
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "logged out");
}

// Spec 008 admin page — operator-facing form to set the bearer token and
// receiver URL. Served only on STA mode + same-LAN (per the spec-007
// trust model).
static void handleAdminGet() {
  if (!requirePin_()) return;
  String html;
  html.reserve(1024);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>");
  html += F("<title>Sync admin — IdeaSpark</title>");
  html += F("<style>body{font-family:system-ui;margin:2em;max-width:32em}label{display:block;margin:.6em 0 .2em}input{width:100%;padding:.4em;font-family:monospace;box-sizing:border-box}button{margin-top:1em;padding:.6em 1.2em}.ok{color:#070}.warn{color:#a40}.banner{padding:.6em 1em;margin:1em 0;border-radius:6px}.banner.ok{background:#d1fae5;color:#065f46}</style>");
  html += F("</head><body><h1>Sync admin</h1>");
  if (server.hasArg("saved")) {
    html += F("<div class='banner ok'>Saved ✓ — bearer ");
    html += bearerToken.length() > 0 ? F("set") : F("empty");
    html += F(", URL ");
    html += receiverUrl.length() > 0 ? F("set") : F("empty");
    html += F("</div>");
  }
  html += F("<p>Configure how this device talks to the Sheets receiver.</p>");
  html += F("<form method=post>");
  html += F("<label>Receiver URL (Apps Script Web App <code>/exec</code>)</label>");
  html += F("<input name=url value='"); html += receiverUrl; html += F("'>");
  html += F("<label>Bearer token (must match <code>BEARER_TOKEN</code> Script Property)</label>");
  html += F("<input name=token placeholder='");
  if (bearerToken.length() > 0) {
    html += F("(set — first 4: ");
    html += bearerToken.substring(0, 4);
    html += F("… — type to replace)");
  } else {
    html += F("(unset)");
  }
  html += F("'>");
  html += F("<button>Save</button>");
  html += F("</form>");
  html += F("<p>Last sync: ");
  if (lastSyncOkAtEpoch > 0) {
    char buf[32];
    time_t t = lastSyncOkAtEpoch + 7 * 3600;
    struct tm tm; gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    html += F("<span class=ok>"); html += buf; html += F("</span>");
  } else {
    html += F("<span class=warn>never</span>");
  }
  html += F(" · queue: "); html += String(queueDepth);
  if (lastFailureReason) {
    html += F(" · last error: <span class=warn>"); html += lastFailureReason; html += F("</span>");
  }
  html += F("</p></body></html>");
  server.send(200, "text/html", html);
}

static void handleAdminPost() {
  if (!requirePin_()) return;
  if (server.hasArg("url")) prefsSaveUrl_(server.arg("url"));
  // Only update the token if a non-empty value was submitted, so the
  // operator can change just the URL without re-typing the token.
  if (server.hasArg("token")) {
    const String t = server.arg("token");
    if (t.length() > 0) prefsSaveBearer_(t);
  }
  server.sendHeader("Location", "/admin?saved=1");
  server.send(302, "text/plain", "saved");
}

// Spec 008 registration page (US3). Served from LittleFS so the HTML is
// editable as an ordinary file (no PROGMEM blob).
static void handleRegisterPage() {
  if (!requirePin_()) return;
  if (!littleFsReady) {
    server.send(500, "text/plain", "littlefs not ready");
    return;
  }
  File f = LittleFS.open("/register.html", FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "register.html missing — upload LittleFS image (pio run --target uploadfs)");
    return;
  }
  // Activate registration mode immediately so any tap during the
  // ~500 ms window before the browser's first poll lands gets routed to
  // the buffer instead of the sync queue. The browser's subsequent polls
  // keep the mode alive via the existing watchdog reset.
  inRegistrationMode = true;
  lastRegPollMs = millis();
  server.streamFile(f, "text/html");
  f.close();
}

// Polled by the registration page every ~500 ms. Returns the most-recent
// scanned UID iff registration mode is active AND the scan is fresh.
// First call activates registration mode (taps stop going to the sync
// queue and start going to the buffer).
static void handleLatestTap() {
  if (!requirePin_()) return;
  lastRegPollMs = millis();

  // Browser passes the currently-selected employee name. Only enter
  // registration mode while a name IS selected. When the dropdown is
  // empty (initial state, or right after a successful register), the
  // page is still polling but taps should flow through the normal sync
  // queue, not be buffered for the page.
  const String requestedName = server.hasArg("name") ? server.arg("name") : String("");
  const bool nameChanged = (requestedName != regSelectedEmployee);
  if (nameChanged) {
    strlcpy(regSelectedEmployee, requestedName.c_str(), sizeof(regSelectedEmployee));
    regNameSelectedAtMs = regSelectedEmployee[0] ? millis() : 0;
  }

  if (regSelectedEmployee[0]) {
    inRegistrationMode = true;
    const CardFeedback want = latestTapUid[0] ? CardFeedback::RegHasTap : CardFeedback::RegPromptTap;
    currentFeedback = want;
    cardScreenUntilMs = millis() + REG_SESSION_MS; // keep the screen up during the session
  } else {
    // No name selected → page is dormant. Exit registration mode so the
    // next physical tap goes into the normal sync queue, and clear any
    // residual feedback screen.
    inRegistrationMode = false;
    latestTapUid[0] = '\0';
    regNameSelectedAtMs = 0;
    if (currentFeedback == CardFeedback::RegPromptTap ||
        currentFeedback == CardFeedback::RegHasTap) {
      currentFeedback = CardFeedback::None;
      cardScreenUntilMs = 0;
    }
  }

  JsonDocument doc;
  if (latestTapUid[0] && (millis() - latestTapScannedAtMs) < REG_TAP_STALE_MS) {
    doc["card_uid"] = latestTapUid;
    doc["scanned_at_ms_ago"] = millis() - latestTapScannedAtMs;
  } else {
    doc["card_uid"] = (const char*)nullptr;
  }
  if (regNameSelectedAtMs > 0) {
    const uint32_t elapsed = millis() - regNameSelectedAtMs;
    doc["seconds_left"] = (elapsed >= REG_SESSION_MS)
                            ? 0
                            : (int)((REG_SESSION_MS - elapsed + 999) / 1000);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Helper used by both relay endpoints: POST or GET to the receiver with
// the device's bearer token, return the receiver's response body to the
// caller verbatim.
static int httpsRelay_(const char* method, const String& routeQuery,
                       const String& bodyOrEmpty, String& outBody) {
  if (bearerToken.length() < 8 || receiverUrl.length() < 16) {
    outBody = "{\"status\":\"error\",\"error\":\"receiver_misconfig\"}";
    return 500;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTPS_TIMEOUT_MS);
  // Manual redirect handling — same protocol as sendTap_. Built-in
  // FORCE_FOLLOW_REDIRECTS doesn't downgrade POST→GET reliably on Apps
  // Script's 302 shape and returns 400 from Google's frontend.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* relayCollectHeaders[] = {"Location"};
  http.collectHeaders(relayCollectHeaders, 1);
  String url = receiverUrl;
  url += (url.indexOf('?') < 0 ? "?" : "&");
  url += routeQuery;
  url += "&token=" + urlEncode_(bearerToken);
  if (!http.begin(client, url)) {
    outBody = "{\"status\":\"error\",\"error\":\"transport\"}";
    return 500;
  }
  http.addHeader("Authorization", String("Bearer ") + bearerToken);
  http.addHeader("Content-Type", "application/json");
  int code = (strcmp(method, "POST") == 0)
    ? http.POST(bodyOrEmpty)
    : http.GET();
  outBody = http.getString();
  Serial.printf("[relay] %s %s → HTTP %d, body len=%u\n",
                method, routeQuery.c_str(), code, (unsigned)outBody.length());
  if (code == 302) {
    const String location = http.header("Location");
    Serial.printf("[relay] Location: %.120s\n", location.c_str());
    http.end();
    if (location.length() == 0) {
      outBody = "{\"status\":\"error\",\"error\":\"transport\"}";
      return 500;
    }
    WiFiClientSecure client2;
    client2.setInsecure();
    HTTPClient http2;
    http2.setTimeout(HTTPS_TIMEOUT_MS);
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http2.begin(client2, location)) {
      outBody = "{\"status\":\"error\",\"error\":\"transport\"}";
      return 500;
    }
    code = http2.GET();
    outBody = http2.getString();
    http2.end();
    Serial.printf("[relay] follow → HTTP %d, body len=%u\n", code, (unsigned)outBody.length());
    if (outBody.length() < 200) {
      Serial.printf("[relay] body: %s\n", outBody.c_str());
    }
  } else {
    http.end();
  }
  if (code <= 0) {
    outBody = "{\"status\":\"error\",\"error\":\"transport\"}";
    return 500;
  }
  return code;
}

// Browser → device → receiver. The device adds the bearer token so the
// browser never has to know it.
static void handleEmployeesRelay() {
  if (!requirePin_()) return;
  String body;
  int code = httpsRelay_("GET", "route=employees", "", body);
  server.send(code, "application/json", body);
}

// Ad-hoc tap test — fires a /tap to the receiver using the device's
// stored bearer token and current wall clock. Lets the operator (or a
// debug curl) verify that a given UID resolves to a name without
// touching the physical card. Gated by ?pin=<ADMIN_PIN> so it can be
// hit with a one-line curl that doesn't need the cookie dance.
//
// curl 'http://<ip>/api/test-tap?pin=908724&uid=3C36A31E'
static void handleTestTap() {
  const String pin = server.hasArg("pin") ? server.arg("pin") : String();
  if (pin != ADMIN_PIN) {
    server.send(401, "application/json",
                "{\"status\":\"error\",\"error\":\"bad_pin\"}");
    return;
  }
  const String uid = server.hasArg("uid") ? server.arg("uid") : String();
  if (uid.length() == 0) {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"error\":\"missing_uid\"}");
    return;
  }
  char iso[40];
  formatIso8601Bangkok_(time(nullptr), iso, sizeof(iso));
  char eventId[32];
  buildEventId_(eventId, sizeof(eventId));
  // Build the JSON tap body manually to keep the path simple.
  String body = String("{\"card_uid\":\"") + uid +
                "\",\"captured_at\":\"" + iso +
                "\",\"event_id\":\"test-" + eventId + "\"}";
  String out;
  int code = httpsRelay_("POST", "route=tap", body, out);
  server.send(code, "application/json", out);
}

static void handleRegisterRelay() {
  if (!requirePin_()) return;
  if (!server.hasArg("plain")) {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"error\":\"bad_request\"}");
    return;
  }
  const String& reqBody = server.arg("plain");
  String body;
  int code = httpsRelay_("POST", "route=register", reqBody, body);
  // On successful register, exit registration mode so subsequent taps
  // resume normal sync flow.
  JsonDocument doc;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    const char* status = doc["status"] | "";
    if (strcmp(status, "ok") == 0) {
      inRegistrationMode = false;
      latestTapUid[0] = '\0';
      regSelectedEmployee[0] = '\0';
      currentFeedback = CardFeedback::None;
      cardScreenUntilMs = 0;
    }
  }
  server.send(code, "application/json", body);
}

// Spec 008 sync-health endpoint: small JSON for the registration page and
// for any other monitoring. Kept distinct from /admin so the form's GET
// isn't confused with a polled state read.
static void handleStatus() {
  JsonDocument doc;
  doc["queue_depth"] = queueDepth;
  doc["delivered_today"] = eventsDeliveredToday;
  doc["last_sync_epoch"] = lastSyncOkAtEpoch;
  doc["time_synced"] = timeSynced;
  doc["bearer_set"] = bearerToken.length() > 0;
  doc["receiver_set"] = receiverUrl.length() > 0;
  doc["last_failure"] = lastFailureReason ? lastFailureReason : "";
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// /diag — wire-check probe. Runs a fresh I²C bus scan, samples MISO idle
// voltage, and re-issues a PN532 firmware-version request. From the
// observed outcomes, classifies each of the 10 wires (4 OLED + 4 PN532
// data + 2 power) as `ok` / `likely_broken` / `uncertain`.
//
// Safe to call as often as the operator wants — non-destructive (no
// writes, no display disruption beyond the brief MISO pinMode flip).
static void handleDiag() {
  JsonDocument doc;

  // ---- I²C side: bus scan + OLED address check ----
  JsonArray addrs = doc["i2c"]["addresses"].to<JsonArray>();
  bool oledFound = false;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X", a);
      addrs.add(buf);
      if (a == OLED_ADDR) oledFound = true;
    }
  }
  doc["i2c"]["oled_present"] = oledFound;

  // ---- SPI side: MISO idle voltage + PN532 firmware probe ----
  // Sample MISO with internal pull-up enabled. If MISO is connected to a
  // healthy PN532, the chip's own pull-up (or our pin pull-up) holds it
  // HIGH. A persistently floating line reads HIGH from the pull-up too,
  // so this only catches gross issues (shorted-to-ground or wire
  // disconnected at the ESP32 side breaking the pull-up).
  pinMode(PN532_MISO, INPUT_PULLUP);
  delay(2);
  const bool misoIdleHigh = (digitalRead(PN532_MISO) == HIGH);
  doc["spi"]["miso_idle_high"] = misoIdleHigh;

  const uint32_t ver = nfc.getFirmwareVersion();
  const bool pn532Ok = (ver != 0);
  if (pn532Ok) {
    char chipBuf[8];
    snprintf(chipBuf, sizeof(chipBuf), "0x%02X", (uint8_t)((ver >> 24) & 0xFF));
    doc["spi"]["pn532_chip"] = chipBuf;
    char verBuf[8];
    snprintf(verBuf, sizeof(verBuf), "%u.%u",
             (uint8_t)((ver >> 16) & 0xFF),
             (uint8_t)((ver >> 8)  & 0xFF));
    doc["spi"]["pn532_firmware"] = verBuf;
  } else {
    doc["spi"]["pn532_firmware"] = (const char*)nullptr;
  }

  // ---- Per-wire verdicts ----
  // Logic:
  //   - OLED on I²C: 0x3C ACK proves SDA + SCL + 3V3 + GND on the OLED
  //     all work (chip wouldn't respond otherwise).
  //   - I²C scan finding ANY device but not 0x3C → bus is OK, OLED chip
  //     itself isn't responding (most likely VCC/GND on the OLED).
  //   - I²C scan finding nothing → SDA or SCL is broken (whole bus dead).
  //   - PN532 firmware reply proves SCK + MISO + MOSI + SS + 3V3 + GND
  //     are all wired (chip can't reply without all of those).
  //   - PN532 silent + MISO idle HIGH → wires reaching the chip have at
  //     least a pull-up. SS/MOSI/SCK/VCC could still be broken.
  //   - PN532 silent + MISO idle LOW → either MISO is grounded
  //     (likely) or the chip is held in reset / lacks power.
  JsonObject v = doc["verdict"].to<JsonObject>();
  const bool i2cBusAlive = (addrs.size() > 0);

  v["OLED.SDA"] = i2cBusAlive ? "ok" : "likely_broken";
  v["OLED.SCL"] = i2cBusAlive ? "ok" : "likely_broken";
  v["OLED.VCC"] = oledFound ? "ok" : (i2cBusAlive ? "likely_broken" : "uncertain");
  v["OLED.GND"] = oledFound ? "ok" : (i2cBusAlive ? "likely_broken" : "uncertain");

  v["PN532.SCK"]  = pn532Ok ? "ok" : "likely_broken";
  v["PN532.MISO"] = pn532Ok ? "ok"
                            : (misoIdleHigh ? "ok" : "likely_broken");
  v["PN532.MOSI"] = pn532Ok ? "ok" : "likely_broken";
  v["PN532.SS"]   = pn532Ok ? "ok" : "likely_broken";
  v["PN532.VCC"]  = pn532Ok ? "ok"
                            : (misoIdleHigh ? "uncertain" : "likely_broken");
  v["PN532.GND"]  = pn532Ok ? "ok"
                            : (misoIdleHigh ? "uncertain" : "likely_broken");

  // Hints — human-readable summary of what to wiggle next.
  JsonArray hints = doc["hints"].to<JsonArray>();
  if (!i2cBusAlive && !pn532Ok) {
    hints.add("Both buses dead — check that the 3V3 + GND wires reach both modules.");
  }
  if (i2cBusAlive && !oledFound) {
    hints.add("I²C bus works but OLED at 0x3C did not ACK — check OLED VCC + GND.");
  }
  if (!i2cBusAlive) {
    hints.add("I²C bus silent — check OLED SDA (GPIO 21) and SCL (GPIO 22).");
  }
  if (!pn532Ok && !misoIdleHigh) {
    hints.add("PN532 silent and MISO floats LOW — check MISO (GPIO 19) and PN532 VCC/GND.");
  }
  if (!pn532Ok && misoIdleHigh) {
    hints.add("PN532 silent but MISO has pull-up — check SCK (GPIO 18), MOSI (GPIO 23), or SS (GPIO 5).");
  }
  if (oledFound && pn532Ok) {
    hints.add("All 10 wires verified OK.");
  }

  String out;
  serializeJsonPretty(doc, out);
  server.send(200, "application/json", out);
}


// ---- Lifecycle ------------------------------------------------------------

static void enterStaMode();

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== Stage-7: WiFi + marquee + PN532 SPI ==="));

  pinMode(LED_PIN, OUTPUT);

  Wire.begin();
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("FATAL: SSD1306 init failed"));
    for (;;) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
  }
  // Crank contrast + bump oscillator to ~127 Hz so phone cameras can read
  // the QR codes without aliasing (see OLED_CONTRAST_MAX / OLED_CLOCKDIV_127HZ).
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(OLED_CONTRAST_MAX);
  oled.ssd1306_command(SSD1306_SETDISPLAYCLOCKDIV);
  oled.ssd1306_command(OLED_CLOCKDIV_127HZ);

  // Bring up u8g2 for Unicode (Thai) text rendering. Shares the SSD1306
  // framebuffer with Adafruit_GFX. Font etl14thai_t is 14 px tall and
  // covers the Thai Unicode block (U+0E01..U+0E5F) plus ASCII.
  u8gThai.begin(oled);
  u8gThai.setFontMode(1);                         // transparent (preserves underlying pixels)
  u8gThai.setForegroundColor(SSD1306_WHITE);
  u8gThai.setFontDirection(0);                    // L→R

  // Spec 008: mount LittleFS + load saved bearer/url before WiFi attempts.
  littleFsReady = littleFsBoot_();
  prefsLoad_();
  Serial.printf("[sync] LittleFS=%s bearer=%s url=%s queue=%lu\n",
                littleFsReady ? "ok" : "fail",
                bearerToken.length() > 0 ? "set" : "unset",
                receiverUrl.length() > 0 ? "set" : "unset",
                (unsigned long)queueCount_());
  queueDepth = queueCount_();

  apSsid = buildApSsid();
  Serial.printf("AP SSID: %s\n", apSsid.c_str());
  oledShowConnecting();

  wm.setDebugOutput(true);
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  wm.setConfigPortalTimeout(0);
  wm.setConfigPortalBlocking(false);
  apQrPayload = "WIFI:T:nopass;S:" + apSsid + ";;";
  wm.setAPCallback([](WiFiManager* /*wm*/) {
    inApMode = true; apScreen = 0; lastApSwitchMs = millis();
    oledShowApMode_text();
    Serial.printf("AP mode. QR: %s\n", apQrPayload.c_str());
  });

  const bool joined = wm.autoConnect(apSsid.c_str(), nullptr);
  if (joined) {
    enterStaMode();
  } else {
    inApMode = true;
    Serial.println(F("AP up — captive portal running."));
  }
}

static void enterStaMode() {
  inApMode = false;
  staIp = WiFi.localIP().toString();
  Serial.printf("Joined: ssid=%s ip=%s rssi=%d\n",
                WiFi.SSID().c_str(), staIp.c_str(), WiFi.RSSI());

  // Bring up PN532 over SPI. Same flow proven in stage-6b.
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  delay(100);
  nfc.begin();
  delay(100);
  uint32_t ver = 0;
  for (int i = 0; i < 5 && !ver; ++i) {
    ver = nfc.getFirmwareVersion();
    if (!ver) { delay(100); }
  }
  if (ver) {
    pn532Ready = true;
    nfc.SAMConfig();
    Serial.printf("PN5%02X firmware %u.%u\n",
                  (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
  } else {
    Serial.println(F("PN532 not detected — continuing without RFID"));
  }

  rebuildMarqueeText();
  renderStaFrame();

  if (!wmHttpStarted) {
    server.on("/", handleRoot);
    server.on("/reset", handleReset);
    // Spec 008 admin endpoints — see below the handleNotFound declaration.
    server.on("/admin", HTTP_GET, handleAdminGet);
    server.on("/admin", HTTP_POST, handleAdminPost);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/diag", HTTP_GET, handleDiag);
    server.on("/register", HTTP_GET, handleRegisterPage);
    server.on("/api/latest-tap", HTTP_GET, handleLatestTap);
    server.on("/api/employees-relay", HTTP_GET, handleEmployeesRelay);
    server.on("/api/register-relay", HTTP_POST, handleRegisterRelay);
    server.on("/api/test-tap", HTTP_GET, handleTestTap);
    server.onNotFound(handleNotFound);
    // Required so requirePin_() can read the session cookie.
    {
      const char* hdrs[] = {"Cookie"};
      server.collectHeaders(hdrs, 1);
    }
    server.on("/login",  HTTP_GET,  handleLoginGet);
    server.on("/login",  HTTP_POST, handleLoginPost);
    server.on("/logout", HTTP_GET,  handleLogout);
    server.begin();
    wmHttpStarted = true;
    Serial.println(F("HTTP server up."));
  }

  // Spec 008: kick off NTP. Asia/Bangkok = UTC+7. configTzTime sets both
  // the offset and the POSIX TZ env var. We don't block here — the loop
  // checks `time(nullptr)` validity before draining (research R-time-sync).
  configTzTime("ICT-7", "pool.ntp.org", "time.google.com");
  Serial.println(F("[sync] NTP configured (Asia/Bangkok)"));

  // Boot-time OTA check: every reboot polls GitHub for a newer firmware
  // tag and flashes it before resuming normal operation. If an update is
  // applied the device resets inside checkOtaOnce_(); on no-op or failure
  // we continue with the running image.
  Serial.printf("[ota] boot check (running=%s)\n", FIRMWARE_VERSION);
  checkOtaOnce_();
  lastOtaCheckMs = millis();
}

void loop() {
  digitalWrite(LED_PIN, (millis() / 500) % 2 == 0 ? HIGH : LOW);

  if (!inApMode) {
    server.handleClient();
    pollPn532();

    // Capture `now` AFTER the handlers run. Earlier we captured at the top
    // of loop(), but handleClient() may call handleLatestTap() which stamps
    // lastRegPollMs with a fresh millis() — making lastRegPollMs greater
    // than a stale `now`. The unsigned `now - lastRegPollMs` then underflows
    // to a huge value and trips watchdogs immediately.
    const uint32_t now = millis();

    // Spec 008: NTP sync gate. Once `time(nullptr)` returns a sane epoch
    // (post-2024), we're in business; the device can timestamp events and
    // drain. Until then, queued events wait.
    if (!timeSynced && time(nullptr) > 1700000000) {
      timeSynced = true;
      Serial.printf("[sync] NTP synced (epoch=%lu)\n", (unsigned long)time(nullptr));
      nextDrainAtMs = now;
    }

    // Drain at most one queued event per loop tick. Backoff is managed
    // inside drainOnce_ on failure; on success it resets so the next
    // queued event is attempted immediately on the very next tick.
    if (now >= nextDrainAtMs && now - lastDrainTickMs >= DRAIN_TICK_MS) {
      lastDrainTickMs = now;
      drainOnce_();
    }

    // Periodic OTA check — once per hour. Skips silently if no newer
    // tag; reboots into the new image if a newer tag is found.
    if (now - lastOtaCheckMs > OTA_CHECK_INTERVAL_MS) {
      lastOtaCheckMs = now;
      checkOtaOnce_();
    }

    // Spec 008 US3 watchdog: if no poll has hit /api/latest-tap for
    // REG_WATCHDOG_MS, assume the browser closed and exit registration
    // mode so subsequent taps resume the sync flow.
    if (inRegistrationMode && (now - lastRegPollMs > REG_WATCHDOG_MS)) {
      inRegistrationMode = false;
      latestTapUid[0] = '\0';
      regSelectedEmployee[0] = '\0';
      regNameSelectedAtMs = 0;
      if (currentFeedback == CardFeedback::RegPromptTap ||
          currentFeedback == CardFeedback::RegHasTap) {
        currentFeedback = CardFeedback::None;
        cardScreenUntilMs = 0;
      }
      Serial.println(F("[reg] watchdog expired — exit registration mode"));
    }

    // 60s session timer: if the operator picked a name but never clicked
    // ลงทะเบียน, auto-clear so the device returns to its normal idle.
    if (regNameSelectedAtMs > 0 && now - regNameSelectedAtMs > REG_SESSION_MS) {
      regSelectedEmployee[0] = '\0';
      regNameSelectedAtMs = 0;
      latestTapUid[0] = '\0';
      currentFeedback = CardFeedback::None;
      cardScreenUntilMs = 0;
      Serial.println(F("[reg] 60s session expired — clear name"));
    }

    // Single display controller. All HTTP handlers and the tap path only
    // mutate state (currentFeedback, cardScreenUntilMs, latestTapUid, etc.);
    // this block is the ONLY place that touches the OLED in STA mode.
    //
    // Decision: if cardScreenUntilMs is in the future → render the
    // feedback frame (and only redraw when state changes). Otherwise →
    // animate the marquee at MARQUEE_FRAME_MS cadence.
    if (cardScreenUntilMs && now >= cardScreenUntilMs) {
      cardScreenUntilMs = 0;
      marqueeX = 0;       // restart marquee from the left for a clean entrance
      lastFrameMs = 0;
      currentFeedback = CardFeedback::None;
    }

    static CardFeedback lastDrawnFeedback = CardFeedback::None;
    static char         lastDrawnUid[24]  = {0};
    static float        lastDrawnHours    = -1.0f;
    static char         lastDrawnName[64] = {0};
    static char         lastDrawnRegEmp[64] = {0};
    static int          lastDrawnRegSec  = -1;

    if (cardScreenUntilMs != 0) {
      // Compute current countdown seconds. Used as part of change-detection
      // so the OLED ticks once per second while a name is selected.
      int regSec = -1;
      if (regNameSelectedAtMs > 0) {
        const uint32_t elapsed = millis() - regNameSelectedAtMs;
        regSec = (elapsed >= REG_SESSION_MS)
                  ? 0
                  : (int)((REG_SESSION_MS - elapsed + 999) / 1000);
      }
      // Feedback screen active. Redraw ONLY when the visible state changes.
      const bool changed = (currentFeedback != lastDrawnFeedback)
        || (strcmp(latestTapUid, lastDrawnUid) != 0)
        || (feedbackHours != lastDrawnHours)
        || (strcmp(feedbackName, lastDrawnName) != 0)
        || (strcmp(regSelectedEmployee, lastDrawnRegEmp) != 0)
        || (regSec != lastDrawnRegSec);
      if (changed) {
        renderCardFeedback_();
        lastDrawnFeedback = currentFeedback;
        strlcpy(lastDrawnUid, latestTapUid, sizeof(lastDrawnUid));
        lastDrawnHours = feedbackHours;
        strlcpy(lastDrawnName, feedbackName, sizeof(lastDrawnName));
        strlcpy(lastDrawnRegEmp, regSelectedEmployee, sizeof(lastDrawnRegEmp));
        lastDrawnRegSec = regSec;
      }
    } else {
      // No feedback active → marquee.
      if (now - lastFrameMs >= MARQUEE_FRAME_MS) {
        lastFrameMs = now;
        marqueeX -= MARQUEE_PX_PER_FRAME;
        if (marqueeX <= -static_cast<int16_t>(marqueePixelW)) marqueeX = 0;
        renderStaFrame();
      }
      lastDrawnFeedback = CardFeedback::None;  // reset so re-entry triggers fresh draw
    }

    if (now - lastSerialMs >= 10000) {
      lastSerialMs = now;
      Serial.printf("[%6lu] ip=%s rssi=%d heap=%lu rfid=%s\n",
                    static_cast<unsigned long>(now), staIp.c_str(),
                    WiFi.RSSI(),
                    static_cast<unsigned long>(ESP.getFreeHeap()),
                    pn532Ready ? "y" : "n");
    }
  } else {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) { enterStaMode(); return; }

    const uint32_t now = millis();
    if (now - lastApSwitchMs >= AP_SCREEN_SWITCH_MS) {
      lastApSwitchMs = now;
      apScreen = (apScreen + 1) % AP_SCREEN_COUNT;
      switch (apScreen) {
        case 0: oledShowApMode_text();   break;
        case 1: oledShowApMode_dualQr(); break;
      }
    }
    if (now - lastSerialMs >= 5000) {
      lastSerialMs = now;
      Serial.printf("[%6lu] AP=%s screen=%u heap=%lu\n",
                    static_cast<unsigned long>(now), apSsid.c_str(),
                    apScreen,
                    static_cast<unsigned long>(ESP.getFreeHeap()));
    }
  }
}
