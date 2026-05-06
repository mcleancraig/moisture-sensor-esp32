#include <WiFi.h>
#include <WiFiUDP.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_mac.h"

// ═══════════════════════════════════════════════════════════
//  v2.3.0
//  - HA MQTT autodiscovery: Restart and Reset Config button entities
//    appear automatically in the device card in Home Assistant
//
//  v2.2.0
//  - saveConfig() takes Config struct (was 24 params)
//  - IP addresses stored as 4-byte NVS blocks (auto-migrates old per-octet keys)
//  - configLoaded now requires MQTT broker to be set
//  - Server-side validation in handleSave()
//  - handleNotFound() uses softAPIP() instead of hardcoded 192.168.4.1
//  - readMoisture() returns MoistureReading struct (was raw mV only)
//  - mqtt.disconnect() before deep sleep (at normal end of cycle)
//  - MQTT buffer size increased to 768
//  - FOTA blocked on development/test builds (version string contains '-')
//  - Syslog over UDP: buffers boot messages, flushes after WiFi connect
//  - Syslog server and port configurable in portal (default: logs:514)
//
//  v2.1.0
//  - Captive portal config on first boot
//  - Boot button hold to reconfigure without reflashing
//  - Reed switch hold to restart (external, no enclosure access needed)
//  - MQTT retained command: reset / restart (self-clearing)
//  - FOTA: automatic firmware update from GitHub Releases
//  - Firmware version in MQTT payload
//  - NTP timestamp in MQTT payload (10s timeout)
//  - Gateway, netmask and DNS configurable via portal
//  - All settings stored in NVS
// ═══════════════════════════════════════════════════════════

#define FIRMWARE_VERSION "2.3.0-dev"

// ── Pins ─────────────────────────────────────────────────
const int MOISTURE_PIN = 0;   // A0 — XIAO ESP32-C6
const int BATTERY_PIN  = 1;   // A1 — voltage divider midpoint
const int BTN_BOOT     = 9;   // Boot button on XIAO ESP32-C6
const int REED_PIN     = 3;   // Reed switch — GND when magnet present

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
const int REED_HOLD_MS       = 3000;
const int NTP_TIMEOUT_MS     = 10000;
const int CMD_LISTEN_MS      = 2000;

// ── AP credentials ────────────────────────────────────────
const char* AP_PASSWORD      = "moisture";

// ── NTP ───────────────────────────────────────────────────
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 0;
const int   DST_OFFSET_S = 0;

// ── FOTA ─────────────────────────────────────────────────
const char* FOTA_VERSION_URL =
  "https://github.com/mcleancraig/moisture-sensor-esp32"
  "/releases/latest/download/version.txt";
const char* FOTA_BIN_URL =
  "https://github.com/mcleancraig/moisture-sensor-esp32"
  "/releases/latest/download/moisture-sensor-esp32.ino.bin";

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
  char    syslogHost[64];
  int     syslogPort;
} cfg;

struct MoistureReading {
  int rawMv;
  int percent;
};

bool configLoaded = false;

