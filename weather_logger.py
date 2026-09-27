"""
Open Weather Station — Python Data Logger & ML Trainer
Downloads CSV from GitHub, analyzes data, trains forecast model
Run: python weather_logger.py
"""

import requests
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime
import time
import os
import base64
import json
import pickle
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score
import warnings
warnings.filterwarnings('ignore')

# ── SETTINGS ────────────────────────────────────────────────
GITHUB_TOKEN = "YOUR_GITHUB_PERSONAL_ACCESS_TOKEN"
GITHUB_USER  = "YOUR_GITHUB_USERNAME"
GITHUB_REPO  = "YOUR_REPO_NAME"
GITHUB_FILE  = "data/readings.csv"
LOCAL_FILE   = "ajmer_weather.csv"
MODEL_FILE   = "rain_model.pkl"
REFRESH_MIN  = 15

# ── FETCH FROM GITHUB ────────────────────────────────────────
def fetch_from_github():
    url = f"https://api.github.com/repos/{GITHUB_USER}/{GITHUB_REPO}/contents/{GITHUB_FILE}"
    headers = {"Authorization": f"token {GITHUB_TOKEN}", "User-Agent": "WeatherLogger"}
    r = requests.get(url, headers=headers)
    if r.status_code == 200:
        content = base64.b64decode(r.json()["content"]).decode("utf-8")
        with open(LOCAL_FILE, "w") as f:
            f.write(content)
        rows = len(content.splitlines()) - 1
        print(f"Downloaded {rows} readings from GitHub")
        return True
    elif r.status_code == 404:
        print("No data yet — waiting for ESP32 to sync")
        return False
    else:
        print(f"GitHub error: {r.status_code}")
        return False

# ── LOAD DATA ────────────────────────────────────────────────
def load_data():
    if not os.path.exists(LOCAL_FILE):
        return None
    df = pd.read_csv(LOCAL_FILE)
    df["timestamp"] = pd.to_datetime(df["timestamp"], errors="coerce")
    df = df.dropna(subset=["timestamp"])
    df = df.sort_values("timestamp").reset_index(drop=True)
    df["is_raining"] = df["is_raining"].astype(bool)
    return df

# ── SUMMARY ──────────────────────────────────────────────────
def print_summary(df):
    if df is None or df.empty:
        print("No data available yet")
        return

    latest = df.iloc[-1]
    print("\n" + "="*50)
    print("  WEATHER STATION — LIVE SUMMARY")
    print("="*50)
    print(f"  Timestamp    : {latest['timestamp']}")
    print(f"  BMP Temp     : {latest['bmp_temp_c']:.1f} °C")
    print(f"  DHT Temp     : {latest['dht_temp_c']:.1f} °C")
    print(f"  Avg Temp     : {latest['avg_temp_c']:.1f} °C")
    print(f"  Humidity     : {latest['humidity_pct']:.1f} %")
    print(f"  Pressure     : {latest['pressure_hpa']:.1f} hPa")
    print(f"  Raining      : {'YES 🌧' if latest['is_raining'] else 'NO ☀️'}")
    if latest.get("battery_v", 0) > 0.5:
        print(f"  Battery      : {latest['battery_v']:.2f} V")
    print(f"\n  Total readings : {len(df)}")
    print(f"  Since          : {df['timestamp'].min()}")
    print(f"  Temp range     : {df['avg_temp_c'].min():.1f} — {df['avg_temp_c'].max():.1f} °C")
    print(f"  Rain events    : {df['is_raining'].sum()}")

    # Pressure trend forecast
    if len(df) >= 12:
        recent = df.tail(12)
        trend = recent["pressure_hpa"].iloc[-1] - recent["pressure_hpa"].iloc[0]
        print(f"\n  Pressure trend (1hr) : {trend:+.1f} hPa", end="")
        if trend < -2:
            print("  ⚠️  Rain likely coming!")
        elif trend > 2:
            print("  ✅ Clearing up")
        else:
            print("  → Stable")
    print("="*50 + "\n")

# ── FEATURE ENGINEERING ──────────────────────────────────────
def engineer_features(df):
    ml = df.copy()
    ml["hour"]        = ml["timestamp"].dt.hour
    ml["day_of_year"] = ml["timestamp"].dt.dayofyear
    ml["month"]       = ml["timestamp"].dt.month
    ml["pressure_1h"] = ml["pressure_hpa"].diff(12)   # 12 x 5min = 1hr
    ml["pressure_3h"] = ml["pressure_hpa"].diff(36)
    ml["humidity_1h"] = ml["humidity_pct"].diff(12)
    ml["temp_1h"]     = ml["avg_temp_c"].diff(12)
    ml["rain_next_1h"]= ml["is_raining"].shift(-12).astype(float)
    ml = ml.dropna()
    return ml

# ── TRAIN MODEL ──────────────────────────────────────────────
def train_model(df):
    if df is None or len(df) < 50:
        print("Need at least 50 readings to train model")
        return None

    ml = engineer_features(df)
    if len(ml) < 30:
        print("Not enough engineered features yet")
        return None

    features = ["hour", "day_of_year", "month",
                "pressure_hpa", "pressure_1h", "pressure_3h",
                "humidity_pct", "humidity_1h",
                "avg_temp_c", "temp_1h"]

    X = ml[features]
    y = ml["rain_next_1h"]

    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)

    model = RandomForestClassifier(n_estimators=100, random_state=42)
    model.fit(X_train, y_train)

    accuracy = accuracy_score(y_test, model.predict(X_test))
    print(f"Model trained! Accuracy: {accuracy:.1%} on {len(ml)} samples")

    # Save model
    with open(MODEL_FILE, "wb") as f:
        pickle.dump(model, f)

    # Feature importance
    importance = pd.Series(model.feature_importances_, index=features)
    print("\nTop features for rain prediction:")
    for feat, imp in importance.sort_values(ascending=False).head(5).items():
        print(f"  {feat:<20} {imp:.1%}")

    return model

