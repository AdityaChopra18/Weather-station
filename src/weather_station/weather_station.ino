// ============================================================
//  Open Weather Station v2.0 — ESP32 Firmware
//  Author  : Adi (github.com/YOUR_GITHUB_USERNAME)
//  Project : github.com/YOUR_GITHUB_USERNAME/YOUR_REPO_NAME
//
//  Sensors  : BMP280 (temperature + pressure)
//             DHT11  (temperature + humidity)
//             FC-37  (rain detection)
//  Storage  : SPIFFS (local buffer when offline)
//  Sync     : GitHub CSV via REST API
//  Alerts   : Telegram bot (low battery + rain warning)
//  Power    : TP4056 + 18650 + optional solar panel
//
//  SETUP:
//  1. Fill in CONFIG section below with your details
//  2. Install libraries listed in README.md
//  3. Tools → Board → ESP32 Dev Module
//  4. Upload and open Serial Monitor at 115200 baud
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <base64.h>
#include <time.h>

// ============================================================
//  CONFIG — FILL IN YOUR DETAILS HERE
// ============================================================

// -- WiFi Networks ------------------------------------------
struct WifiCredential {
  const char* ssid;
  const char* password;
};

WifiCredential wifiNetworks[] = {
  { "YOUR_HOME_WIFI",     "YOUR_HOME_PASSWORD"    },
  { "YOUR_PHONE_HOTSPOT", "YOUR_HOTSPOT_PASSWORD" },
  // add more networks here
};
const int WIFI_COUNT = sizeof(wifiNetworks) / sizeof(wifiNetworks[0]);

// -- GitHub -------------------------------------------------
const char* GITHUB_TOKEN  = "YOUR_GITHUB_PERSONAL_ACCESS_TOKEN";
const char* GITHUB_USER   = "YOUR_GITHUB_USERNAME";
const char* GITHUB_REPO   = "YOUR_REPO_NAME";
const char* GITHUB_FILE   = "data/readings.csv";
const char* GITHUB_BRANCH = "main";

// -- Telegram -----------------------------------------------
const char* TELEGRAM_TOKEN   = "YOUR_TELEGRAM_BOT_TOKEN";
const char* TELEGRAM_CHAT_ID = "YOUR_TELEGRAM_CHAT_ID";

// -- Station Info -------------------------------------------
const char* STATION_NAME = "YOUR_CITY_Weather_Station";
const char* STATION_LAT  = "26.4499";   // Ajmer latitude
const char* STATION_LON  = "74.6399";   // Ajmer longitude

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define DHT_PIN        4    // DHT11 data pin
#define DHT_TYPE       DHT11
#define RAIN_AO_PIN    34   // Rain sensor analog
#define RAIN_DO_PIN    35   // Rain sensor digital
#define BATTERY_PIN    32   // Battery voltage divider (optional)
#define SDA_PIN        21   // BMP280 SDA
#define SCL_PIN        22   // BMP280 SCL

// ============================================================
//  TIMING
// ============================================================
const unsigned long READ_INTERVAL    = 5  * 60 * 1000UL; // 5 minutes
const unsigned long SYNC_INTERVAL    = 15 * 60 * 1000UL; // 15 minutes
const float         BATTERY_LOW      = 3.5;
const float         BATTERY_CRITICAL = 3.3;

// Rain alert thresholds
const float PRESSURE_DROP_THRESHOLD  = 2.0;  // hPa drop in 1 hour = rain coming
const float HUMIDITY_THRESHOLD       = 75.0; // % humidity spike

// ============================================================
//  GLOBALS
// ============================================================
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastReadTime     = 0;
unsigned long lastSyncTime     = 0;
bool alertSentLow              = false;
bool alertSentCritical         = false;
bool rainAlertSent             = false;
unsigned long lastRainAlert    = 0;
const char* LOCAL_CSV          = "/readings.csv";

// Store last few pressure readings for trend detection
float pressureHistory[12];   // 12 x 5min = 1 hour
int   pressureIndex = 0;
bool  pressureHistoryFull = false;

