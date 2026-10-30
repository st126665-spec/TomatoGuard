"""
============================================================
TomatoGuard - server (SENSOR LAYER ONLY, no CNN)
============================================================
Runs on the cloud container (Proxmox) AND/OR the Raspberry Pi.
Three jobs:
  1) subscribes to the local Mosquitto MQTT topic (if a broker is present)
  2) ALSO accepts readings over HTTP POST /ingest  (edge->gateway->cloud)
  3) writes every reading to InfluxDB + serves the live dashboard & JSON API

The /ingest endpoint lets a gateway (the Pi) forward readings up to this
server over plain HTTPS, which is how data reaches the Proxmox container
(it does not accept inbound MQTT). Protect it with INGEST_KEY.

Run:  python3 app.py        then open  http://<host-ip>:5001
"""
from flask import Flask, jsonify, render_template, request
from collections import deque
import threading, json, time, os, datetime as dt
import paho.mqtt.client as mqtt

# ----------------------------- CONFIG -------------------------------------
MQTT_HOST   = os.getenv("MQTT_HOST", "localhost")   # local Mosquitto
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC  = os.getenv("MQTT_TOPIC", "agri/+/+/+/status")

# InfluxDB 2.x (leave INFLUX_TOKEN empty to disable Influx writes for now)
INFLUX_URL    = os.getenv("INFLUX_URL", "http://localhost:8086")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN", "")          # paste your token to enable
INFLUX_ORG    = os.getenv("INFLUX_ORG", "tomatoguard")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "sensors")

# Shared secret for the HTTP POST /ingest endpoint (empty = no check).
# Set the SAME value here (on the cloud server) and in the Pi forwarder.
INGEST_KEY = os.getenv("INGEST_KEY", "")

BUFFER_SIZE = 720     # ~2 h at 10 s/sample kept in memory for the charts

app = Flask(__name__)
buffer = deque(maxlen=BUFFER_SIZE)     # each item: dict reading + 'timestamp'
latest = {}                            # most recent reading
lock = threading.Lock()

# ----------------------- InfluxDB (optional) ------------------------------
write_api = None
if INFLUX_TOKEN:
    try:
        from influxdb_client import InfluxDBClient, Point, WritePrecision
        from influxdb_client.client.write_api import SYNCHRONOUS
        _ic = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        write_api = _ic.write_api(write_options=SYNCHRONOUS)
        print(f"[influx] connected -> {INFLUX_URL} bucket={INFLUX_BUCKET}")
    except Exception as e:
        print(f"[influx] disabled ({e}); running on in-memory buffer only")
else:
    print("[influx] no token set; running on in-memory buffer only")

def influx_write(r):
    if not write_api:
        return
    try:
        p = (Point("reading")
             .tag("device", r.get("device", "esp32_plot01"))
             .tag("status", r.get("status", ""))
             .tag("disease_risk", r.get("disease_risk", ""))
             .field("temperature_C",    float(r["temperature"]))
             .field("humidity_pct",     float(r["humidity"]))
             .field("soil_moisture_pct",float(r["soil_moisture"]))
             .field("light_lux",        float(r["light"])))
        write_api.write(bucket=INFLUX_BUCKET, record=p)
    except Exception as e:
        print(f"[influx] write failed: {e}")

# --------------------- shared: store one reading --------------------------
def store_reading(r, source="mqtt"):
    """Stamp, buffer, update latest, persist. Used by MQTT and HTTP /ingest."""
    r["timestamp"] = dt.datetime.now(dt.timezone(dt.timedelta(hours=7))).strftime("%Y-%m-%d %H:%M:%S")
    with lock:
        buffer.append(r)
        latest.clear(); latest.update(r)
    influx_write(r)
    print(f"[{source}] {r['timestamp']} {r.get('status')} {r.get('disease_risk')}")

# ------------------------------- MQTT -------------------------------------
def on_connect(c, u, flags, rc):
    print(f"[mqtt] connected rc={rc}; subscribing {MQTT_TOPIC}")
    c.subscribe(MQTT_TOPIC, qos=1)

def on_message(c, u, msg):
    try:
        r = json.loads(msg.payload.decode())
    except Exception as e:
        print(f"[mqtt] bad payload: {e}"); return
    store_reading(r, source="rx")

def mqtt_thread():
    c = mqtt.Client(client_id="tomatoguard_server")
    c.on_connect = on_connect
    c.on_message = on_message
    while True:
        try:
            c.connect(MQTT_HOST, MQTT_PORT, 60)
            c.loop_forever()
        except Exception as e:
            print(f"[mqtt] reconnect in 5s ({e})"); time.sleep(5)

# ------------------------------ ROUTES ------------------------------------
@app.route("/")
def index():
    return render_template("dashboard.html")

@app.route("/ingest", methods=["POST"])
def api_ingest():
    """Accept a single reading (JSON) forwarded by the Pi gateway over HTTPS."""
    if INGEST_KEY and request.headers.get("X-Ingest-Key") != INGEST_KEY:
        return jsonify({"error": "unauthorized"}), 401
    try:
        r = request.get_json(force=True)
        if not isinstance(r, dict):
            raise ValueError("body must be a JSON object")
    except Exception as e:
        return jsonify({"error": f"bad payload: {e}"}), 400
    store_reading(r, source="ingest")
    return jsonify({"ok": True, "timestamp": r["timestamp"]})

@app.route("/api/latest")
def api_latest():
    with lock:
        return jsonify(latest or {})

@app.route("/api/series")
def api_series():
    """Everything currently in the buffer, for the live charts."""
    with lock:
        rows = list(buffer)
    return jsonify({
        "labels":        [r["timestamp"][5:16] for r in rows],
        "temperature":   [r["temperature"]   for r in rows],
        "humidity":      [r["humidity"]       for r in rows],
        "soil_moisture": [r["soil_moisture"]  for r in rows],
        "light":         [r["light"]          for r in rows],
        # used by the dashboard's status-change log and CSV export
        "timestamps":    [r["timestamp"]                for r in rows],
        "status":        [r.get("status", "")           for r in rows],
        "disease_risk":  [r.get("disease_risk", "None") for r in rows],
    })

if __name__ == "__main__":
    threading.Thread(target=mqtt_thread, daemon=True).start()
    print("\nDashboard: http://0.0.0.0:5001  (open http://<host-ip>:5001)\n")
    app.run(host="0.0.0.0", port=5001)
