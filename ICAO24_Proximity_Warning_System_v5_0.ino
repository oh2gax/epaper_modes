#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ctime>
#include <climits>
#include <cstring>
#include <cctype>
#include <cstdlib>                // strtoul() for the Russian ICAO24 range check
#include <cmath>                  // lroundf() for the Russian-flag star icon's vertices
#include <WebServer.h>           // synchronous server (stable) - status/restart only now
#include <vector>
#include <algorithm>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include "secrets.h"              // WiFi credentials - gitignored, see secrets.h.example

// APWS = Aircraft Proximity Warning System
// ESP32 E-Paper display that polls the modes_logger JSON API
// (http://.../api/liveflights) every 10s and shows whatever aircraft the
// server currently has flagged - either on one of its own mil/gov/civ/
// eastern watchlists, or squawking an emergency code (7500/7600/7700).
// Falls back to an EFHK METAR/TAF display when nothing is flagged.
//
// There is no aircraft list stored on the device: modes_logger decides
// what's flagged and reports it per-aircraft via "alert" and
// "squawk_alarm" in the JSON, and the device just displays whatever comes
// back flagged. To watch a specific aircraft that isn't on an official
// list, add it through modes_logger's own /admin watchlist page.
//
// Multiple simultaneously-flagged aircraft are shown together as a
// compact table. An active squawk alarm draws an inverse-video banner and
// forces an immediate screen update.
//
// The onboard web server is a minimal read-only status page (WiFi RSSI,
// free heap, flagged count, last-fetch status) plus /restart - there is
// nothing to configure locally, so no config form.
//
// Stability measures: an ESP32 task watchdog with self-restart after
// repeated fetch failures, WiFi auto-reconnect, explicit timeouts on every
// HTTP request, JSON parsed by streaming the HTTP response directly
// (no full-body String buffer), and a single reused JSON document instead
// of a fresh heap allocation per poll, to keep heap fragmentation low over
// weeks/months of uptime.
//
// 20.09.2026 / Version 5.0 / OH2GAX

// ==== Display ==== //
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

GxEPD2_BW<GxEPD2_420, GxEPD2_420::HEIGHT> display(GxEPD2_420(/*CS=*/22, /*DC=*/15, /*RST=*/13, /*BUSY=*/34));

// ==== WiFi ==== //
// Actual values live in secrets.h (gitignored) - copy secrets.h.example to
// secrets.h and fill in your own SSID/password before building.
const char *WIFI_SSID = SECRET_WIFI_SSID;
const char *WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

// ==== Aircraft data source (modes_logger) ==== //
const char *AIRCRAFT_API_URL = "http://blog.oh2gax.org:5000/api/liveflights";

// ==== Timing ==== //
static const unsigned long FETCH_INTERVAL_MS        = 10UL * 1000UL;         // poll aircraft feed every 10s
static const unsigned long DISPLAY_MIN_REFRESH_MS    = 60UL * 1000UL;        // normal aircraft-table redraw throttle
// METAR/TAF is refreshed on a wall-clock grid aligned to :00 and :30 (EFHK
// METARs are normally published around :20 and :50, so fetching right at
// :00/:30 means the latest one is always already out by the time we ask).
static const unsigned long WEATHER_SLOT_SECONDS      = 30UL * 60UL;
static const unsigned long NO_TARGET_REVERT_MS       = 60UL * 1000UL;        // how long to keep showing the aircraft screen after the last target disappears
static const unsigned long WIFI_RECONNECT_INTERVAL_MS = 15UL * 1000UL;
static const unsigned long HEAP_LOG_INTERVAL_MS       = 5UL * 60UL * 1000UL;
static const int DAYTIME_START_HOUR = 3;  // UTC
static const int DAYTIME_END_HOUR   = 21; // UTC

// If the aircraft feed fails this many polls in a row (~5 min at 10s/poll),
// assume the device is wedged (network stack, heap exhaustion, etc.) and
// self-restart rather than staying silently stuck until someone power
// cycles it.
static const int MAX_CONSECUTIVE_FETCH_FAILURES = 30;
static const uint32_t WDT_TIMEOUT_S = 30; // task watchdog: reset if loop() stalls this long

// ==== Flagged aircraft (server-flagged: alert and/or squawk_alarm) ==== //
#define MAX_FLAGGED_AIRCRAFT 12
#define MAX_DISPLAY_ENTRIES 3 // how many three-line aircraft cards fit on screen at 12pt

struct AircraftHit {
  char icao24[7];
  char registration[12]; // e.g. "RA-73724" - generous headroom, most are 5-8 chars
  char callsign[9];
  char squawk[5];      // transponder code, e.g. "7000", up to 4 digits
  char type[5];       // ICAO type designator, e.g. "A321", up to 4 chars
  long altitude;     // feet, LONG_MIN = unknown
  long speed;        // knots, LONG_MIN = unknown
  long verticalRate;    // ft/min, +climb/-descend, LONG_MIN = unknown
  long selectedAltitude; // FMS/MCP target altitude in feet, LONG_MIN = not available
  long track;         // degrees true, 0-359, LONG_MIN = unknown
  char alert;         // 'M'/'G'/'C'/'E' or 0 = none
  bool squawkAlarm;   // 7500/7600/7700
  bool isRussian;     // ICAO24 falls in Russia's allocated block (0x100000-0x1FFFFF)
};

// Which single warning icon (if any) a card should show - see cardIconKind()
// and drawCardWarningIcon() further down for how this is picked and drawn.
// Declared here, next to AircraftHit, rather than next to the functions
// that use it: the Arduino IDE auto-generates forward declarations for
// every function and inserts them near the top of the translated file,
// ABOVE where a mid-file type would otherwise be defined - so a type used
// in a function signature has to be declared this early, or the
// auto-generated prototype references it before it exists and the sketch
// fails to compile ("'CardIconKind' does not name a type").
enum CardIconKind { ICON_NONE, ICON_SQUAWK_ALARM, ICON_RUSSIAN, ICON_MILITARY };

// Russia's ICAO24 allocation block, exactly as modes_logger's own
// is_russian_icao24() defines it server-side - computed independently
// on-device so the warning icon works regardless of whether
// modes_logger's "eastern-red" toggle happens to be enabled.
static const unsigned long RUSSIA_ICAO24_MIN = 0x100000UL;
static const unsigned long RUSSIA_ICAO24_MAX = 0x1FFFFFUL;

static bool isRussianIcao24(const char *icaoHex) {
  if (!icaoHex || !icaoHex[0]) return false;
  char *endPtr = nullptr;
  unsigned long n = strtoul(icaoHex, &endPtr, 16);
  if (endPtr == icaoHex) return false; // not parseable as hex at all
  return n >= RUSSIA_ICAO24_MIN && n <= RUSSIA_ICAO24_MAX;
}

static AircraftHit flaggedAircraft[MAX_FLAGGED_AIRCRAFT];
static int flaggedCount = 0;
static char apiUpdatedUtc[10] = {0};

// snapshot of the previous flagged set, used to force a redraw as soon as
// the set of visible flagged aircraft (or a squawk alarm) changes, rather
// than waiting up to DISPLAY_MIN_REFRESH_MS
static AircraftHit prevFlaggedAircraft[MAX_FLAGGED_AIRCRAFT];
static int prevFlaggedCount = -1; // -1 = "no previous snapshot yet"

