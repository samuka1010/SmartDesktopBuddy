#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <sntp.h>
#include <time.h>

#include "mfactoryfont.h"  // Custom font
#include "tz_lookup.h"     // Timezone lookup, do not duplicate mapping here!
#include "days_lookup.h"   // Languages for the Days of the Week

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define CLK_PIN 12
#define DATA_PIN 15
#define CS_PIN 13
#define BUTTON_PIN 4

const String localFirmwareVersion = "0.1.0";

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
AsyncWebServer server(80);

// --- Global Scroll Speed Settings ---
const int IP_SCROLL_SPEED = 115;

// WiFi and configuration globals
char ssid[32] = "";
char password[32] = "";
char openWeatherApiKey[64] = "";
char openWeatherCity[64] = "";
char openWeatherCountry[64] = "";
char weatherUnits[12] = "metric";
char timeZone[64] = "";
char language[8] = "en";
String mainDesc = "";
String detailedDesc = "";

// Timing and display settings
unsigned long clockDuration = 10000;
unsigned long weatherDuration = 5000;
int brightness = 7;
bool twelveHourToggle = false;
bool showHumidity = false;
char ntpServer1[64] = "pool.ntp.org";
char ntpServer2[64] = "time.nist.gov";

// Dimming
bool dimmingEnabled = false;
int dimStartHour = 18;  // 6pm default
int dimStartMinute = 0;
int dimEndHour = 8;  // 8am default
int dimEndMinute = 0;
int dimBrightness = 2;  // Dimming level (0-15)

// State management
WiFiClient client;
const byte DNS_PORT = 53;
DNSServer dnsServer;

String currentTemp = "";
bool weatherAvailable = false;
bool weatherFetched = false;
bool weatherFetchInitiated = false;
bool isAPMode = false;
char tempSymbol = '[';
bool shouldFetchWeatherNow = false;

unsigned long lastSwitch = 0;
unsigned long lastColonBlink = 0;
int displayMode = 0;  // 0: Clock, 1: Weather
int currentHumidity = -1;
bool ntpSyncSuccessful = false;

bool colonVisible = true;

unsigned long ntpAnimTimer = 0;
int ntpAnimFrame = 0;
bool tzSetAfterSync = false;
const unsigned long weatherFetchInterval = 300000;  // 5 minutes
unsigned long lastWeatherFetch = 0;

// NTP Synchronization State Machine
enum NtpState {
  NTP_IDLE,
  NTP_SYNCING,
  NTP_SUCCESS,
  NTP_FAILED
};
NtpState ntpState = NTP_IDLE;
unsigned long ntpStartTime = 0;
const int ntpTimeout = 30000;  // 30 seconds
const int maxNtpRetries = 30;
int ntpRetryCount = 0;
unsigned long lastNtpStatusPrintTime = 0;
const unsigned long ntpStatusPrintInterval = 1000;  // Print status every 1 seconds (adjust as needed)

// Non-blocking IP display globals
bool showingIp = false;
int ipDisplayCount = 0;
const int ipDisplayMax = 2;  // As per working copy for how long IP shows
String pendingIpToShow = "";

// Loading interface
bool updateChecked = false;
bool loadingInterface = true;
const unsigned long loaderInterval = 800;
String loadingFrame = ".";
unsigned long lastLoaderAnim = 0;

bool lastButtonStatus = false;

