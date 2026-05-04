#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <time.h>
#include "esp_mac.h"

// ═══════════════════════════════════════════════════════════
//  v2.2.0
//  - Captive portal config on first boot
//  - Boot button hold to reconfigure without reflashing
//  - OTA firmware updates over WiFi
//  - Firmware version in MQTT payload
//  - NTP timestamp in MQTT payload (10s timeout)
//  - Gateway, netmask and DNS configurable via portal
//  - All settings stored in NVS
// ═══════════════════════════════════════════════════════════

#define FIRMWARE_VERSION "2.0.2"


// ── Pins ─────────────────────────────────────────────────
const int MOISTURE_PIN = 0;   // A0 — XIAO ESP32-C6
const int BATTERY_PIN  = 1;   // A1 — voltage divider midpoint
const int BTN_BOOT     = 9;   // Boot button on XIAO ESP32-C6

// ── Fixed calibration ─────────────────────────────────────
const int   DRY_MV         = 2800;
const int   WET_MV         = 1000;
const float DIVIDER_RATIO  = 2.0;
const float BAT_CAL_OFFSET = 0.0;
const float BAT_MIN        = 2.5;
const float BAT_MAX        = 4.3;

// ── Timing ────────────────────────────────────────────────
const int SLEEP_MINUTES      = 15;
const int AP_TIMEOUT_MIN     = 10;
const int AP_SLEEP_MIN       = 10;
const int BOOT_HOLD_MS       = 3000;
const int OTA_WAIT_MS        = 5000;
const int NTP_TIMEOUT_MS     = 10000;  // give up on NTP after 10 seconds

// ── AP credentials ────────────────────────────────────────
const char* AP_PASSWORD      = "moisture";

// ── NTP ───────────────────────────────────────────────────
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 0;   // UTC — HA handles timezone display
const int   DST_OFFSET_S = 0;

// ── HA discovery prefix ───────────────────────────────────
const char* HA_DISCOVERY_PREFIX = "homeassistant";

// ═══════════════════════════════════════════════════════════
//  RUNTIME CONFIG — loaded from NVS
// ═══════════════════════════════════════════════════════════

Preferences prefs;

struct Config {
  int     sensorNumber;
  char    wifiSSID[64];
  char    wifiPassword[64];
  bool    staticIP;
  uint8_t ip[4];
  uint8_t gw[4];
  uint8_t sn[4];
  uint8_t dns[4];
  char    mqttBroker[64];
  int     mqttPort;
  char    mqttUser[32];
  char    mqttPassword[64];
} cfg;

bool configLoaded = false;

void loadConfig() {
  prefs.begin("sensor", true);
  cfg.sensorNumber = prefs.getInt("sensorNum", 0);
  prefs.getString("wifiSSID",   cfg.wifiSSID,    sizeof(cfg.wifiSSID));
  prefs.getString("wifiPass",   cfg.wifiPassword,sizeof(cfg.wifiPassword));
  cfg.staticIP  = prefs.getBool("staticIP", false);
  // Static IP
  cfg.ip[0]  = prefs.getUChar("ip0",  192);
  cfg.ip[1]  = prefs.getUChar("ip1",  168);
  cfg.ip[2]  = prefs.getUChar("ip2",  220);
  cfg.ip[3]  = prefs.getUChar("ip3",    1);
  // Gateway
  cfg.gw[0]  = prefs.getUChar("gw0",  192);
  cfg.gw[1]  = prefs.getUChar("gw1",  168);
  cfg.gw[2]  = prefs.getUChar("gw2",    1);
  cfg.gw[3]  = prefs.getUChar("gw3",    1);
  // Subnet mask
  cfg.sn[0]  = prefs.getUChar("sn0",  255);
  cfg.sn[1]  = prefs.getUChar("sn1",  255);
  cfg.sn[2]  = prefs.getUChar("sn2",    0);
  cfg.sn[3]  = prefs.getUChar("sn3",    0);
  // DNS
  cfg.dns[0] = prefs.getUChar("dns0", 192);
  cfg.dns[1] = prefs.getUChar("dns1", 168);
  cfg.dns[2] = prefs.getUChar("dns2",   1);
  cfg.dns[3] = prefs.getUChar("dns3",   1);

  prefs.getString("mqttBroker", cfg.mqttBroker,  sizeof(cfg.mqttBroker));
  cfg.mqttPort  = prefs.getInt("mqttPort", 1883);
  prefs.getString("mqttUser",   cfg.mqttUser,    sizeof(cfg.mqttUser));
  prefs.getString("mqttPass",   cfg.mqttPassword,sizeof(cfg.mqttPassword));
  prefs.end();

  configLoaded = (cfg.sensorNumber > 0 && strlen(cfg.wifiSSID) > 0);
}

