/*
 * ============================================================
 * TomatoGuard - ESP32 Sensor Node v2 (edge publishing + buffering)
 * ------------------------------------------------------------
 * Same sensors/logic/LCD as before, PLUS the edge features the
 * paper's Objective 2 needs:
 *   - PUBLISH_ALL flag:
 *        true  = publish EVERY reading   (cloud-only baseline)
 *        false = publish ON STATUS CHANGE + heartbeat every 5 min (edge mode)
 *   - offline RAM buffer: readings taken while MQTT is down are
 *           queued and re-sent on reconnect (no data lost in a short outage)
 *   - topic hierarchy: agri/<region>/<user>/<device>/status
 *   - prints a running published-message count to Serial for the benchmark
 *
 * HOW TO GET THE COMPARISON NUMBER (objective 2, >=30% fewer msgs):
 *   1) flash with PUBLISH_ALL = true,  run ~30 min, note msgs at the broker
 *   2) flash with PUBLISH_ALL = false, run the SAME ~30 min, note msgs
 *   reduction % = (baseline - edge) / baseline * 100
 * Report the measured number. Do not assume it.
 *
 * IMPORTANT: the offline buffer must be TESTED before you claim it:
 *   unplug the Pi/broker for ~1 min, keep the node running, restore it,
 *   and confirm the queued readings arrive. Only then write it into the paper.
 *
 * LIBRARIES: DHT sensor library, LiquidCrystal_I2C, PubSubClient, ArduinoJson
 * ============================================================
 */

#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ---------- EDIT THESE ----------
const char *WIFI_SSID = "WIoT_24G_1";
const char *WIFI_PASS = "tcictait";
const char *MQTT_HOST = "192.168.1.105"; // Raspberry Pi IP
const int MQTT_PORT = 1883;

// ---- identity -> topic hierarchy agri/<region>/<user>/<device>/status ----
const char *REGION = "th-pathumthani";
const char *USER_ID = "user_st126665";
const char *DEVICE_ID = "esp32_plot01";

// ---- MQTT auth (set after you enable passwords on the broker; see security/) ----
const char *MQTT_USER = "esp32_plot01";
const char *MQTT_PASS = "CHANGE_ME_DEVICE_PASSWORD";

// ---- PUBLISH MODE ----
const bool PUBLISH_ALL = false;            // true = baseline (every reading)
const unsigned long READ_INTERVAL = 5000;  // sample every 5 s
const unsigned long HEARTBEAT_MS = 300000; // 5 min heartbeat in edge mode

// ---------- Pins ----------
#define DHTPIN 15
#define DHTTYPE DHT22
#define SOIL_PIN 34
#define LDR_PIN 35
#define LED_PIN 2

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- thresholds (unchanged) ----------
const float TEMP_ALERT_LOW = 15.0, TEMP_ALERT_HIGH = 32.0;
const float HUM_ALERT_HIGH = 85.0;
const float SOIL_ALERT_LOW = 35.0, SOIL_ALERT_HIGH = 75.0;
const float LIGHT_ALERT_LOW = 5000.0;

// ---------- soil calibration ----------
const bool USE_CALIBRATION = false; // true on the real capacitive sensor
int SOIL_RAW_AIR = 3200, SOIL_RAW_WATER = 1300;

// ---------- timing / state ----------
const unsigned long LCD_INTERVAL = 2500, LED_INTERVAL = 500;
unsigned long lastRead = 0, lastLcd = 0, lastLed = 0, lastPublish = 0;
bool ledState = false, lcdScreenA = true;
char topicStatus[160];
String lastStatusPublished = "";
unsigned long msgCount = 0;

float gTemp = 0, gHum = 0, gSoil = 0, gLight = 0;
String gStatus = "STARTING", gRiskShort = "", gRiskFull = "None";

// ---------- offline RAM buffer ----------
#define BUF_SIZE 60
String ringbuf[BUF_SIZE];
int bufHead = 0, bufCount = 0;
void enqueue(const String &m)
{
    ringbuf[bufHead] = m;
    bufHead = (bufHead + 1) % BUF_SIZE;
    if (bufCount < BUF_SIZE)
        bufCount++;
}
void flushBuffer()
{
    while (bufCount > 0 && mqtt.connected())
    {
        int idx = (bufHead - bufCount + BUF_SIZE) % BUF_SIZE;
        if (mqtt.publish(topicStatus, ringbuf[idx].c_str()))
        {
            bufCount--;
            msgCount++;
        }
        else
            break;
    }
}

float readSoilMoisture()
{
    int raw = analogRead(SOIL_PIN);
    if (USE_CALIBRATION)
    {
        float p = 100.0 * (SOIL_RAW_AIR - raw) / (float)(SOIL_RAW_AIR - SOIL_RAW_WATER);
        return p < 0 ? 0 : (p > 100 ? 100 : p);
    }
    return (raw / 4095.0) * 100.0;
}
float readLightIntensity() { return (analogRead(LDR_PIN) / 4095.0) * 90000.0; }