// ==== State ==== //
unsigned long lastFetchTime = 0;
unsigned long lastDisplayUpdateTime = 0;
unsigned long lastWeatherFetchSlot = 0; // epoch/WEATHER_SLOT_SECONDS of the last weather fetch attempt, 0 = never
unsigned long lastWifiAttempt = 0;
unsigned long lastHeapLogTime = 0;
unsigned long lastSuccessfulFetchTime = 0;
unsigned long lastFlaggedTime = 0;   // last time flaggedCount was > 0
bool aircraftScreenShown = false;    // true if the screen currently shows the aircraft table
int consecutiveFetchFailures = 0;
bool weatherApiOk = false;           // did the most recent METAR fetch attempt actually get data back?

// ---- Web server (sync) - status/restart only, no config form ---- //
WebServer server(80);

// ---- Shared JSON document, reused for aircraft/METAR/TAF parsing ---- //
// Reusing one buffer instead of allocating a fresh DynamicJsonDocument for
// every poll avoids repeated large heap alloc/free cycles, which helps
// keep the heap unfragmented over long (weeks/months) uptime.
DynamicJsonDocument sharedJsonDoc(49152);
StaticJsonDocument<448> aircraftFilter; // bumped from 384 to add "registration"

// ==== FWD decls ==== //
void fetchWeatherData();
bool fetchAircraftData();
void processAircraftDoc();
void sortFlaggedByPriority();
void renderAircraftTable();
void ensureWiFiConnected(unsigned long now);
void maybeLogHeap(unsigned long now);
void setupWatchdog();
void handleAircraftResult(unsigned long now);
bool flaggedSetChanged();
void renderIdleScreen(bool showIp = false);

// ===== Helper: centered token-aware word wrap (never splits inside a token) =====
void printWrappedCentered(const String &text, int &y, int lineHeight, int bottomLimitPx) {
  display.setFont(&FreeMonoBold9pt7b);
  const int margin = 10;
  const int maxWidth = display.width() - 2 * margin;

  String line = "";
  int i = 0;
  int n = text.length();

  auto printLine = [&](const String &l) {
    if (l.length() == 0) return;
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(l, 0, 0, &tbx, &tby, &tbw, &tbh);
    int x = ((display.width() - tbw) / 2) - tbx;
    display.setCursor(x, y);
    display.println(l);
    y += lineHeight;
  };

  while (i < n) {
    // handle explicit newlines as hard breaks
    if (text[i] == '\n' || text[i] == '\r') {
      if (line.length()) {
        if (y > bottomLimitPx) return;
        printLine(line);
        line = "";
      }
      while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;
      continue;
    }

    // skip leading spaces
    while (i < n && text[i] == ' ') i++;

    // next token until space/newline
    int start = i;
    while (i < n && text[i] != ' ' && text[i] != '\n' && text[i] != '\r') i++;
    String word = text.substring(start, i);

    // Try appending token to current line
    String testLine = (line.length() ? (line + " " + word) : word);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(testLine, 0, 0, &tbx, &tby, &tbw, &tbh);

    if (tbw <= maxWidth) {
      line = testLine;
    } else {
      // print current line, start a new one with the word
      if (line.length()) {
        if (y > bottomLimitPx) return;
        printLine(line);
        line = word;
      } else {
        // single token longer than line width — print it as-is (still not split)
        if (y > bottomLimitPx) return;
        printLine(word);
        line = "";
      }
    }
  }

  if (line.length() && y <= bottomLimitPx) {
    printLine(line);
  }
}

// -------------------- SETUP -------------------- //
void setup()
{
  Serial.begin(115200);
  display.init();
  display.setRotation(0);
  display.setFont(&FreeMonoBold24pt7b);
  display.setTextColor(GxEPD_BLACK);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // avoid WiFi modem-sleep induced latency/drops on long-running ESP32s
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  timeClient.begin();
  timeClient.setTimeOffset(0); // UTC

  // Filter for /api/liveflights: keep only the fields we actually use, to
  // keep the parsed document small regardless of how many aircraft the
  // receiver currently sees.
  {
    const char *filterJson =
      "{\"aircraft\":[{\"icao24\":true,\"registration\":true,\"callsign\":true,"
      "\"squawk\":true,\"type\":true,\"altitude\":true,\"speed\":true,"
      "\"vertical_rate\":true,\"selected_altitude\":true,\"track\":true,"
      "\"alert\":true,\"squawk_alarm\":true}],\"updated_utc\":true}";
    DeserializationError ferr = deserializeJson(aircraftFilter, filterJson);
    if (ferr) {
      Serial.printf("Failed to build aircraft JSON filter: %s\n", ferr.c_str());
    }
  }

  setupWatchdog();

  // ----- Web routes (sync) - read-only status + restart ----- //
  server.on("/", HTTP_GET, []() {
    char status[400];
    unsigned long sinceFetch = lastSuccessfulFetchTime ? (millis() - lastSuccessfulFetchTime) / 1000UL : 0;
    snprintf(status, sizeof(status),
      "APWS v5.0<br>WiFi RSSI: %d dBm<br>Free heap: %u bytes (min seen: %u)<br>"
      "Flagged now: %d<br>"
      "Last successful fetch: %lus ago &nbsp; Consecutive failures: %d<br>"
      "Weather API: %s (last attempt slot: %lu)",
      WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
      flaggedCount, sinceFetch, consecutiveFetchFailures,
      lastWeatherFetchSlot ? (weatherApiOk ? "OK" : "FAIL") : "not yet checked", lastWeatherFetchSlot);

    String html = String("<html><body>") + status + "<hr>"
                  "<form action=\"/restart\" method=\"POST\">"
                  "<input type=\"submit\" value=\"Restart\">"
                  "</form>"
                  "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/restart", HTTP_POST, []() {
    server.send(200, "text/plain", "Restarting...");
    delay(1000);
    ESP.restart();
  });

  server.begin();

  renderIdleScreen(true); // boot screen only: show the IP address once here
}

// Plain "APWS ONLINE" screen - shown at boot (with the device's IP address
// underneath, once, so it's readable off the panel right after it gets an
// address without needing Serial), and at night once the last flagged
// aircraft has been gone for a while (there's no weather screen to fall
// back to outside the daytime window, so this keeps the display from being
// stuck showing stale aircraft data) - that second case never shows the IP,
// since showIp defaults to false and every other call site leaves it out.
void renderIdleScreen(bool showIp)
{
  display.setFont(&FreeMonoBold24pt7b);
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds("APWS ONLINE", 0, 0, &tbx, &tby, &tbw, &tbh);
    uint16_t x = ((display.width() - tbw) / 2) - tbx;
    uint16_t y = ((display.height() - tbh) / 2) - tby;
    display.setCursor(x, y);
    display.println("APWS ONLINE");

    if (showIp && WiFi.status() == WL_CONNECTED) {
      String ipStr = WiFi.localIP().toString();
      char ipLine[24];
      ipStr.toCharArray(ipLine, sizeof(ipLine));

      display.setFont(&FreeMonoBold12pt7b);
      int16_t ibx, iby;
      uint16_t ibw, ibh;
      display.getTextBounds(ipLine, 0, 0, &ibx, &iby, &ibw, &ibh);
      int ix = ((display.width() - (int)ibw) / 2) - ibx;
      int iy = (int)y + 40; // clear of the 24pt title's descent
      display.setCursor(ix, iy);
      display.println(ipLine);

      display.setFont(&FreeMonoBold24pt7b); // restore in case this loop iterates again
    }
  } while (display.nextPage());
}