void clearConfig() {
  prefs.begin("sensor", false);
  prefs.clear();
  prefs.end();
  Serial.println("Config    — NVS cleared");
}

void saveConfig(int sensorNum,
                const char* ssid, const char* wifiPass,
                bool staticIp,
                uint8_t ip0,  uint8_t ip1,  uint8_t ip2,  uint8_t ip3,
                uint8_t gw0,  uint8_t gw1,  uint8_t gw2,  uint8_t gw3,
                uint8_t sn0,  uint8_t sn1,  uint8_t sn2,  uint8_t sn3,
                uint8_t dns0, uint8_t dns1, uint8_t dns2, uint8_t dns3,
                const char* broker, int port,
                const char* user, const char* mqttPass) {
  prefs.begin("sensor", false);
  prefs.putInt("sensorNum",  sensorNum);
  prefs.putString("wifiSSID",  ssid);
  prefs.putString("wifiPass",  wifiPass);
  prefs.putBool("staticIP",    staticIp);
  prefs.putUChar("ip0",  ip0);  prefs.putUChar("ip1",  ip1);
  prefs.putUChar("ip2",  ip2);  prefs.putUChar("ip3",  ip3);
  prefs.putUChar("gw0",  gw0);  prefs.putUChar("gw1",  gw1);
  prefs.putUChar("gw2",  gw2);  prefs.putUChar("gw3",  gw3);
  prefs.putUChar("sn0",  sn0);  prefs.putUChar("sn1",  sn1);
  prefs.putUChar("sn2",  sn2);  prefs.putUChar("sn3",  sn3);
  prefs.putUChar("dns0", dns0); prefs.putUChar("dns1", dns1);
  prefs.putUChar("dns2", dns2); prefs.putUChar("dns3", dns3);
  prefs.putString("mqttBroker", broker);
  prefs.putInt("mqttPort",     port);
  prefs.putString("mqttUser",  user);
  prefs.putString("mqttPass",  mqttPass);
  prefs.end();
  Serial.println("Config    — saved to NVS");
}

// ═══════════════════════════════════════════════════════════
//  DERIVED TOPICS
// ═══════════════════════════════════════════════════════════

char SENSOR_ID[16];
char SENSOR_NAME[32];
char STATE_TOPIC[64];
char DISC_MOISTURE[128];
char DISC_BAT_V[128];
char DISC_BAT_PCT[128];
char DISC_TS[128];


void buildDerivedConfig() {
  snprintf(SENSOR_ID,   sizeof(SENSOR_ID),   "sensor%d",                  cfg.sensorNumber);
  snprintf(SENSOR_NAME, sizeof(SENSOR_NAME), "Garden Moisture Sensor %d", cfg.sensorNumber);
  snprintf(STATE_TOPIC, sizeof(STATE_TOPIC), "garden/%s/state",           SENSOR_ID);
  snprintf(DISC_MOISTURE, sizeof(DISC_MOISTURE),
    "%s/sensor/%s_moisture/config",    HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_V, sizeof(DISC_BAT_V),
    "%s/sensor/%s_battery_v/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_PCT, sizeof(DISC_BAT_PCT),
    "%s/sensor/%s_battery_pct/config", HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_TS, sizeof(DISC_TS),
    "%s/sensor/%s_ts/config", HA_DISCOVERY_PREFIX, SENSOR_ID);
}

// ═══════════════════════════════════════════════════════════
//  CAPTIVE PORTAL HTML
// ═══════════════════════════════════════════════════════════

WebServer server(80);
DNSServer dnsServer;