float lastGoodHumidity    = 0;
float lastGoodDhtTemp     = 0;

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n================================");
  Serial.println("  Open Weather Station v2.0");
  Serial.println("================================");
  Serial.println("Station : " + String(STATION_NAME));
  Serial.println("Location: " + String(STATION_LAT) + ", " + String(STATION_LON));

  pinMode(RAIN_DO_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT);

  // BMP280
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!bmp.begin(0x76) && !bmp.begin(0x77)) {
    Serial.println("ERROR: BMP280 not found! Check wiring.");
    while (1) delay(1000);
  }
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                  Adafruit_BMP280::SAMPLING_X2,
                  Adafruit_BMP280::SAMPLING_X16,
                  Adafruit_BMP280::FILTER_X16,
                  Adafruit_BMP280::STANDBY_MS_500);
  Serial.println("BMP280 OK");

  // DHT11
  dht.begin();
  Serial.println("DHT11 OK");

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("ERROR: SPIFFS failed");
    while (1) delay(1000);
  }
  Serial.println("SPIFFS OK");

  // Create CSV header if new
  if (!SPIFFS.exists(LOCAL_CSV)) {
    File f = SPIFFS.open(LOCAL_CSV, FILE_WRITE);
    if (f) {
      f.println("timestamp,bmp_temp_c,dht_temp_c,avg_temp_c,humidity_pct,pressure_hpa,is_raining,rain_analog,battery_v,sent");
      f.close();
      Serial.println("CSV created");
    }
  }

  // Initialize pressure history
  for (int i = 0; i < 12; i++) pressureHistory[i] = 0;

  // Try sync on boot
  if (connectWifi()) {
    syncTime();
    syncToGithub();
    WiFi.disconnect(true);
  }

  Serial.println("\nStarting readings...\n");
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  if (now - lastReadTime >= READ_INTERVAL || lastReadTime == 0) {
    lastReadTime = now;
    takeReading();
  }

  if (now - lastSyncTime >= SYNC_INTERVAL || lastSyncTime == 0) {
    lastSyncTime = now;
    if (connectWifi()) {
      syncToGithub();
      checkBatteryAlert();
      WiFi.disconnect(true);
    }
  }

  delay(10000);
}

// ============================================================
//  TAKE SENSOR READING
// ============================================================
void takeReading() {
  // BMP280
  float bmpTemp  = bmp.readTemperature();
  float pressure = bmp.readPressure() / 100.0F;

  // DHT11 with retry (handles unreliable readings)
  float humidity = NAN;
  float dhtTemp  = NAN;
  for (int i = 0; i < 3; i++) {
    humidity = dht.readHumidity();
    dhtTemp  = dht.readTemperature();
    if (!isnan(humidity) && !isnan(dhtTemp)) break;
    delay(500);
  }

  // Use last good values if DHT11 fails
  if (isnan(humidity)) {
    humidity = lastGoodHumidity;
    Serial.println("DHT11 failed, using last good humidity");
  } else {
    lastGoodHumidity = humidity;
  }

  if (isnan(dhtTemp)) {
    dhtTemp = lastGoodDhtTemp;
  } else {
    lastGoodDhtTemp = dhtTemp;
  }

  // Average temperature from both sensors
  float avgTemp = (bmpTemp + dhtTemp) / 2.0;

  // Rain sensor
  int   rainAO  = analogRead(RAIN_AO_PIN);
  bool  raining = (digitalRead(RAIN_DO_PIN) == LOW);

  // Battery
  float battery = readBatteryVoltage();

  // Timestamp
  String ts = getTimestamp();

  // Update pressure history for trend detection
  pressureHistory[pressureIndex] = pressure;
  pressureIndex = (pressureIndex + 1) % 12;
  if (pressureIndex == 0) pressureHistoryFull = true;

  // Check for rain alert
  checkRainAlert(pressure, humidity, raining);

  // Print to serial
  Serial.println("─────────────────────────────");
  Serial.println("Time     : " + ts);
  Serial.printf("BMP Temp : %.1f°C\n", bmpTemp);
  Serial.printf("DHT Temp : %.1f°C\n", dhtTemp);
  Serial.printf("Avg Temp : %.1f°C\n", avgTemp);
  Serial.printf("Humidity : %.1f%%\n", humidity);
  Serial.printf("Pressure : %.1f hPa\n", pressure);
  Serial.printf("Raining  : %s\n", raining ? "YES" : "NO");
  if (battery > 0.5) Serial.printf("Battery  : %.2fV\n", battery);
  Serial.println("─────────────────────────────");

  // Save to SPIFFS
  File f = SPIFFS.open(LOCAL_CSV, FILE_APPEND);
  if (f) {
    f.printf("%s,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%.2f,0\n",
      ts.c_str(), bmpTemp, dhtTemp, avgTemp,
      humidity, pressure,
      raining ? 1 : 0, rainAO, battery);
    f.close();
    Serial.println("Saved to SPIFFS");
  }
}