// -----------------------------------------------------------------------------
// Configuration Load & Save
// -----------------------------------------------------------------------------
void loadConfig() {
  Serial.println(F("[CONFIG] Loading configuration..."));

  // Check if config.json exists, if not, create default
  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] config.json not found, creating with defaults..."));
    DynamicJsonDocument doc(1024);
    doc[F("ssid")] = "";
    doc[F("password")] = "";
    doc[F("openWeatherApiKey")] = "";
    doc[F("openWeatherCity")] = "";
    doc[F("openWeatherCountry")] = "";
    doc[F("weatherUnits")] = "metric";
    doc[F("clockDuration")] = 10000;
    doc[F("weatherDuration")] = 5000;
    doc[F("timeZone")] = "";
    doc[F("language")] = "en";
    doc[F("brightness")] = brightness;
    doc[F("twelveHourToggle")] = twelveHourToggle;
    doc[F("showHumidity")] = showHumidity;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;
    doc[F("dimmingEnabled")] = dimmingEnabled;
    doc[F("dimStartHour")] = dimStartHour;
    doc[F("dimStartMinute")] = dimStartMinute;
    doc[F("dimEndHour")] = dimEndHour;
    doc[F("dimEndMinute")] = dimEndMinute;
    doc[F("dimBrightness")] = dimBrightness;

    File f = LittleFS.open("/config.json", "w");
    if (f) {
      serializeJsonPretty(doc, f);
      f.close();
      Serial.println(F("[CONFIG] Default config.json created."));
    } else {
      Serial.println(F("[ERROR] Failed to create default config.json"));
    }
  }

  Serial.println(F("[CONFIG] Attempting to open config.json for reading."));
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[ERROR] Failed to open config.json for reading. Cannot load config."));
    return;
  }

  DynamicJsonDocument doc(1024);  // Size based on ArduinoJson Assistant + buffer
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.print(F("[ERROR] JSON parse failed during load: "));
    Serial.println(error.f_str());
    return;
  }

  strlcpy(ssid, doc["ssid"] | "", sizeof(ssid));
  strlcpy(password, doc["password"] | "", sizeof(password));
  strlcpy(openWeatherApiKey, doc["openWeatherApiKey"] | "", sizeof(openWeatherApiKey));  // Corrected typo here
  strlcpy(openWeatherCity, doc["openWeatherCity"] | "", sizeof(openWeatherCity));
  strlcpy(openWeatherCountry, doc["openWeatherCountry"] | "", sizeof(openWeatherCountry));
  strlcpy(weatherUnits, doc["weatherUnits"] | "metric", sizeof(weatherUnits));
  clockDuration = doc["clockDuration"] | 10000;
  weatherDuration = doc["weatherDuration"] | 5000;
  strlcpy(timeZone, doc["timeZone"] | "Etc/UTC", sizeof(timeZone));
  if (doc.containsKey("language")) {
    strlcpy(language, doc["language"], sizeof(language));
  } else {
    strlcpy(language, "en", sizeof(language));
    Serial.println(F("[CONFIG] 'language' key not found in config.json, defaulting to 'en'."));
  }

  brightness = doc["brightness"] | 7;
  twelveHourToggle = doc["twelveHourToggle"] | false;
  showHumidity = doc["showHumidity"] | false;

  String de = doc["dimmingEnabled"].as<String>();
  dimmingEnabled = (de == "true" || de == "on" || de == "1");

  dimStartHour = doc["dimStartHour"] | 18;
  dimStartMinute = doc["dimStartMinute"] | 0;
  dimEndHour = doc["dimEndHour"] | 8;
  dimEndMinute = doc["dimEndMinute"] | 0;
  dimBrightness = doc["dimBrightness"] | 0;

  strlcpy(ntpServer1, doc["ntpServer1"] | "pool.ntp.org", sizeof(ntpServer1));
  strlcpy(ntpServer2, doc["ntpServer2"] | "time.nist.gov", sizeof(ntpServer2));

  if (strcmp(weatherUnits, "imperial") == 0)
    tempSymbol = ']';
  else
    tempSymbol = '[';

  Serial.println(F("[CONFIG] Configuration loaded."));
}



// -----------------------------------------------------------------------------
// WiFi Setup
// -----------------------------------------------------------------------------
const char *DEFAULT_AP_PASSWORD = "12345678";
const char *AP_SSID = "TimeBuddy";