void loadConfig() {
  prefs.begin("sensor", true);
  cfg.sensorNumber = prefs.getInt("sensorNum", 0);
  prefs.getString("wifiSSID",   cfg.wifiSSID,    sizeof(cfg.wifiSSID));
  prefs.getString("wifiPass",   cfg.wifiPassword,sizeof(cfg.wifiPassword));
  cfg.staticIP  = prefs.getBool("staticIP", false);

  // IP addresses — new format uses 4-byte blocks; fall back to old per-octet keys
  // if not present (sensors upgrading from v2.1.0 auto-migrate on next save)
  if (prefs.getBytes("ip",  cfg.ip,  4) == 0) {
    cfg.ip[0] = prefs.getUChar("ip0", 192); cfg.ip[1] = prefs.getUChar("ip1", 168);
    cfg.ip[2] = prefs.getUChar("ip2", 220); cfg.ip[3] = prefs.getUChar("ip3",   1);
  }
  if (prefs.getBytes("gw",  cfg.gw,  4) == 0) {
    cfg.gw[0] = prefs.getUChar("gw0", 192); cfg.gw[1] = prefs.getUChar("gw1", 168);
    cfg.gw[2] = prefs.getUChar("gw2",   1); cfg.gw[3] = prefs.getUChar("gw3",   1);
  }
  if (prefs.getBytes("sn",  cfg.sn,  4) == 0) {
    cfg.sn[0] = prefs.getUChar("sn0", 255); cfg.sn[1] = prefs.getUChar("sn1", 255);
    cfg.sn[2] = prefs.getUChar("sn2",   0); cfg.sn[3] = prefs.getUChar("sn3",   0);
  }
  if (prefs.getBytes("dns", cfg.dns, 4) == 0) {
    cfg.dns[0] = prefs.getUChar("dns0", 192); cfg.dns[1] = prefs.getUChar("dns1", 168);
    cfg.dns[2] = prefs.getUChar("dns2",   1); cfg.dns[3] = prefs.getUChar("dns3",   1);
  }

  prefs.getString("mqttBroker", cfg.mqttBroker,  sizeof(cfg.mqttBroker));
  cfg.mqttPort  = prefs.getInt("mqttPort", 1883);
  prefs.getString("mqttUser",   cfg.mqttUser,    sizeof(cfg.mqttUser));
  prefs.getString("mqttPass",   cfg.mqttPassword,sizeof(cfg.mqttPassword));

  prefs.getString("syslogHost", cfg.syslogHost, sizeof(cfg.syslogHost));
  cfg.syslogPort = prefs.getInt("syslogPort", 514);

  prefs.end();

  configLoaded = (cfg.sensorNumber > 0 && strlen(cfg.wifiSSID) > 0 && strlen(cfg.mqttBroker) > 0);
}

// Forward use of _logf() — Arduino IDE generates the prototype; macro must be
// defined before clearConfig()/saveConfig() so it resolves here, not to math.h.
#define logf(fmt, ...) _logf(__func__, fmt, ##__VA_ARGS__)

void clearConfig() {
  prefs.begin("sensor", false);
  prefs.clear();
  prefs.end();
  logf("Config    — NVS cleared\n");
}

void saveConfig(const Config& c) {
  prefs.begin("sensor", false);
  prefs.putInt("sensorNum",     c.sensorNumber);
  prefs.putString("wifiSSID",   c.wifiSSID);
  prefs.putString("wifiPass",   c.wifiPassword);
  prefs.putBool("staticIP",     c.staticIP);
  prefs.putBytes("ip",  c.ip,  4);
  prefs.putBytes("gw",  c.gw,  4);
  prefs.putBytes("sn",  c.sn,  4);
  prefs.putBytes("dns", c.dns, 4);
  prefs.putString("mqttBroker", c.mqttBroker);
  prefs.putInt("mqttPort",      c.mqttPort);
  prefs.putString("mqttUser",   c.mqttUser);
  prefs.putString("mqttPass",   c.mqttPassword);
  prefs.putString("syslogHost", c.syslogHost);
  prefs.putInt("syslogPort",    c.syslogPort);
  prefs.end();
  logf("Config    — saved to NVS\n");
}

// ═══════════════════════════════════════════════════════════
//  DERIVED TOPICS
// ═══════════════════════════════════════════════════════════

char SENSOR_ID[16];
char SENSOR_NAME[32];
char STATE_TOPIC[64];
char CMD_TOPIC[64];
char DISC_MOISTURE[128];
char DISC_BAT_V[128];
char DISC_BAT_PCT[128];
char DISC_TS[128];
char DISC_BTN_RESTART[128];
char DISC_BTN_RESET[128];