String evaluateStatus(float t, float h, float s, float l)
{
    bool c = false, r = false;
    if (t < 10 || t > 35)
        c = true;
    else if (t < TEMP_ALERT_LOW || t > TEMP_ALERT_HIGH)
        r = true;
    if (h > 90)
        c = true;
    else if (h > HUM_ALERT_HIGH)
        r = true;
    if (s < 30 || s > 85)
        c = true;
    else if (s < SOIL_ALERT_LOW || s > SOIL_ALERT_HIGH)
        r = true;
    if (l < 2000)
        c = true;
    else if (l < LIGHT_ALERT_LOW)
        r = true;
    if (c)
        return "CRITICAL";
    if (r)
        return "AT_RISK";
    return "NORMAL";
}
void identifyDiseaseRisk(float t, float h, float s, float l, String &sn, String &fn)
{
    if (h > HUM_ALERT_HIGH && t >= 10 && t <= 20)
    {
        sn = "Late Blight";
        fn = "Late Blight (Phytophthora infestans)";
    }
    else if (h > HUM_ALERT_HIGH)
    {
        sn = "Leaf Mold";
        fn = "Leaf Mold (Cladosporium fulvum)";
    }
    else if (t > TEMP_ALERT_HIGH && h < 50)
    {
        sn = "Spider Mites";
        fn = "Spider Mites (Tetranychus urticae)";
    }
    else if (t >= 24 && t <= 30 && h > 70)
    {
        sn = "Bact. Spot";
        fn = "Bacterial Spot (Xanthomonas spp.)";
    }
    else if (s < SOIL_ALERT_LOW)
    {
        sn = "Water Stress";
        fn = "Water stress / root damage";
    }
    else if (s > SOIL_ALERT_HIGH)
    {
        sn = "Root Rot";
        fn = "Root Rot (waterlogged conditions)";
    }
    else if (l < LIGHT_ALERT_LOW)
    {
        sn = "Low Light";
        fn = "Growth stress (insufficient light)";
    }
    else
    {
        sn = "";
        fn = "None";
    }
}
void updateDisplay()
{
    lcd.clear();
    if (lcdScreenA)
    {
        lcd.setCursor(0, 0);
        lcd.print("T:");
        lcd.print(gTemp, 1);
        lcd.print(" H:");
        lcd.print(gHum, 0);
        lcd.print("%");
        lcd.setCursor(0, 1);
        lcd.print("S:");
        lcd.print(gSoil, 0);
        lcd.print("% L:");
        lcd.print(gLight, 0);
    }
    else
    {
        lcd.setCursor(0, 0);
        lcd.print(gStatus);
        lcd.setCursor(0, 1);
        lcd.print(gRiskShort.length() > 0 ? gRiskShort : String("No disease risk"));
    }
    lcdScreenA = !lcdScreenA;
}

void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
    {
        delay(400);
        Serial.print(".");
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi OK" : "\nWiFi down");
}
void connectMqtt()
{
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    int tries = 0;
    while (!mqtt.connected() && tries++ < 3)
    {
        // if you have NOT enabled broker passwords yet, use: mqtt.connect(DEVICE_ID)
        if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS))
        {
            Serial.println("MQTT connected");
            flushBuffer();
        }
        else
        {
            Serial.printf("MQTT fail rc=%d\n", mqtt.state());
            delay(2000);
        }
    }
}

void setup()
{
    Serial.begin(115200);
    dht.begin();
    pinMode(LED_PIN, OUTPUT);
    Wire.begin();
    lcd.init();
    lcd.backlight();
    snprintf(topicStatus, sizeof(topicStatus), "agri/%s/%s/%s/status", REGION, USER_ID, DEVICE_ID);
    Serial.printf("Topic: %s  mode: %s\n", topicStatus, PUBLISH_ALL ? "PUBLISH_ALL(baseline)" : "on-change+heartbeat(edge)");
    connectWiFi();
    connectMqtt();
}

void readAndEvaluateSensors()
{
    float t = dht.readTemperature(), h = dht.readHumidity();
    float s = readSoilMoisture(), l = readLightIntensity();
    if (isnan(t) || isnan(h))
    {
        Serial.println("{\"error\":\"DHT read failed\"}");
        return;
    }
    gTemp = t;
    gHum = h;
    gSoil = s;
    gLight = l;
    gStatus = evaluateStatus(t, h, s, l);
    identifyDiseaseRisk(t, h, s, l, gRiskShort, gRiskFull);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"device\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,"
             "\"soil_moisture\":%.1f,\"light\":%.0f,\"status\":\"%s\",\"disease_risk\":\"%s\"}",
             DEVICE_ID, t, h, s, l, gStatus.c_str(), gRiskFull.c_str());
    Serial.println(json);

    bool changed = (gStatus != lastStatusPublished);
    bool heartbeat = (millis() - lastPublish >= HEARTBEAT_MS);
    bool doPublish = PUBLISH_ALL || changed || heartbeat;

    if (mqtt.connected())
    {
        if (doPublish)
        {
            if (mqtt.publish(topicStatus, json))
            {
                msgCount++;
                lastPublish = millis();
                lastStatusPublished = gStatus;
                Serial.printf("[pub #%lu] %s\n", msgCount, PUBLISH_ALL ? "(all)" : (changed ? "(change)" : "(heartbeat)"));
            }
        }
    }
    else
    {
        enqueue(String(json)); // offline: keep it for resend
        Serial.println("[buffered - MQTT down]");
    }
}

void loop()
{
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED)
        connectWiFi();
    if (!mqtt.connected())
        connectMqtt();
    mqtt.loop();
    if (now - lastRead >= READ_INTERVAL)
    {
        lastRead = now;
        readAndEvaluateSensors();
    }
    if (now - lastLcd >= LCD_INTERVAL)
    {
        lastLcd = now;
        updateDisplay();
    }
    if (now - lastLed >= LED_INTERVAL)
    {
        lastLed = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
}
