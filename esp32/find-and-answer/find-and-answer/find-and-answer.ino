// ===========================================================================
// UCS Presence - ESP32 port of find-and-answer.js
//
// Connects to eduroam (WPA2-Enterprise), logs into the UCS SOU API, finds
// today's class, then polls the attendance registration once a minute and
// answers it as soon as it opens. Mirrors the original Node.js script.
//
// Board: any ESP32 (tested mentally against ESP32-WROOM / DevKitC).
// Libraries required (install via Arduino Library Manager):
//   - ArduinoJson (v7.x)
// Everything else (WiFi, WiFiClientSecure, HTTPClient) ships with the
// arduino-esp32 core. Use core 2.0.x or 3.x - both are handled below.
// ===========================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_task_wdt.h"

// The WPA2-Enterprise API was renamed between arduino-esp32 core 2.x and 3.x.
#if __has_include("esp_eap_client.h")
  #include "esp_eap_client.h"
  #define UCS_USE_EAP_CLIENT 1
#else
  #include "esp_wpa2.h"
#endif

#include "secrets.h"

// Set to 1 to print raw HTTP response bodies to Serial (handy while testing).
#define HTTP_DEBUG 1

// ---- API endpoints -------------------------------------------------------
static const char* TOKEN_URL = "https://auth.ucs.br/auth-token/api-token-auth/";
static const char* API_BASE  = "https://sou.ucs.br/api/v1";

// ---- Time / NTP ----------------------------------------------------------
// UCS is in Caxias do Sul, RS, Brazil (America/Sao_Paulo). Brazil dropped DST
// in 2019, so it's a fixed UTC-3. POSIX TZ sign is inverted, hence "<-03>3".
static const char* TZ_INFO    = "<-03>3";
static const char* NTP_SERVER1 = "pool.ntp.org";
static const char* NTP_SERVER2 = "time.google.com";

// ---- Active window (local time) ------------------------------------------
// Only look for a class and answer attendance between these times.
static const int WINDOW_START_MIN = 19 * 60 + 30;  // 19:30
static const int WINDOW_END_MIN   = 22 * 60 + 30;  // 22:30

// ---- Timing --------------------------------------------------------------
static const unsigned long POLL_INTERVAL_MS = 60UL * 1000UL;        // 1 min
static const unsigned long SCAN_RETRY_MS    = 10UL * 60UL * 1000UL; // 10 min
static const unsigned long HEARTBEAT_MS     = 60UL * 60UL * 1000UL; // 1 h

// ---- Robustness (sealed, unattended deploy) ------------------------------
// Hardware watchdog: if loop() ever stops feeding it for this long, the chip
// resets itself. Generous because a full class scan is several HTTP calls.
static const uint32_t      WDT_TIMEOUT_S    = 180;                  // 3 min
// Per-request network timeouts so a stuck socket can never hang the loop.
static const uint16_t      HTTP_CONNECT_MS  = 10000;                // TCP connect
static const uint16_t      HTTP_READ_MS     = 15000;                // body read
// Self-heal: reboot once a day (only outside the active window) to clear any
// heap fragmentation from repeated TLS and re-seed the clock. millis()-based
// so it can't loop on the wall clock after a reboot.
static const unsigned long MAX_UPTIME_MS    = 24UL * 60UL * 60UL * 1000UL;
// Reboot if free heap ever drops this low (TLS leak / fragmentation guard).
static const uint32_t      MIN_FREE_HEAP    = 25000;
// How hard to retry Wi-Fi before giving up and rebooting.
static const int           WIFI_RETRIES     = 3;

// ---- Runtime state -------------------------------------------------------
String        gToken;
String        gTodaysClassUrl;
long          gTodaysSequencia = -1;
long          gClassDay        = -1;  // day key gTodaysClassUrl is valid for
long          gAnsweredDay      = -1;  // day key we already answered
unsigned long gLastScan        = 0;
unsigned long gLastPoll        = 0;
unsigned long gLastHeartbeat   = 0;

