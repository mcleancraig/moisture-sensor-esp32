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
#include "esp_sleep.h"

// ═══════════════════════════════════════════════════════════
//  v2.6.0
//  - Reed switch wakes device from deep sleep immediately on magnet contact
//    via esp_deep_sleep_enable_gpio_wakeup(). Previously the device only
//    checked the reed on the 15-minute timer tick, making the 3s/10s hold
//    timing impractical (user couldn't know when the check window started).
//    Now: magnet present → device wakes within ms → hold 3s to restart,
//    hold 10s to wipe config. Existing checkReedSwitch() timing unchanged.
//
//  v2.5.3
//  - NVS magic key: loadConfig() checks for "moisture-1" magic in the
//    "sensor" namespace; if a different value is found (stale NVS from
//    a foreign firmware), the namespace is cleared and the device falls
//    through to portal. Absent magic (pre-2.5.3 sensors) is left alone
//    for backwards compatibility — magic is written on next saveConfig().
//
//  v2.5.2
//  - MQTT password masked in publishConfigState() — publishes "***" instead
//    of the real credential to the retained config/state topic
//  - snprintf overflow: added publishMqttEntity() helper; both publishDiscovery()
//    and publishConfigDiscovery() now skip-and-log instead of publishing
//    silently-truncated JSON
//  - LOW_BATTERY_PCT promoted to #define constant; magic 15 in Jinja template
//    replaced so threshold and discovery payload stay in sync
//  - BAT_CAL_OFFSET (always 0.0, no config path) removed
//  - analogSetPinAttenuation() moved from readMoisture() to setup() — was
//    called on every wake; now called once after config is loaded
//
//  v2.5.1
//  - FOTA: add setHandshakeTimeout() to cover TLS handshake phase —
//    setTimeout() only covers socket read/write; mbedTLS handshake could
//    hang indefinitely without this. Applied to both version check and
//    binary download connections.
//
//  v2.5.0
//  - Configurable GPIO pins: moisturePin, batteryPin, reedPin stored in NVS
//    with safe defaults (0, 1, 3); settable via captive portal Advanced
//    section and MQTT remote config — accommodates hardware variations
//
//  v2.4.1
//  - Battery curve recalibrated: 3.25 V is now 0% (was 10%) based on
//    real-world observation that sensors go offline before reaching the
//    old 10% threshold; all other breakpoints rescaled proportionally
//  - HA low battery binary sensor: autodiscovered entity reports ON when
//    battery_pct <= 15%, using existing state topic (no new MQTT topic)
//  - Reed switch two-stage hold: 3s = restart, 10s = wipe config + portal;
//    log feedback at 3s mark so user knows to keep holding for full reset
//
//  v2.4.0
//  - MQTT remote config: partial JSON updates to garden/sensorN/config/set
//    apply individual settings (mqttBroker, mqttPort, mqttUser, mqttPassword,
//    syslogHost, syslogPort) without a factory reset; same validation as portal
//  - Config state published retained to garden/sensorN/config/state after
//    each change and on every boot (HA text/number entities read from here)
//  - HA MQTT autodiscovery for 6 config entities (text + number) so settings
//    are editable directly from the HA device card
//
//  v2.3.0
//  - HA MQTT autodiscovery: Restart and Reset Config button entities
//    appear automatically in the device card in Home Assistant
//  - Syslog: hostname resolved once at startup with 3 s timeout;
//    uses pre-resolved IPAddress for all sends (no per-packet DNS blocking);
//    syslog disabled for the cycle if DNS fails — no startup delay
//  - Portal input validation: octet range (0-255), port range (1–65535),
//    string length caps, control-character rejection, hostname/IP format
//    checks on mqttBroker and syslogHost; specific error message shown
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

// Dev builds: update the SHA suffix with `git rev-parse --short HEAD` before flashing.
#define FIRMWARE_VERSION "2.6.0"

// ── Pins ─────────────────────────────────────────────────
const int MOISTURE_PIN = 0;   // A0 — XIAO ESP32-C6
const int BATTERY_PIN  = 1;   // A1 — voltage divider midpoint
const int BTN_BOOT     = 9;   // Boot button on XIAO ESP32-C6
const int REED_PIN     = 3;   // Reed switch — GND when magnet present

// ── Fixed calibration ─────────────────────────────────────
const int   DRY_MV         = 2800;
const int   WET_MV         = 1000;
const float DIVIDER_RATIO  = 2.0;
const float BAT_MIN        = 2.5;
const float BAT_MAX        = 4.3;

// ── Timing ────────────────────────────────────────────────
const int SLEEP_MINUTES      = 15;
const int AP_TIMEOUT_MIN     = 10;
const int AP_SLEEP_MIN       = 10;
const int BOOT_HOLD_MS       = 3000;
const int REED_RESTART_MS    = 3000;   // hold 3s  → restart
const int REED_RESET_MS      = 10000;  // hold 10s → wipe config + restart into portal
const int NTP_TIMEOUT_MS     = 10000;
const int CMD_LISTEN_MS      = 2000;
const int WIFI_TIMEOUT_MS    = 10000;  // max time waiting for WiFi association
const int MQTT_TIMEOUT_S     =     5;  // TCP socket timeout per connect attempt
const int FOTA_VERSION_TIMEOUT_MS = 8000;   // version.txt HTTP fetch
const int FOTA_DL_TIMEOUT_MS      = 60000;  // firmware.bin download (large file)

// ── Thresholds ────────────────────────────────────────────
#define LOW_BATTERY_PCT 15   // battery_pct at which low-battery alert fires

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
  // GPIO pin assignments — configurable to accommodate hardware variations
  int     moisturePin;   // default 0  (A0/GPIO0)
  int     batteryPin;    // default 1  (A1/GPIO1)
  int     reedPin;       // default 3  (D3/GPIO3)
} cfg;

struct MoistureReading {
  int rawMv;
  int percent;
};

bool configLoaded = false;

// Forward use of _logf() — Arduino IDE generates the prototype; macro must be
// defined before any call site (loadConfig, clearConfig, saveConfig) so it
// resolves here, not to math.h's float logf(float).
#define logf(fmt, ...) _logf(__func__, fmt, ##__VA_ARGS__)

// NVS magic — identifies config written by this firmware.
// Wrong value means a different firmware used our namespace; absent means
// a pre-2.5.3 sensor (backwards-compatible, magic written on next save).
const char* NVS_MAGIC_KEY   = "magic";
const char* NVS_MAGIC_VALUE = "moisture-1";

void loadConfig() {
  // Check magic before reading anything else.
  // Wrong → clear stale NVS and return (configLoaded stays false → portal).
  // Missing → proceed normally for pre-2.5.3 sensors.
  prefs.begin("sensor", true);
  String magic = prefs.getString(NVS_MAGIC_KEY, "");
  prefs.end();
  if (magic.length() > 0 && magic != NVS_MAGIC_VALUE) {
    logf("Config    — NVS magic mismatch ('%s'), clearing\n", magic.c_str());
    prefs.begin("sensor", false);
    prefs.clear();
    prefs.end();
    return;
  }

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
  cfg.syslogPort  = prefs.getInt("syslogPort",  514);

  cfg.moisturePin = prefs.getInt("moisturePin", 0);
  cfg.batteryPin  = prefs.getInt("batteryPin",  1);
  cfg.reedPin     = prefs.getInt("reedPin",     3);

  prefs.end();

  configLoaded = (cfg.sensorNumber > 0 && strlen(cfg.wifiSSID) > 0 && strlen(cfg.mqttBroker) > 0);
}