// -------------------- WATCHDOG -------------------- //
void setupWatchdog()
{
  // arduino-esp32 core 3.x (IDF5) uses a config struct; core 2.x (IDF4)
  // uses the older (timeout_seconds, panic) signature. Guarding on the IDF
  // major version (rather than the Arduino core version macro) keeps this
  // building correctly regardless of which core the user has installed.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000UL,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#endif
}

// -------------------- LOOP -------------------- //
void loop() {
  esp_task_wdt_reset();
  server.handleClient(); // important for sync WebServer
  unsigned long currentTime = millis();
  timeClient.update();

  ensureWiFiConnected(currentTime);

  if (currentTime - lastFetchTime >= FETCH_INTERVAL_MS) {
    lastFetchTime = currentTime;
    bool ok = fetchAircraftData();
    if (ok) {
      consecutiveFetchFailures = 0;
      lastSuccessfulFetchTime = currentTime;
      handleAircraftResult(currentTime);
    } else {
      consecutiveFetchFailures++;
      Serial.printf("Aircraft fetch failed (%d/%d consecutive)\n",
                     consecutiveFetchFailures, MAX_CONSECUTIVE_FETCH_FAILURES);
      if (consecutiveFetchFailures >= MAX_CONSECUTIVE_FETCH_FAILURES) {
        Serial.println("Too many consecutive fetch failures - restarting.");
        delay(200);
        ESP.restart();
      }
    }
  }

  maybeLogHeap(currentTime);
}

