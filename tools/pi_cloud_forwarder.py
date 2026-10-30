#!/usr/bin/env python3
"""
TomatoGuard - Pi -> Cloud forwarder
===================================
Edge (ESP32) -> Gateway (this Pi) -> Cloud (Proxmox container).

Subscribes to the Pi's local Mosquitto broker and HTTP-POSTs every reading
up to the cloud container's /ingest endpoint over the public HTTPS tunnel.
The Proxmox container does not accept inbound MQTT, so we deliver over HTTP.

Config via environment variables:
  LOCAL_MQTT_HOST   (default: localhost)
  LOCAL_MQTT_PORT   (default: 1883)
  LOCAL_MQTT_TOPIC  (default: agri/#)
  CLOUD_INGEST_URL  (required: https://<your-tunnel>.trycloudflare.com/ingest)
  INGEST_KEY        (shared secret; must match the server's INGEST_KEY)
"""
import os, time
import urllib.request
import paho.mqtt.client as mqtt

LOCAL_HOST  = os.getenv("LOCAL_MQTT_HOST", "localhost")
LOCAL_PORT  = int(os.getenv("LOCAL_MQTT_PORT", "1883"))
LOCAL_TOPIC = os.getenv("LOCAL_MQTT_TOPIC", "agri/#")

CLOUD_URL  = os.getenv("CLOUD_INGEST_URL", "https://REPLACE-ME.trycloudflare.com/ingest")
INGEST_KEY = os.getenv("INGEST_KEY", "")

def post_reading(payload_bytes):
    req = urllib.request.Request(
        CLOUD_URL, data=payload_bytes,
        headers={"Content-Type": "application/json", "X-Ingest-Key": INGEST_KEY},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"[fwd] {resp.status} -> {payload_bytes[:70]!r}")
    except Exception as e:
        print(f"[fwd] FAILED ({e})")

def on_connect(c, u, flags, rc):
    print(f"[local] connected rc={rc}; subscribing {LOCAL_TOPIC}; cloud={CLOUD_URL}")
    c.subscribe(LOCAL_TOPIC, qos=0)

def on_message(c, u, msg):
    post_reading(msg.payload)

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
