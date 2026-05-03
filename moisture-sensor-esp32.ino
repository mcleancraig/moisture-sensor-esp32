#include <WiFi.h>
#include <secrets.h>
#include <PubSubClient.h>

// ═══════════════════════════════════════════════════════════
//  CONFIG — only edit these for each sensor unit
// ═══════════════════════════════════════════════════════════

// ── Single number that configures this unit ───────────────
const int SENSOR_NUMBER = 2;  // ← change this only, everything derives from it

// WiFi
const char* WIFI_SSID       = S_WIFI_SSID;
const char* WIFI_PASSWORD   = S_WIFI_PASSWORD;

IPAddress GATEWAY  (192, 168,  1,   1);
IPAddress SUBNET   (255, 255, 0,   0);
IPAddress DNS      (192, 168,  1,   1);

// MQTT
const char* MQTT_BROKER     = "192.168.11.1";
const int   MQTT_PORT       = 1883;
const char* MQTT_USER       = S_MQTT_USER;
const char* MQTT_PASSWORD   = S_MQTT_PASSWORD;

// Moisture calibration — recalibrate per sensor
const int   DRY_MV          = 2800;
const int   WET_MV          = 1000;
                                       // HW-390 inverted: dry > wet

// Battery
const int   BATTERY_PIN     = 1;
const float DIVIDER_RATIO   = 2;
const float BAT_CAL_OFFSET  = 0.0;
const float BAT_MIN         = 2;
const float BAT_MAX         = 5;

// Pins
const int   MOISTURE_PIN    = 0;

// Deep sleep
const int   SLEEP_MINUTES   = 1;

// HA autodiscovery prefix
const char* HA_DISCOVERY_PREFIX = "homeassistant";

// ═══════════════════════════════════════════════════════════
//  DERIVED CONFIG — do not edit
// ═══════════════════════════════════════════════════════════

// These are populated in setup() from SENSOR_NUMBER
char SENSOR_ID[16];
char SENSOR_NAME[32];
IPAddress STATIC_IP;

// State + discovery topics
char STATE_TOPIC[64];
char DISC_MOISTURE[128];
char DISC_BAT_V[128];
char DISC_BAT_PCT[128];

// ═══════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ═══════════════════════════════════════════════════════════
//  FUNCTIONS
// ═══════════════════════════════════════════════════════════

void goToSleep() {
  Serial.printf("Sleep     — going to sleep for %d minutes\n", SLEEP_MINUTES);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ── Moisture ───────────────────────────────────────────────

int readMoisture() {
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);
  delay(500);

  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(MOISTURE_PIN);
    delay(10);
  }
  int avgMv = sum / 10;

  int percent = map(avgMv, DRY_MV, WET_MV, 0, 100);
  percent = constrain(percent, 0, 100);

  Serial.printf("Moisture  — raw: %dmV  →  %d%%\n", avgMv, percent);
  return avgMv;
}

// ── Battery ────────────────────────────────────────────────

float readBatteryVoltage(int &rawMv) {
  // No attenuation on A0 — internal pull-down handles the divider
  // Seeed reference uses plain analogReadMilliVolts with no attenuation set
  delay(10);

  long sum = 0;
  for (int i = 0; i < 16; i++) {   // 16 samples as per Seeed reference
    sum += analogReadMilliVolts(BATTERY_PIN);
    delay(5);
  }
  rawMv = sum / 16;

  float voltage = (rawMv / 1000.0 * DIVIDER_RATIO) + BAT_CAL_OFFSET;

  if (voltage < BAT_MIN || voltage > BAT_MAX) {
    Serial.printf("Battery   — suspicious reading %.2fV (raw: %dmV), discarding\n",
      voltage, rawMv);
    return -1.0;
  }

  Serial.printf("Battery   — raw: %dmV  →  %.2fV\n", rawMv, voltage);
  return voltage;
}


int batteryPercent(float voltage) {
  if (voltage >= 4.10) return 100;
  if (voltage >= 3.90) return 80;
  if (voltage >= 3.70) return 60;
  if (voltage >= 3.50) return 40;
  if (voltage >= 3.30) return 20;
  if (voltage >= 3.10) return  5;
  return 0;
}

// ── WiFi ───────────────────────────────────────────────────

void connectWifi() {
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS)) {
    Serial.println("WiFi      — static IP config failed");
  }

  Serial.printf("WiFi      — connecting to %s as %s",
    WIFI_SSID, STATIC_IP.toString().c_str());

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi      — connected, IP: %s\n",
      WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi      — failed, going to sleep");
    goToSleep();
  }
}

// ── MQTT ───────────────────────────────────────────────────

void connectMqtt() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(512);   // needed for discovery payloads
  String clientId = String("garden-") + SENSOR_ID;

  Serial.printf("MQTT      — connecting to %s as %s\n",
    MQTT_BROKER, clientId.c_str());

  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("MQTT      — connected");
    } else {
      Serial.printf("MQTT      — failed (rc=%d), retrying\n", mqtt.state());
      delay(500);
      attempts++;
    }
  }

  if (!mqtt.connected()) {
    Serial.println("MQTT      — could not connect, going to sleep");
    goToSleep();
  }
}

// ── HA Autodiscovery ───────────────────────────────────────