// ============================================================
//  RAIN ALERT — runs on ESP32 directly, instant notification
// ============================================================
void checkRainAlert(float pressure, float humidity, bool raining) {
  // Cooldown — don't spam alerts (2 hour gap minimum)
  unsigned long now = millis();
  if (rainAlertSent && (now - lastRainAlert < 2 * 60 * 60 * 1000UL)) return;

  // Calculate pressure trend over last 1 hour
  float pressureDrop = 0;
  if (pressureHistoryFull) {
    float oldest = pressureHistory[pressureIndex]; // oldest reading
    float latest = pressureHistory[(pressureIndex + 11) % 12]; // latest
    pressureDrop = oldest - latest; // positive = dropping
  }

  // Rain conditions
  bool pressureDropping = pressureDrop > PRESSURE_DROP_THRESHOLD;
  bool humidityHigh     = humidity > HUMIDITY_THRESHOLD;

  // Alert if rain sensor triggered OR (pressure dropping AND humidity high)
  if (raining || (pressureDropping && humidityHigh)) {
    String reason = "";
    if (raining)          reason += "Rain sensor triggered\n";
    if (pressureDropping) reason += "Pressure dropping: -" + String(pressureDrop, 1) + " hPa/hr\n";
    if (humidityHigh)     reason += "Humidity high: " + String(humidity, 1) + "%\n";

    String msg = "⚠️ Rain Alert — " + String(STATION_NAME) + "\n" + reason;
    if (pressureDropping && !raining) {
      msg += "🌧 Rain expected within 30-60 minutes";
    } else {
      msg += "🌧 Rain detected now!";
    }

    if (connectWifi()) {
      sendTelegram(msg);
      WiFi.disconnect(true);
      rainAlertSent = true;
      lastRainAlert = now;
    }
  }

  // Reset alert when conditions clear
  if (!raining && humidity < 60 && pressureDrop < 0.5) {
    if (rainAlertSent) {
      if (connectWifi()) {
        sendTelegram("✅ Rain cleared — " + String(STATION_NAME) + "\nSkies clearing up!");
        WiFi.disconnect(true);
      }
    }
    rainAlertSent = false;
  }
}

// ============================================================
//  SYNC TO GITHUB
// ============================================================
void syncToGithub() {
  Serial.println("Syncing to GitHub...");

  File f = SPIFFS.open(LOCAL_CSV, FILE_READ);
  if (!f) return;

  String unsyncedRows = "";
  int count = 0;
  bool firstLine = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) { firstLine = false; continue; }
    if (line.endsWith(",0")) {
      unsyncedRows += line.substring(0, line.length() - 2) + "\n";
      count++;
    }
  }
  f.close();

  if (count == 0) { Serial.println("Nothing to sync"); return; }
  Serial.printf("Syncing %d rows...\n", count);

  String sha = getGithubFileSHA();
  if (appendToGithub(unsyncedRows, sha)) {
    markRowsAsSent();
    Serial.println("GitHub sync successful!");
  } else {
    Serial.println("Sync failed, will retry next cycle");
  }
}

String getGithubFileSHA() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.github.com/repos/" + String(GITHUB_USER) +
               "/" + String(GITHUB_REPO) + "/contents/" + String(GITHUB_FILE);
  http.begin(client, url);
  http.addHeader("Authorization", "token " + String(GITHUB_TOKEN));
  http.addHeader("User-Agent", "ESP32-WeatherStation");
  String sha = "";
  if (http.GET() == 200) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, http.getString());
    sha = doc["sha"].as<String>();
  }
  http.end();
  return sha;
}