void clearConfig() {
  prefs.begin("sensor", false);
  prefs.clear();
  prefs.end();
  logf("Config    — NVS cleared\n");
}

void saveConfig(const Config& c) {
  prefs.begin("sensor", false);
  prefs.putString(NVS_MAGIC_KEY, NVS_MAGIC_VALUE);
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
  prefs.putInt("moisturePin",   c.moisturePin);
  prefs.putInt("batteryPin",    c.batteryPin);
  prefs.putInt("reedPin",       c.reedPin);
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
char DISC_BAT_LOW[128];
char DISC_FW[128];
char CONFIG_SET_PREFIX[80];   // garden/sensorN/config/set  (subscribe as .../+)
char CONFIG_STATE_TOPIC[80];  // garden/sensorN/config/state
char DISC_CFG_MQTT_BROKER[128];
char DISC_CFG_MQTT_PORT[128];
char DISC_CFG_MQTT_USER[128];
char DISC_CFG_MQTT_PASS[128];
char DISC_CFG_SYSLOG_HOST[128];
char DISC_CFG_SYSLOG_PORT[128];
char DISC_CFG_MOISTURE_PIN[128];
char DISC_CFG_BATTERY_PIN[128];
char DISC_CFG_REED_PIN[128];
// Network config discovery topics
char DISC_CFG_STATIC_IP[128];
char DISC_CFG_IP[128];
char DISC_CFG_GW[128];
char DISC_CFG_SN[128];
char DISC_CFG_DNS[128];

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
    "%s/button/%s_reset/config",             HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_BAT_LOW, sizeof(DISC_BAT_LOW),
    "%s/binary_sensor/%s_battery_low/config", HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_FW, sizeof(DISC_FW),
    "%s/sensor/%s_fw/config",                 HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(CONFIG_SET_PREFIX,  sizeof(CONFIG_SET_PREFIX),  "garden/%s/config/set",    SENSOR_ID);
  snprintf(CONFIG_STATE_TOPIC, sizeof(CONFIG_STATE_TOPIC), "garden/%s/config/state",  SENSOR_ID);
  snprintf(DISC_CFG_MQTT_BROKER, sizeof(DISC_CFG_MQTT_BROKER),
    "%s/text/%s_cfg_mqtt_broker/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_MQTT_PORT, sizeof(DISC_CFG_MQTT_PORT),
    "%s/number/%s_cfg_mqtt_port/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_MQTT_USER, sizeof(DISC_CFG_MQTT_USER),
    "%s/text/%s_cfg_mqtt_user/config",     HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_MQTT_PASS, sizeof(DISC_CFG_MQTT_PASS),
    "%s/text/%s_cfg_mqtt_pass/config",     HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_SYSLOG_HOST, sizeof(DISC_CFG_SYSLOG_HOST),
    "%s/text/%s_cfg_syslog_host/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_SYSLOG_PORT, sizeof(DISC_CFG_SYSLOG_PORT),
    "%s/number/%s_cfg_syslog_port/config", HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_MOISTURE_PIN, sizeof(DISC_CFG_MOISTURE_PIN),
    "%s/number/%s_cfg_moisture_pin/config", HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_BATTERY_PIN, sizeof(DISC_CFG_BATTERY_PIN),
    "%s/number/%s_cfg_battery_pin/config",  HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_REED_PIN, sizeof(DISC_CFG_REED_PIN),
    "%s/number/%s_cfg_reed_pin/config",     HA_DISCOVERY_PREFIX, SENSOR_ID);

  snprintf(DISC_CFG_STATIC_IP, sizeof(DISC_CFG_STATIC_IP),
    "%s/switch/%s_cfg_static_ip/config",   HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_IP,        sizeof(DISC_CFG_IP),
    "%s/text/%s_cfg_ip/config",            HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_GW,        sizeof(DISC_CFG_GW),
    "%s/text/%s_cfg_gw/config",            HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_SN,        sizeof(DISC_CFG_SN),
    "%s/text/%s_cfg_sn/config",            HA_DISCOVERY_PREFIX, SENSOR_ID);
  snprintf(DISC_CFG_DNS,       sizeof(DISC_CFG_DNS),
    "%s/text/%s_cfg_dns/config",           HA_DISCOVERY_PREFIX, SENSOR_ID);
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
static int       syslogHead  = 0;
static int       syslogTotal = 0;
static bool      syslogReady = false;
static IPAddress syslogIP;          // resolved once in syslogFlush(); used by syslogSend()

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

  // Use pre-resolved IPAddress — beginPacket(IPAddress) never blocks
  syslogUdp.beginPacket(syslogIP, cfg.syslogPort);
  syslogUdp.print(packet);
  syslogUdp.endPacket();
}