void publishDiscovery() {
  // Device block — shared across all entities, groups them in HA
  // under one device card
  char device[192];
  snprintf(device, sizeof(device),
    "\"device\":{"
      "\"identifiers\":[\"%s\"],"
      "\"name\":\"%s\","
      "\"model\":\"ESP32-C6 Soil Sensor\","
      "\"manufacturer\":\"DIY\""
    "}",
    SENSOR_ID, SENSOR_NAME);

  // ── Moisture % ──────────────────────────────────────────
  char payload[512];
  snprintf(payload, sizeof(payload),
    "{"
      "\"name\":\"Moisture\","
      "\"unique_id\":\"%s_moisture\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{ value_json.moisture }}\","
      "\"unit_of_measurement\":\"%%\","
      "\"device_class\":\"moisture\","
      "\"state_class\":\"measurement\","
      "\"icon\":\"mdi:water-percent\","
      "%s"
    "}",
    SENSOR_ID, STATE_TOPIC, device);

  mqtt.publish(DISC_MOISTURE, payload, true);
  Serial.printf("Discovery — moisture: %s\n", DISC_MOISTURE);
  mqtt.loop(); delay(50);

  // ── Battery voltage ─────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{"
      "\"name\":\"Battery Voltage\","
      "\"unique_id\":\"%s_battery_v\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{ value_json.battery_v }}\","
      "\"unit_of_measurement\":\"V\","
      "\"device_class\":\"voltage\","
      "\"state_class\":\"measurement\","
      "\"icon\":\"mdi:battery\","
      "%s"
    "}",
    SENSOR_ID, STATE_TOPIC, device);

  mqtt.publish(DISC_BAT_V, payload, true);
  Serial.printf("Discovery — battery_v: %s\n", DISC_BAT_V);
  mqtt.loop(); delay(50);

  // ── Battery percent ─────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{"
      "\"name\":\"Battery\","
      "\"unique_id\":\"%s_battery_pct\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{ value_json.battery_pct }}\","
      "\"unit_of_measurement\":\"%%\","
      "\"device_class\":\"battery\","
      "\"state_class\":\"measurement\","
      "\"icon\":\"mdi:battery-percent\","
      "%s"
    "}",
    SENSOR_ID, STATE_TOPIC, device);

  mqtt.publish(DISC_BAT_PCT, payload, true);
  Serial.printf("Discovery — battery_pct: %s\n", DISC_BAT_PCT);
  mqtt.loop(); delay(50);

  Serial.println("Discovery — complete");
}

// ═══════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n╔══════════════════════════╗");
  Serial.println(  "║   Garden Sensor Boot     ║");
  Serial.println(  "╚══════════════════════════╝");

// Derive all identity fields from SENSOR_NUMBER
  snprintf(SENSOR_ID,   sizeof(SENSOR_ID),   "sensor%d",                  SENSOR_NUMBER);
  snprintf(SENSOR_NAME, sizeof(SENSOR_NAME), "Garden Moisture Sensor %d", SENSOR_NUMBER);
  STATIC_IP = IPAddress(192, 168, 220, SENSOR_NUMBER);

  // Build topics from SENSOR_ID
  snprintf(STATE_TOPIC,    sizeof(STATE_TOPIC),
    "garden/%s/state", SENSOR_ID);
  snprintf(DISC_MOISTURE,  sizeof(DISC_MOISTURE),
    "%s/sensor/%s_moisture/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_V,     sizeof(DISC_BAT_V),
    "%s/sensor/%s_battery_v/config",  HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_PCT,   sizeof(DISC_BAT_PCT),
    "%s/sensor/%s_battery_pct/config",HA_DISCOVERY_PREFIX, SENSOR_ID);

  // Read sensors before WiFi — radio noise affects ADC
  int   moistureRawMv = readMoisture();
  int   moisturePct   = map(moistureRawMv, DRY_MV, WET_MV, 0, 100);
  moisturePct         = constrain(moisturePct, 0, 100);

  int   batRawMv  = 0;
  float batV      = readBatteryVoltage(batRawMv);
  int   batPct    = (batV > 0) ? batteryPercent(batV) : -1;

  connectWifi();
  connectMqtt();

  // Publish discovery payloads — retained so HA gets them even
  // if it restarts between sensor wake cycles
  publishDiscovery();

  // Publish state
  if (mqtt.connected()) {
    char payload[256];

    if (batV > 0) {
      snprintf(payload, sizeof(payload),
        "{"
          "\"sensor\":\"%s\","
          "\"moisture\":%d,"
          "\"moisture_raw_mv\":%d,"
          "\"dry_mv\":%d,"
          "\"wet_mv\":%d,"
          "\"battery_v\":%.2f,"
          "\"battery_pct\":%d,"
          "\"battery_raw_mv\":%d"
        "}",
        SENSOR_ID,
        moisturePct, moistureRawMv,
        DRY_MV, WET_MV,
        batV, batPct, batRawMv);
    } else {
      snprintf(payload, sizeof(payload),
        "{"
          "\"sensor\":\"%s\","
          "\"moisture\":%d,"
          "\"moisture_raw_mv\":%d,"
          "\"dry_mv\":%d,"
          "\"wet_mv\":%d,"
          "\"battery_v\":null,"
          "\"battery_pct\":null,"
          "\"battery_raw_mv\":%d"
        "}",
        SENSOR_ID,
        moisturePct, moistureRawMv,
        DRY_MV, WET_MV,
        batRawMv);
    }

    bool ok = mqtt.publish(STATE_TOPIC, payload, true);
    Serial.printf("State     — %s: %s\n", ok ? "published" : "FAILED", payload);
    mqtt.loop();
    delay(100);
  }

  goToSleep();
}

void loop() {
  // Intentionally empty — deep sleep reboots into setup()
}