void buildDerivedConfig() {
  snprintf(SENSOR_ID,   sizeof(SENSOR_ID),   "sensor%d",                  cfg.sensorNumber);
  snprintf(SENSOR_NAME, sizeof(SENSOR_NAME), "Garden Moisture Sensor %d", cfg.sensorNumber);
  snprintf(STATE_TOPIC, sizeof(STATE_TOPIC), "garden/%s/state",           SENSOR_ID);
  snprintf(CMD_TOPIC,   sizeof(CMD_TOPIC),   "garden/%s/cmd",             SENSOR_ID);
  snprintf(DISC_MOISTURE, sizeof(DISC_MOISTURE),
    "%s/sensor/%s_moisture/config",        HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_V, sizeof(DISC_BAT_V),
    "%s/sensor/%s_battery_v/config",       HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_PCT, sizeof(DISC_BAT_PCT),
    "%s/sensor/%s_battery_pct/config",     HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_TS, sizeof(DISC_TS),
    "%s/sensor/%s_ts/config",              HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BTN_RESTART, sizeof(DISC_BTN_RESTART),
    "%s/button/%s_restart/config",         HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BTN_RESET, sizeof(DISC_BTN_RESET),
    "%s/button/%s_reset/config",           HA_DISCOVERY_PREFIX, SENSOR_ID);
}

// ═══════════════════════════════════════════════════════════
//  SYSLOG
//  Boot messages are buffered until WiFi connects, then flushed.
//  All subsequent messages are sent immediately.
//  logf() is used in place of Serial.printf/println throughout.
// ═══════════════════════════════════════════════════════════

#define SYSLOG_LINES 24
#define SYSLOG_LINE  120
#define SYSLOG_FUNC  32

struct SyslogEntry {
  char msg[SYSLOG_LINE];
  char func[SYSLOG_FUNC];
};

static SyslogEntry syslogBuf[SYSLOG_LINES];
static int  syslogHead  = 0;
static int  syslogTotal = 0;
static bool syslogReady = false;

WiFiUDP syslogUdp;

void syslogSend(const char* func, const char* msg) {
  char clean[SYSLOG_LINE];
  strlcpy(clean, msg, sizeof(clean));
  int len = strlen(clean);
  while (len > 0 && (clean[len-1] == '\n' || clean[len-1] == '\r')) clean[--len] = '\0';
  if (len == 0) return;

  // RFC 3164 timestamp: "Jan  1 12:34:56" — falls back to epoch if NTP not yet synced
  char timestamp[16];
  struct tm t;
  if (getLocalTime(&t)) {
    strftime(timestamp, sizeof(timestamp), "%b %e %T", &t);
  } else {
    strlcpy(timestamp, "Jan  1 00:00:00", sizeof(timestamp));
  }

  const char* hostname = (strlen(SENSOR_ID) > 0) ? SENSOR_ID : "sensor";
  char packet[220];
  // RFC 3164: <PRI>TIMESTAMP HOSTNAME APP[FUNC]: MSG
  // facility=local0(16), severity=info(6) → priority 134
  snprintf(packet, sizeof(packet), "<134>%s %s moisture-sensor-esp32[%s]: %s",
    timestamp, hostname, func, clean);

  syslogUdp.beginPacket(cfg.syslogHost, cfg.syslogPort);
  syslogUdp.print(packet);
  syslogUdp.endPacket();
}

void syslogFlush() {
  if (strlen(cfg.syslogHost) == 0) { syslogReady = true; return; }
  int count = min(syslogTotal, SYSLOG_LINES);
  int start = (syslogTotal >= SYSLOG_LINES) ? syslogHead : 0;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % SYSLOG_LINES;
    syslogSend(syslogBuf[idx].func, syslogBuf[idx].msg);
    delay(2);
  }
  syslogHead  = 0;
  syslogTotal = 0;
  syslogReady = true;
}