void connectWiFi() {
  Serial.println(F("[WIFI] Connecting to WiFi..."));

  bool credentialsExist = (strlen(ssid) > 0);

  if (!credentialsExist) {
    Serial.println(F("[WIFI] No saved credentials. Starting AP mode directly."));
    WiFi.disconnect(true);

    ESP.eraseConfig();
    WiFi.mode(WIFI_AP);
    delay(100);

    if (strlen(DEFAULT_AP_PASSWORD) < 8) {
      WiFi.softAP(AP_SSID);
      Serial.println(F("[WIFI] AP Mode started (no password, too short)."));
    } else {
      WiFi.softAP(AP_SSID, DEFAULT_AP_PASSWORD);
      Serial.println(F("[WIFI] AP Mode started."));
    }

    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.print(F("[WIFI] AP IP address: "));
    Serial.println(WiFi.softAPIP());
    isAPMode = true;

    WiFiMode_t mode = WiFi.getMode();
    Serial.printf("[WIFI] WiFi mode after setting AP: %s\n",
                  mode == WIFI_OFF ? "OFF" : mode == WIFI_STA    ? "STA ONLY"
                                           : mode == WIFI_AP     ? "AP ONLY"
                                           : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                                                 : "UNKNOWN");

    Serial.println(F("[WIFI] AP Mode Started"));
    return;
  }

  // If credentials exist, attempt STA connection
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  WiFi.setPhyMode(WIFI_PHY_MODE_11B);
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();

  const unsigned long timeout = 30000;
  unsigned long animTimer = 0;
  int animFrame = 0;
  bool animating = true;

  while (animating) {
    unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[WIFI] Connected: ") + WiFi.localIP().toString());
      isAPMode = false;

      WiFiMode_t mode = WiFi.getMode();
      Serial.printf("[WIFI] WiFi mode after STA connection: %s\n",
                    mode == WIFI_OFF ? "OFF" : mode == WIFI_STA    ? "STA ONLY"
                                             : mode == WIFI_AP     ? "AP ONLY"
                                             : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                                                   : "UNKNOWN");

      // --- IP Display initiation ---
      pendingIpToShow = WiFi.localIP().toString();
      showingIp = true;
      ipDisplayCount = 0;  // Reset count for IP display
      P.displayClear();
      P.setCharSpacing(1);  // Set spacing for IP scroll
      textEffect_t actualScrollDirection = PA_SCROLL_LEFT;
      P.displayScroll(pendingIpToShow.c_str(), PA_CENTER, actualScrollDirection, IP_SCROLL_SPEED);
      // --- END IP Display initiation ---

      animating = false;  // Exit the connection loop
      break;
    } else if (now - startAttemptTime >= timeout) {
      Serial.println(F("[WiFi] Failed. Starting AP mode..."));
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, DEFAULT_AP_PASSWORD);
      Serial.print(F("[WiFi] AP IP address: "));
      Serial.println(WiFi.softAPIP());
      dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
      isAPMode = true;

      WiFiMode_t mode = WiFi.getMode();
      Serial.printf("[WIFI] WiFi mode after STA failure and setting AP: %s\n",
                    mode == WIFI_OFF ? "OFF" : mode == WIFI_STA    ? "STA ONLY"
                                             : mode == WIFI_AP     ? "AP ONLY"
                                             : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                                                   : "UNKNOWN");

      animating = false;
      Serial.println(F("[WIFI] AP Mode Started"));
      break;
    }
    if (now - animTimer > 750) {
      animTimer = now;
      P.setTextAlignment(PA_CENTER);
      switch (animFrame % 3) {
        case 0: P.print(F("# ©")); break;
        case 1: P.print(F("# ª")); break;
        case 2: P.print(F("# «")); break;
      }
      animFrame++;
    }
    yield();
  }
}