// ===========================================================================
// Hardware watchdog. Subscribes the loop task so a hang anywhere (a wedged
// TLS socket, a library deadlock) triggers a chip reset instead of a brick.
// The API changed between arduino-esp32 core 2.x and 3.x.
// ===========================================================================
inline void feedWatchdog() { esp_task_wdt_reset(); }

void initWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {
      .timeout_ms     = WDT_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic  = true,
  };
  // The arduino-esp32 3.x core auto-inits the Task WDT with a short default
  // timeout. In that case init() returns ESP_ERR_INVALID_STATE and our 180s is
  // silently dropped, leaving a timeout a single TLS call can blow through.
  // Reconfigure the existing instance so our generous timeout actually applies.
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&cfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);   // watch the task running setup()/loop().
  feedWatchdog();
}

// ===========================================================================
// Logging helpers (Serial + optional Discord webhook, like debug()/success())
// ===========================================================================
void discordPost(const String& json) {
  if (strlen(DISCORD_WEBHOOK) == 0) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, DISCORD_WEBHOOK)) return;
  http.setConnectTimeout(HTTP_CONNECT_MS);
  http.setTimeout(HTTP_READ_MS);
  http.addHeader("Content-Type", "application/json");
  http.POST(json);
  http.end();
}

void debugMsg(const String& msg) {
  Serial.println(msg);
  JsonDocument doc;
  doc["content"] = msg;
  String out;
  serializeJson(doc, out);
  discordPost(out);
}

void successMsg(const String& msg) {
  Serial.println(msg);
  JsonDocument doc;
  JsonObject embed = doc["embeds"].add<JsonObject>();
  embed["title"] = msg;
  embed["color"] = 10747768;
  String out;
  serializeJson(doc, out);
  discordPost(out);
}

// ===========================================================================
// eduroam / WPA2-Enterprise connection
// ===========================================================================
bool connectEduroam();  // defined below; used by connectWifi()

bool waitForConnection() {
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 30000) {
    delay(500);
    Serial.print(".");
    feedWatchdog();
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection FAILED.");
    return false;
  }
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool connectWifi() {
#if USE_HOME_WIFI
  // Plain WPA2-PSK home network for testing.
  Serial.printf("Connecting to home Wi-Fi '%s'...\n", WIFI_HOME_SSID);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_HOME_SSID, WIFI_HOME_PASSWORD);
  return waitForConnection();
#else
  return connectEduroam();
#endif
}

bool connectEduroam() {
  Serial.printf("Connecting to '%s' as '%s'...\n", WIFI_SSID, EAP_USERNAME);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

#ifdef UCS_USE_EAP_CLIENT
  // arduino-esp32 core 3.x
  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  if (EDUROAM_CA_PEM != nullptr) {
    esp_eap_client_set_ca_cert((const unsigned char*)EDUROAM_CA_PEM,
                               strlen(EDUROAM_CA_PEM) + 1);
  }
  esp_wifi_sta_enterprise_enable();
#else
  // arduino-esp32 core 2.x
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  if (EDUROAM_CA_PEM != nullptr) {
    esp_wifi_sta_wpa2_ent_set_ca_cert((const unsigned char*)EDUROAM_CA_PEM,
                                      strlen(EDUROAM_CA_PEM) + 1);
  }
  esp_wifi_sta_wpa2_ent_enable();
#endif

  WiFi.begin(WIFI_SSID);
  return waitForConnection();
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("Wi-Fi dropped, reconnecting...");
  for (int i = 0; i < WIFI_RETRIES; i++) {
    feedWatchdog();
    if (connectWifi()) return;
    Serial.printf("Reconnect attempt %d/%d failed.\n", i + 1, WIFI_RETRIES);
  }
  // Couldn't get back on the network at all: reboot and start clean. setup()
  // will keep rebooting until Wi-Fi (and NTP) come back, which is the only
  // sane behaviour for a sealed device with no console.
  Serial.println("Wi-Fi unrecoverable, rebooting.");
  delay(500);
  ESP.restart();
}