void _logf(const char* func, const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Serial.print(line);

  if (strlen(cfg.syslogHost) == 0) return;

  char sysline[SYSLOG_LINE];
  strlcpy(sysline, line, sizeof(sysline));

  if (syslogReady) {
    syslogSend(func, sysline);
  } else {
    strlcpy(syslogBuf[syslogHead].msg,  sysline, SYSLOG_LINE);
    strlcpy(syslogBuf[syslogHead].func, func,    SYSLOG_FUNC);
    syslogHead = (syslogHead + 1) % SYSLOG_LINES;
    syslogTotal++;
  }
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
  .pw-wrap{position:relative;margin-top:4px}
  .pw-wrap input{margin-top:0;padding-right:38px}
  .pw-toggle{position:absolute;right:6px;top:50%;transform:translateY(-50%);
    width:auto;padding:4px;background:none;border:none;margin:0;
    color:#888;cursor:pointer;display:flex;align-items:center;line-height:1}
  .pw-toggle:hover{background:none;color:#333}
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
      <div class="pw-wrap">
        <input type="password" name="wifiPass"
          placeholder="Leave blank for open networks">
        <button type="button" class="pw-toggle" onclick="togglePw(this)" aria-label="Show password"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg></button>
      </div>
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
        <input type="number" name="gw1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw4" value="1" min="0" max="255">
      </div>

      <h3>Subnet mask</h3>
      <div class="ip-wrap">
        <input type="number" name="sn1" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn2" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn3" value="0" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn4" value="0" min="0" max="255">
      </div>

      <h3>DNS server</h3>
      <div class="ip-wrap">
        <input type="number" name="dns1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns4" value="1" min="0" max="255">
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
      <div class="pw-wrap">
        <input type="password" name="mqttPass"
          placeholder="Leave blank if not required">
        <button type="button" class="pw-toggle" onclick="togglePw(this)" aria-label="Show password"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg></button>
      </div>
    </label>
  </div>

  <div class="section">
    <label>Syslog server <span class="optional">(optional)</span>
      <input type="text" name="syslogHost" id="syslogHost"
        placeholder="192.168.1.10 or logs.local">
    </label>
    <p class="hint">Must be an IP address or FQDN — short hostnames are not resolved. Leave blank to disable.</p>
    <label>Syslog port
      <input type="number" name="syslogPort" value="514">
    </label>
    <p class="hint">UDP syslog (RFC 3164). All log output is mirrored here.
      Compatible with rsyslog, syslog-ng, Graylog, and the Home Assistant
      Syslog add-on.</p>
  </div>

  <button type="submit">Save &amp; Restart</button>
</form>

<script>
var EYE     = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>';
var EYE_OFF = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';
function togglePw(btn) {
  var inp = btn.previousElementSibling;
  var show = inp.type === 'password';
  inp.type = show ? 'text' : 'password';
  btn.innerHTML = show ? EYE_OFF : EYE;
  btn.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
}
function syncNet() {
  var n = document.getElementById('sensorNum').value;
  if (document.getElementById('staticChk').checked) {
    document.getElementById('ip4').value = n;
  }
}
function toggleNet(cb) {
  document.getElementById('network-rows').style.display =
    cb.checked ? 'block' : 'none';
  if (cb.checked) syncNet();
}
document.querySelector('form').addEventListener('submit', function(e) {
  var h = document.getElementById('syslogHost').value.trim();
  if (h.length > 0 && h.indexOf('.') === -1) {
    e.preventDefault();
    alert('Syslog server must be an IP address or FQDN (must contain a dot), or leave blank to disable.');
  }
});
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

const char ERROR_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Invalid input</title>
<style>
  body{font-family:sans-serif;max-width:420px;margin:80px auto;padding:0 16px;
    text-align:center;background:#f5f5f5}
  h1{color:#c0392b}p{color:#555}a{color:#2c7a4b}
</style>
</head>
<body>
<h1>Invalid input</h1>
<p>Sensor number must be 1–254. WiFi SSID and MQTT broker address are required.</p>
<p><a href="/">Go back</a></p>
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
  int sensorNum = server.arg("sensorNum").toInt();

  String syslogHost = server.arg("syslogHost");
  bool syslogHostBad = syslogHost.length() > 0 && syslogHost.indexOf('.') == -1;

  if (sensorNum < 1 || sensorNum > 254 ||
      server.arg("ssid").length() == 0 ||
      server.arg("mqttBroker").length() == 0 ||
      syslogHostBad) {
    server.send_P(400, "text/html", ERROR_HTML);
    return;
  }

  Config c = {};
  c.sensorNumber = sensorNum;
  strlcpy(c.wifiSSID,    server.arg("ssid").c_str(),       sizeof(c.wifiSSID));
  strlcpy(c.wifiPassword,server.arg("wifiPass").c_str(),   sizeof(c.wifiPassword));
  c.staticIP = server.hasArg("staticIP");

  if (c.staticIP) {
    c.ip[0]  = server.arg("ip1").toInt();  c.ip[1]  = server.arg("ip2").toInt();
    c.ip[2]  = server.arg("ip3").toInt();  c.ip[3]  = server.arg("ip4").toInt();
    c.gw[0]  = server.arg("gw1").toInt();  c.gw[1]  = server.arg("gw2").toInt();
    c.gw[2]  = server.arg("gw3").toInt();  c.gw[3]  = server.arg("gw4").toInt();
    c.sn[0]  = server.arg("sn1").toInt();  c.sn[1]  = server.arg("sn2").toInt();
    c.sn[2]  = server.arg("sn3").toInt();  c.sn[3]  = server.arg("sn4").toInt();
    c.dns[0] = server.arg("dns1").toInt(); c.dns[1] = server.arg("dns2").toInt();
    c.dns[2] = server.arg("dns3").toInt(); c.dns[3] = server.arg("dns4").toInt();
  } else {
    c.ip[0]  = 192; c.ip[1]  = 168; c.ip[2]  = 220; c.ip[3]  = (uint8_t)sensorNum;
    c.gw[0]  = 192; c.gw[1]  = 168; c.gw[2]  =   1; c.gw[3]  =   1;
    c.sn[0]  = 255; c.sn[1]  = 255; c.sn[2]  =   0; c.sn[3]  =   0;
    c.dns[0] = 192; c.dns[1] = 168; c.dns[2] =   1; c.dns[3] =   1;
  }

  strlcpy(c.mqttBroker,  server.arg("mqttBroker").c_str(), sizeof(c.mqttBroker));
  c.mqttPort = server.arg("mqttPort").toInt();
  if (c.mqttPort == 0) c.mqttPort = 1883;
  strlcpy(c.mqttUser,    server.arg("mqttUser").c_str(),   sizeof(c.mqttUser));
  strlcpy(c.mqttPassword,server.arg("mqttPass").c_str(),   sizeof(c.mqttPassword));

  strlcpy(c.syslogHost, server.arg("syslogHost").c_str(), sizeof(c.syslogHost));
  c.syslogPort = server.arg("syslogPort").toInt();
  if (c.syslogPort == 0) c.syslogPort = 514;

  saveConfig(c);

  server.send_P(200, "text/html", SAVED_HTML);
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  String location = "http://" + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", location, true);
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

  logf("Portal    — starting AP: %s\n", apName);
  logf("Portal    — password: %s\n", AP_PASSWORD);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName, AP_PASSWORD);

  IPAddress apIP = WiFi.softAPIP();
  logf("Portal    — IP: %s\n", apIP.toString().c_str());
  logf("Portal    — will close in %d minutes\n", AP_TIMEOUT_MIN);

  dnsServer.start(53, "*", apIP);

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long deadline =
    millis() + (unsigned long)AP_TIMEOUT_MIN * 60 * 1000UL;

  while (millis() < deadline) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
  }

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  logf("Portal    — timed out, sleeping %d minutes\n", AP_SLEEP_MIN);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)AP_SLEEP_MIN * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════
//  FOTA
// ═══════════════════════════════════════════════════════════

void checkForUpdate() {
  if (strchr(FIRMWARE_VERSION, '-') != NULL) {
    logf("FOTA      — skipped: development build (%s)\n", FIRMWARE_VERSION);
    return;
  }

  logf("FOTA      — checking for update...\n");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, FOTA_VERSION_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();

  if (code != 200) {
    logf("FOTA      — version check failed (HTTP %d)\n", code);
    http.end();
    return;
  }

  String remoteVersion = http.getString();
  remoteVersion.trim();
  http.end();

  logf("FOTA      — local: %s  remote: %s\n",
    FIRMWARE_VERSION, remoteVersion.c_str());

  if (remoteVersion == FIRMWARE_VERSION) {
    logf("FOTA      — firmware is current, no update needed\n");
    return;
  }

  logf("FOTA      — update available: %s -> %s, downloading...\n",
    FIRMWARE_VERSION, remoteVersion.c_str());

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onStart([]() {
    logf("FOTA      — flashing...\n");
  });
  httpUpdate.onEnd([]() {
    logf("FOTA      — flash complete\n");
  });
  httpUpdate.onError([](int e) {
    logf("FOTA      — error: %d\n", e);
  });
  httpUpdate.onProgress([](int cur, int tot) {
    Serial.printf("FOTA      — %d%%\r", (cur * 100) / tot);  // serial only, too noisy for syslog
  });

  t_httpUpdate_return result = httpUpdate.update(client, FOTA_BIN_URL);

  switch (result) {
    case HTTP_UPDATE_FAILED:
      logf("FOTA      — failed: %s\n", httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      logf("FOTA      — no update\n");
      break;
    case HTTP_UPDATE_OK:
      break;   // restarts automatically
  }
}

// ═══════════════════════════════════════════════════════════
//  NTP + TIMESTAMP
// ═══════════════════════════════════════════════════════════

bool syncNTP() {
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  logf("NTP       — syncing\n");
  struct tm t;
  unsigned long start = millis();
  while (!getLocalTime(&t)) {
    if (millis() - start >= NTP_TIMEOUT_MS) {
      logf("NTP       — timed out after 10s, timestamp will be omitted\n");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  logf("NTP       — synced: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
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
//  REED SWITCH
// ═══════════════════════════════════════════════════════════

void checkReedSwitch() {
  pinMode(REED_PIN, INPUT_PULLUP);
  delay(50);

  if (digitalRead(REED_PIN) == LOW) {
    logf("Reed      — magnet detected, waiting to confirm...\n");
    unsigned long holdStart = millis();
    while (digitalRead(REED_PIN) == LOW) {
      if (millis() - holdStart >= REED_HOLD_MS) {
        logf("Reed      — confirmed, restarting\n");
        Serial.flush();
        delay(200);
        ESP.restart();
      }
      delay(50);
    }
    logf("Reed      — magnet removed early, ignoring\n");
  }
}

// ═══════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════

void goToSleep() {
  logf("Sleep     — going to sleep for %d minutes\n", SLEEP_MINUTES);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

MoistureReading readMoisture() {
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);
  delay(500);
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(MOISTURE_PIN);
    delay(10);
  }
  MoistureReading r;
  r.rawMv   = sum / 10;
  r.percent = constrain(map(r.rawMv, DRY_MV, WET_MV, 0, 100), 0, 100);
  logf("Moisture  — raw: %dmV  ->  %d%%\n", r.rawMv, r.percent);
  return r;
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
    logf("Battery   — suspicious reading %.2fV (raw: %dmV), discarding\n",
      voltage, rawMv);
    return -1.0;
  }
  logf("Battery   — raw: %dmV  ->  %.2fV\n", rawMv, voltage);
  return voltage;
}

int batteryPercent(float voltage) {
  const float v[] = { 3.00, 3.10, 3.25, 3.50, 3.60,
                      3.70, 3.80, 3.90, 4.00, 4.10, 4.20 };
  const int   p[] = {    0,    5,   10,   20,   35,
                        50,   65,   80,   90,   95,  100 };
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
//  WIFI
// ═══════════════════════════════════════════════════════════

bool connectWifi() {
  WiFi.mode(WIFI_STA);

  if (cfg.staticIP) {
    IPAddress ip (cfg.ip[0],  cfg.ip[1],  cfg.ip[2],  cfg.ip[3]);
    IPAddress gw (cfg.gw[0],  cfg.gw[1],  cfg.gw[2],  cfg.gw[3]);
    IPAddress sn (cfg.sn[0],  cfg.sn[1],  cfg.sn[2],  cfg.sn[3]);
    IPAddress dns(cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3]);
    if (!WiFi.config(ip, gw, sn, dns)) {
      logf("WiFi      — static IP config failed\n");
    }
  }

  logf("WiFi      — connecting to %s\n", cfg.wifiSSID);
  WiFi.begin(cfg.wifiSSID,
    strlen(cfg.wifiPassword) > 0 ? cfg.wifiPassword : nullptr);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    logf("WiFi      — connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  logf("WiFi      — failed\n");
  return false;
}

// ═══════════════════════════════════════════════════════════
//  MQTT + COMMAND HANDLING
// ═══════════════════════════════════════════════════════════

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

bool cmdReceived    = false;
char cmdPayload[32] = "";

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0) return;
  if (length >= sizeof(cmdPayload)) return;

  memcpy(cmdPayload, payload, length);
  cmdPayload[length] = '\0';
  cmdReceived = true;
  logf("Command   — received: %s\n", cmdPayload);

  // Clear retained message immediately
  mqtt.publish(CMD_TOPIC, "", true);
  mqtt.loop();
}

void connectMqtt() {
  mqtt.setServer(cfg.mqttBroker, cfg.mqttPort);
  mqtt.setBufferSize(768);
  mqtt.setCallback(mqttCallback);
  String clientId = String("garden-") + SENSOR_ID;
  logf("MQTT      — connecting to %s as %s\n",
    cfg.mqttBroker, clientId.c_str());

  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    bool ok = (strlen(cfg.mqttUser) > 0)
      ? mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPassword)
      : mqtt.connect(clientId.c_str());

    if (ok) {
      logf("MQTT      — connected\n");
      mqtt.subscribe(CMD_TOPIC);
      logf("MQTT      — subscribed to %s\n", CMD_TOPIC);
    } else {
      logf("MQTT      — failed (rc=%d), retrying\n", mqtt.state());
      delay(500);
      attempts++;
    }
  }

  if (!mqtt.connected()) {
    logf("MQTT      — could not connect, going to sleep\n");
    goToSleep();
  }
}

void processMqttCommand() {
  if (!cmdReceived) return;
  cmdReceived = false;

  if (strcmp(cmdPayload, "reset") == 0) {
    logf("Command   — resetting config and restarting into portal\n");
    Serial.flush();
    delay(200);
    clearConfig();
    ESP.restart();

  } else if (strcmp(cmdPayload, "restart") == 0) {
    logf("Command   — restarting\n");
    Serial.flush();
    delay(200);
    ESP.restart();

  } else {
    logf("Command   — unknown: %s\n", cmdPayload);
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

  // ── Button: Restart ──────────────────────────────────────
  // Publishes a retained "restart" to CMD_TOPIC when pressed in HA.
  // The sensor picks this up on its next wake, restarts, then clears it.
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Restart\",\"unique_id\":\"%s_restart\","
    "\"command_topic\":\"%s\",\"payload_press\":\"restart\","
    "\"retain\":true,\"device_class\":\"restart\","
    "\"icon\":\"mdi:restart\",%s}",
    SENSOR_ID, CMD_TOPIC, device);
  mqtt.publish(DISC_BTN_RESTART, payload, true);
  mqtt.loop(); delay(50);

  // ── Button: Reset Config ─────────────────────────────────
  // Publishes a retained "reset" to CMD_TOPIC when pressed in HA.
  // The sensor clears NVS and opens the captive portal on next wake.
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Reset Config\",\"unique_id\":\"%s_reset\","
    "\"command_topic\":\"%s\",\"payload_press\":\"reset\","
    "\"retain\":true,"
    "\"icon\":\"mdi:restore\",%s}",
    SENSOR_ID, CMD_TOPIC, device);
  mqtt.publish(DISC_BTN_RESET, payload, true);
  mqtt.loop(); delay(50);

  logf("Discovery — complete (sensors + buttons)\n");
}

// ═══════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  logf("\n╔══════════════════════════╗\n");
  logf(  "║   Garden Sensor Boot     ║\n");
  logf(  "║   Firmware v%-12s ║\n", FIRMWARE_VERSION);
  logf(  "╚══════════════════════════╝\n");

  // ── Load config first so syslog server address is available ──
  loadConfig();

  // ── Reed switch check ─────────────────────────────────────
  checkReedSwitch();

  // ── Boot button check — hold for 3s to force reconfiguration ──
  pinMode(BTN_BOOT, INPUT_PULLUP);
  delay(100);
  if (digitalRead(BTN_BOOT) == LOW) {
    logf("Config    — boot button held, waiting to confirm...\n");
    unsigned long holdStart = millis();
    while (digitalRead(BTN_BOOT) == LOW) {
      if (millis() - holdStart >= BOOT_HOLD_MS) {
        logf("Config    — confirmed, clearing config\n");
        clearConfig();
        delay(200);
        startConfigPortal();
        return;
      }
      delay(50);
    }
    logf("Config    — button released early, continuing normal boot\n");
  }

  if (!configLoaded) {
    logf("Config    — none found, starting portal\n");
    startConfigPortal();
    return;
  }

  buildDerivedConfig();
  logf("Config    — sensor%d, SSID: %s, broker: %s\n",
    cfg.sensorNumber, cfg.wifiSSID, cfg.mqttBroker);

  // ── Read sensors before WiFi — radio noise affects ADC ───
  MoistureReading moisture = readMoisture();

  int   batRawMv = 0;
  float batV     = readBatteryVoltage(batRawMv);
  int   batPct   = (batV > 0) ? batteryPercent(batV) : -1;

  // ── Connect WiFi — open portal if it fails ────────────────
  if (!connectWifi()) {
    logf("Config    — WiFi failed, starting portal\n");
    startConfigPortal();
    return;
  }
  // ── NTP sync (10s timeout) ───────────────────────────────
  syncNTP();

  // ── Flush buffered boot messages to syslog (after NTP for real timestamps) ──
  syslogUdp.begin(0);   // bind to any local port before first beginPacket()
  syslogFlush();

  // ── FOTA check — skipped on dev/test builds ───────────────
  checkForUpdate();

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
        SENSOR_ID, moisture.percent, moisture.rawMv,
        DRY_MV, WET_MV, batV, batPct, batRawMv,
        FIRMWARE_VERSION, timestamp);
    } else {
      snprintf(payload, sizeof(payload),
        "{\"sensor\":\"%s\",\"moisture\":%d,\"moisture_raw_mv\":%d,"
        "\"dry_mv\":%d,\"wet_mv\":%d,\"battery_v\":null,"
        "\"battery_pct\":null,\"battery_raw_mv\":%d,\"fw\":\"%s\",\"ts\":\"%s\"}",
        SENSOR_ID, moisture.percent, moisture.rawMv,
        DRY_MV, WET_MV, batRawMv,
        FIRMWARE_VERSION, timestamp);
    }
    bool ok = mqtt.publish(STATE_TOPIC, payload, true);
    logf("State     — %s: %s\n", ok ? "published" : "FAILED", payload);

    // ── Listen for incoming commands ──────────────────────
    logf("MQTT      — listening for commands...\n");
    unsigned long listenDeadline = millis() + CMD_LISTEN_MS;
    while (millis() < listenDeadline) {
      mqtt.loop();
      delay(10);
    }

    processMqttCommand();
  }

  mqtt.disconnect();
  goToSleep();
}

void loop() {
  // Intentionally empty — deep sleep reboots into setup()
}