// -----------------------------------------------------------------------------
// Time / NTP Functions
// -----------------------------------------------------------------------------
void setupTime() {
  sntp_stop();
  if (!isAPMode) {
    Serial.println(F("[TIME] Starting NTP sync"));
  }
  configTime(0, 0, ntpServer1, ntpServer2);
  setenv("TZ", ianaToPosix(timeZone), 1);
  tzset();
  ntpState = NTP_SYNCING;
  ntpStartTime = millis();
  ntpRetryCount = 0;
  ntpSyncSuccessful = false;
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------
void printConfigToSerial() {
  Serial.println(F("========= Loaded Configuration ========="));
  Serial.print(F("WiFi SSID: "));
  Serial.println(ssid);
  Serial.print(F("WiFi Password: "));
  Serial.println(password);
  Serial.print(F("OpenWeather City: "));
  Serial.println(openWeatherCity);
  Serial.print(F("OpenWeather Country: "));
  Serial.println(openWeatherCountry);
  Serial.print(F("OpenWeather API Key: "));
  Serial.println(openWeatherApiKey);
  Serial.print(F("Temperature Unit: "));
  Serial.println(weatherUnits);
  Serial.print(F("Clock duration: "));
  Serial.println(clockDuration);
  Serial.print(F("Weather duration: "));
  Serial.println(weatherDuration);
  Serial.print(F("TimeZone (IANA): "));
  Serial.println(timeZone);
  Serial.print(F("Days of the Week/Weather description language: "));
  Serial.println(language);
  Serial.print(F("Brightness: "));
  Serial.println(brightness);
  Serial.print(F("Show 12h Clock: "));
  Serial.println(twelveHourToggle ? "Yes" : "No");
  Serial.print(F("Show Humidity "));
  Serial.println(showHumidity ? "Yes" : "No");
  Serial.print(F("NTP Server 1: "));
  Serial.println(ntpServer1);
  Serial.print(F("NTP Server 2: "));
  Serial.println(ntpServer2);
  Serial.print(F("Dimming Enabled: "));
  Serial.println(dimmingEnabled);
  Serial.print(F("Dimming Start Hour: "));
  Serial.println(dimStartHour);
  Serial.print(F("Dimming Start Minute: "));
  Serial.println(dimStartMinute);
  Serial.print(F("Dimming End Hour: "));
  Serial.println(dimEndHour);
  Serial.print(F("Dimming End Minute: "));
  Serial.println(dimEndMinute);
  Serial.print(F("Dimming Brightness: "));
  Serial.println(dimBrightness);
  Serial.println(F("========================================"));
  Serial.println();
}

// -----------------------------------------------------------------------------
// Web Server and Captive Portal
// -----------------------------------------------------------------------------
void handleCaptivePortal(AsyncWebServerRequest *request);

void setupWebServer() {
  Serial.println(F("[WEBSERVER] Setting up web server..."));

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /"));
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /config.json"));
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
      Serial.println(F("[WEBSERVER] Error opening /config.json"));
      request->send(500, "application/json", "{\"error\":\"Failed to open config.json\"}");
      return;
    }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Error parsing /config.json: "));
      Serial.println(err.f_str());
      request->send(500, "application/json", "{\"error\":\"Failed to parse config.json\"}");
      return;
    }
    doc[F("mode")] = isAPMode ? "ap" : "sta";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /save"));
    DynamicJsonDocument doc(2048);

    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      Serial.println(F("[WEBSERVER] Existing config.json found, loading for update..."));
      DeserializationError err = deserializeJson(doc, configFile);
      configFile.close();
      if (err) {
        Serial.print(F("[WEBSERVER] Error parsing existing config.json: "));
        Serial.println(err.f_str());
      }
    } else {
      Serial.println(F("[WEBSERVER] config.json not found, starting with empty doc for save."));
    }

    for (int i = 0; i < request->params(); i++) {
      const AsyncWebParameter *p = request->getParam(i);
      String n = p->name();
      String v = p->value();

      if (n == "brightness") doc[n] = v.toInt();
      else if (n == "clockDuration") doc[n] = v.toInt();
      else if (n == "weatherDuration") doc[n] = v.toInt();
      else if (n == "twelveHourToggle") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "showHumidity") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "dimStartHour") doc[n] = v.toInt();
      else if (n == "dimStartMinute") doc[n] = v.toInt();
      else if (n == "dimEndHour") doc[n] = v.toInt();
      else if (n == "dimEndMinute") doc[n] = v.toInt();
      else if (n == "dimBrightness") doc[n] = v.toInt();
      else if (n == "dimmingEnabled") doc[n] = (v == "true" || v == "on" || v == "1");
      else if (n == "weatherUnits") doc[n] = v;
      else {
        doc[n] = v;
      }
    }

    FSInfo fs_info;
    LittleFS.info(fs_info);
    Serial.printf("[SAVE] LittleFS total bytes: %u, used bytes: %u\n", fs_info.totalBytes, fs_info.usedBytes);

    if (LittleFS.exists("/config.json")) {
      Serial.println(F("[SAVE] Renaming /config.json to /config.bak"));
      LittleFS.rename("/config.json", "/config.bak");
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for writing!"));
      DynamicJsonDocument errorDoc(256);
      errorDoc[F("error")] = "Failed to write config file.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    size_t bytesWritten = serializeJson(doc, f);
    Serial.printf("[SAVE] Bytes written to /config.json: %u\n", bytesWritten);
    f.close();
    Serial.println(F("[SAVE] /config.json file closed."));

    File verify = LittleFS.open("/config.json", "r");
    if (!verify) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for reading during verification!"));
      DynamicJsonDocument errorDoc(256);
      errorDoc[F("error")] = "Verification failed: Could not re-open config file.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    while (verify.available()) {
      verify.read();
    }
    verify.seek(0);

    DynamicJsonDocument test(2048);
    DeserializationError err = deserializeJson(test, verify);
    verify.close();

    if (err) {
      Serial.print(F("[SAVE] Config corrupted after save: "));
      Serial.println(err.f_str());
      DynamicJsonDocument errorDoc(256);
      errorDoc[F("error")] = String("Config corrupted. Reboot cancelled. Error: ") + err.f_str();
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    Serial.println(F("[SAVE] Config verification successful."));
    DynamicJsonDocument okDoc(128);
    okDoc[F("message")] = "Saved successfully. Rebooting...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
    Serial.println(F("[WEBSERVER] Sending success response and scheduling reboot..."));

    request->onDisconnect([]() {
      Serial.println(F("[WEBSERVER] Client disconnected, rebooting ESP..."));
      ESP.restart();
    });
  });

  server.on("/restore", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /restore"));
    if (LittleFS.exists("/config.bak")) {
      File src = LittleFS.open("/config.bak", "r");
      if (!src) {
        Serial.println(F("[WEBSERVER] Failed to open /config.bak"));
        DynamicJsonDocument errorDoc(128);
        errorDoc[F("error")] = "Failed to open backup file.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      File dst = LittleFS.open("/config.json", "w");
      if (!dst) {
        src.close();
        Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
        DynamicJsonDocument errorDoc(128);
        errorDoc[F("error")] = "Failed to open config for writing.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }

      while (src.available()) {
        dst.write(src.read());
      }
      src.close();
      dst.close();

      DynamicJsonDocument okDoc(128);
      okDoc[F("message")] = "✅ Backup restored! Device will now reboot.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
      request->onDisconnect([]() {
        Serial.println(F("[WEBSERVER] Rebooting after restore..."));
        ESP.restart();
      });

    } else {
      Serial.println(F("[WEBSERVER] No backup found"));
      DynamicJsonDocument errorDoc(128);
      errorDoc[F("error")] = "No backup found.";
      String response;
      serializeJson(errorDoc, response);
      request->send(404, "application/json", response);
    }
  });

  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(isAPMode);
    String json = "{\"isAP\": ";
    json += (isAPMode) ? "true" : "false";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/set_brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    int newBrightness = request->getParam("value", true)->value().toInt();
    if (newBrightness < 0) newBrightness = 0;
    if (newBrightness > 15) newBrightness = 15;
    brightness = newBrightness;
    P.setIntensity(brightness);
    Serial.printf("[WEBSERVER] Set brightness to %d\n", brightness);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_twelvehour", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool twelveHour = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      twelveHour = (v == "1" || v == "true" || v == "on");
    }
    twelveHourToggle = twelveHour;
    Serial.printf("[WEBSERVER] Set twelveHourToggle to %d\n", twelveHourToggle);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_humidity", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool showHumidityNow = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      showHumidityNow = (v == "1" || v == "true" || v == "on");
    }
    showHumidity = showHumidityNow;
    Serial.printf("[WEBSERVER] Set showHumidity to %d\n", showHumidity);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_language", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    String lang = request->getParam("value", true)->value();
    strlcpy(language, lang.c_str(), sizeof(language));
    Serial.printf("[WEBSERVER] Set language to %s\n", language);
    shouldFetchWeatherNow = true;
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_units", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      if (v == "1" || v == "true" || v == "on") {
        strcpy(weatherUnits, "imperial");
        tempSymbol = ']';
      } else {
        strcpy(weatherUnits, "metric");
        tempSymbol = '[';
      }
      Serial.printf("[WEBSERVER] Set weatherUnits to %s\n", weatherUnits);
      shouldFetchWeatherNow = true;
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing value parameter\"}");
    }
  });

  server.begin();
  Serial.println(F("[WEBSERVER] Web server started"));
}