void syslogFlush() {
  if (strlen(cfg.syslogHost) == 0) { syslogReady = true; return; }

  // Resolve syslog host → IP once per cycle so syslogSend() can use
  // beginPacket(IPAddress) — which never blocks — instead of beginPacket(hostname).
  //
  // Try numeric IP first (instant, no DNS).  Fall back to hostByName() only if
  // needed; lwIP's internal DNS timeout is ~4 s so this blocks at most once per
  // wake cycle rather than on every log call.
  if (!syslogIP.fromString(cfg.syslogHost)) {
    if (WiFi.hostByName(cfg.syslogHost, syslogIP) != 1) {
      // Mark ready + IP=0 before logf() so the message goes to serial only
      syslogReady = true;
      syslogHead  = 0;
      syslogTotal = 0;
      logf("DNS failed for '%s' — syslog disabled this cycle\n", cfg.syslogHost);
      return;
    }
  }

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
  Serial.printf("[%s] %s", func, line);

  // Skip syslog if not configured, or if DNS resolution failed this cycle
  // (syslogIP == 0 means either not yet resolved or resolution failed)
  if (strlen(cfg.syslogHost) == 0) return;
  if (syslogReady && (uint32_t)syslogIP == 0) return;

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
<meta charset="utf-8">
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

  <details style="margin:16px 0">
    <summary style="font-weight:600;font-size:.95em;color:#555;cursor:pointer;
      padding:10px 14px;background:#fff;border-radius:8px;
      box-shadow:0 1px 4px rgba(0,0,0,.08);list-style:none">
      &#9654; Advanced — GPIO pin assignments
    </summary>
    <div class="section" style="margin-top:0;border-top-left-radius:0;border-top-right-radius:0">
      <p class="hint" style="margin-top:0">Change only if your hardware uses different pins than the defaults.
        All three pins must be different. Valid range: 0–10 (XIAO GPIO numbers).</p>
      <label>Moisture sensor pin
        <input type="number" name="moisturePin" value="0" min="0" max="10">
      </label>
      <p class="hint">Default: 0 (A0/GPIO0)</p>
      <label>Battery voltage pin
        <input type="number" name="batteryPin" value="1" min="0" max="10">
      </label>
      <p class="hint">Default: 1 (A1/GPIO1)</p>
      <label>Reed switch pin
        <input type="number" name="reedPin" value="3" min="0" max="10">
      </label>
      <p class="hint">Default: 3 (D3/GPIO3)</p>
    </div>
  </details>

  <div id="err" style="display:none;background:#fde8e8;border:1px solid #c0392b;
    border-radius:8px;padding:12px 16px;margin-top:12px;color:#c0392b;
    font-size:.9em;font-weight:600"></div>

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
function isValidIP(s) {
  var dots = (s.match(/\./g)||[]).length;
  if (dots !== 3) return false;
  var p = s.split('.');
  if (p.length !== 4) return false;
  return p.every(function(o) {
    return /^\d+$/.test(o) && parseInt(o,10) >= 0 && parseInt(o,10) <= 255;
  });
}
function isValidHost(s) {
  if (!s || !s.length) return false;
  if (s.indexOf('.') === -1) return false;
  if (!/^[A-Za-z0-9.\-]+$/.test(s)) return false;
  if (s[0]==='.' || s[0]==='-' || s[s.length-1]==='.' || s[s.length-1]==='-') return false;
  if (/^[\d.]+$/.test(s)) return isValidIP(s);
  return true;
}
function v(id) {
  var el = document.querySelector('[name="'+id+'"]');
  return el ? el.value.trim() : '';
}
function fail(e, msg) {
  var el = document.getElementById('err');
  el.textContent = msg;
  el.style.display = 'block';
  el.scrollIntoView({behavior:'smooth', block:'center'});
  e.preventDefault();
}
function validateForm(e) {
  document.getElementById('err').style.display = 'none';
  var n = parseInt(v('sensorNum'),10);
  if (isNaN(n)||n<1||n>254)
    return fail(e, 'Sensor number must be between 1 and 254.');
  if (!v('ssid'))
    return fail(e, 'WiFi SSID is required.');
  if (v('ssid').length>63)
    return fail(e, 'WiFi SSID is too long (max 63 characters).');
  if (v('wifiPass').length>63)
    return fail(e, 'WiFi password is too long (max 63 characters).');
  if (document.querySelector('[name="staticIP"]').checked) {
    var ipGroups=['ip','gw','sn','dns'];
    for (var i=0;i<ipGroups.length;i++) {
      for (var j=1;j<=4;j++) {
        var val=parseInt(v(ipGroups[i]+j),10);
        if (isNaN(val)||val<0||val>255)
          return fail(e, 'IP field '+ipGroups[i]+j+' must be 0-255.');
      }
    }
  }
  var broker = v('mqttBroker');
  if (!broker)
    return fail(e, 'MQTT broker address is required.');
  if (!isValidHost(broker))
    return fail(e, 'MQTT broker must be a valid IP address or fully-qualified hostname (e.g. 192.168.1.1 or mqtt.local). Invalid value: "' + broker + '"');
  var mp = parseInt(v('mqttPort'),10);
  if (v('mqttPort') && (isNaN(mp)||mp<1||mp>65535))
    return fail(e, 'MQTT port must be between 1 and 65535.');
  if (v('mqttUser').length>31)
    return fail(e, 'MQTT username is too long (max 31 characters).');
  if (v('mqttPass').length>63)
    return fail(e, 'MQTT password is too long (max 63 characters).');
  var sh = v('syslogHost');
  if (sh.length>0 && !isValidHost(sh))
    return fail(e, 'Syslog host must be a valid IP address or fully-qualified hostname (e.g. 192.168.1.10 or logs.local). Invalid value: "' + sh + '"');
  var sp = parseInt(v('syslogPort'),10);
  if (v('syslogPort') && (isNaN(sp)||sp<1||sp>65535))
    return fail(e, 'Syslog port must be between 1 and 65535.');
  var mPin = parseInt(v('moisturePin'),10);
  var bPin = parseInt(v('batteryPin'),10);
  var rPin = parseInt(v('reedPin'),10);
  if (isNaN(mPin)||mPin<0||mPin>10) return fail(e, 'Moisture pin must be between 0 and 10.');
  if (isNaN(bPin)||bPin<0||bPin>10) return fail(e, 'Battery pin must be between 0 and 10.');
  if (isNaN(rPin)||rPin<0||rPin>10) return fail(e, 'Reed switch pin must be between 0 and 10.');
  if (mPin===bPin||mPin===rPin||bPin===rPin)
    return fail(e, 'Moisture, battery and reed switch pins must all be different.');
}
document.querySelector('form').addEventListener('submit', validateForm);
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

// Error page is generated dynamically so the specific reason can be shown.

// ═══════════════════════════════════════════════════════════
//  INPUT VALIDATION HELPERS
// ═══════════════════════════════════════════════════════════

// Returns true if the string contains any ASCII control character (< 0x20 or DEL).
static bool hasControlChars(const String& s) {
  for (int i = 0; i < (int)s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
}

// Returns true if every character is valid in a hostname or IP address: [A-Za-z0-9.-]
static bool isValidHostChars(const String& s) {
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (!isalnum((uint8_t)c) && c != '.' && c != '-') return false;
  }
  return true;
}

// Validates a hostname/IP string: non-empty, valid chars, contains a dot,
// doesn't start or end with a dot or hyphen.
// If the string contains only digits and dots it is treated as an IPv4
// address and validated with IPAddress.fromString() — catches 999.999.999.999 etc.
static String validateHost(const String& s, const char* label) {
  if (s.length() == 0)           return String(label) + " is required.";
  if (s.length() > 63)           return String(label) + " is too long (max 63 characters).";
  if (!isValidHostChars(s))      return String(label) + " contains invalid characters — use only letters, digits, hyphens and dots.";
  if (s.indexOf('.') == -1)      return String(label) + " must be an IP address or fully-qualified hostname (e.g. 192.168.1.1 or mqtt.local).";
  if (s[0] == '.' || s[0] == '-')
    return String(label) + " must not start with a dot or hyphen.";
  if (s[s.length()-1] == '.' || s[s.length()-1] == '-')
    return String(label) + " must not end with a dot or hyphen.";

  // If every character is a digit or dot, validate strictly as IPv4.
  // We do NOT use IPAddress::fromString() here because LWIP accepts non-standard
  // short forms like "999.999" (BSD two-part notation) as valid.
  // Instead: count dots (must be exactly 3), then check each octet is 0-255.
  bool looksLikeIP = true;
  for (int i = 0; i < (int)s.length(); i++) {
    if (!isdigit((uint8_t)s[i]) && s[i] != '.') { looksLikeIP = false; break; }
  }
  if (looksLikeIP) {
    // Count dots
    int dots = 0;
    for (int i = 0; i < (int)s.length(); i++) if (s[i] == '.') dots++;
    if (dots != 3)
      return String(label) + " '" + s + "' is not a valid IPv4 address — must have exactly 4 octets.";
    // Validate each octet
    int start = 0;
    for (int i = 0; i <= (int)s.length(); i++) {
      if (i == (int)s.length() || s[i] == '.') {
        String seg = s.substring(start, i);
        if (seg.length() == 0 || seg.length() > 3)
          return String(label) + " '" + s + "' — octet '" + seg + "' is invalid.";
        int val = seg.toInt();
        if (val < 0 || val > 255)
          return String(label) + " '" + s + "' — octet " + seg + " must be 0-255.";
        start = i + 1;
      }
    }
  }

  return "";
}

// Validates all POST fields from the config form.
// Returns an empty string on success, or a human-readable error on failure.
static String validateSave() {
  // ── Sensor number ───────────────────────────────────────
  int sensorNum = server.arg("sensorNum").toInt();
  if (sensorNum < 1 || sensorNum > 254)
    return "Sensor number must be between 1 and 254.";

  // ── WiFi SSID ───────────────────────────────────────────
  String ssid = server.arg("ssid");
  if (ssid.length() == 0)   return "WiFi SSID is required.";
  if (ssid.length() > 63)   return "WiFi SSID is too long (max 63 characters).";

  // ── WiFi password (optional) ────────────────────────────
  String wifiPass = server.arg("wifiPass");
  if (wifiPass.length() > 63)          return "WiFi password is too long (max 63 characters).";
  if (hasControlChars(wifiPass))       return "WiFi password contains control characters.";

  // ── Static IP octets ────────────────────────────────────
  if (server.hasArg("staticIP")) {
    static const char* ipFields[] = {
      "ip1","ip2","ip3","ip4",
      "gw1","gw2","gw3","gw4",
      "sn1","sn2","sn3","sn4",
      "dns1","dns2","dns3","dns4"
    };
    for (int i = 0; i < 16; i++) {
      String v = server.arg(ipFields[i]);
      if (v.length() == 0)
        return String("IP field '") + ipFields[i] + "' is empty.";
      for (int j = 0; j < (int)v.length(); j++) {
        if (!isdigit((uint8_t)v[j]))
          return String("IP field '") + ipFields[i] + "' must be a number (0-255).";
      }
      int oct = v.toInt();
      if (oct < 0 || oct > 255)
        return String("IP field '") + ipFields[i] + "' must be 0-255 (got " + v + ").";
    }
  }

  // ── MQTT broker ─────────────────────────────────────────
  String err = validateHost(server.arg("mqttBroker"), "MQTT broker");
  if (err.length()) return err;

  // ── MQTT port ───────────────────────────────────────────
  String mqttPortStr = server.arg("mqttPort");
  if (mqttPortStr.length() > 0) {
    int p = mqttPortStr.toInt();
    if (p < 1 || p > 65535) return "MQTT port must be between 1 and 65535.";
  }

  // ── MQTT credentials (optional) ─────────────────────────
  String mqttUser = server.arg("mqttUser");
  String mqttPass = server.arg("mqttPass");
  if (mqttUser.length() > 31)     return "MQTT username is too long (max 31 characters).";
  if (mqttPass.length() > 63)     return "MQTT password is too long (max 63 characters).";
  if (hasControlChars(mqttUser))  return "MQTT username contains control characters.";
  if (hasControlChars(mqttPass))  return "MQTT password contains control characters.";

  // ── Syslog host (optional) ──────────────────────────────
  String syslogHost = server.arg("syslogHost");
  if (syslogHost.length() > 0) {
    err = validateHost(syslogHost, "Syslog host");
    if (err.length()) return err;
  }

  // ── Syslog port (optional) ──────────────────────────────
  String syslogPortStr = server.arg("syslogPort");
  if (syslogPortStr.length() > 0) {
    int p = syslogPortStr.toInt();
    if (p < 1 || p > 65535) return "Syslog port must be between 1 and 65535.";
  }

  // ── GPIO pin assignments ─────────────────────────────────
  int mPin = server.arg("moisturePin").toInt();
  int bPin = server.arg("batteryPin").toInt();
  int rPin = server.arg("reedPin").toInt();
  if (mPin < 0 || mPin > 10) return "Moisture pin must be between 0 and 10.";
  if (bPin < 0 || bPin > 10) return "Battery pin must be between 0 and 10.";
  if (rPin < 0 || rPin > 10) return "Reed switch pin must be between 0 and 10.";
  if (mPin == bPin || mPin == rPin || bPin == rPin)
    return "Moisture, battery and reed switch pins must all be different.";

  return "";  // all good
}

// Sends a 400 error page with a specific reason shown to the user.
static void sendError(const String& reason) {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Invalid input</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:80px auto;padding:0 16px;"
    "text-align:center;background:#f5f5f5}h1{color:#c0392b}p{color:#555}a{color:#2c7a4b}</style>"
    "</head><body>"
    "<h1>Invalid input</h1>"
    "<p>" + reason + "</p>"
    "<p><a href=\"/\">Go back</a></p>"
    "</body></html>";
  server.send(400, "text/html", html);
}

// ═══════════════════════════════════════════════════════════
//  PORTAL HANDLERS
// ═══════════════════════════════════════════════════════════

void handleRoot() {
  server.send_P(200, "text/html", CONFIG_HTML);
}

void handleSave() {
  String err = validateSave();
  if (err.length()) {
    sendError(err);
    return;
  }

  int sensorNum = server.arg("sensorNum").toInt();

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

  c.moisturePin = server.arg("moisturePin").toInt();
  c.batteryPin  = server.arg("batteryPin").toInt();
  c.reedPin     = server.arg("reedPin").toInt();

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
  client.setTimeout(FOTA_VERSION_TIMEOUT_MS / 1000);         // socket read/write timeout (seconds)
  client.setHandshakeTimeout(FOTA_VERSION_TIMEOUT_MS / 1000); // TLS handshake timeout (seconds)
  // Note: setTimeout() does NOT cover the mbedTLS handshake phase — setHandshakeTimeout()
  // is required to prevent an indefinite hang when GitHub CDN is slow to complete TLS.

  HTTPClient http;
  http.begin(client, FOTA_VERSION_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(FOTA_VERSION_TIMEOUT_MS);
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

  // Fresh client for the binary download — longer timeout for large file.
  // CDN redirect means a new TLS handshake — update both timeouts.
  client.setTimeout(FOTA_DL_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(FOTA_DL_TIMEOUT_MS / 1000);

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
  pinMode(cfg.reedPin, INPUT_PULLUP);
  delay(50);

  if (digitalRead(cfg.reedPin) == LOW) {
    logf("Reed      — magnet detected, waiting to confirm...\n");
    unsigned long holdStart  = millis();
    bool          restartArmed = false;

    while (digitalRead(cfg.reedPin) == LOW) {
      unsigned long held = millis() - holdStart;

      if (held >= REED_RESET_MS) {
        logf("Reed      — 10s hold: wiping config, restarting into portal\n");
        Serial.flush();
        delay(200);
        clearConfig();
        ESP.restart();
      }

      if (!restartArmed && held >= REED_RESTART_MS) {
        restartArmed = true;
        logf("Reed      — 3s hold: release to restart, keep holding for 10s to wipe config\n");
      }

      delay(50);
    }

    if (restartArmed) {
      logf("Reed      — released, restarting\n");
      Serial.flush();
      delay(200);
      ESP.restart();
    } else {
      logf("Reed      — magnet removed early, ignoring\n");
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════

void goToSleep() {
  logf("Sleep     — going to sleep for %d minutes\n", SLEEP_MINUTES);

  // Enable GPIO wakeup so a magnet presentation wakes the device immediately.
  // INPUT_PULLUP keeps the pin HIGH (reed open); closing to GND fires the wake.
  // The pullup state is retained during deep sleep so no spurious wakeups occur.
  pinMode(cfg.reedPin, INPUT_PULLUP);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << cfg.reedPin, ESP_GPIO_WAKEUP_GPIO_LOW);

  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

MoistureReading readMoisture() {
  delay(500);
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(cfg.moisturePin);
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
    sum += analogReadMilliVolts(cfg.batteryPin);
    delay(5);
  }
  rawMv = sum / 16;
  float voltage = rawMv / 1000.0 * DIVIDER_RATIO;
  if (voltage < BAT_MIN || voltage > BAT_MAX) {
    logf("Battery   — suspicious reading %.2fV (raw: %dmV), discarding\n",
      voltage, rawMv);
    return -1.0;
  }
  logf("Battery   — raw: %dmV  ->  %.2fV\n", rawMv, voltage);
  return voltage;
}

int batteryPercent(float voltage) {
  // Curve anchored at real-world empty (3.25 V = 0%) based on observed field
  // behaviour: sensors become unreachable before reaching the old 10% threshold.
  // Rescaled linearly from (old 10%→0%) to (old 100%→100%).
  const float v[] = { 3.25, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20 };
  const int   p[] = {    0,   11,   28,   44,   61,   78,   89,   94,  100 };
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

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < (unsigned long)WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
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

// ── Pending remote config updates ────────────────────────────
// Set in mqttCallback (minimal work only — no validation, no String objects,
// no mqtt.loop() — see note on callback safety below).
// Processed in applyConfigChange() outside the callback.
static char pendingMqttBroker[64]    = "";
static char pendingMqttPort[8]       = "";
static char pendingMqttUser[32]      = "";
static char pendingMqttPassword[64]  = "";
static char pendingSyslogHost[64]    = "";
static char pendingSyslogPort[8]     = "";
static char pendingMoisturePin[4]    = "";
static char pendingBatteryPin[4]     = "";
static char pendingReedPin[4]        = "";
static char pendingStaticIP[8]       = "";  // "true" or "false"
static char pendingIP[16]            = "";
static char pendingGW[16]            = "";
static char pendingSN[16]            = "";
static char pendingDNS[16]           = "";
static uint16_t pendingFields        = 0;   // bitmask — which fields arrived this cycle
#define PF_MQTT_BROKER    (1<<0)
#define PF_MQTT_PORT      (1<<1)
#define PF_MQTT_USER      (1<<2)
#define PF_MQTT_PASSWORD  (1<<3)
#define PF_SYSLOG_HOST    (1<<4)
#define PF_SYSLOG_PORT    (1<<5)
#define PF_MOISTURE_PIN   (1<<6)
#define PF_BATTERY_PIN    (1<<7)
#define PF_REED_PIN       (1<<8)
#define PF_STATIC_IP      (1<<9)
#define PF_IP             (1<<10)
#define PF_GW             (1<<11)
#define PF_SN             (1<<12)
#define PF_DNS            (1<<13)

// ── IMPORTANT: callback safety rules ────────────────────────
// PubSubClient passes topic and payload as pointers INTO its internal buffer.
// Calling mqtt.publish() inside the callback overwrites that same buffer, so
// topic/payload become invalid immediately after. Calling mqtt.loop() inside
// the callback can trigger re-entrant processing, corrupting internal state.
//
// Rules enforced here:
//   1. Copy ALL data needed (topic suffix, payload) to local/global buffers
//      BEFORE calling mqtt.publish() to clear the retained message.
//   2. Never call mqtt.loop() inside the callback.
//   3. Never allocate heap (no String objects) inside the callback.
//   4. Never call validateHost() or other functions that create String objects.
//
// All validation, cfg mutation, and logging happens in applyConfigChange()
// outside the callback, after the listen loop completes.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0) return;

  if (strcmp(topic, CMD_TOPIC) == 0) {
    // ── Command (restart / reset) ──────────────────────────
    if (length >= sizeof(cmdPayload)) return;
    memcpy(cmdPayload, payload, length);
    cmdPayload[length] = '\0';
    cmdReceived = true;
    // DO NOT call mqtt.publish() or mqtt.loop() here.
    // PubSubClient uses one buffer for both rx and tx. Calling publish() inside
    // the callback overwrites the buffer mid-read, corrupting internal state and
    // crashing the next publish() call outside the callback.
    // CMD_TOPIC retained message is cleared in processMqttCommand() instead.

  } else {
    // ── Config update (config/set/<field>) ────────────────
    size_t prefixLen = strlen(CONFIG_SET_PREFIX);
    if (strncmp(topic, CONFIG_SET_PREFIX, prefixLen) != 0 ||
        topic[prefixLen] != '/') return;

    // Copy field name and value — all we do in the callback.
    // NO mqtt.publish(), NO mqtt.loop(), NO heap allocation (no String objects).
    // Retained messages are cleared in applyConfigChange() outside the callback.
    char fieldName[32];
    strlcpy(fieldName, topic + prefixLen + 1, sizeof(fieldName));

    char val[128] = "";
    if (length < sizeof(val)) {
      memcpy(val, payload, length);
      val[length] = '\0';
    }

    if      (strcmp(fieldName, "mqttBroker")   == 0) { strlcpy(pendingMqttBroker,   val, sizeof(pendingMqttBroker));   pendingFields |= PF_MQTT_BROKER;   }
    else if (strcmp(fieldName, "mqttPort")     == 0) { strlcpy(pendingMqttPort,     val, sizeof(pendingMqttPort));     pendingFields |= PF_MQTT_PORT;     }
    else if (strcmp(fieldName, "mqttUser")     == 0) { strlcpy(pendingMqttUser,     val, sizeof(pendingMqttUser));     pendingFields |= PF_MQTT_USER;     }
    else if (strcmp(fieldName, "mqttPassword") == 0) { strlcpy(pendingMqttPassword, val, sizeof(pendingMqttPassword)); pendingFields |= PF_MQTT_PASSWORD; }
    else if (strcmp(fieldName, "syslogHost")   == 0) { strlcpy(pendingSyslogHost,   val, sizeof(pendingSyslogHost));   pendingFields |= PF_SYSLOG_HOST;   }
    else if (strcmp(fieldName, "syslogPort")   == 0) { strlcpy(pendingSyslogPort,   val, sizeof(pendingSyslogPort));   pendingFields |= PF_SYSLOG_PORT;   }
    else if (strcmp(fieldName, "moisturePin")  == 0) { strlcpy(pendingMoisturePin,  val, sizeof(pendingMoisturePin));  pendingFields |= PF_MOISTURE_PIN;  }
    else if (strcmp(fieldName, "batteryPin")   == 0) { strlcpy(pendingBatteryPin,   val, sizeof(pendingBatteryPin));   pendingFields |= PF_BATTERY_PIN;   }
    else if (strcmp(fieldName, "reedPin")      == 0) { strlcpy(pendingReedPin,      val, sizeof(pendingReedPin));      pendingFields |= PF_REED_PIN;      }
    else if (strcmp(fieldName, "staticIP")    == 0) { strlcpy(pendingStaticIP,    val, sizeof(pendingStaticIP));    pendingFields |= PF_STATIC_IP;    }
    else if (strcmp(fieldName, "ip")          == 0) { strlcpy(pendingIP,          val, sizeof(pendingIP));          pendingFields |= PF_IP;           }
    else if (strcmp(fieldName, "gw")          == 0) { strlcpy(pendingGW,          val, sizeof(pendingGW));          pendingFields |= PF_GW;           }
    else if (strcmp(fieldName, "sn")          == 0) { strlcpy(pendingSN,          val, sizeof(pendingSN));          pendingFields |= PF_SN;           }
    else if (strcmp(fieldName, "dns")         == 0) { strlcpy(pendingDNS,         val, sizeof(pendingDNS));         pendingFields |= PF_DNS;          }
    // Unknown fields silently ignored
  }
}

void connectMqtt() {
  mqtt.setServer(cfg.mqttBroker, cfg.mqttPort);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(MQTT_TIMEOUT_S);  // caps TCP connect per attempt
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
      char configWildcard[88];
      snprintf(configWildcard, sizeof(configWildcard), "%s/+", CONFIG_SET_PREFIX);
      mqtt.subscribe(CMD_TOPIC);
      mqtt.subscribe(configWildcard);
      logf("MQTT      — subscribed to %s and %s\n", CMD_TOPIC, configWildcard);
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
  logf("Command   — received: %s\n", cmdPayload);

  // Clear the retained CMD message from the broker now that we have a local copy.
  // Safe to publish here — we are outside mqttCallback so the PubSubClient buffer
  // is not in use by the callback.  Must happen before acting on the command so
  // a reset/restart doesn't leave a stale retained "reset"/"restart" on the broker
  // that would fire again on the next boot.
  mqtt.publish(CMD_TOPIC, "", true);
  mqtt.loop();

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

// Publishes a retained HA discovery payload.
// Pass the return value of snprintf() as `written` — if it equals or exceeds
// `bufSize` the output was truncated (snprintf returns the would-be length).
// Truncated payloads are silently malformed JSON; we skip them and log instead.
static void publishMqttEntity(const char* topic, const char* payload,
                               size_t bufSize, int written) {
  if (written < 0 || written >= (int)bufSize) {
    logf("Discovery — SKIPPED %s (payload %d bytes, buffer %d)\n",
         topic, written + 1, (int)bufSize);
    return;
  }
  mqtt.publish(topic, payload, true);
  mqtt.loop();
  delay(50);
}

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

  char payload[640];

  publishMqttEntity(DISC_MOISTURE, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Moisture\",\"unique_id\":\"%s_moisture\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.moisture }}\","
      "\"unit_of_measurement\":\"%%\",\"device_class\":\"moisture\","
      "\"state_class\":\"measurement\",\"icon\":\"mdi:water-percent\",%s}",
      SENSOR_ID, STATE_TOPIC, device));

  publishMqttEntity(DISC_BAT_V, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Battery Voltage\",\"unique_id\":\"%s_battery_v\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.battery_v }}\","
      "\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\","
      "\"state_class\":\"measurement\",\"icon\":\"mdi:battery\",%s}",
      SENSOR_ID, STATE_TOPIC, device));

  publishMqttEntity(DISC_BAT_PCT, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Battery\",\"unique_id\":\"%s_battery_pct\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.battery_pct }}\","
      "\"unit_of_measurement\":\"%%\",\"device_class\":\"battery\","
      "\"state_class\":\"measurement\",\"icon\":\"mdi:battery-percent\",%s}",
      SENSOR_ID, STATE_TOPIC, device));

  // ── Binary sensor: Low Battery ───────────────────────────
  // Uses the existing state topic — no extra publish needed.
  // Reads battery_pct from the JSON and reports ON (low) when <= LOW_BATTERY_PCT.
  // battery_pct is null when a suspicious reading is discarded; treated as
  // not-low in that case so a bad ADC reading doesn't spam alerts.
  publishMqttEntity(DISC_BAT_LOW, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Battery Low\",\"unique_id\":\"%s_battery_low\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{ 'ON' if value_json.battery_pct is not none and value_json.battery_pct | int <= %d else 'OFF' }}\","
      "\"device_class\":\"battery\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
      "\"icon\":\"mdi:battery-alert\",%s}",
      SENSOR_ID, STATE_TOPIC, LOW_BATTERY_PCT, device));

  publishMqttEntity(DISC_TS, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Last Seen\",\"unique_id\":\"%s_ts\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.ts }}\","
      "\"device_class\":\"timestamp\","
      "\"icon\":\"mdi:clock-outline\",%s}",
      SENSOR_ID, STATE_TOPIC, device));

  publishMqttEntity(DISC_FW, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Firmware Version\",\"unique_id\":\"%s_fw\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.fw }}\","
      "\"icon\":\"mdi:chip\",%s}",
      SENSOR_ID, STATE_TOPIC, device));

  // ── Button: Restart ──────────────────────────────────────
  // Publishes a retained "restart" to CMD_TOPIC when pressed in HA.
  // The sensor picks this up on its next wake, restarts, then clears it.
  publishMqttEntity(DISC_BTN_RESTART, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Restart\",\"unique_id\":\"%s_restart\","
      "\"command_topic\":\"%s\",\"payload_press\":\"restart\","
      "\"retain\":true,\"device_class\":\"restart\","
      "\"icon\":\"mdi:restart\",%s}",
      SENSOR_ID, CMD_TOPIC, device));

  // ── Button: Reset Config ─────────────────────────────────
  // Publishes a retained "reset" to CMD_TOPIC when pressed in HA.
  // The sensor clears NVS and opens the captive portal on next wake.
  publishMqttEntity(DISC_BTN_RESET, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Reset Config\",\"unique_id\":\"%s_reset\","
      "\"command_topic\":\"%s\",\"payload_press\":\"reset\","
      "\"retain\":true,"
      "\"icon\":\"mdi:restore\",%s}",
      SENSOR_ID, CMD_TOPIC, device));

  logf("Discovery — complete (sensors + buttons)\n");
}