// -------------------- WIFI RECONNECT -------------------- //
void ensureWiFiConnected(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttempt != 0 && now - lastWifiAttempt < WIFI_RECONNECT_INTERVAL_MS) return;
  lastWifiAttempt = now;
  Serial.println("WiFi not connected - attempting reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// -------------------- HEAP DIAGNOSTICS -------------------- //
void maybeLogHeap(unsigned long now) {
  if (lastHeapLogTime != 0 && now - lastHeapLogTime < HEAP_LOG_INTERVAL_MS) return;
  lastHeapLogTime = now;
  Serial.printf("[heap] free=%u minFree=%u maxAlloc=%u uptime=%lus\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(), millis() / 1000UL);
}

// -------------------- AIRCRAFT FETCH (modes_logger JSON API) -------------------- //
bool fetchAircraftData()
{
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);

  if (!http.begin(AIRCRAFT_API_URL)) {
    Serial.println("http.begin failed for aircraft API");
    return false;
  }

  bool ok = false;
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    sharedJsonDoc.clear();
    DeserializationError err = deserializeJson(
      sharedJsonDoc, http.getStream(), DeserializationOption::Filter(aircraftFilter));
    if (!err) {
      if (sharedJsonDoc.overflowed()) {
        Serial.println("Aircraft JSON document overflowed - increase sharedJsonDoc size");
      }
      processAircraftDoc();
      ok = true;
    } else {
      Serial.printf("Aircraft JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("Aircraft API HTTP error: %d\n", httpCode);
  }

  http.end();
  return ok;
}

static void toUpperTrunc(const char *src, char *dst, size_t dstSize) {
  size_t n = strlen(src);
  if (n > dstSize - 1) n = dstSize - 1;
  for (size_t i = 0; i < n; i++) dst[i] = toupper((unsigned char)src[i]);
  dst[n] = 0;
}

// Aircraft are shown purely on what the SERVER flagged - no local watchlist.
// modes_logger reports "alert" (mil/gov/civ/eastern watchlist match) and/or
// "squawk_alarm" (7500/7600/7700) per aircraft; we display any aircraft
// with either set. To track a specific aircraft that isn't on an official
// list, add it to modes_logger's own watchlist via its /admin page.
void processAircraftDoc()
{
  flaggedCount = 0;
  JsonArray arr = sharedJsonDoc["aircraft"].as<JsonArray>();
  for (JsonObject ac : arr) {
    const char *icaoRaw = ac["icao24"] | "";
    if (!icaoRaw[0]) continue;

    const char *alertStr = ac["alert"] | "";
    bool hasAlert = alertStr[0] != 0;
    bool squawkAlarm = ac["squawk_alarm"] | false;

    if (!hasAlert && !squawkAlarm) continue; // nothing flagged on this aircraft
    if (flaggedCount >= MAX_FLAGGED_AIRCRAFT) break;

    char icaoUpper[8];
    toUpperTrunc(icaoRaw, icaoUpper, sizeof(icaoUpper));

    AircraftHit &h = flaggedAircraft[flaggedCount];
    strncpy(h.icao24, icaoUpper, sizeof(h.icao24) - 1);
    h.icao24[sizeof(h.icao24) - 1] = 0;

    const char *reg = ac["registration"] | "";
    strncpy(h.registration, reg, sizeof(h.registration) - 1);
    h.registration[sizeof(h.registration) - 1] = 0;

    const char *cs = ac["callsign"] | "";
    strncpy(h.callsign, cs, sizeof(h.callsign) - 1);
    h.callsign[sizeof(h.callsign) - 1] = 0;
    for (int k = (int)strlen(h.callsign) - 1; k >= 0 && h.callsign[k] == ' '; k--) h.callsign[k] = 0;

    const char *sq = ac["squawk"] | "";
    strncpy(h.squawk, sq, sizeof(h.squawk) - 1);
    h.squawk[sizeof(h.squawk) - 1] = 0;

    const char *typ = ac["type"] | "";
    strncpy(h.type, typ, sizeof(h.type) - 1);
    h.type[sizeof(h.type) - 1] = 0;

    h.altitude = ac["altitude"].isNull() ? LONG_MIN : ac["altitude"].as<long>();
    h.speed    = ac["speed"].isNull()    ? LONG_MIN : ac["speed"].as<long>();
    h.verticalRate    = ac["vertical_rate"].isNull()    ? LONG_MIN : ac["vertical_rate"].as<long>();
    h.selectedAltitude = ac["selected_altitude"].isNull() ? LONG_MIN : ac["selected_altitude"].as<long>();
    h.track = ac["track"].isNull() ? LONG_MIN : ac["track"].as<long>();

    h.alert = 0;
    if (hasAlert) {
      if (!strcmp(alertStr, "mil")) h.alert = 'M';
      else if (!strcmp(alertStr, "gov")) h.alert = 'G';
      else if (!strcmp(alertStr, "civ")) h.alert = 'C';
      else if (!strcmp(alertStr, "eastern")) h.alert = 'E';
      else h.alert = '?';
    }
    h.squawkAlarm = squawkAlarm;
    h.isRussian = isRussianIcao24(h.icao24);

    flaggedCount++;
  }

  const char *upd = sharedJsonDoc["updated_utc"] | "";
  strncpy(apiUpdatedUtc, upd, sizeof(apiUpdatedUtc) - 1);
  apiUpdatedUtc[sizeof(apiUpdatedUtc) - 1] = 0;

  sortFlaggedByPriority();
}

// Reorders flaggedAircraft[0..flaggedCount) so the most urgent aircraft are
// first: squawk alarm (7500/7600/7700) ahead of Russian-flagged ahead of
// military-watchlist-matched ahead of everything else, each tier keeping
// the API's original relative order (stable_sort). This runs BEFORE the
// display picks its first MAX_DISPLAY_ENTRIES cards, so a squawk alarm, a
// Russian-flagged, or a military-flagged aircraft is never silently pushed
// into "+N more" behind lower-priority traffic just because it happened to
// appear later in the API response. Mirrored exactly by cardIconKind()
// below, which uses this same precedence to pick each card's icon.
static int flaggedPriorityTier(const AircraftHit &h) {
  if (h.squawkAlarm) return 0;
  if (h.isRussian) return 1;
  if (h.alert == 'M') return 2;
  return 3;
}

void sortFlaggedByPriority() {
  std::stable_sort(flaggedAircraft, flaggedAircraft + flaggedCount,
    [](const AircraftHit &a, const AircraftHit &b) {
      return flaggedPriorityTier(a) < flaggedPriorityTier(b);
    });
}

// Returns true if the visible flagged set differs from the last time we
// drew the screen (aircraft appeared/disappeared, or a squawk alarm flag
// flipped) - used to redraw immediately instead of waiting for the normal
// 60s throttle, since that's exactly the kind of change a "proximity
// warning" display should not sit on.
//
// This is deliberately ORDER-INDEPENDENT (checks each current aircraft is
// present somewhere in the previous snapshot with the same flags, rather
// than comparing index-by-index). flaggedAircraft[] is now reordered by
// priority tier every poll (see sortFlaggedByPriority()), so a pure
// reorder - e.g. a Russian-flagged aircraft moving up a slot because a
// higher-priority one disappeared - must NOT by itself count as a change
// and force a redraw; only an actual appearance/disappearance or a flag
// flip should.
bool flaggedSetChanged()
{
  bool changed = (prevFlaggedCount != flaggedCount);
  if (!changed) {
    for (int i = 0; i < flaggedCount && !changed; i++) {
      bool found = false;
      for (int j = 0; j < prevFlaggedCount; j++) {
        if (strcmp(prevFlaggedAircraft[j].icao24, flaggedAircraft[i].icao24) == 0 &&
            prevFlaggedAircraft[j].squawkAlarm == flaggedAircraft[i].squawkAlarm &&
            prevFlaggedAircraft[j].alert == flaggedAircraft[i].alert) {
          found = true;
          break;
        }
      }
      if (!found) changed = true;
    }
  }
  memcpy(prevFlaggedAircraft, flaggedAircraft, sizeof(AircraftHit) * flaggedCount);
  prevFlaggedCount = flaggedCount;
  return changed;
}

void handleAircraftResult(unsigned long now)
{
  if (flaggedCount > 0) {
    lastFlaggedTime = now;
    bool setChanged = flaggedSetChanged();
    bool dueForRefresh = (lastDisplayUpdateTime == 0) || (now - lastDisplayUpdateTime >= DISPLAY_MIN_REFRESH_MS);
    if (setChanged || dueForRefresh) {
      renderAircraftTable();
      lastDisplayUpdateTime = now;
      aircraftScreenShown = true;
    }
  } else {
    // nothing flagged currently -> clear the previous-set memory so the
    // next flagged sighting is treated as "new" and redraws immediately
    prevFlaggedCount = -1;

    // Once the aircraft screen has been sitting there with nothing to show
    // for a while, get off it - even if the normal :00/:30 weather refresh
    // grid isn't due yet - rather than leaving the last tracked aircraft's
    // stale data on screen indefinitely. A watch can easily run past a
    // :00/:30 boundary, so this revert always fetches fresh weather right
    // away, regardless of the grid.
    bool aircraftScreenStale = aircraftScreenShown && lastFlaggedTime != 0 &&
                                (now - lastFlaggedTime >= NO_TARGET_REVERT_MS);

    int hours = timeClient.getHours();
    bool isDaytime = hours >= DAYTIME_START_HOUR && hours < DAYTIME_END_HOUR; // 03:00-21:00 UTC

    // Weather is fetched at most once per WEATHER_SLOT_SECONDS wall-clock
    // slot (aligned to :00/:30 since epoch time is itself :00/:30-aligned),
    // so refreshes land right after EFHK's normal METAR publish times.
    unsigned long currentSlot = timeClient.getEpochTime() / WEATHER_SLOT_SECONDS;
    bool weatherDue = (lastWeatherFetchSlot == 0) || (currentSlot != lastWeatherFetchSlot);

    if (isDaytime && (weatherDue || aircraftScreenStale)) {
      fetchWeatherData();
      lastWeatherFetchSlot = currentSlot;
      aircraftScreenShown = false;
    } else if (!isDaytime && aircraftScreenStale) {
      // No weather screen at night - fall back to a plain idle screen so
      // the display doesn't sit on stale aircraft data until sunrise.
      renderIdleScreen();
      aircraftScreenShown = false;
    }
  }
}

// -------------------- AIRCRAFT TABLE DISPLAY -------------------- //
static const long TRANSITION_ALTITUDE_FT = 5000; // FL notation at/above this, plain hundreds-of-feet below
static const long VS_LEVEL_THRESHOLD_FPM = 150;   // |vertical rate| below this counts as "level" ('=')

// 3-digit "hundreds of feet" code shared by both the current and the
// selected/target altitude, e.g. 18000 -> "180", 4300 -> "043". ROUNDED to
// the nearest hundred, not truncated: modes_logger passes selected_altitude
// (and altitude) straight through from the upstream ADS-B decode unmodified
// (confirmed from its source - no server-side rounding at all), and that
// raw value isn't always an exact round hundred - e.g. an intended FL370
// can arrive as ~36990ft. Truncating division read that a whole bucket low
// ("369" instead of "370"); rounding (+50 before dividing) fixes it for
// both fields since they share this one helper.
static void formatAltCode(long altFt, char *out, size_t outSize) {
  long code = (altFt + 50) / 100;
  if (code < 0) code = 0;
  if (code > 999) code = 999;
  snprintf(out, outSize, "%03ld", code);
}

// Builds the altitude field shown on a card's second line, ATC-shorthand
// style - a self-contained code with no separate "ALT"/"A" label needed:
//   - unknown altitude:                    "---"
//   - no selected altitude, level/unknown vertical rate: "F180" or "A045"
//   - no selected altitude, climbing:      "+F180" or "+A045"
//   - no selected altitude, descending:    "-F180" or "-A045"
//   - selected altitude available:         "F180-100" / "A045-020" / "F380=380"
//     current altitude, then '+'/'-'/'=' for climbing/descending/level (by
//     vertical rate), then the selected altitude's own 3-digit code (never
//     letter-prefixed, since the leading code already sets the scale).
// The current altitude always gets a single-letter prefix: "F" (flight
// level) at/above the transition altitude, "A" (altitude) below it - both
// real ATC shorthand conventions, e.g. "FL180"/"F180" vs "A045" for 4500ft.
static void formatAltitudeField(long altFt, long selFt, long vsFpm, char *out, size_t outSize) {
  if (altFt == LONG_MIN) { snprintf(out, outSize, "---"); return; }

  char prefix = (altFt >= TRANSITION_ALTITUDE_FT) ? 'F' : 'A';
  char curCode[8];
  formatAltCode(altFt, curCode, sizeof(curCode));

  if (selFt == LONG_MIN) {
    // No selected/target altitude to compare against, so show a climb/
    // descend trend directly on the current-altitude code instead - same
    // ±150 ft/min deadband (VS_LEVEL_THRESHOLD_FPM) used just below for the
    // selected-altitude case, so both branches agree on what counts as
    // "level". Unlike that branch (which always shows one of '+'/'-'/'='),
    // level or unknown vertical rate here shows NO sign at all, not a
    // literal '=' - deliberately different, per what was asked: "if the
    // altitude is steady then show just altitude", not an equals sign.
    if (vsFpm != LONG_MIN && vsFpm > VS_LEVEL_THRESHOLD_FPM) {
      snprintf(out, outSize, "+%c%s", prefix, curCode);
    } else if (vsFpm != LONG_MIN && vsFpm < -VS_LEVEL_THRESHOLD_FPM) {
      snprintf(out, outSize, "-%c%s", prefix, curCode);
    } else {
      snprintf(out, outSize, "%c%s", prefix, curCode);
    }
    return;
  }

  char selCode[8];
  formatAltCode(selFt, selCode, sizeof(selCode));

  char sign;
  if (vsFpm == LONG_MIN) sign = '=';
  else if (vsFpm > VS_LEVEL_THRESHOLD_FPM) sign = '+';
  else if (vsFpm < -VS_LEVEL_THRESHOLD_FPM) sign = '-';
  else sign = '=';

  snprintf(out, outSize, "%c%s%c%s", prefix, curCode, sign, selCode);
}

static void formatSpeed(long spd, char *out, size_t outSize) {
  if (spd == LONG_MIN) snprintf(out, outSize, "---");
  else snprintf(out, outSize, "%ld", spd);
}

// Track (heading over ground), always shown as a zero-padded 3-digit
// degree code, e.g. 40 -> "040" - same style as the altitude codes.
static void formatTrack(long trackDeg, char *out, size_t outSize) {
  if (trackDeg == LONG_MIN) { snprintf(out, outSize, "---"); return; }
  long t = trackDeg % 360;
  if (t < 0) t += 360; // guard against a stray negative reading
  snprintf(out, outSize, "%03ld", t);
}

// Unit-vector directions (dx, dy) for the 10 vertices of a regular
// five-pointed star, one point straight up, at 36-degree steps - outer
// (point) vertices at even indices, inner (concave) vertices at odd
// indices. Precomputed once rather than calling sinf()/cosf() at draw
// time, since these 10 numbers never change.
static const float STAR_DX[10] = {
   0.000000f,  0.587785f,  0.951057f,  0.951057f,  0.587785f,
   0.000000f, -0.587785f, -0.951057f, -0.951057f, -0.587785f
};
static const float STAR_DY[10] = {
  -1.000000f, -0.809017f, -0.309017f,  0.309017f,  0.809017f,
   1.000000f,  0.809017f,  0.309017f, -0.309017f, -0.809017f
};

// Ratio of the star's inner (concave) radius to its outer radius. The
// "geometrically correct" pentagram ratio (~0.38 - the shape you get by
// extending a regular pentagon's sides) has thin points that lose
// definition at this icon's small size (~2*outerRadius px across, no
// anti-aliasing on a 1-bit e-paper panel); 0.5 draws a fuller, more solid
// star that stays legible at that size. Tune here if a thinner/fatter
// star is wanted.
static const float STAR_INNER_RATIO = 0.5f;

// Fills a 5-pointed star (one point up) centered at (cx, cy) with the
// given outer radius. The star is star-shaped with respect to its own
// center (every boundary point is visible from the center with no other
// edge in the way), so a 10-triangle fan from the center to each boundary
// edge exactly tiles it with no gaps or overlap - same trick as filling
// any star-convex polygon, just applied to this fixed 10-vertex shape.
static void drawFilledStar(int cx, int cy, int outerRadius) {
  int px[10], py[10];
  for (int k = 0; k < 10; k++) {
    float r = (k % 2 == 0) ? outerRadius : (outerRadius * STAR_INNER_RATIO);
    px[k] = cx + (int)lroundf(STAR_DX[k] * r);
    py[k] = cy + (int)lroundf(STAR_DY[k] * r);
  }
  for (int k = 0; k < 10; k++) {
    int nk = (k + 1) % 10;
    display.fillTriangle(cx, cy, px[k], py[k], px[nk], py[nk], GxEPD_BLACK);
  }
}

// Draws a simplified NATO-style "fixed wing aircraft" icon: a closed square
// box (per NATO APP-6 convention the real air-domain frame is open at the
// bottom, but a closed box reads more cleanly at this icon's small size and
// friend/hostile/neutral framing doesn't apply here anyway) containing a
// two-blade airscrew/propeller glyph - APP-6's actual "fixed wing aircraft"
// symbol is a stylized airscrew, not a plane-shaped silhouette. The
// airscrew is simplified to a bowtie/hourglass: two triangles meeting at
// the box's center, each running from a short vertical edge near the box's
// left/right side to that center point. Box is inset slightly from the
// shared "half" envelope (outerRadius on the star, half-size on the
// triangle) so it doesn't read as visually heavier than the other two icon
// shapes sharing this same slot.
static void drawMilitaryIcon(int cx, int cy, int half) {
  int boxHalf = half - 2;
  display.drawRect(cx - boxHalf, cy - boxHalf, boxHalf * 2 + 1, boxHalf * 2 + 1, GxEPD_BLACK);

  int bladeInset = 4;                 // gap between each blade's outer edge and the box edge
  int bladeHalfHeight = boxHalf - 6;  // vertical half-height of each blade's outer edge
  int bladeX = boxHalf - bladeInset;  // distance from center to each blade's outer edge
  display.fillTriangle(cx - bladeX, cy - bladeHalfHeight,
                        cx - bladeX, cy + bladeHalfHeight,
                        cx, cy,
                        GxEPD_BLACK);
  display.fillTriangle(cx + bladeX, cy - bladeHalfHeight,
                        cx + bladeX, cy + bladeHalfHeight,
                        cx, cy,
                        GxEPD_BLACK);
}

// Which single warning icon (if any) a card should show. Only one icon fits
// in the card's icon slot, so this picks exactly one using the same
// precedence flaggedPriorityTier() sorts by - squawk alarm beats Russian
// beats military beats no icon at all - so "which icon wins" and "which
// aircraft sorts first" can never disagree with each other. (CardIconKind
// itself is declared up near AircraftHit, not here - see the comment there
// for why.)
static CardIconKind cardIconKind(const AircraftHit &h) {
  if (h.squawkAlarm) return ICON_SQUAWK_ALARM;
  if (h.isRussian) return ICON_RUSSIAN;
  if (h.alert == 'M') return ICON_MILITARY;
  return ICON_NONE;
}

// Draws the right-edge warning icon for a card - the same position/size is
// reused for all icon kinds so they read as one consistent "look here"
// signal rather than unrelated shapes:
//   - squawk alarm (7500/7600/7700): an OUTLINED triangle with a "!"
//     centered inside it. Left hollow (not filled) so it's visually
//     distinct from the other two icons at a glance, and this is
//     deliberately a secondary/per-card cue - the full-width inverse-video
//     banner at the top of the screen is still the primary alert for this
//     case; the per-card icon just helps when scanning several cards and
//     only one of them has the alarm.
//   - Russian-registered (ICAO24 in 0x100000-0x1FFFFF): a SOLID filled
//     five-pointed star (one point up), matching the familiar red-star
//     symbol rather than a plain triangle - see drawFilledStar() above.
//   - military-watchlist match (alert == 'M'): a boxed airscrew glyph - see
//     drawMilitaryIcon() above.
// All three share the same bounding box (outer radius/half-size = "half"),
// so no position/clearance math elsewhere needs to change when the icon
// kind changes.
// If a card matches more than one of these at once, only one icon is drawn
// - see cardIconKind() above for which one wins. The others are still
// visible via their own flag letter in the card's flags text (e.g. an "E"
// or "M" alongside a card showing the squawk-alarm triangle).
static void drawCardWarningIcon(int apexX, int midY, int half, CardIconKind kind) {
  if (kind == ICON_SQUAWK_ALARM) {
    display.drawTriangle(apexX, midY - half,
                          apexX - half, midY + half,
                          apexX + half, midY + half,
                          GxEPD_BLACK);
    // The triangle's centroid (1/3 of the way up from its base) is a
    // better visual center for the "!" than the midpoint between apex and
    // base, since the shape is wider at the bottom. Centering math follows
    // the same target-center-minus-half-glyph-size pattern already used
    // elsewhere in this sketch for text (e.g. the boot screen's title).
    display.setFont(&FreeMonoBold9pt7b);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds("!", 0, 0, &tbx, &tby, &tbw, &tbh);
    int centerY = midY + (half / 3);
    int tx = apexX - ((int)tbw / 2) - tbx;
    int ty = (centerY - (int)tbh / 2) - tby;
    display.setCursor(tx, ty);
    display.print("!");
    display.setFont(&FreeMonoBold12pt7b); // restore - card text loop is mid-draw in 12pt
  } else if (kind == ICON_RUSSIAN) {
    drawFilledStar(apexX, midY, half);
  } else if (kind == ICON_MILITARY) {
    drawMilitaryIcon(apexX, midY, half);
  }
}

// Each flagged aircraft is drawn as a three-line "card". At
// FreeMonoBold12pt7b only ~27 characters fit per line on this 400px-wide
// panel, and the registration+ICAO24 identity pair alone can run to ~15
// characters - too much to keep everything on two lines once it's added.
//   line 1: REGISTRATION ICAO24 (e.g. "RA-73724 151FFC") - or just ICAO24
//           alone if the aircraft has no known registration
//   line 2: CALLSIGN SQUAWK TYPE (moved here from the old line 1)
//   line 3: <alt field> S<spd> T<track> <flags> - unchanged ATC-shorthand
//           format from before (see formatAltitudeField()/formatSpeed()/
//           formatTrack() and the flags loop below)
// Only 3 cards now fit on screen (MAX_DISPLAY_ENTRIES, was 4) to make room
// for the extra line; anything beyond that still shows as a "+N more"
// summary line.
// <alt field> is "F180-100" / "A045-020" style: current altitude,
// "F"-prefixed (flight level) at/above 5000ft or "A"-prefixed (altitude)
// below it, then '+'/'-'/'=' from the vertical rate, then the
// selected/target altitude's own bare 3-digit code - or just the current
// altitude alone if no selected altitude is available. Both 3-digit codes
// are ROUNDED to the nearest hundred feet (formatAltCode()), not
// truncated, since the raw altitude/selected_altitude values coming
// through the API aren't always exact round hundreds.
// <spd> is speed in knots directly after "S" (e.g. "S491"); <track> is
// heading over ground as a zero-padded 3-digit degree code directly after
// "T" (e.g. "T040") - a single space separates the alt field, the S block
// and the T block, but not the label from its own value.
// <flags> is the watchlist letter (M/G/C/E) and/or "!" for a squawk alarm,
// preceded by its own space so a track ending in a digit never runs
// straight into a following flag letter (e.g. "T254 E", not "T254E").
// A Russian-registered aircraft gets a solid five-pointed star on the right
// edge of its card, a military-watchlist match gets a boxed airscrew glyph,
// and a squawk-alarm aircraft gets an outlined triangle with "!" instead
// (see drawCardWarningIcon()/cardIconKind() above) - only one icon per
// card even if more than one applies. Cards are pre-sorted by
// sortFlaggedByPriority() (squawk alarm, then Russian-flagged, then
// military-flagged, then the rest) so these are never the ones bumped into
// "+N more" below.
void renderAircraftTable()
{
  bool alarmActive = false;
  for (int i = 0; i < flaggedCount; i++) {
    if (flaggedAircraft[i].squawkAlarm) { alarmActive = true; break; }
  }

  display.setFont(&FreeMonoBold12pt7b);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    char line[48];
    int y;

    if (alarmActive) {
      // Inverse-video banner for an active 7500/7600/7700 squawk - this is
      // safety-relevant and should not look like routine status text.
      display.fillRect(0, 0, display.width(), 28, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.setCursor(6, 21);
      display.print("!! SQUAWK ALARM !!");
      display.setTextColor(GxEPD_BLACK);
    } else {
      snprintf(line, sizeof(line), "APWS - %d FLAGGED", flaggedCount);
      display.setCursor(10, 21);
      display.print(line);
    }
    display.drawFastHLine(10, 29, display.width() - 20, GxEPD_BLACK);
    y = 52; // leaves clearance below the separator line for a 12pt glyph's ascent

    int entriesToShow = flaggedCount < MAX_DISPLAY_ENTRIES ? flaggedCount : MAX_DISPLAY_ENTRIES;
    for (int i = 0; i < entriesToShow; i++) {
      AircraftHit &h = flaggedAircraft[i];
      char altStr[12], spdStr[8], trkStr[8], flagStr[4];
      formatAltitudeField(h.altitude, h.selectedAltitude, h.verticalRate, altStr, sizeof(altStr));
      formatSpeed(h.speed, spdStr, sizeof(spdStr));
      formatTrack(h.track, trkStr, sizeof(trkStr));
      int fi = 0;
      if (h.alert) flagStr[fi++] = h.alert;
      if (h.squawkAlarm) flagStr[fi++] = '!';
      flagStr[fi] = 0;

      // line 1: registration + ICAO24, or just ICAO24 if no registration
      // is known for this aircraft.
      if (h.registration[0]) {
        snprintf(line, sizeof(line), "%s %s", h.registration, h.icao24);
      } else {
        snprintf(line, sizeof(line), "%s", h.icao24);
      }
      display.setCursor(10, y);
      display.print(line);
      int line1Baseline = y;
      y += 24;

      // line 2: callsign, squawk, type (moved here from the old line 1)
      snprintf(line, sizeof(line), "%s %s %s",
               h.callsign[0] ? h.callsign : "-",
               h.squawk[0] ? h.squawk : "-", h.type[0] ? h.type : "-");
      display.setCursor(10, y);
      display.print(line);
      y += 24;

      // line 3: altitude/speed/track/flags - unchanged from the 2-line layout
      snprintf(line, sizeof(line), "%s S%-3s T%-3s %s", altStr, spdStr, trkStr, flagStr);
      display.setCursor(10, y);
      display.print(line);
      int line3Baseline = y;

      CardIconKind iconKind = cardIconKind(h);
      if (iconKind != ICON_NONE) {
        // Right-aligned in the same margin as the card separator line,
        // roughly equilateral, vertically centered across all three of
        // this card's text lines (line 1 to line 3). Worst-case line 3
        // width (longest altitude field, full speed/track, non-empty
        // flags) still ends around x=290-320, well clear of the icon's
        // zone (left edge at display.width()-14-2*iconHalf, so ~x=352+
        // at the current size) - lines 1 and 2 are shorter still, so no
        // new width risk from adding the registration line above them.
        // Vertical size is bounded by the card separator, drawn full-width
        // at y = line3Baseline+10: the icon's bottom (cardMidY+iconHalf,
        // i.e. line3Baseline-24+iconHalf) needs to stay comfortably above
        // that, since the separator's x-range (10 to width-20) overlaps
        // the icon's own x-range. iconHalf=17 leaves ~16px of clearance
        // there - enlarge further with that margin in mind.
        int cardMidY = (line1Baseline + line3Baseline) / 2;
        int iconHalf = 17; // outer radius for the star, half-size for the triangle/box
        int iconApexX = (display.width() - 14) - iconHalf;
        drawCardWarningIcon(iconApexX, cardMidY, iconHalf, iconKind);
      }

      if (i < entriesToShow - 1) {
        // Separator sits in the gap between this card's line 3 and the next
        // card's line 1. yAdvance (24) alone is the font's own no-overlap
        // baseline spacing, i.e. right where the next line would start with
        // zero gap - so the extra headroom has to come from a bigger jump
        // than that, not from nudging the line a few px past a +24 step.
        display.drawFastHLine(10, line3Baseline + 10, display.width() - 20, GxEPD_BLACK);
        y = line3Baseline + 34; // next card's line 1 baseline
      } else {
        y = line3Baseline + 24;
      }
    }

    if (flaggedCount > entriesToShow) {
      // Smaller font for the overflow summary: at 3 full cards this is the
      // tightest spot in the new layout (worked out on paper against the
      // font's known yAdvance, not yet confirmed on real hardware like
      // every other pixel-budget change in this sketch) - a smaller font
      // for this one secondary line buys real clearance above the footer
      // timestamp instead of relying on an untested exact pixel gap.
      display.setFont(&FreeMonoBold9pt7b);
      snprintf(line, sizeof(line), "+ %d more not shown", flaggedCount - entriesToShow);
      display.setCursor(10, y);
      display.print(line);
      display.setFont(&FreeMonoBold12pt7b);
    }

    if (apiUpdatedUtc[0]) {
      display.setFont(&FreeMonoBold9pt7b);
      int16_t tbx, tby; uint16_t tbw, tbh;
      display.getTextBounds(apiUpdatedUtc, 0, 0, &tbx, &tby, &tbw, &tbh);
      display.setCursor(display.width() - (int)tbw - 8, display.height() - 6);
      display.print(apiUpdatedUtc);
    }
  } while (display.nextPage());
}

// -------------------- JSON field extraction (METAR/TAF) -------------------- //
// These operate on an already-parsed JsonDocument (the shared buffer) so
// the caller controls when/how the HTTP body is parsed (streamed, not
// buffered into a String first).
// obsEpoch is the actual observation time (matches the raw METAR's own
// DDHHMMZ group exactly) - AWC's "obsTime" field, a plain Unix epoch.
// reportISO ("reportTime") is only a fallback: AWC rounds it to the
// station's nominal reporting cycle, which can read a few minutes *later*
// than the real observation (e.g. a 16:50Z METAR reported as reportTime
// "17:00:00Z") - fine as a bucket label, misleading as an on-screen clock.
static bool extractMetarFields(JsonDocument &doc, String &metar, time_t &obsEpoch, String &reportISO)
{
  JsonVariant root = doc.as<JsonVariant>();
  JsonVariant item;

  if (root.is<JsonArray>()) {
    if (root.size() == 0) return false;
    item = root[0];
  } else if (root.is<JsonObject>()) {
    if (root.containsKey("metars") && root["metars"].is<JsonArray>() && root["metars"].size() > 0) {
      item = root["metars"][0];
    } else if (root.containsKey("data") && root["data"].is<JsonArray>() && root["data"].size() > 0) {
      item = root["data"][0];
    } else {
      item = root;
    }
  } else {
    return false;
  }

  if (item.containsKey("rawOb"))         metar = item["rawOb"].as<String>();
  else if (item.containsKey("raw"))      metar = item["raw"].as<String>();
  else if (item.containsKey("raw_text")) metar = item["raw_text"].as<String>();
  else if (item.containsKey("metar"))    metar = item["metar"].as<String>();

  obsEpoch = 0;
  if (item.containsKey("obsTime") && !item["obsTime"].isNull()) {
    obsEpoch = (time_t)item["obsTime"].as<long>();
  }

  if (item.containsKey("reportTime"))    reportISO = item["reportTime"].as<String>();

  return metar.length() > 0;
}

static bool extractTAFFields(JsonDocument &doc, String &taf, String &issueTimeISO)
{
  JsonVariant root = doc.as<JsonVariant>();
  JsonVariant item;

  if (root.is<JsonArray>()) {
    if (root.size() == 0) return false;
    item = root[0];
  } else if (root.is<JsonObject>()) {
    if (root.containsKey("tafs") && root["tafs"].is<JsonArray>() && root["tafs"].size() > 0) {
      item = root["tafs"][0];
    } else if (root.containsKey("data") && root["data"].is<JsonArray>() && root["data"].size() > 0) {
      item = root["data"][0];
    } else {
      item = root;
    }
  } else {
    return false;
  }

  if (item.containsKey("rawTAF"))        taf = item["rawTAF"].as<String>();
  else if (item.containsKey("raw"))      taf = item["raw"].as<String>();
  else if (item.containsKey("raw_text")) taf = item["raw_text"].as<String>();
  else if (item.containsKey("taf"))      taf = item["taf"].as<String>();

  if (item.containsKey("issueTime"))     issueTimeISO = item["issueTime"].as<String>();
  else if (item.containsKey("validTime")) issueTimeISO = item["validTime"].as<String>();
  else if (item.containsKey("bulletinTime")) issueTimeISO = item["bulletinTime"].as<String>();

  return taf.length() > 0;
}

// Converts an AWC-style ISO8601 timestamp ("2026-09-20T13:20:00Z") into a
// plain "2026-09-20 13:20" for display. Returns an empty string if iso
// isn't in the expected shape.
static void isoToDateTime(const String &iso, char *out, size_t outSize) {
  out[0] = 0;
  if (iso.length() >= 16 && iso[10] == 'T') {
    char buf[17];
    iso.substring(0, 16).toCharArray(buf, sizeof(buf));
    buf[10] = ' ';
    strncpy(out, buf, outSize - 1);
    out[outSize - 1] = 0;
  }
}

// -------------------- EFHK METAR + TAF (JSON via AWC Data API) -------------------- //
void fetchWeatherData() {
  String metar = "";
  time_t metarObsEpoch = 0;
  String metarReportISO = "";
  String taf = "";
  String tafTimeISO = "";

  // Up to 4 sequential HTTPS legs follow (METAR json+raw fallback, TAF
  // json+raw fallback), each with its own timeout below WDT_TIMEOUT_S, but
  // together they can comfortably exceed it - feed the watchdog between
  // legs so a slow-but-legitimate weather fetch doesn't trigger a reset.
  esp_task_wdt_reset();

  // HTTPS with insecure client (avoids cert management on ESP32)
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  // METAR (JSON)
  {
    HTTPClient http;
    const char *url_json = "https://aviationweather.gov/api/data/metar?ids=EFHK&format=json";
    Serial.println(String("Fetching EFHK METAR (JSON): ") + url_json);

    if (http.begin(client, url_json)) {
      http.setConnectTimeout(5000);
      http.setTimeout(15000);
      int httpCode = http.GET();
      if (httpCode > 0) {
        sharedJsonDoc.clear();
        DeserializationError err = deserializeJson(sharedJsonDoc, http.getStream());
        if (!err) {
          if (!extractMetarFields(sharedJsonDoc, metar, metarObsEpoch, metarReportISO)) {
            Serial.println("JSON present but could not extract METAR fields.");
          }
        } else {
          Serial.printf("METAR JSON parse error: %s\n", err.c_str());
        }
      } else {
        Serial.printf("Error fetching EFHK JSON METAR: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("HTTP begin failed for METAR JSON endpoint");
    }

    esp_task_wdt_reset();

    // Fallback: raw text
    if (metar.length() == 0) {
      HTTPClient httpRaw;
      const char *url_raw = "https://aviationweather.gov/api/data/metar?ids=EFHK&format=raw";
      Serial.println(String("Falling back to EFHK METAR (raw): ") + url_raw);
      if (httpRaw.begin(client, url_raw)) {
        httpRaw.setConnectTimeout(5000);
        httpRaw.setTimeout(15000);
        int code = httpRaw.GET();
        if (code > 0) {
          String rawPayload = httpRaw.getString();
          rawPayload.trim();
          metar = rawPayload;
        } else {
          Serial.printf("Error fetching EFHK raw METAR: %d\n", code);
        }
        httpRaw.end();
      }
    }
  }

  esp_task_wdt_reset();

  // TAF (JSON)
  {
    HTTPClient http;
    const char *url_taf = "https://aviationweather.gov/api/data/taf?ids=EFHK&format=json";
    Serial.println(String("Fetching EFHK TAF (JSON): ") + url_taf);

    if (http.begin(client, url_taf)) {
      http.setConnectTimeout(5000);
      http.setTimeout(15000);
      int httpCode = http.GET();
      if (httpCode > 0) {
        sharedJsonDoc.clear();
        DeserializationError err = deserializeJson(sharedJsonDoc, http.getStream());
        if (!err) {
          if (!extractTAFFields(sharedJsonDoc, taf, tafTimeISO)) {
            Serial.println("JSON present but could not extract TAF fields.");
          }
        } else {
          Serial.printf("TAF JSON parse error: %s\n", err.c_str());
        }
      } else {
        Serial.printf("Error fetching EFHK JSON TAF: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("HTTP begin failed for TAF JSON endpoint");
    }

    esp_task_wdt_reset();

    // Fallback: raw text
    if (taf.length() == 0) {
      HTTPClient httpRaw;
      const char *url_raw = "https://aviationweather.gov/api/data/taf?ids=EFHK&format=raw";
      Serial.println(String("Falling back to EFHK TAF (raw): ") + url_raw);
      if (httpRaw.begin(client, url_raw)) {
        httpRaw.setConnectTimeout(5000);
        httpRaw.setTimeout(15000);
        int code = httpRaw.GET();
        if (code > 0) {
          String rawPayload = httpRaw.getString();
          rawPayload.trim();
          taf = rawPayload;
        } else {
          Serial.printf("Error fetching EFHK raw TAF: %d\n", code);
        }
        httpRaw.end();
      }
    }
  }

  // Simple API health check: METAR is the primary data we depend on here
  // (TAF is a nice-to-have), so treat a successful METAR fetch - JSON or
  // raw fallback - as "the API is up and reachable", and log the outcome
  // so it's not only visible on the next screen redraw.
  weatherApiOk = (metar.length() > 0);
  Serial.printf("Weather API health check: %s\n", weatherApiOk ? "OK" : "FAIL");

  // Display METAR+TAF
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Header
    display.setFont(&FreeMonoBold24pt7b);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds("APWS ONLINE", 0, 0, &tbx, &tby, &tbw, &tbh);
    int x = ((display.width() - tbw) / 2) - tbx;
    int y = tbh + 10;
    display.setCursor(x, y);
    display.println("APWS ONLINE");

    // Start METAR block lower to avoid overlap with header
    y += 26;

    const int bottomGuard = 36; // room for the bigger 12pt timestamp line below
    const int bottomLimit = display.height() - bottomGuard;

    display.setFont(&FreeMonoBold9pt7b);

    if (metar.length()) {
      printWrappedCentered(metar, y, 16, bottomLimit);
      y += 16;
    }

    if (taf.length() && y < bottomLimit) {
      printWrappedCentered(taf, y, 16, bottomLimit);
    }

    // "YYYY-MM-DD HH:MM" - the METAR's own actual observation time (AWC's
    // "obsTime", a plain epoch that matches the raw METAR text's DDHHMMZ
    // group exactly) when available. Deliberately NOT "reportTime": AWC
    // rounds that to the station's nominal reporting cycle, which can read
    // a few minutes *later* than the real observation (seen live: a
    // 16:50Z METAR whose reportTime was "17:00:00Z") - i.e. a timestamp
    // that looks like it's from the future. Falls back to reportTime if
    // obsTime is missing, then to the device's current NTP time, so this
    // also doubles as a "system is alive and polling" indicator.
    // " / API OK" or " / API FAIL" is appended from the health check above,
    // so it's visible at a glance whether the background downloads are
    // actually succeeding right now.
    char timeLabel[20];
    char timeAndStatus[32];
    timeLabel[0] = 0;
    if (metarObsEpoch > 0) {
      struct tm *ptm = gmtime(&metarObsEpoch);
      strftime(timeLabel, sizeof(timeLabel), "%Y-%m-%d %H:%M", ptm);
    } else if (metarReportISO.length()) {
      isoToDateTime(metarReportISO, timeLabel, sizeof(timeLabel));
    }
    if (!timeLabel[0]) {
      time_t epoch = timeClient.getEpochTime();
      struct tm *ptm = gmtime(&epoch);
      strftime(timeLabel, sizeof(timeLabel), "%Y-%m-%d %H:%M", ptm);
    }
    snprintf(timeAndStatus, sizeof(timeAndStatus), "%s / API %s", timeLabel, weatherApiOk ? "OK" : "FAIL");

    display.setFont(&FreeMonoBold12pt7b);
    display.getTextBounds(timeAndStatus, 0, 0, &tbx, &tby, &tbw, &tbh);
    x = ((display.width() - (int)tbw) / 2) - tbx;
    int yBottom = display.height() - (int)tbh - tby - 8;
    display.setCursor(x, yBottom);
    display.println(timeAndStatus);

  } while (display.nextPage());
}