void handleCaptivePortal(AsyncWebServerRequest *request) {
  Serial.print(F("[WEBSERVER] Captive Portal Redirecting: "));
  Serial.println(request->url());
  request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
}

bool isNumber(const char *str) {
  for (int i = 0; str[i]; i++) {
    if (!isdigit(str[i]) && str[i] != '.' && str[i] != '-') return false;
  }
  return true;
}

bool isFiveDigitZip(const char *str) {
  if (strlen(str) != 5) return false;
  for (int i = 0; i < 5; i++) {
    if (!isdigit(str[i])) return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Weather Fetching and API settings
// -----------------------------------------------------------------------------
String buildWeatherURL() {
  String base = "http://api.open-meteo.com/v1/forecast?current=temperature_2m,relative_humidity_2m&";

  float lat = atof(openWeatherCity);
  float lon = atof(openWeatherCountry);

  bool latValid = isNumber(openWeatherCity) && isNumber(openWeatherCountry) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;

  if (latValid) {
    base += "latitude=" + String(lat, 8) + "&longitude=" + String(lon, 8);
  }

  String langForAPI = String(language);

  return base;
}


void fetchWeather() {
  Serial.println(F("[WEATHER] Fetching weather data..."));
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WEATHER] Skipped: WiFi not connected"));
    weatherAvailable = false;
    weatherFetched = false;
    return;
  }
  if (!openWeatherApiKey || strlen(openWeatherApiKey) != 32) {
    Serial.println(F("[WEATHER] Skipped: Invalid API key (must be exactly 32 characters)"));
    weatherAvailable = false;
    weatherFetched = false;
    return;
  }
  if (!(strlen(openWeatherCity) > 0 && strlen(openWeatherCountry) > 0)) {
    Serial.println(F("[WEATHER] Skipped: City or Country is empty."));
    weatherAvailable = false;
    return;
  }

  Serial.println(F("[WEATHER] Connecting to OpenWeatherMap..."));
  String url = buildWeatherURL();
  Serial.println(F("[WEATHER] URL: ") + url);

  HTTPClient http;    // Create an HTTPClient object
  WiFiClient client;  // Create a WiFiClient object

  http.begin(client, url);  // Pass the WiFiClient object and the URL

  http.setTimeout(10000);  // Sets both connection and stream timeout to 10 seconds

  Serial.println(F("[WEATHER] Sending GET request..."));
  int httpCode = http.GET();  // Send the GET request

  if (httpCode == HTTP_CODE_OK) {  // Check if HTTP response code is 200 (OK)
    Serial.println(F("[WEATHER] HTTP 200 OK. Reading payload..."));

    String payload = http.getString();
    Serial.println(F("[WEATHER] Response received."));
    Serial.println(F("[WEATHER] Payload: ") + payload);

    DynamicJsonDocument doc(1536);  // Adjust size as needed, use ArduinoJson Assistant
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("[WEATHER] JSON parse error: "));
      Serial.println(error.f_str());
      weatherAvailable = false;
      return;
    }

    if (doc.containsKey(F("current")) && doc[F("current")].containsKey(F("temperature_2m"))) {
      float temp = doc[F("current")][F("temperature_2m")];
      currentTemp = String((int)round(temp)) + "º";
      Serial.printf("[WEATHER] Temp: %s\n", currentTemp.c_str());
      weatherAvailable = true;
    } else {
      Serial.println(F("[WEATHER] Temperature not found in JSON payload"));
      weatherAvailable = false;
      return;
    }

    if (doc.containsKey(F("current")) && doc[F("current")].containsKey(F("relative_humidity_2m"))) {
      currentHumidity = doc[F("current")][F("relative_humidity_2m")];
      Serial.printf("[WEATHER] Humidity: %d%%\n", currentHumidity);
    } else {
      currentHumidity = -1;
    }

    weatherFetched = true;

  } else {
    Serial.printf("[WEATHER] HTTP GET failed, error code: %d, reason: %s\n", httpCode, http.errorToString(httpCode).c_str());
    weatherAvailable = false;
    weatherFetched = false;
  }

  http.end();
}



