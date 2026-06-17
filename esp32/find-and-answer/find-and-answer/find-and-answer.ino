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
// Logging helpers (Serial + optional Discord webhook, like debug()/success())
// ===========================================================================
void discordPost(const String& json) {
  if (strlen(DISCORD_WEBHOOK) == 0) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, DISCORD_WEBHOOK)) return;
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
  connectWifi();
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

// ===========================================================================
// Arduino entry points
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== UCS Presence (ESP32) ===");

  if (!connectWifi()) {
    debugMsg("Could not join Wi-Fi. Will reboot in 30s to retry.");
    delay(30000);
    ESP.restart();
  }

  if (!syncTime()) {
    debugMsg("Could not sync time via NTP. Will reboot in 30s to retry.");
    delay(30000);
    ESP.restart();
  }

  debugMsg("Device online and time-synced. Watching for the 19:30-22:30 window.");
}

void loop() {
  struct tm t;
  if (!getLocalTime(&t, 1000)) {
    // RTC not valid (e.g. lost sync) - try to re-sync before doing anything.
    Serial.println("Local time unavailable, re-syncing NTP...");
    syncTime();
    delay(1000);
    return;
  }

  const long today  = dayKey(t);
  const int  minutes = t.tm_hour * 60 + t.tm_min;
  const bool inWindow = (minutes >= WINDOW_START_MIN && minutes <= WINDOW_END_MIN);
  const unsigned long now = millis();

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
               ", outside the 19:30-22:30 window.");
      gLastHeartbeat = millis();
    }
  }

  delay(250);
}