const char CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Moisture Sensor Setup</title>
<style>
  body{font-family:sans-serif;max-width:460px;margin:40px auto;padding:0 16px;background:#f5f5f5}
  h1{font-size:1.3em;color:#2c7a4b;margin-bottom:4px}
  p.sub{color:#666;font-size:.85em;margin-top:0}
  label{display:block;margin-top:14px;font-size:.9em;color:#333;font-weight:600}
  input{width:100%;padding:8px;margin-top:4px;border:1px solid #ccc;
    border-radius:6px;font-size:1em;box-sizing:border-box}
  .optional{color:#888;font-weight:400;font-size:.8em}
  .section{background:#fff;border-radius:10px;padding:16px;margin:16px 0;
    box-shadow:0 1px 4px rgba(0,0,0,.08)}
  button{width:100%;padding:12px;background:#2c7a4b;color:#fff;border:none;
    border-radius:8px;font-size:1em;cursor:pointer;margin-top:20px}
  button:hover{background:#215c38}
  .hint{font-size:.78em;color:#888;margin-top:2px}
  .ip-wrap{display:flex;gap:4px;margin-top:4px;align-items:center}
  .ip-wrap input{width:58px;padding:8px;text-align:center;flex:none}
  .ip-wrap span{color:#555;font-weight:700}
  #network-rows{display:none;margin-top:4px}
  .chk-row{display:flex;align-items:center;gap:8px;margin-top:14px}
  .chk-row input{width:auto;margin:0}
  h3{font-size:.95em;color:#555;margin:16px 0 4px}
</style>
</head>
<body>
<h1>Moisture Sensor Setup</h1>
<p class="sub">Configure this sensor then click Save. It will restart and begin reporting.</p>

<form method="POST" action="/save">

  <div class="section">
    <label>Sensor number
      <input type="number" name="sensorNum" id="sensorNum" min="1" max="254"
        value="1" required oninput="syncNet()">
    </label>
    <p class="hint">Sets sensor ID, friendly name, and default last IP octet</p>
  </div>

  <div class="section">
    <label>WiFi SSID
      <input type="text" name="ssid" placeholder="Your network name" required>
    </label>
    <label>WiFi password <span class="optional">(optional)</span>
      <input type="password" name="wifiPass"
        placeholder="Leave blank for open networks">
    </label>
    <div class="chk-row">
      <input type="checkbox" name="staticIP" id="staticChk"
        onchange="toggleNet(this)">
      <label for="staticChk" style="margin:0;font-weight:600">Use static IP</label>
    </div>

    <div id="network-rows">
      <h3>IP address</h3>
      <div class="ip-wrap">
        <input type="number" name="ip1" id="ip1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip2" id="ip2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip3" id="ip3" value="220" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip4" id="ip4" min="1" max="254" value="1">
      </div>

      <h3>Gateway (router)</h3>
      <div class="ip-wrap">
        <input type="number" name="gw1" id="gw1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw2" id="gw2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw3" id="gw3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw4" id="gw4" value="1" min="0" max="255">
      </div>

      <h3>Subnet mask</h3>
      <div class="ip-wrap">
        <input type="number" name="sn1" id="sn1" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn2" id="sn2" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn3" id="sn3" value="0" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn4" id="sn4" value="0" min="0" max="255">
      </div>

      <h3>DNS server</h3>
      <div class="ip-wrap">
        <input type="number" name="dns1" id="dns1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns2" id="dns2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns3" id="dns3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns4" id="dns4" value="1" min="0" max="255">
      </div>
    </div>
  </div>

  <div class="section">
    <label>MQTT broker address
      <input type="text" name="mqttBroker" placeholder="192.168.1.100" required>
    </label>
    <label>MQTT port
      <input type="number" name="mqttPort" value="1883">
    </label>
    <label>MQTT username <span class="optional">(optional)</span>
      <input type="text" name="mqttUser"
        placeholder="Leave blank if not required">
    </label>
    <label>MQTT password <span class="optional">(optional)</span>
      <input type="password" name="mqttPass"
        placeholder="Leave blank if not required">
    </label>
  </div>

  <button type="submit">Save &amp; Restart</button>
</form>

<script>
function syncNet() {
  var n = document.getElementById('sensorNum').value;
  document.getElementById('ip4').value = n;
}
function toggleNet(cb) {
  document.getElementById('network-rows').style.display =
    cb.checked ? 'block' : 'none';
  if (cb.checked) syncNet();
}
</script>
</body>
</html>
)rawhtml";

const char SAVED_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>
  body{font-family:sans-serif;max-width:420px;margin:80px auto;padding:0 16px;
    text-align:center;background:#f5f5f5}
  h1{color:#2c7a4b}p{color:#555}
</style>
</head>
<body>
<h1>Saved!</h1>
<p>Configuration saved. The sensor will restart and connect to your network.</p>
<p><small>You can close this page.</small></p>
</body>
</html>
)rawhtml";

// ═══════════════════════════════════════════════════════════
//  PORTAL HANDLERS
// ═══════════════════════════════════════════════════════════

void handleRoot() {
  server.send_P(200, "text/html", CONFIG_HTML);
}

void handleSave() {
  int  sensorNum = server.arg("sensorNum").toInt();
  bool staticIp  = server.hasArg("staticIP");
  int  port      = server.arg("mqttPort").toInt();
  if (port == 0) port = 1883;

  // Static IP
  uint8_t ip0 = staticIp ? server.arg("ip1").toInt() : 192;
  uint8_t ip1 = staticIp ? server.arg("ip2").toInt() : 168;
  uint8_t ip2 = staticIp ? server.arg("ip3").toInt() : 220;
  uint8_t ip3 = staticIp ? server.arg("ip4").toInt() : (uint8_t)sensorNum;
  // Gateway
  uint8_t gw0 = staticIp ? server.arg("gw1").toInt() : 192;
  uint8_t gw1 = staticIp ? server.arg("gw2").toInt() : 168;
  uint8_t gw2 = staticIp ? server.arg("gw3").toInt() : 1;
  uint8_t gw3 = staticIp ? server.arg("gw4").toInt() : 1;
  // Subnet
  uint8_t sn0 = staticIp ? server.arg("sn1").toInt() : 255;
  uint8_t sn1 = staticIp ? server.arg("sn2").toInt() : 255;
  uint8_t sn2 = staticIp ? server.arg("sn3").toInt() : 0;
  uint8_t sn3 = staticIp ? server.arg("sn4").toInt() : 0;
  // DNS
  uint8_t dns0 = staticIp ? server.arg("dns1").toInt() : 192;
  uint8_t dns1 = staticIp ? server.arg("dns2").toInt() : 168;
  uint8_t dns2 = staticIp ? server.arg("dns3").toInt() : 1;
  uint8_t dns3 = staticIp ? server.arg("dns4").toInt() : 1;

  saveConfig(
    sensorNum,
    server.arg("ssid").c_str(),
    server.arg("wifiPass").c_str(),
    staticIp,
    ip0, ip1, ip2, ip3,
    gw0, gw1, gw2, gw3,
    sn0, sn1, sn2, sn3,
    dns0, dns1, dns2, dns3,
    server.arg("mqttBroker").c_str(),
    port,
    server.arg("mqttUser").c_str(),
    server.arg("mqttPass").c_str()
  );

  server.send_P(200, "text/html", SAVED_HTML);
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// ═══════════════════════════════════════════════════════════
//  START CONFIG PORTAL
// ═══════════════════════════════════════════════════════════

void startConfigPortal() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char apName[32];
  snprintf(apName, sizeof(apName), "MOISTURE_%02X%02X%02X%02X%02X%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.printf("Portal    — starting AP: %s\n", apName);
  Serial.printf("Portal    — password: %s\n", AP_PASSWORD);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, AP_PASSWORD);

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("Portal    — IP: %s\n", apIP.toString().c_str());
  Serial.printf("Portal    — will close in %d minutes\n", AP_TIMEOUT_MIN);

  dnsServer.start(53, "*", apIP);

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long deadline = millis() + (unsigned long)AP_TIMEOUT_MIN * 60 * 1000UL;

  while (millis() < deadline) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  Serial.printf("Portal    — timed out, sleeping %d minutes\n", AP_SLEEP_MIN);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)AP_SLEEP_MIN * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════
//  OTA
// ═══════════════════════════════════════════════════════════

void setupOTA() {
  ArduinoOTA.setHostname(SENSOR_ID);
  ArduinoOTA.setPassword(AP_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA       — starting update");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA       — complete, restarting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA       — %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA       — error [%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.printf("OTA       — ready, hostname: %s\n", SENSOR_ID);
}

// ═══════════════════════════════════════════════════════════
//  NTP + TIMESTAMP
// ═══════════════════════════════════════════════════════════

bool syncNTP() {
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  Serial.print("NTP       — syncing");
  struct tm t;
  unsigned long start = millis();
  while (!getLocalTime(&t)) {
    if (millis() - start >= NTP_TIMEOUT_MS) {
      Serial.println("\nNTP       — timed out after 10s, timestamp will be omitted");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nNTP       — synced: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
    t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

void getTimestamp(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t)) {
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
      t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    snprintf(buf, len, "unknown");
  }
}

// ═══════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════

void goToSleep() {
  Serial.printf("Sleep     — going to sleep for %d minutes\n", SLEEP_MINUTES);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

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

float readBatteryVoltage(int &rawMv) {
  delay(10);
  long sum = 0;
  for (int i = 0; i < 16; i++) {
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
  const float v[] = { 3.00, 3.10, 3.25, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20 };
  const int   p[] = {    0,    5,   10,   20,   35,   50,   65,   80,   90,   95,  100 };
  const int   n   = sizeof(v) / sizeof(v[0]);

  if (voltage <= v[0])   return 0;
  if (voltage >= v[n-1]) return 100;

  for (int i = 0; i < n - 1; i++) {
    if (voltage >= v[i] && voltage < v[i+1]) {
      float ratio = (voltage - v[i]) / (v[i+1] - v[i]);
      return (int)(p[i] + ratio * (p[i+1] - p[i]));
    }
  }
  return 0;
}

// ═══════════════════════════════════════════════════════════
//  WIFI + MQTT
// ═══════════════════════════════════════════════════════════

bool connectWifi() {
  WiFi.mode(WIFI_STA);

  if (cfg.staticIP) {
    IPAddress ip (cfg.ip[0],  cfg.ip[1],  cfg.ip[2],  cfg.ip[3]);
    IPAddress gw (cfg.gw[0],  cfg.gw[1],  cfg.gw[2],  cfg.gw[3]);
    IPAddress sn (cfg.sn[0],  cfg.sn[1],  cfg.sn[2],  cfg.sn[3]);
    IPAddress dns(cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3]);
    if (!WiFi.config(ip, gw, sn, dns)) {
      Serial.println("WiFi      — static IP config failed");
    }
  }

  Serial.printf("WiFi      — connecting to %s", cfg.wifiSSID);
  WiFi.begin(cfg.wifiSSID,
    strlen(cfg.wifiPassword) > 0 ? cfg.wifiPassword : nullptr);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi      — connected, IP: %s\n",
      WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("\nWiFi      — failed");
  return false;
}

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

void connectMqtt() {
  mqtt.setServer(cfg.mqttBroker, cfg.mqttPort);
  mqtt.setBufferSize(512);
  String clientId = String("garden-") + SENSOR_ID;
  Serial.printf("MQTT      — connecting to %s as %s\n",
    cfg.mqttBroker, clientId.c_str());

  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    bool ok = (strlen(cfg.mqttUser) > 0)
      ? mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPassword)
      : mqtt.connect(clientId.c_str());

    if (ok) {
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

// ═══════════════════════════════════════════════════════════
//  HA AUTODISCOVERY
// ═══════════════════════════════════════════════════════════

void publishDiscovery() {
  char device[256];
  snprintf(device, sizeof(device),
    "\"device\":{"
      "\"identifiers\":[\"%s\"],"
      "\"name\":\"%s\","
      "\"model\":\"ESP32-C6 Soil Sensor\","
      "\"manufacturer\":\"DIY\","
      "\"sw_version\":\"%s\""
    "}",
    SENSOR_ID, SENSOR_NAME, FIRMWARE_VERSION);

  char payload[512];

  snprintf(payload, sizeof(payload),
    "{\"name\":\"Moisture\",\"unique_id\":\"%s_moisture\","
    "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.moisture }}\","
    "\"unit_of_measurement\":\"%%\",\"device_class\":\"moisture\","
    "\"state_class\":\"measurement\",\"icon\":\"mdi:water-percent\",%s}",
    SENSOR_ID, STATE_TOPIC, device);
  mqtt.publish(DISC_MOISTURE, payload, true);
  mqtt.loop(); delay(50);

  snprintf(payload, sizeof(payload),
    "{\"name\":\"Battery Voltage\",\"unique_id\":\"%s_battery_v\","
    "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.battery_v }}\","
    "\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\","
    "\"state_class\":\"measurement\",\"icon\":\"mdi:battery\",%s}",
    SENSOR_ID, STATE_TOPIC, device);
  mqtt.publish(DISC_BAT_V, payload, true);
  mqtt.loop(); delay(50);

  snprintf(payload, sizeof(payload),
    "{\"name\":\"Battery\",\"unique_id\":\"%s_battery_pct\","
    "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.battery_pct }}\","
    "\"unit_of_measurement\":\"%%\",\"device_class\":\"battery\","
    "\"state_class\":\"measurement\",\"icon\":\"mdi:battery-percent\",%s}",
    SENSOR_ID, STATE_TOPIC, device);
  mqtt.publish(DISC_BAT_PCT, payload, true);
  mqtt.loop(); delay(50);

  snprintf(payload, sizeof(payload),
    "{\"name\":\"Last Seen\",\"unique_id\":\"%s_ts\","
    "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.ts }}\","
    "\"device_class\":\"timestamp\","
    "\"icon\":\"mdi:clock-outline\",%s}",
    SENSOR_ID, STATE_TOPIC, device);
  mqtt.publish(DISC_TS, payload, true);
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
  Serial.printf(   "║   Firmware v%-12s ║\n", FIRMWARE_VERSION);
  Serial.println(  "╚══════════════════════════╝");

  // ── Boot button check — hold for 3s to force reconfiguration ──
  pinMode(BTN_BOOT, INPUT_PULLUP);
  delay(100);
  if (digitalRead(BTN_BOOT) == LOW) {
    Serial.println("Config    — boot button held, waiting to confirm...");
    unsigned long holdStart = millis();
    while (digitalRead(BTN_BOOT) == LOW) {
      if (millis() - holdStart >= BOOT_HOLD_MS) {
        Serial.println("Config    — confirmed, clearing config");
        clearConfig();
        delay(200);
        startConfigPortal();
        return;
      }
      delay(50);
    }
    Serial.println("Config    — button released early, continuing normal boot");
  }

  // ── Load config ───────────────────────────────────────────
  loadConfig();

  if (!configLoaded) {
    Serial.println("Config    — none found, starting portal");
    startConfigPortal();
    return;
  }

  buildDerivedConfig();
  Serial.printf("Config    — sensor%d, SSID: %s, broker: %s\n",
    cfg.sensorNumber, cfg.wifiSSID, cfg.mqttBroker);

  // ── Read sensors before WiFi — radio noise affects ADC ───
  int   moistureRawMv = readMoisture();
  int   moisturePct   = map(moistureRawMv, DRY_MV, WET_MV, 0, 100);
  moisturePct         = constrain(moisturePct, 0, 100);

  int   batRawMv = 0;
  float batV     = readBatteryVoltage(batRawMv);
  int   batPct   = (batV > 0) ? batteryPercent(batV) : -1;

  // ── Connect WiFi — open portal if it fails ────────────────
  if (!connectWifi()) {
    Serial.println("Config    — WiFi failed, starting portal");
    startConfigPortal();
    return;
  }

  // ── NTP sync (10s timeout) ───────────────────────────────
  syncNTP();

  // ── OTA — stay awake briefly to accept waiting updates ───
  setupOTA();
  if (!MDNS.begin(SENSOR_ID)) {
    Serial.println("OTA       — mDNS failed to start");
  }
  Serial.printf("OTA       — listening for %dms\n", OTA_WAIT_MS);
  unsigned long otaDeadline = millis() + OTA_WAIT_MS;
  while (millis() < otaDeadline) {
    ArduinoOTA.handle();
    delay(10);
  }

  // ── MQTT ─────────────────────────────────────────────────
  connectMqtt();
  publishDiscovery();

  if (mqtt.connected()) {
    char timestamp[32];
    getTimestamp(timestamp, sizeof(timestamp));

    char payload[384];
    if (batV > 0) {
      snprintf(payload, sizeof(payload),
        "{\"sensor\":\"%s\",\"moisture\":%d,\"moisture_raw_mv\":%d,"
        "\"dry_mv\":%d,\"wet_mv\":%d,\"battery_v\":%.2f,"
        "\"battery_pct\":%d,\"battery_raw_mv\":%d,\"fw\":\"%s\",\"ts\":\"%s\"}",
        SENSOR_ID, moisturePct, moistureRawMv,
        DRY_MV, WET_MV, batV, batPct, batRawMv,
        FIRMWARE_VERSION, timestamp);
    } else {
      snprintf(payload, sizeof(payload),
        "{\"sensor\":\"%s\",\"moisture\":%d,\"moisture_raw_mv\":%d,"
        "\"dry_mv\":%d,\"wet_mv\":%d,\"battery_v\":null,"
        "\"battery_pct\":null,\"battery_raw_mv\":%d,\"fw\":\"%s\",\"ts\":\"%s\"}",
        SENSOR_ID, moisturePct, moistureRawMv,
        DRY_MV, WET_MV, batRawMv,
        FIRMWARE_VERSION, timestamp);
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