// -----------------------------------------------------------------------------
// Main setup() and loop()
// -----------------------------------------------------------------------------
/*
DisplayMode key:
  0: Clock
  1: Weather
*/

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("[SETUP] Starting setup..."));

  pinMode(BUTTON_PIN, INPUT_PULLUP);


  if (!LittleFS.begin()) {
    Serial.println(F("[ERROR] LittleFS mount failed in setup! Halting."));
    while (true) {
      delay(1000);
      yield();
    }
  }
  Serial.println(F("[SETUP] LittleFS file system mounted successfully."));

  P.begin();  // Initialize Parola library

  P.setCharSpacing(0);
  P.setFont(mFactory);
  loadConfig();  // This function now has internal yields and prints

  P.setIntensity(brightness);

  Serial.println(F("[SETUP] Parola (LED Matrix) initialized"));

  connectWiFi();

  if (isAPMode) {
    Serial.println(F("[SETUP] WiFi connection failed. Device is in AP Mode."));
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("[SETUP] WiFi connected successfully to local network."));
  } else {
    Serial.println(F("[SETUP] WiFi state is uncertain after connection attempt."));
  }

  setupWebServer();
  Serial.println(F("[SETUP] Webserver setup complete"));
  Serial.println(F("[SETUP] Setup complete"));
  Serial.println();
  printConfigToSerial();
  setupTime();
  displayMode = 0;
  lastSwitch = millis();
  lastColonBlink = millis();
}

void checkUpdate() {
  const String jsonUrl = "https://samuka1010.github.io/SmartDesktopBuddy/firmware.json?t=" + String(millis());

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  if (http.begin(client, jsonUrl)) {
    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      Serial.println("JSON recebido:");

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.println("Erro ao ler JSON!");
        loadingInterface = false;
        return;
      }

      String ultimaVersao = doc["version"];
      String urlFirmware = doc["firmware"];

      Serial.println("Versão remota: " + ultimaVersao);
      Serial.println("Versão local: " + localFirmwareVersion);

      if (ultimaVersao != localFirmwareVersion) {
        Serial.println("Atualização disponível! Baixando...");
        Serial.println("Atualizando");
        Serial.println(urlFirmware);
        
        updateFirmware(client, urlFirmware);
      } else {
        Serial.println("Firmware já está atualizado.");
        loadingInterface = false;
      }
    } else {
      Serial.printf("Erro HTTP ao buscar JSON: %d\n", httpCode);
      loadingInterface = false;
    }

    http.end();
  } else {
    Serial.println("Falha ao conectar ao servidor JSON.");
    loadingInterface = false;
  }
  
}

void updateFirmware(WiFiClientSecure client, String firmwareUrl) {
  bool updateSuccessful = true;

  ESPhttpUpdate.onStart([]() {
    showLoader();
  });
  ESPhttpUpdate.onProgress([](size_t done, size_t total) {
    showLoader();
  });
  ESPhttpUpdate.onEnd([]() {
    P.displayClear();
  });
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, firmwareUrl);
  

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      updateSuccessful = false;
      Serial.printf("Falha na atualização. Código: %d\n", ESPhttpUpdate.getLastError());
      Serial.println(ESPhttpUpdate.getLastErrorString());
      loadingInterface = false;
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("Sem atualizações disponíveis.");
      loadingInterface = false;
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Atualização concluída. Reiniciando...");
      break;
  }

  updatePage();
}

