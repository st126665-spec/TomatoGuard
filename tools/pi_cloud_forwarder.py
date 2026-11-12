#!/usr/bin/env python3
"""
TomatoGuard - Pi -> Cloud forwarder (with optional CSV logging)
===============================================================
Edge (ESP32) -> Gateway (this Pi) -> Cloud (Proxmox container).

Subscribes to the Pi's local Mosquitto broker and HTTP-POSTs every reading
up to the cloud container's /ingest endpoint.

Config via environment variables:
  LOCAL_MQTT_HOST   (default: localhost)
  LOCAL_MQTT_PORT   (default: 1883)
  LOCAL_MQTT_TOPIC  (default: agri/#)
  CLOUD_INGEST_URL  (default: http://tomatoguard.iotcloud-lab.com/ingest)
  INGEST_KEY        (shared secret; must match the server's INGEST_KEY)
  FWD_LOG_CSV       (optional: path to a CSV; if set, every forwarded reading
                     is appended as one row — used for the message-rate benchmark)
"""
import os, time, json, csv, datetime as dt
import urllib.request
import paho.mqtt.client as mqtt

LOCAL_HOST  = os.getenv("LOCAL_MQTT_HOST", "localhost")
LOCAL_PORT  = int(os.getenv("LOCAL_MQTT_PORT", "1883"))
LOCAL_TOPIC = os.getenv("LOCAL_MQTT_TOPIC", "agri/#")

CLOUD_URL   = os.getenv("CLOUD_INGEST_URL", "http://tomatoguard.iotcloud-lab.com/ingest")
INGEST_KEY  = os.getenv("INGEST_KEY", "")
FWD_LOG_CSV = os.getenv("FWD_LOG_CSV", "")   # set to a filename to log each reading

CSV_FIELDS = ["received_at", "device", "temperature", "humidity",
              "soil_moisture", "light", "status", "disease_risk", "http_status"]

def log_csv(row):
    if not FWD_LOG_CSV:
        return
    new_file = not os.path.exists(FWD_LOG_CSV)
    with open(FWD_LOG_CSV, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if new_file:
            w.writeheader()
        w.writerow(row)

def post_reading(payload_bytes):
    req = urllib.request.Request(
        CLOUD_URL, data=payload_bytes,
        headers={"Content-Type": "application/json", "X-Ingest-Key": INGEST_KEY, "User-Agent": "Mozilla/5.0 (compatible; TomatoGuard-Forwarder/1.0)"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"[fwd] {resp.status} -> {payload_bytes[:70]!r}")
            return resp.status
    except Exception as e:
        print(f"[fwd] FAILED ({e})")
        return -1

def on_connect(c, u, flags, rc):
    print(f"[local] connected rc={rc}; subscribing {LOCAL_TOPIC}; "
          f"cloud={CLOUD_URL}; log={FWD_LOG_CSV or '(off)'}")
    c.subscribe(LOCAL_TOPIC, qos=0)

def on_message(c, u, msg):
    status = post_reading(msg.payload)
    if FWD_LOG_CSV:
        row = {k: "" for k in CSV_FIELDS}
        row["received_at"] = dt.datetime.now(
            dt.timezone(dt.timedelta(hours=7))).strftime("%Y-%m-%d %H:%M:%S")
        row["http_status"] = status
        try:
            d = json.loads(msg.payload.decode())
            for k in ("device", "temperature", "humidity",
                      "soil_moisture", "light", "status", "disease_risk"):
                row[k] = d.get(k, "")
        except Exception:
            pass
        log_csv(row)

def main():
    c = mqtt.Client(client_id="pi_cloud_forwarder")
    c.on_connect = on_connect
    c.on_message = on_message
    while True:
        try:
            c.connect(LOCAL_HOST, LOCAL_PORT, 60)
            c.loop_forever()
        except Exception as e:
            print(f"[local] reconnect in 5s ({e})"); time.sleep(5)

if __name__ == "__main__":
    main()
