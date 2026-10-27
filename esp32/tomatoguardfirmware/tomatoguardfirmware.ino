/*
 * ============================================================
 * TomatoGuard - ESP32 Sensor Node v2 (anonymous MQTT - no auth)
 * ------------------------------------------------------------
 * Edge features: PUBLISH_ALL flag, on-change + heartbeat publishing,
 * offline RAM buffer, topic hierarchy agri/<region>/<user>/<device>/status.
 *
 * NOTE: broker is anonymous for now, so there is NO MQTT username/password.
 * When you enable broker auth later (for the paper's security section),
 * add MQTT_USER/MQTT_PASS back and use mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS).
 *
 * STILL TO SET before this connects:
 *   - WIFI_SSID / WIFI_PASS : a network the ESP32 can actually join
 *   - MQTT_HOST             : the Pi's current IP on that same network
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
const char *WIFI_SSID = "WTIoT_24G_1";
const char *WIFI_PASS = "tcictait";
const char *MQTT_HOST = "192.168.1.104"; // Raspberry Pi IP
const int MQTT_PORT = 1883;

// ---- identity -> topic hierarchy agri/<region>/<user>/<device>/status ----
const char *REGION = "th-pathumthani";
const char *USER_ID = "user_st126665";
const char *DEVICE_ID = "esp32_plot01";

// ---- MQTT: broker is anonymous for now, no username/password needed ----

// ---- PUBLISH MODE ----
const bool PUBLISH_ALL = false;            // true = baseline (every reading)
const unsigned long READ_INTERVAL = 5000;  // sample every 5 s
const unsigned long HEARTBEAT_MS = 300000; // 5 min heartbeat in edge mode
const unsigned long WIFI_CONNECT_TIMEOUT = 20000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

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
const bool USE_CALIBRATION = true; // true on the real capacitive sensor
int SOIL_RAW_AIR = 3616, SOIL_RAW_WATER = 254;

// ---------- timing / state ----------
const unsigned long LCD_INTERVAL = 2500, LED_INTERVAL = 500;
unsigned long lastRead = 0, lastLcd = 0, lastLed = 0, lastPublish = 0;
bool ledState = false, lcdScreenA = true;
char topicStatus[160];
String lastStatusPublished = "";
unsigned long msgCount = 0;
bool wifiAttemptActive = false;
bool wifiWasConnected = false;
unsigned long wifiAttemptStarted = 0;
unsigned long lastMqttAttempt = 0;

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

void startWiFiConnection()
{
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiAttemptStarted = millis();
    wifiAttemptActive = true;
    Serial.println("Connecting to WiFi...");
}

bool maintainWiFi()
{
    unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED)
    {
        wifiAttemptActive = false;
        if (!wifiWasConnected)
        {
            Serial.println("WiFi OK");
            Serial.print("WiFi IP: ");
            Serial.println(WiFi.localIP());
            Serial.print("WiFi RSSI: ");
            Serial.println(WiFi.RSSI());
        }
        wifiWasConnected = true;
        return true;
    }

    if (wifiWasConnected)
    {
        Serial.printf("WiFi lost; status=%d\n", WiFi.status());
        mqtt.disconnect();
        wifiWasConnected = false;
    }

    if (wifiAttemptActive)
    {
        if (now - wifiAttemptStarted < WIFI_CONNECT_TIMEOUT)
            return false;

        Serial.printf("WiFi connection timed out; status=%d\n", WiFi.status());
        WiFi.disconnect(false, false);
        wifiAttemptActive = false;
        wifiAttemptStarted = now;
        return false;
    }

    if (now - wifiAttemptStarted >= WIFI_RETRY_INTERVAL)
        startWiFiConnection();

    return false;
}

void connectMqtt()
{
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED || mqtt.connected())
        return;
    if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL)
        return;

    lastMqttAttempt = now;
    // Anonymous broker: connect with just the client id, no credentials.
    if (mqtt.connect(DEVICE_ID))
    {
        Serial.println("MQTT connected");
        flushBuffer();
    }
    else
    {
        Serial.printf("MQTT fail rc=%d\n", mqtt.state());
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
    WiFi.mode(WIFI_STA);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    startWiFiConnection();
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
    bool wifiConnected = maintainWiFi();
    if (wifiConnected && !mqtt.connected())
        connectMqtt();
    if (wifiConnected && mqtt.connected())
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