void updatePage() {
  HTTPClient http;

  const String& caminhoLocal = "/index.html";
  const String pageUrl = "https://samuka1010.github.io/SmartDesktopBuddy/data/index.html?t=" + String(millis());

  Serial.print("Baixando index.html: ");
  Serial.println(pageUrl);
  http.begin(client, pageUrl);
  http.setUserAgent("ESP8266-Agent");  // Adiciona User-Agent

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    File f = LittleFS.open(caminhoLocal, "w");
    if (!f) {
      Serial.println("Erro ao abrir arquivo para escrita");
      return;
    }

    http.writeToStream(&f);
    f.close();

    Serial.println("Arquivo salvo em " + caminhoLocal);
  } else {
    Serial.printf("Erro HTTP: %d\n", httpCode);
  }

  http.end();
}


void advanceDisplayMode() {
  int oldMode = displayMode;  // Store the old mode

  // Determine the next display mode based on the current mode and conditions
  if (displayMode == 0) {  // Current mode is Clock
    if (weatherAvailable && (strlen(openWeatherApiKey) == 32) && (strlen(openWeatherCity) > 0) && (strlen(openWeatherCountry) > 0)) {
      displayMode = 1;  // Clock -> Weather (if weather is available and configured)
      Serial.println(F("[DISPLAY] Switching to display mode: WEATHER (from Clock)"));
    } else {
      displayMode = 0;  // Clock -> Clock (if weather is not available)
      Serial.println(F("[DISPLAY] Staying in CLOCK (from Clock, no weather available)"));
    }
  } else if (displayMode == 1) {  // Current mode is Weather
      displayMode = 0; 
      Serial.println(F("[DISPLAY] Switching to display mode: CLOCK (from Weather)"));
  } 
  // --- Common cleanup/reset after mode switch ---
  lastSwitch = millis();  // Reset the timer for the new mode's duration
}

void showLoader() {
  if (millis() - lastLoaderAnim > loaderInterval) {
    lastLoaderAnim = millis();
    loadingFrame = String(loadingFrame) + ".";
    if (loadingFrame.length() == 6){
      loadingFrame = ".";
    }

    P.displayClear();
    P.displayText(loadingFrame.c_str(), PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    P.displayAnimate();
  }
}

void APAnimation() {
  static unsigned long apAnimTimer = 0;
  static int apAnimFrame = 0;
    unsigned long now = millis();
    if (now - apAnimTimer > 750) {
      apAnimTimer = now;
      apAnimFrame++;
    }
    P.setTextAlignment(PA_CENTER);
    switch (apAnimFrame % 3) {
      case 0: P.print(F("= ©")); break;
      case 1: P.print(F("= ª")); break;
      case 2: P.print(F("= «")); break;
    }
    yield();
    return;
  
}

void CheckDimming() {
  time_t now_time = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now_time, &timeinfo);
  int curHour = timeinfo.tm_hour;
  int curMinute = timeinfo.tm_min;
  int curTotal = curHour * 60 + curMinute;
  int startTotal = dimStartHour * 60 + dimStartMinute;
  int endTotal = dimEndHour * 60 + dimEndMinute;
  bool isDimmingActive = false;

  if (dimmingEnabled) {
    if (startTotal < endTotal) {
      isDimmingActive = (curTotal >= startTotal && curTotal < endTotal);
    } else {  // Overnight dimming
      isDimmingActive = (curTotal >= startTotal || curTotal < endTotal);
    }
    if (isDimmingActive) {
      P.setIntensity(dimBrightness);
    } else {
      P.setIntensity(brightness);
    }
  } else {
    P.setIntensity(brightness);
  }
}

void showIP() {
    if (P.displayAnimate()) {
      ipDisplayCount++;
      if (ipDisplayCount < ipDisplayMax) {
        P.displayScroll(pendingIpToShow.c_str(), PA_CENTER, PA_SCROLL_LEFT, 120);
      } else {
        showingIp = false;
        P.displayClear();
        delay(500);  // Blocking delay as in working copy
        displayMode = 0;
        lastSwitch = millis();
      }
    }
    yield();
    return;  // Exit loop early if showing IP
  
}