// ═══════════════════════════════════════════════════════════
//  MQTT REMOTE CONFIG
//
//  garden/sensorN/config/set   — incoming partial JSON update (retained,
//                                self-cleared by device after receipt)
//  garden/sensorN/config/state — outgoing retained snapshot of current
//                                configurable settings (read by HA entities)
//
//  Configurable via MQTT: mqttBroker, mqttPort, mqttUser, mqttPassword,
//                         syslogHost, syslogPort.
//  WiFi credentials and sensorNumber are portal-only (changing either
//  remotely risks total loss of connectivity with no recovery path).
//
//  Validation parity: same rules as the captive portal and validateSave().
// ═══════════════════════════════════════════════════════════

// Publishes current configurable settings as a retained JSON object.
// Called on every boot (so HA text/number entities reflect current state)
// and again after applying a remote config change.
// Note: mqttPassword is masked ("***") — the plaintext credential must not
//       sit in a retained topic readable by any MQTT subscriber on the LAN.
void publishConfigState() {
  char ipStr[16], gwStr[16], snStr[16], dnsStr[16];
  snprintf(ipStr,  sizeof(ipStr),  "%d.%d.%d.%d", cfg.ip[0],  cfg.ip[1],  cfg.ip[2],  cfg.ip[3]);
  snprintf(gwStr,  sizeof(gwStr),  "%d.%d.%d.%d", cfg.gw[0],  cfg.gw[1],  cfg.gw[2],  cfg.gw[3]);
  snprintf(snStr,  sizeof(snStr),  "%d.%d.%d.%d", cfg.sn[0],  cfg.sn[1],  cfg.sn[2],  cfg.sn[3]);
  snprintf(dnsStr, sizeof(dnsStr), "%d.%d.%d.%d", cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3]);

  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"mqttBroker\":\"%s\",\"mqttPort\":%d,"
    "\"mqttUser\":\"%s\",\"mqttPassword\":\"***\","
    "\"syslogHost\":\"%s\",\"syslogPort\":%d,"
    "\"moisturePin\":%d,\"batteryPin\":%d,\"reedPin\":%d,"
    "\"staticIP\":%s,\"ip\":\"%s\",\"gw\":\"%s\",\"sn\":\"%s\",\"dns\":\"%s\"}",
    cfg.mqttBroker, cfg.mqttPort,
    cfg.mqttUser,
    cfg.syslogHost, cfg.syslogPort,
    cfg.moisturePin, cfg.batteryPin, cfg.reedPin,
    cfg.staticIP ? "true" : "false", ipStr, gwStr, snStr, dnsStr);
  mqtt.publish(CONFIG_STATE_TOPIC, payload, true);
  mqtt.loop();
  logf("Config    — state published to %s\n", CONFIG_STATE_TOPIC);
}