// ===========================================================================
// Time / NTP
// ===========================================================================
// Sync the ESP32's internal RTC from NTP. Returns true once a valid time is
// obtained (getLocalTime fails until the clock is actually set).
bool syncTime() {
  Serial.println("Syncing time via NTP...");
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  struct tm t;
  if (getLocalTime(&t, 10000)) {  // waits up to 10s for the first sync
    Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return true;
  }
  Serial.println("NTP sync FAILED.");
  return false;
}

// A stable per-day key (year*1000 + day-of-year) used to do something only
// once per calendar day.
long dayKey(const struct tm& t) {
  return (long)(t.tm_year + 1900) * 1000 + t.tm_yday;
}

bool fetchToken();  // defined below; used by apiRequest() for 401 re-auth

// ===========================================================================
// HTTP helper. Returns the HTTP status code (<=0 on transport/parse failure).
// If outDoc/filter are provided, the response body is parsed into outDoc.
// On an authenticated 401 the token is refreshed and the request retried once
// (the token can expire during the 24h polling window).
// ===========================================================================
int apiRequest(const char* method, const String& url, const String& body,
               JsonDocument* outDoc, JsonDocument* filter, bool auth) {
  ensureWifi();

  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();  // Skip UCS server-cert validation (simple + reliable).
    HTTPClient http;
    if (!http.begin(client, url)) {
      Serial.println("http.begin() failed for " + url);
      return -1;
    }
    http.setConnectTimeout(HTTP_CONNECT_MS);
    http.setTimeout(HTTP_READ_MS);
    http.addHeader("Content-Type", "application/json");
    if (auth) http.addHeader("Authorization", "Token " + gToken);

    int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();

    // Token expired/rejected: refresh once and retry the same request.
    if (auth && code == 401 && attempt == 0) {
      http.end();
      debugMsg("Token rejected (401), refreshing...");
      if (!fetchToken()) return 401;
      continue;
    }

    if (code > 0) {
      // Read the full body via getString(): HTTPClient de-chunks (and would
      // decompress) it for us. Parsing http.getStream() directly leaves the raw
      // Transfer-Encoding: chunked size markers in the data, which makes
      // ArduinoJson silently parse nothing.
      String resp = http.getString();
#if HTTP_DEBUG
      Serial.printf("<- HTTP %d (%u bytes): %s\n", code, (unsigned)resp.length(),
                    resp.substring(0, 300).c_str());
#endif
      if (code < 400 && outDoc != nullptr) {
        DeserializationError err = filter
            ? deserializeJson(*outDoc, resp, DeserializationOption::Filter(*filter))
            : deserializeJson(*outDoc, resp);
        if (err) {
          Serial.printf("JSON parse error (%s): %s\n", url.c_str(), err.c_str());
          http.end();
          return -2;
        }
      }
    } else {
      Serial.printf("HTTP %s %s failed: %s\n", method, url.c_str(),
                    http.errorToString(code).c_str());
    }

    http.end();
    return code;
  }

  return -1;  // unreachable
}

// ===========================================================================
// API flow (mirrors find-and-answer.js)
// ===========================================================================
bool fetchToken() {
  debugMsg("Starting script, fetching token...");

  JsonDocument body;
  body["username"] = UCS_USERNAME;
  body["password"] = UCS_PASSWORD;
  String payload;
  serializeJson(body, payload);

  JsonDocument resp;
  JsonDocument filter;
  filter["token"] = true;

  int code = apiRequest("POST", TOKEN_URL, payload, &resp, &filter, false);
  if (code != 200 || !resp["token"].is<const char*>()) {
    debugMsg("Failed to fetch token (HTTP " + String(code) + ").");
    return false;
  }
  gToken = resp["token"].as<String>();
  return gToken.length() > 0;
}