void NTPSync() {
  switch (ntpState) {
    case NTP_IDLE: break;
    case NTP_SYNCING:
      {
        time_t now = time(nullptr);
        if (now > 1000) {  // NTP sync successful
          Serial.println(F("[TIME] NTP sync successful."));
          ntpSyncSuccessful = true;
          ntpState = NTP_SUCCESS;
        } else if (millis() - ntpStartTime > ntpTimeout || ntpRetryCount >= maxNtpRetries) {
          Serial.println(F("[TIME] NTP sync failed."));
          ntpSyncSuccessful = false;
          ntpState = NTP_FAILED;
        } else {
          if (millis() - ntpStartTime > ((unsigned long)(ntpRetryCount + 1) * 1000UL)) {
            ntpRetryCount++;
          }
        }
        break;
      }
    case NTP_SUCCESS:
      if (!tzSetAfterSync) {
        const char *posixTz = ianaToPosix(timeZone);
        setenv("TZ", posixTz, 1);
        tzset();
        tzSetAfterSync = true;
      }
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
    case NTP_FAILED:
      ntpAnimTimer = 0;
      ntpAnimFrame = 0;
      break;
  }
}

void weatherSync() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!weatherFetchInitiated || shouldFetchWeatherNow || (millis() - lastWeatherFetch > weatherFetchInterval)) {
      if (shouldFetchWeatherNow) {
        Serial.println(F("[LOOP] Immediate weather fetch requested by web server."));
        shouldFetchWeatherNow = false;
      } else if (!weatherFetchInitiated) {
        Serial.println(F("[LOOP] Initial weather fetch."));
      } else {
        Serial.println(F("[LOOP] Regular interval weather fetch."));
      }
      weatherFetchInitiated = true;
      weatherFetched = false;
      fetchWeather();
      lastWeatherFetch = millis();
    }
  } else {
    weatherFetchInitiated = false;
    shouldFetchWeatherNow = false;
  }
}

void showTime() {
  P.setCharSpacing(0);

  time_t now_time = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now_time, &timeinfo);
  const char *const *daysOfTheWeek = getDaysOfWeek(language);
  const char *daySymbol = daysOfTheWeek[timeinfo.tm_wday];

  char timeStr[9];
  if (twelveHourToggle) {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    sprintf(timeStr, "%d:%02d", hour12, timeinfo.tm_min);
  } else {
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }

  char timeSpacedStr[20];
  int j = 0;
  for (int i = 0; timeStr[i] != '\0'; i++) {
    timeSpacedStr[j++] = timeStr[i];
    if (timeStr[i + 1] != '\0') {
      timeSpacedStr[j++] = ' ';
    }
  }
  timeSpacedStr[j] = '\0';

  String formattedTime;
  formattedTime = String(daySymbol) + " " + String(timeSpacedStr);
  
  String timeString = formattedTime;
  if (!colonVisible) timeString.replace(":", " ");
  // Serial.println(timeString);
  P.print(timeString);

  yield();
  return;
}

void showWeather() {
  P.setCharSpacing(1);
    if (weatherAvailable) {
      String weatherDisplay;
      if (showHumidity && currentHumidity != -1) {
        int cappedHumidity = (currentHumidity > 99) ? 99 : currentHumidity;
        weatherDisplay = currentTemp + " " + String(cappedHumidity) + "%";
      } else {
        weatherDisplay = currentTemp + tempSymbol;
      }
      P.print(weatherDisplay.c_str());
    } 
    yield();
    return;
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    APAnimation();
    return;
  }
  bool isButtonPressed = digitalRead(BUTTON_PIN) == 0;

  const unsigned long colonBlinkInterval = 800;
  if (millis() - lastColonBlink > colonBlinkInterval) {
    colonVisible = !colonVisible;
    lastColonBlink = millis();
  }
  
  if (!updateChecked) {
    Serial.print("CHECANDO UPDATE");
    updateChecked = true;
    checkUpdate();
    P.displayClear();
  }

  if (loadingInterface) {
    showLoader();
    return;
  }

  // Dimming
  CheckDimming();

  // IP Display
  if (showingIp) {
    showIP();
    return;
  }

  //NTP State Machine
  NTPSync();

  //Weather sync
  weatherSync();

  if (isButtonPressed && (isButtonPressed != lastButtonStatus)) {
    lastButtonStatus = isButtonPressed;
    advanceDisplayMode();
    delay(100);
    yield();
  } else {
    lastButtonStatus = isButtonPressed;
  }

   // Only advance mode by timer for clock/weather, not description!
  unsigned long displayDuration = (displayMode == 0) ? clockDuration : weatherDuration;
  if ((displayMode == 0 || displayMode == 1) && millis() - lastSwitch > displayDuration) {
    advanceDisplayMode();
  }

  // CLOCK Mode
  if (displayMode == 0) {
    showTime();
  }

  // WEATHER Mode
  if (displayMode == 1) {
    showWeather();
  }

  yield();
}