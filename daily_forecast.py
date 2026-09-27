"""
Daily Forecast Script — runs via GitHub Actions every morning
Fetches latest data, trains model, posts forecast to Telegram
"""

import os
import requests
import pandas as pd
import base64
import pickle
import warnings
warnings.filterwarnings('ignore')
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score

# ── ENV VARIABLES (set as GitHub Secrets) ───────────────────
GITHUB_TOKEN   = os.environ.get("WEATHER_GITHUB_TOKEN")
GITHUB_USER    = os.environ.get("WEATHER_GITHUB_USER")
GITHUB_REPO    = os.environ.get("WEATHER_GITHUB_REPO")
TELEGRAM_TOKEN = os.environ.get("TELEGRAM_TOKEN")
TELEGRAM_CHAT  = os.environ.get("TELEGRAM_CHAT_ID")
STATION_NAME   = os.environ.get("STATION_NAME", "Weather Station")

# ── FETCH DATA ───────────────────────────────────────────────
def fetch_data():
    url = f"https://api.github.com/repos/{GITHUB_USER}/{GITHUB_REPO}/contents/data/readings.csv"
    headers = {"Authorization": f"token {GITHUB_TOKEN}", "User-Agent": "WeatherBot"}
    r = requests.get(url, headers=headers)
    if r.status_code != 200:
        print(f"Failed to fetch data: {r.status_code}")
        return None
    content = base64.b64decode(r.json()["content"]).decode("utf-8")
    with open("readings.csv", "w") as f:
        f.write(content)
    df = pd.read_csv("readings.csv")
    df["timestamp"] = pd.to_datetime(df["timestamp"], errors="coerce")
    df = df.dropna(subset=["timestamp"]).sort_values("timestamp").reset_index(drop=True)
    print(f"Loaded {len(df)} readings")
    return df

# ── ENGINEER FEATURES ────────────────────────────────────────
def engineer_features(df):
    ml = df.copy()
    ml["hour"]        = ml["timestamp"].dt.hour
    ml["day_of_year"] = ml["timestamp"].dt.dayofyear
    ml["month"]       = ml["timestamp"].dt.month
    ml["pressure_1h"] = ml["pressure_hpa"].diff(12)
    ml["pressure_3h"] = ml["pressure_hpa"].diff(36)
    ml["humidity_1h"] = ml["humidity_pct"].diff(12)
    ml["temp_1h"]     = ml["avg_temp_c"].diff(12)
    ml["rain_next_1h"]= ml["is_raining"].shift(-12).astype(float)
    return ml.dropna()

FEATURES = ["hour", "day_of_year", "month",
            "pressure_hpa", "pressure_1h", "pressure_3h",
            "humidity_pct", "humidity_1h",
            "avg_temp_c", "temp_1h"]

# ── TRAIN MODEL ──────────────────────────────────────────────
def train(df):
    ml = engineer_features(df)
    if len(ml) < 30:
        print("Not enough data for training")
        return None, 0
    X, y = ml[FEATURES], ml["rain_next_1h"]
    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)
    model = RandomForestClassifier(n_estimators=100, random_state=42)
    model.fit(X_train, y_train)
    acc = accuracy_score(y_test, model.predict(X_test))
    print(f"Model accuracy: {acc:.1%}")
    with open("rain_model.pkl", "wb") as f:
        pickle.dump(model, f)
    return model, acc

# ── PREDICT ──────────────────────────────────────────────────
def predict(df, model):
    if model is None:
        return None
    ml = engineer_features(df)
    if ml.empty:
        return None
    latest = ml[FEATURES].iloc[[-1]]
    return model.predict_proba(latest)[0][1]

# ── SEND TELEGRAM ────────────────────────────────────────────
def send_telegram(message):
    url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage"
    r = requests.post(url, json={"chat_id": TELEGRAM_CHAT, "text": message})
    print(f"Telegram: {r.status_code}")

# ── MAIN ─────────────────────────────────────────────────────
def main():
    print("Running daily forecast...")

    df = fetch_data()
    if df is None or df.empty:
        send_telegram(f"⚠️ {STATION_NAME}\nNo data available today.")
        return

    # Latest readings
    latest = df.iloc[-1]
    temp     = latest["avg_temp_c"]
    humidity = latest["humidity_pct"]
    pressure = latest["pressure_hpa"]
    raining  = bool(latest["is_raining"])

    # Pressure trend
    trend = 0
    if len(df) >= 12:
        trend = df["pressure_hpa"].iloc[-1] - df["pressure_hpa"].iloc[-12]

    # Train model
    model, accuracy = train(df)

    # Predict
    rain_prob = predict(df, model)

    # Weather emoji
    if raining:
        weather_emoji = "🌧"
    elif rain_prob and rain_prob > 0.6:
        weather_emoji = "⛅"
    elif temp > 40:
        weather_emoji = "🔥"
    else:
        weather_emoji = "☀️"

    # Trend emoji
    if trend < -2:
        trend_emoji = "📉 Rain possible"
    elif trend > 2:
        trend_emoji = "📈 Clearing up"
    else:
        trend_emoji = "➡️ Stable"

    # Build message
    msg = f"""{weather_emoji} Good Morning — {STATION_NAME}
━━━━━━━━━━━━━━━━━━━━
🌡 Temperature : {temp:.1f}°C
💧 Humidity    : {humidity:.1f}%
🔵 Pressure    : {pressure:.1f} hPa
🌧 Raining now : {'Yes' if raining else 'No'}
━━━━━━━━━━━━━━━━━━━━
📊 Pressure trend : {trend:+.1f} hPa/hr {trend_emoji}"""

    if rain_prob is not None:
        msg += f"\n🤖 Rain chance (1hr) : {rain_prob:.0%}"
        if rain_prob > 0.7:
            msg += " ⚠️ High!"
        elif rain_prob > 0.4:
            msg += " 🌤 Moderate"
        else:
            msg += " ✅ Low"

    if accuracy > 0:
        msg += f"\n📈 Model accuracy : {accuracy:.0%} ({len(df)} readings)"

    msg += f"\n━━━━━━━━━━━━━━━━━━━━"

    print(msg)
    send_telegram(msg)
    print("Done!")

if __name__ == "__main__":
    main()