# ── PREDICT ──────────────────────────────────────────────────
def predict_rain(df, model):
    if model is None or df is None or len(df) < 36:
        return None

    ml = engineer_features(df)
    if ml.empty:
        return None

    features = ["hour", "day_of_year", "month",
                "pressure_hpa", "pressure_1h", "pressure_3h",
                "humidity_pct", "humidity_1h",
                "avg_temp_c", "temp_1h"]

    latest = ml[features].iloc[[-1]]
    prob = model.predict_proba(latest)[0][1]
    return prob

# ── PLOT ─────────────────────────────────────────────────────
def plot_data(df):
    if df is None or len(df) < 2:
        print("Not enough data to plot")
        return

    fig, axes = plt.subplots(5, 1, figsize=(14, 12), sharex=True)
    fig.suptitle("Open Weather Station — Ajmer", fontsize=14, fontweight="bold")

    axes[0].plot(df["timestamp"], df["avg_temp_c"], color="#E24B4A", linewidth=1.5, label="Avg")
    axes[0].plot(df["timestamp"], df["bmp_temp_c"], color="#FF9999", linewidth=0.8, linestyle="--", label="BMP280", alpha=0.7)
    axes[0].plot(df["timestamp"], df["dht_temp_c"], color="#FFB3B3", linewidth=0.8, linestyle="--", label="DHT11", alpha=0.7)
    axes[0].set_ylabel("Temperature (°C)")
    axes[0].legend(fontsize=8)
    axes[0].grid(True, alpha=0.3)
    axes[0].fill_between(df["timestamp"], df["avg_temp_c"], alpha=0.1, color="#E24B4A")

    axes[1].plot(df["timestamp"], df["humidity_pct"], color="#378ADD", linewidth=1.5)
    axes[1].set_ylabel("Humidity (%)")
    axes[1].grid(True, alpha=0.3)
    axes[1].fill_between(df["timestamp"], df["humidity_pct"], alpha=0.1, color="#378ADD")

    axes[2].plot(df["timestamp"], df["pressure_hpa"], color="#1D9E75", linewidth=1.5)
    axes[2].set_ylabel("Pressure (hPa)")
    axes[2].grid(True, alpha=0.3)
    axes[2].fill_between(df["timestamp"], df["pressure_hpa"], alpha=0.1, color="#1D9E75")

    axes[3].fill_between(df["timestamp"], df["is_raining"].astype(int),
                         color="#7F77DD", alpha=0.6, step="post")
    axes[3].set_ylabel("Raining")
    axes[3].set_yticks([0, 1])
    axes[3].set_yticklabels(["No", "Yes"])
    axes[3].grid(True, alpha=0.3)

    if "battery_v" in df.columns:
        batt = df[df["battery_v"] > 0.5]
        if len(batt) > 0:
            axes[4].plot(batt["timestamp"], batt["battery_v"], color="#BA7517", linewidth=1.5)
            axes[4].axhline(y=3.5, color="orange", linestyle="--", alpha=0.5, label="Low")
            axes[4].axhline(y=3.3, color="red", linestyle="--", alpha=0.5, label="Critical")
            axes[4].set_ylabel("Battery (V)")
            axes[4].legend(fontsize=8)
            axes[4].grid(True, alpha=0.3)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%d %b\n%H:%M"))
    plt.tight_layout()
    plt.savefig("weather_plot.png", dpi=150, bbox_inches="tight")
    print("Chart saved: weather_plot.png")
    plt.show()

# ── EXPORT ML DATASET ────────────────────────────────────────
def export_ml_dataset(df):
    if df is None or len(df) < 24:
        return
    ml = engineer_features(df)
    ml.to_csv("weather_ml_dataset.csv", index=False)
    print(f"ML dataset saved: weather_ml_dataset.csv ({len(ml)} rows)")

# ── MAIN ─────────────────────────────────────────────────────
if __name__ == "__main__":
    print("Open Weather Station Logger")
    print("Press Ctrl+C to stop\n")

    model = None

    # Load existing model if available
    if os.path.exists(MODEL_FILE):
        with open(MODEL_FILE, "rb") as f:
            model = pickle.load(f)
        print("Loaded existing model")

    while True:
        try:
            fetch_from_github()
            df = load_data()
            print_summary(df)

            # Train/retrain model
            model = train_model(df)

            # Predict rain
            if model is not None:
                prob = predict_rain(df, model)
                if prob is not None:
                    print(f"\n  Rain probability next 1hr: {prob:.1%}")
                    if prob > 0.7:
                        print("  ⚠️  High chance of rain!")
                    elif prob > 0.4:
                        print("  🌤  Moderate chance of rain")
                    else:
                        print("  ☀️  Low chance of rain")

            # Plot and export
            if df is not None and len(df) >= 2:
                plot_data(df)
                export_ml_dataset(df)

            print(f"\nNext refresh in {REFRESH_MIN} minutes...")
            time.sleep(REFRESH_MIN * 60)

        except KeyboardInterrupt:
            print("\nStopped.")
            break
        except Exception as e:
            print(f"Error: {e}")
            time.sleep(60)