// Called after the listen window. Validates and applies any config/set/+
// messages that arrived during mqttCallback (stored in pending* globals).
// Saves to NVS and restarts if any field passes validation.
void applyConfigChange() {
  if (!pendingFields) return;

  // Clear each per-field retained topic from the broker before validation.
  // Safe to publish here — we are outside mqttCallback (listen window has ended).
  // Clearing unconditionally on receipt (even if validation later rejects the
  // value) is correct: a bad value should not keep retrying every wake cycle.
  {
    char t[96];
    if (pendingFields & PF_MQTT_BROKER)   { snprintf(t, sizeof(t), "%s/mqttBroker",   CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_MQTT_PORT)     { snprintf(t, sizeof(t), "%s/mqttPort",     CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_MQTT_USER)     { snprintf(t, sizeof(t), "%s/mqttUser",     CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_MQTT_PASSWORD) { snprintf(t, sizeof(t), "%s/mqttPassword", CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_SYSLOG_HOST)   { snprintf(t, sizeof(t), "%s/syslogHost",   CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_SYSLOG_PORT)   { snprintf(t, sizeof(t), "%s/syslogPort",   CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_MOISTURE_PIN)  { snprintf(t, sizeof(t), "%s/moisturePin",  CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_BATTERY_PIN)   { snprintf(t, sizeof(t), "%s/batteryPin",   CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_REED_PIN)      { snprintf(t, sizeof(t), "%s/reedPin",      CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_STATIC_IP)   { snprintf(t, sizeof(t), "%s/staticIP",    CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_IP)          { snprintf(t, sizeof(t), "%s/ip",          CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_GW)          { snprintf(t, sizeof(t), "%s/gw",          CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_SN)          { snprintf(t, sizeof(t), "%s/sn",          CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
    if (pendingFields & PF_DNS)         { snprintf(t, sizeof(t), "%s/dns",         CONFIG_SET_PREFIX); mqtt.publish(t, "", true); mqtt.loop(); }
  }

  bool changed = false;
  String err;

  if (pendingFields & PF_MQTT_BROKER) {
    err = validateHost(String(pendingMqttBroker), "MQTT broker");
    if (err.length()) { logf("Config    — mqttBroker rejected: %s\n", err.c_str()); }
    else { strlcpy(cfg.mqttBroker, pendingMqttBroker, sizeof(cfg.mqttBroker)); logf("Config    — mqttBroker -> %s\n", pendingMqttBroker); changed = true; }
  }

  if (pendingFields & PF_MQTT_PORT) {
    int p = atoi(pendingMqttPort);
    if (p < 1 || p > 65535) { logf("Config    — mqttPort rejected: must be 1-65535\n"); }
    else { cfg.mqttPort = p; logf("Config    — mqttPort -> %d\n", p); changed = true; }
  }

  if (pendingFields & PF_MQTT_USER) {
    String s(pendingMqttUser);
    if (s.length() > 31)    { logf("Config    — mqttUser rejected: too long\n"); }
    else if (hasControlChars(s)) { logf("Config    — mqttUser rejected: control chars\n"); }
    else { strlcpy(cfg.mqttUser, pendingMqttUser, sizeof(cfg.mqttUser)); logf("Config    — mqttUser -> %s\n", pendingMqttUser); changed = true; }
  }

  if (pendingFields & PF_MQTT_PASSWORD) {
    String s(pendingMqttPassword);
    if (s.length() > 63)    { logf("Config    — mqttPassword rejected: too long\n"); }
    else if (hasControlChars(s)) { logf("Config    — mqttPassword rejected: control chars\n"); }
    else { strlcpy(cfg.mqttPassword, pendingMqttPassword, sizeof(cfg.mqttPassword)); logf("Config    — mqttPassword updated\n"); changed = true; }
  }

  if (pendingFields & PF_SYSLOG_HOST) {
    String s(pendingSyslogHost);
    err = (s.length() > 0) ? validateHost(s, "Syslog host") : "";
    if (err.length()) { logf("Config    — syslogHost rejected: %s\n", err.c_str()); }
    else { strlcpy(cfg.syslogHost, pendingSyslogHost, sizeof(cfg.syslogHost)); logf("Config    — syslogHost -> '%s'\n", pendingSyslogHost); changed = true; }
  }

  if (pendingFields & PF_SYSLOG_PORT) {
    int p = atoi(pendingSyslogPort);
    if (p < 1 || p > 65535) { logf("Config    — syslogPort rejected: must be 1-65535\n"); }
    else { cfg.syslogPort = p; logf("Config    — syslogPort -> %d\n", p); changed = true; }
  }

  if (pendingFields & PF_MOISTURE_PIN) {
    int p = atoi(pendingMoisturePin);
    if (p < 0 || p > 10) { logf("Config    — moisturePin rejected: must be 0-10\n"); }
    else if (p == cfg.batteryPin || p == cfg.reedPin) { logf("Config    — moisturePin rejected: conflicts with another pin\n"); }
    else { cfg.moisturePin = p; logf("Config    — moisturePin -> %d\n", p); changed = true; }
  }

  if (pendingFields & PF_BATTERY_PIN) {
    int p = atoi(pendingBatteryPin);
    if (p < 0 || p > 10) { logf("Config    — batteryPin rejected: must be 0-10\n"); }
    else if (p == cfg.moisturePin || p == cfg.reedPin) { logf("Config    — batteryPin rejected: conflicts with another pin\n"); }
    else { cfg.batteryPin = p; logf("Config    — batteryPin -> %d\n", p); changed = true; }
  }

  if (pendingFields & PF_REED_PIN) {
    int p = atoi(pendingReedPin);
    if (p < 0 || p > 10) { logf("Config    — reedPin rejected: must be 0-10\n"); }
    else if (p == cfg.moisturePin || p == cfg.batteryPin) { logf("Config    — reedPin rejected: conflicts with another pin\n"); }
    else { cfg.reedPin = p; logf("Config    — reedPin -> %d\n", p); changed = true; }
  }

  if (pendingFields & PF_STATIC_IP) {
    if (strcmp(pendingStaticIP, "true") == 0)       { cfg.staticIP = true;  logf("Config    — staticIP -> true\n");  changed = true; }
    else if (strcmp(pendingStaticIP, "false") == 0) { cfg.staticIP = false; logf("Config    — staticIP -> false\n"); changed = true; }
    else { logf("Config    — staticIP rejected: must be 'true' or 'false'\n"); }
  }

  auto applyIPField = [&](const char* val, uint8_t* bytes, const char* name) {
    IPAddress addr;
    if (addr.fromString(val)) {
      bytes[0] = addr[0]; bytes[1] = addr[1]; bytes[2] = addr[2]; bytes[3] = addr[3];
      logf("Config    — %s -> %s\n", name, val);
      changed = true;
    } else {
      logf("Config    — %s rejected: not a valid IPv4 address\n", name);
    }
  };

  if (pendingFields & PF_IP)  applyIPField(pendingIP,  cfg.ip,  "ip");
  if (pendingFields & PF_GW)  applyIPField(pendingGW,  cfg.gw,  "gw");
  if (pendingFields & PF_SN)  applyIPField(pendingSN,  cfg.sn,  "sn");
  if (pendingFields & PF_DNS) applyIPField(pendingDNS, cfg.dns, "dns");

  if (changed) {
    saveConfig(cfg);
    publishConfigState();
    logf("Config    — all changes saved, restarting\n");
    Serial.flush();
    delay(500);
    ESP.restart();
  } else {
    logf("Config    — pending fields present but none passed validation\n");
  }
}

// Publishes HA MQTT autodiscovery payloads for the 6 remote-configurable
// settings as text and number entities in the device card.
void publishConfigDiscovery() {
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

  char payload[640];

  // Each entity gets its own command_topic (config/set/<field>) so multiple
  // simultaneous changes each get their own retained slot on the broker and
  // aren't overwritten by each other. "retain":true ensures HA publishes the
  // command as retained so sleeping sensors receive it on their next wake.
  // No command_template needed — HA sends the raw field value directly.

  // ── Text: MQTT Broker ────────────────────────────────────
  publishMqttEntity(DISC_CFG_MQTT_BROKER, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"MQTT Broker\",\"unique_id\":\"%s_cfg_mqtt_broker\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.mqttBroker }}\","
      "\"command_topic\":\"%s/mqttBroker\",\"retain\":true,\"max\":63,%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Number: MQTT Port ────────────────────────────────────
  publishMqttEntity(DISC_CFG_MQTT_PORT, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"MQTT Port\",\"unique_id\":\"%s_cfg_mqtt_port\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.mqttPort }}\","
      "\"command_topic\":\"%s/mqttPort\",\"retain\":true,"
      "\"min\":1,\"max\":65535,\"step\":1,\"mode\":\"box\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Text: MQTT Username ──────────────────────────────────
  publishMqttEntity(DISC_CFG_MQTT_USER, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"MQTT Username\",\"unique_id\":\"%s_cfg_mqtt_user\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.mqttUser }}\","
      "\"command_topic\":\"%s/mqttUser\",\"retain\":true,\"max\":31,%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Text: MQTT Password ──────────────────────────────────
  // Note: state_topic publishes "***" for the password — this entity is
  // write-only from HA's perspective; the current value is never displayed.
  publishMqttEntity(DISC_CFG_MQTT_PASS, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"MQTT Password\",\"unique_id\":\"%s_cfg_mqtt_pass\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.mqttPassword }}\","
      "\"command_topic\":\"%s/mqttPassword\",\"retain\":true,\"max\":63,%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Text: Syslog Host ────────────────────────────────────
  publishMqttEntity(DISC_CFG_SYSLOG_HOST, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Syslog Host\",\"unique_id\":\"%s_cfg_syslog_host\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.syslogHost }}\","
      "\"command_topic\":\"%s/syslogHost\",\"retain\":true,\"max\":63,%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Number: Syslog Port ──────────────────────────────────
  publishMqttEntity(DISC_CFG_SYSLOG_PORT, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Syslog Port\",\"unique_id\":\"%s_cfg_syslog_port\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.syslogPort }}\","
      "\"command_topic\":\"%s/syslogPort\",\"retain\":true,"
      "\"min\":1,\"max\":65535,\"step\":1,\"mode\":\"box\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Number: Moisture Pin ─────────────────────────────────
  publishMqttEntity(DISC_CFG_MOISTURE_PIN, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Moisture Pin\",\"unique_id\":\"%s_cfg_moisture_pin\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.moisturePin }}\","
      "\"command_topic\":\"%s/moisturePin\",\"retain\":true,"
      "\"min\":0,\"max\":10,\"step\":1,\"mode\":\"box\","
      "\"icon\":\"mdi:water-percent\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Number: Battery Pin ──────────────────────────────────
  publishMqttEntity(DISC_CFG_BATTERY_PIN, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Battery Pin\",\"unique_id\":\"%s_cfg_battery_pin\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.batteryPin }}\","
      "\"command_topic\":\"%s/batteryPin\",\"retain\":true,"
      "\"min\":0,\"max\":10,\"step\":1,\"mode\":\"box\","
      "\"icon\":\"mdi:battery\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Number: Reed Switch Pin ──────────────────────────────
  publishMqttEntity(DISC_CFG_REED_PIN, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Reed Switch Pin\",\"unique_id\":\"%s_cfg_reed_pin\","
      "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.reedPin }}\","
      "\"command_topic\":\"%s/reedPin\",\"retain\":true,"
      "\"min\":0,\"max\":10,\"step\":1,\"mode\":\"box\","
      "\"icon\":\"mdi:magnet\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Switch: Static IP ────────────────────────────────────
  // payload_on/off are the raw values sent to the per-field subtopic
  publishMqttEntity(DISC_CFG_STATIC_IP, payload, sizeof(payload),
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Static IP\",\"unique_id\":\"%s_cfg_static_ip\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{%% if value_json.staticIP %%}ON{%% else %%}OFF{%% endif %%}\","
      "\"command_topic\":\"%s/staticIP\",\"retain\":true,"
      "\"payload_on\":\"true\",\"payload_off\":\"false\","
      "\"entity_category\":\"config\",%s}",
      SENSOR_ID, CONFIG_STATE_TOPIC, CONFIG_SET_PREFIX, device));

  // ── Text: IP, Gateway, Subnet Mask, DNS ──────────────────
  const struct { const char* name; const char* uid; const char* key; const char* disc; } netFields[] = {
    { "IP Address",  "cfg_ip",  "ip",  DISC_CFG_IP  },
    { "Gateway",     "cfg_gw",  "gw",  DISC_CFG_GW  },
    { "Subnet Mask", "cfg_sn",  "sn",  DISC_CFG_SN  },
    { "DNS Server",  "cfg_dns", "dns", DISC_CFG_DNS },
  };
  for (auto& f : netFields) {
    publishMqttEntity(f.disc, payload, sizeof(payload),
      snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\","
        "\"command_topic\":\"%s/%s\",\"retain\":true,\"max\":15,"
        "\"entity_category\":\"config\",%s}",
        f.name, SENSOR_ID, f.uid,
        CONFIG_STATE_TOPIC, f.key,
        CONFIG_SET_PREFIX, f.key,
        device));
  }

  logf("Discovery — config entities published\n");
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
  // Log if we were woken by the reed switch rather than the timer.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    logf("Wake      — GPIO wakeup (reed switch)\n");
  }
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
  // Set ADC attenuation once here (after config load so we have the right pin).
  // Moved out of readMoisture() so it runs once per wake, not per call.
  analogSetPinAttenuation(cfg.moisturePin, ADC_11db);
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
  publishConfigDiscovery();
  publishConfigState();

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
    applyConfigChange();
  }

  mqtt.disconnect();
  goToSleep();
}

void loop() {
  // Intentionally empty — deep sleep reboots into setup()
}
