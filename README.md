# TomatoGuard

Edge-to-cloud IoT monitoring of a tomato plot for early disease-risk warning.
AST01.41 IoT and Cloud capstone, Group 1, Asian Institute of Technology.

Team: Mark Reyvin D. Espiritu (st126665), Liza Shrestha (st126674)

Live dashboard: https://tomatoguard.iotcloud-lab.com/

## Architecture

```
ESP32 sensor node            Raspberry Pi gateway                Proxmox cloud container
(edge)                       (fog)                               (cloud)
DHT22, soil, LDR, LCD  --MQTT-->  Mosquitto :1883  --HTTPS POST-->  Flask app  /ingest
grades NORMAL / AT_RISK /    pi_cloud_forwarder.py               InfluxDB 2 (history)
CRITICAL on the node,                                            dashboard  :5001
publishes on change + 5 min                                      -> tomatoguard.iotcloud-lab.com
heartbeat, 60-reading RAM
buffer while offline
```

1. **Edge (ESP32).** Samples every 5 s, grades the reading against the
   thresholds and names the likely disease risk on the node itself. In edge
   mode it publishes only when the grade changes, plus a 5-minute heartbeat
   (`PUBLISH_ALL = false`). Setting `PUBLISH_ALL = true` gives the baseline
   (publish every reading) used for the message-count comparison. If Wi-Fi
   or the broker is down, up to 60 readings wait in RAM and are sent on
   reconnect.
2. **Gateway (Raspberry Pi).** Runs Mosquitto. `tools/pi_cloud_forwarder.py`
   subscribes to `agri/#` and POSTs each reading to the cloud over HTTPS,
   because the Proxmox container accepts HTTPS but not inbound MQTT.
3. **Cloud (Proxmox container).** `dashboard/app.py` accepts readings on
   `POST /ingest` (checked with the `X-Ingest-Key` header), stores them in
   InfluxDB and serves the dashboard.

MQTT topic: `agri/<region>/<user>/<device>/status`, for example
`agri/th-pathumthani/user_st126665/esp32_plot01/status`.

Message body:

```json
{"device":"esp32_plot01","temperature":29.9,"humidity":69.0,"soil_moisture":61.0,
 "light":64154,"status":"NORMAL","disease_risk":"None"}
```

## Repository layout

| Path | What it is |
|---|---|
| `esp32/tomatoguardfirmware/` | ESP32 firmware (Arduino) |
| `mosquitto/config/` | Mosquitto broker configuration |
| `dashboard/` | Flask server: MQTT subscriber, `/ingest`, InfluxDB writer, dashboard UI |
| `tools/pi_cloud_forwarder.py` | Pi to cloud forwarder |
| `docker-compose.yml` | Mosquitto + InfluxDB + dashboard stack |
| `.env.example` | Settings to copy into `.env` |

## Hardware

| Part | ESP32 pin |
|---|---|
| DHT22 temperature / humidity | GPIO 15 |
| Capacitive soil moisture sensor | GPIO 34 (ADC) |
| LDR light sensor | GPIO 35 (ADC) |
| 16x2 I2C LCD (0x27) | SDA 21, SCL 22 |
| Status LED | GPIO 2 |

## Running it

### 1. Cloud server (Proxmox container) and/or Pi

```bash
git clone https://github.com/st126665-spec/TomatoGuard.git
cd TomatoGuard
cp .env.example .env        # set INFLUX_PASSWORD, INFLUX_TOKEN and INGEST_KEY
docker compose up -d --build
docker compose logs -f dashboard
```

Open `http://<host-ip>:5001`.

### 2. Forwarder (on the Pi)

```bash
pip install paho-mqtt==1.6.1
export CLOUD_INGEST_URL=https://tomatoguard.iotcloud-lab.com/ingest
export INGEST_KEY=<same key as on the server>
python3 tools/pi_cloud_forwarder.py
```

Each forwarded reading logs `[fwd] 200 -> ...`.

### 3. ESP32

Open `esp32/tomatoguardfirmware/tomatoguardfirmware.ino` in the Arduino IDE,
install the libraries DHT sensor library, LiquidCrystal_I2C, PubSubClient and
ArduinoJson, set `WIFI_SSID`, `WIFI_PASS` and `MQTT_HOST` (the Pi's IP), and
flash. The serial monitor (115200 baud) prints each publish as
`[pub #n] (change)` or `(heartbeat)`.

### Test without the sensor

```bash
curl -X POST https://tomatoguard.iotcloud-lab.com/ingest \
  -H "Content-Type: application/json" -H "X-Ingest-Key: <key>" \
  -d '{"device":"test_node","temperature":24.5,"humidity":88,"soil_moisture":50,"light":25000,"status":"AT_RISK","disease_risk":"Late Blight"}'
```

## API

| Method | Path | Returns |
|---|---|---|
| `POST` | `/ingest` | stores one reading; `200`, `400` bad JSON, `401` wrong key |
| `GET` | `/api/latest` | latest reading |
| `GET` | `/api/series` | readings in memory, for the charts and CSV export |
| `GET` | `/` | dashboard |

## Known limitations

- MQTT between the ESP32 and the Pi is anonymous and unencrypted (QoS 0), on
  the local Wi-Fi only.
- The forwarder does not queue: a reading is lost if the cloud is unreachable
  when it arrives. The ESP32's own 60-reading buffer covers Wi-Fi/broker
  outages only.
- The light value is the LDR's ADC reading scaled to 0-90,000, not a
  calibrated lux measurement.
- Disease risk is rule-based from threshold combinations; there is no image
  model.