String registroUrl(const String& classUrl) {
  return String(API_BASE) +
         "/ambientes/segmentos/graduacao/ambientes/" + classUrl +
         "/ferramentas/registro-frequencia/";
}

// Collect every class "url" from the ambientes listing.
bool fetchClassUrls(std::vector<String>& urls) {
  debugMsg("Fetching classes...");

  JsonDocument resp;
  JsonDocument filter;
  // Top-level is an array; each element has an "itens" array of {url, nome}.
  JsonObject elem = filter.add<JsonObject>();
  elem["itens"][0]["url"]  = true;
  elem["itens"][0]["nome"] = true;

  String url = String(API_BASE) +
               "/ambientes/segmentos/graduacao/ambientes/";
  int code = apiRequest("GET", url, "", &resp, &filter, true);
  if (code != 200) {
    debugMsg("Failed to fetch classes (HTTP " + String(code) + ").");
    return false;
  }

  for (JsonObject ambiente : resp.as<JsonArray>()) {
    for (JsonObject item : ambiente["itens"].as<JsonArray>()) {
      if (item["url"].is<const char*>()) {
        urls.push_back(item["url"].as<String>());
      }
    }
  }
  Serial.printf("Found %u class entries.\n", (unsigned)urls.size());
  return true;
}

// Find the class whose registro-frequencia has an "encontro_hoje" today.
bool findTodaysClass(const std::vector<String>& urls) {
  debugMsg("Trying to find today's class...");

  for (const String& classUrl : urls) {
    feedWatchdog();
    JsonDocument resp;
    JsonDocument filter;
    filter["dados"]["encontro_hoje"] = true;

    int code = apiRequest("GET", registroUrl(classUrl), "", &resp, &filter, true);
    if (code != 200) continue;

    JsonVariant encontro = resp["dados"]["encontro_hoje"];
    if (!encontro.isNull()) {
      gTodaysClassUrl  = classUrl;
      gTodaysSequencia = encontro["sequencia"] | -1;
      debugMsg("Found today's class!");
      debugMsg(gTodaysClassUrl);
      return true;
    }
  }

  debugMsg("No class found for today. Will scan again next hour.");
  return false;
}

// One poll cycle: check availability, answer if open. Mirrors
// checkAndAnswerAttendence(). Returns true once attendance was answered.
// Routine per-minute lines go to Serial only to avoid spamming Discord; the
// meaningful events (responding / success / failures) still hit Discord.
bool checkAndAnswer() {
  Serial.println("Checking attendance registration availability...");

  JsonDocument resp;
  JsonDocument filter;
  filter["dados"]["encontro_hoje"] = true;

  int code = apiRequest("GET", registroUrl(gTodaysClassUrl), "", &resp, &filter, true);
  if (code != 200) {
    debugMsg("Poll request failed (HTTP " + String(code) + ").");
    return false;
  }

  bool available = resp["dados"]["encontro_hoje"]["chamada_app_hoje"]
                       ["liberada_para_membros"] |
                   false;

  if (!available) {
    Serial.println("Attendance registration not available.");
    return false;
  }

  debugMsg("Attendance registration available!! Responding now...");

  JsonDocument body;
  body["funcao"] = "atualizar_registro_frequencia_via_membro";
  body["parametros"]["sequencia"] = gTodaysSequencia;
  String payload;
  serializeJson(body, payload);

  int postCode = apiRequest("POST", registroUrl(gTodaysClassUrl), payload,
                            nullptr, nullptr, true);
  if (postCode > 0 && postCode < 400) {
    successMsg("Success responding to attendance registration!");
    return true;
  }
  debugMsg("Answer POST failed (HTTP " + String(postCode) + ").");
  return false;
}

// Refresh token, fetch classes, and look for today's class. On success sets
// gTodaysClassUrl / gTodaysSequencia and returns true.
bool findClassForToday() {
  gLastScan = millis();
  debugMsg("Looking for today's class...");

  if (!fetchToken()) return false;

  std::vector<String> urls;
  if (!fetchClassUrls(urls) || urls.empty()) return false;

  return findTodaysClass(urls);
}