bool appendToGithub(String newRows, String sha) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.github.com/repos/" + String(GITHUB_USER) +
               "/" + String(GITHUB_REPO) + "/contents/" + String(GITHUB_FILE);

  String existingContent = "";
  if (sha.length() > 0) {
    http.begin(client, url);
    http.addHeader("Authorization", "token " + String(GITHUB_TOKEN));
    http.addHeader("User-Agent", "ESP32-WeatherStation");
    if (http.GET() == 200) {
      StaticJsonDocument<4096> doc;
      deserializeJson(doc, http.getString());
      String encoded = doc["content"].as<String>();
      encoded.replace("\n", "");
      existingContent = base64::decode(encoded);
    }
    http.end();
  } else {
    existingContent = "timestamp,bmp_temp_c,dht_temp_c,avg_temp_c,humidity_pct,pressure_hpa,is_raining,rain_analog,battery_v\n";
  }

  String fullContent = existingContent + newRows;
  String encoded = base64::encode(fullContent);

  StaticJsonDocument<2048> body;
  body["message"] = "Weather update — " + String(STATION_NAME);
  body["content"] = encoded;
  body["branch"]  = GITHUB_BRANCH;
  if (sha.length() > 0) body["sha"] = sha;

  String bodyStr;
  serializeJson(body, bodyStr);

  http.begin(client, url);
  http.addHeader("Authorization", "token " + String(GITHUB_TOKEN));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-WeatherStation");
  int code = http.PUT(bodyStr);
  http.end();
  return (code == 200 || code == 201);
}

void markRowsAsSent() {
  File f = SPIFFS.open(LOCAL_CSV, FILE_READ);
  if (!f) return;
  String newContent = "";
  bool firstLine = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) { newContent += line + "\n"; firstLine = false; continue; }
    if (line.endsWith(",0")) line = line.substring(0, line.length() - 1) + "1";
    newContent += line + "\n";
  }
  f.close();
  File fw = SPIFFS.open(LOCAL_CSV, FILE_WRITE);
  if (fw) { fw.print(newContent); fw.close(); }
}

// ============================================================
//  BATTERY
// ============================================================
float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  return (raw / 4095.0) * 3.3 * 2.0;
}

void checkBatteryAlert() {
  float v = readBatteryVoltage();
  if (v < 0.5) return;
  if (v <= BATTERY_CRITICAL && !alertSentCritical) {
    sendTelegram("🔴 CRITICAL! Battery at " + String(v, 2) + "V\nCharge immediately! — " + String(STATION_NAME));
    alertSentCritical = true;
  } else if (v <= BATTERY_LOW && !alertSentLow) {
    sendTelegram("🟡 Battery low: " + String(v, 2) + "V\nPlease charge soon — " + String(STATION_NAME));
    alertSentLow = true;
  }
  if (v > 3.8) { alertSentLow = false; alertSentCritical = false; }
}

// ============================================================
//  TELEGRAM
// ============================================================
void sendTelegram(String message) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_TOKEN) + "/sendMessage";
  StaticJsonDocument<512> body;
  body["chat_id"] = TELEGRAM_CHAT_ID;
  body["text"]    = message;
  String bodyStr;
  serializeJson(body, bodyStr);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(bodyStr);
  Serial.printf("Telegram: %d\n", code);
  http.end();
}

// ============================================================
//  WIFI
// ============================================================
bool connectWifi() {
  for (int i = 0; i < WIFI_COUNT; i++) {
    Serial.printf("Trying: %s\n", wifiNetworks[i].ssid);
    WiFi.begin(wifiNetworks[i].ssid, wifiNetworks[i].password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500); Serial.print("."); attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s\n", wifiNetworks[i].ssid);
      return true;
    }
    WiFi.disconnect(true);
    delay(100);
  }
  Serial.println("No WiFi available");
  return false;
}

// ============================================================
//  TIME
// ============================================================
void syncTime() {
  configTime(19800, 0, "pool.ntp.org");
  Serial.print("Syncing time");
  int a = 0;
  while (time(nullptr) < 1000000000 && a < 20) {
    delay(500); Serial.print("."); a++;
  }
  Serial.println(" OK");
}

String getTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000) return "boot_" + String(millis() / 1000);
  struct tm* t = localtime(&now);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
  return String(buf);
}
