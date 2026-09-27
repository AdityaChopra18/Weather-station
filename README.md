# Open Weather Station 🌤

A DIY solar-powered weather station built with ESP32.
Logs data to GitHub every 15 minutes, trains an ML model daily,
and sends forecasts + rain alerts to Telegram automatically.

Built in Ajmer, Rajasthan, India 🇮🇳

---

## What it measures

| Sensor | Measures |
|--------|---------|
| BMP280 | Temperature + Pressure |
| DHT11  | Temperature + Humidity |
| FC-37  | Rain detection |

Two temperature sensors cross-verify each other for accuracy.

## Features

- Reads sensors every 5 minutes
- Stores data locally on ESP32 (SPIFFS) when offline
- Syncs all missed readings to GitHub when WiFi available
- Connects to multiple WiFi networks automatically
- Instant Telegram rain alerts from ESP32 directly
- Daily 7am forecast via GitHub Actions
- ML model retrains daily on new data — gets smarter over time
- Pressure trend detection for rain prediction

## Hardware — Total cost ~₹800

| Component | Purpose | Cost |
|-----------|---------|------|
| ESP32 DevKit V1 (38-pin) | Brain | ₹400-500 |
| BMP280 sensor | Temp + Pressure | ₹56 |
| DHT11 module | Humidity | ₹59 |
| FC-37 rain sensor | Rain detection | ₹40-50 |
| TP4056 module (CA-033T) | Charge controller | ₹20-25 |
| 18650 Li-ion cell | Battery | ₹100-120 |
| Solar panel 70x70mm 5.5V | Charging | ₹150-200 |
| Jumper wires (F-F) | Connections | ₹20-30 |

## Wiring

```
BMP280          ESP32
VCC      →      3.3V  (pin 1)
GND      →      GND   (pin 1)
SDA      →      GPIO21
SCL      →      GPIO22

DHT11           ESP32
VCC      →      3.3V  (pin 2)
GND      →      GND   (pin 2)
DATA     →      GPIO4

Rain Sensor     ESP32
VCC      →      3.3V  (pin 3)
GND      →      GND   (pin 3)
AO       →      GPIO34
DO       →      GPIO35

TP4056          ESP32
OUT+     →      VIN
OUT-     →      GND

Solar Panel     TP4056
+        →      IN+
-        →      IN-

18650           TP4056
+        →      B+
-        →      B-
```

⚠️ Never connect USB to ESP32 and TP4056 OUT+ at the same time!

## Setup

### 1. Arduino IDE

Download from [arduino.cc](https://arduino.cc/en/software)

### 2. Add ESP32 board support

File → Preferences → Additional Board URLs:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Tools → Board Manager → search "esp32" → install **esp32 by Espressif Systems**

### 3. Install libraries

Tools → Manage Libraries → install:
- `Adafruit BMP280 Library` (click Install All for dependencies)
- `DHT sensor library` by Adafruit (click Install All)
- `ArduinoJson` by Benoit Blanchon
- `base64` by Densaugeo

### 4. Configure the code

Open `src/weather_station.ino` and fill in:

```cpp
// WiFi
{ "YOUR_WIFI_NAME", "YOUR_WIFI_PASSWORD" },

// GitHub
const char* GITHUB_TOKEN = "YOUR_TOKEN";
const char* GITHUB_USER  = "YOUR_USERNAME";
const char* GITHUB_REPO  = "YOUR_REPO_NAME";

// Telegram
const char* TELEGRAM_TOKEN   = "YOUR_BOT_TOKEN";
const char* TELEGRAM_CHAT_ID = "YOUR_CHAT_ID";

// Location
const char* STATION_NAME = "Ajmer_Weather_Station";
const char* STATION_LAT  = "26.4499";
const char* STATION_LON  = "74.6399";
```

### 5. Get GitHub token

github.com → Settings → Developer settings → Personal access tokens → New token → select `repo` scope → copy token

### 6. Create Telegram bot

1. Message [@BotFather](https://t.me/botfather) on Telegram
2. Send `/newbot` → give it a name
3. Copy the bot token
4. Message your new bot once
5. Visit `https://api.telegram.org/bot<TOKEN>/getUpdates`
6. Copy your chat ID from the response

### 7. Flash ESP32

1. Connect ESP32 via USB (TP4056 disconnected!)
2. Tools → Board → ESP32 Dev Module
3. Tools → Port → your COM port
4. Click Upload
5. Open Serial Monitor at 115200 baud
6. Watch first reading appear!

### 8. Set up GitHub Actions (daily forecast)

Add these as GitHub Secrets (repo Settings → Secrets → Actions):

```
WEATHER_GITHUB_TOKEN  →  your GitHub token
WEATHER_GITHUB_USER   →  your GitHub username
WEATHER_GITHUB_REPO   →  your repo name
TELEGRAM_TOKEN        →  your Telegram bot token
TELEGRAM_CHAT_ID      →  your Telegram chat ID
STATION_NAME          →  your station name
```

### 9. Run Python logger locally (optional)

```bash
pip install -r requirements.txt
python weather_logger.py
```

## File Structure

```
weather-station/
├── src/
│   └── weather_station.ino   →  ESP32 firmware
├── data/
│   └── readings.csv          →  auto updated by ESP32
├── .github/
│   └── workflows/
│       └── daily_forecast.yml →  GitHub Actions automation
├── weather_logger.py          →  local Python analysis
├── daily_forecast.py          →  GitHub Actions forecast script
├── requirements.txt           →  Python dependencies
├── .gitignore                 →  keeps secrets safe
└── README.md                  →  this file
```

## CSV Format

```
timestamp,bmp_temp_c,dht_temp_c,avg_temp_c,humidity_pct,pressure_hpa,is_raining,rain_analog,battery_v
2026-09-26 07:00:00,38.2,37.8,38.0,45.1,1008.3,0,2840,3.85
```

## Telegram Messages

**Daily 7am forecast:**
```
☀️ Good Morning — Ajmer Weather Station
━━━━━━━━━━━━━━━━━━━━
🌡 Temperature : 38.0°C
💧 Humidity    : 45.1%
🔵 Pressure    : 1008.3 hPa
🌧 Raining now : No
━━━━━━━━━━━━━━━━━━━━
📊 Pressure trend : +0.2 hPa/hr ➡️ Stable
🤖 Rain chance (1hr) : 12% ✅ Low
📈 Model accuracy : 84% (2016 readings)
━━━━━━━━━━━━━━━━━━━━
```

**Instant rain alert:**
```
⚠️ Rain Alert — Ajmer Weather Station
Pressure dropping: -3.1 hPa/hr
Humidity high: 78.2%
🌧 Rain expected within 30-60 minutes
```

## ML Model

Trains daily on all historical data using Random Forest.
Gets more accurate as more data is collected.

Features used: hour, day_of_year, month, pressure trend,
humidity trend, temperature trend

Target: will it rain in the next 1 hour?

## Troubleshooting

| Problem | Solution |
|---------|----------|
| BMP280 not found | Check SDA/SCL wires, try address 0x77 |
| DHT11 always failing | Check DATA wire on GPIO4 |
| WiFi not connecting | Check password in code |
| GitHub sync failing | Check token has `repo` scope |
| No serial output | Check baud rate is 115200 |
| Rain sensor always on | Adjust potentiometer on sensor |

## Contributing

Built this in your city? Open a PR!
The more stations across India the better.

## License

MIT — free to use, modify, and share.