// Human-readable reason the chip last reset, so the boot heartbeat can tell a
// manual power-cycle apart from a self-reboot (watchdog, low heap, daily) or a
// brownout (a power-supply problem worth knowing about in a sealed box).
const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (self-reboot)";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog (hang)";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout (power dip)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default:                return "unknown";
  }
}

// ===========================================================================
// Arduino entry points
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== UCS Presence (ESP32) ===");
  Serial.printf("Reset reason: %d, free heap: %u\n",
                (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());

  initWatchdog();

  // Don't wear out flash writing the SSID every boot, and let the core retry
  // the association on its own between our explicit reconnects.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  if (!connectWifi()) {
    debugMsg("Could not join Wi-Fi. Will reboot in 30s to retry.");
    feedWatchdog();
    delay(30000);
    ESP.restart();
  }
  feedWatchdog();

  if (!syncTime()) {
    debugMsg("Could not sync time via NTP. Will reboot in 30s to retry.");
    feedWatchdog();
    delay(30000);
    ESP.restart();
  }
  feedWatchdog();

  debugMsg(String("Device online and time-synced (reset: ") + resetReasonStr() +
           "). Watching for the 19:30-22:30 window.");
}

void loop() {
  feedWatchdog();

  struct tm t;
  if (!getLocalTime(&t, 1000)) {
    // RTC not valid (e.g. lost sync) - get back online, then re-sync before
    // doing anything that depends on the wall clock.
    Serial.println("Local time unavailable, re-syncing NTP...");
    ensureWifi();
    syncTime();
    delay(1000);
    return;
  }

  const long today  = dayKey(t);
  const int  minutes = t.tm_hour * 60 + t.tm_min;
  const bool inWindow = (minutes >= WINDOW_START_MIN && minutes <= WINDOW_END_MIN);
  const unsigned long now = millis();

  // ---- Self-heal guards (never interrupt the active answering window) ----
  if (!inWindow) {
    // Critically low heap: bail out and come back fresh before we crash mid-run.
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      debugMsg("Free heap low (" + String((unsigned)ESP.getFreeHeap()) +
               " bytes), rebooting to recover.");
      delay(500);
      ESP.restart();
    }
    // Routine once-a-day reboot to shed TLS fragmentation and re-seed the clock.
    // millis()-based, so it resets after the reboot and can never loop.
    if (now >= MAX_UPTIME_MS) {
      debugMsg("Scheduled daily reboot (clears memory, re-syncs clock).");
      delay(500);
      ESP.restart();
    }
  }

  if (inWindow) {
    // Already answered today? Nothing more to do until tomorrow.
    if (gAnsweredDay == today) {
      delay(1000);
      return;
    }

    // Make sure we've identified today's class (retry every SCAN_RETRY_MS
    // until found, in case attendance data appears late).
    if (gClassDay != today &&
        (gLastScan == 0 || now - gLastScan >= SCAN_RETRY_MS)) {
      if (findClassForToday()) {
        gClassDay = today;
        gLastPoll = 0;  // poll immediately now that we found it
      }
    }

    // Poll once a minute while we have today's class.
    if (gClassDay == today &&
        (gLastPoll == 0 || now - gLastPoll >= POLL_INTERVAL_MS)) {
      if (checkAndAnswer()) {
        gAnsweredDay = today;
        successMsg("Done for today. Idling until tomorrow's window.");
      }
      gLastPoll = millis();
    }
  } else {
    // Outside the window: hourly heartbeat so we know the device is alive.
    if (gLastHeartbeat == 0 || now - gLastHeartbeat >= HEARTBEAT_MS) {
      char buf[6];
      snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
      debugMsg(String("Heartbeat: alive at ") + buf +
               ", outside the 19:30-22:30 window. Free heap: " +
               String((unsigned)ESP.getFreeHeap()) + " bytes.");
      gLastHeartbeat = millis();
    }
  }

  delay(250);
}
