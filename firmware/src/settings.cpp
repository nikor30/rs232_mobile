#include "settings.h"
#include "config.h"
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

Settings settings;
static Preferences prefs;
static const char *NS = "rs232";

static String macSuffix() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

__attribute__((unused)) static String randomPassword() {
  // no 0/O, 1/l/I to make reading it off the OLED easy
  const char *alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
  const size_t n = strlen(alphabet);
  String p;
  for (int i = 0; i < 10; i++) p += alphabet[esp_random() % n];
  return p;
}

static const int8_t DEF_PINS[MAX_PORTS][2] = PORT_DEFAULT_PINS;
static const int8_t RX_OK[] = PINS_RX;
static const int8_t TX_OK[] = PINS_TX;
static const int8_t ADC_OK[] = PINS_ADC;

template <size_t N>
static bool inList(const int8_t (&list)[N], int pin) {
  for (size_t i = 0; i < N; i++)
    if (list[i] == pin) return true;
  return false;
}

// NVS keys: port 1 keeps the keys of firmware <= 1.3 ("baud", "bits" ...)
static String key(uint8_t port, const char *k) {
  if (port == 0 && (!strcmp(k, "bd") || !strcmp(k, "bt") || !strcmp(k, "pa") || !strcmp(k, "st") || !strcmp(k, "sw"))) {
    if (!strcmp(k, "bd")) return "baud";
    if (!strcmp(k, "bt")) return "bits";
    if (!strcmp(k, "pa")) return "par";
    if (!strcmp(k, "st")) return "stop";
    return "swap";
  }
  return String("p") + port + k;
}

static void loadSerial(uint8_t i) {
  SerialCfg &c = settings.port[i].serial;
  c.baud   = prefs.getUInt(key(i, "bd").c_str(), DEFAULT_BAUD);
  c.bits   = prefs.getUChar(key(i, "bt").c_str(), 8);
  c.parity = (char)prefs.getUChar(key(i, "pa").c_str(), 'N');
  c.stop   = prefs.getUChar(key(i, "st").c_str(), 1);
  c.swap   = prefs.getBool(key(i, "sw").c_str(), false);
  if (!Store::validSerial(c)) c = SerialCfg();
}

static void putSerial(uint8_t i) {
  const SerialCfg &c = settings.port[i].serial;
  prefs.putUInt(key(i, "bd").c_str(), c.baud);
  prefs.putUChar(key(i, "bt").c_str(), c.bits);
  prefs.putUChar(key(i, "pa").c_str(), (uint8_t)c.parity);
  prefs.putUChar(key(i, "st").c_str(), c.stop);
  prefs.putBool(key(i, "sw").c_str(), c.swap);
}

namespace Store {

bool rxPinOk(int pin) { return inList(RX_OK, pin); }
bool txPinOk(int pin) { return inList(TX_OK, pin); }
bool adcPinOk(int pin) { return inList(ADC_OK, pin); }

String portName(uint8_t port) {
  if (port < MAX_PORTS && settings.port[port].name.length()) return settings.port[port].name;
  return "Port " + String(port + 1);
}

const char *checkPins(const Settings &s) {
  static char err[64];
  int8_t used[2 * MAX_PORTS + 2];
  size_t n = 0;
  if (!s.port[0].enabled) return "Port 1 muss aktiv sein";
  for (uint8_t i = 0; i < MAX_PORTS; i++) {
    const PortCfg &p = s.port[i];
    if (!p.enabled) continue;
    if (!rxPinOk(p.rx)) { snprintf(err, sizeof(err), "Port %u: RX-Pin GPIO%d nicht erlaubt", i + 1, p.rx); return err; }
    if (!txPinOk(p.tx)) { snprintf(err, sizeof(err), "Port %u: TX-Pin GPIO%d nicht erlaubt", i + 1, p.tx); return err; }
    used[n++] = p.rx;
    used[n++] = p.tx;
  }
  if (s.batPin >= 0) {
    if (!adcPinOk(s.batPin)) { snprintf(err, sizeof(err), "Akku-Pin GPIO%d kann nicht messen", s.batPin); return err; }
    used[n++] = s.batPin;
  }
  if (s.batLbo >= 0) {
    if (!rxPinOk(s.batLbo)) { snprintf(err, sizeof(err), "LBO-Pin GPIO%d nicht erlaubt", s.batLbo); return err; }
    used[n++] = s.batLbo;
  }
  for (size_t a = 0; a < n; a++)
    for (size_t b = a + 1; b < n; b++)
      if (used[a] == used[b]) { snprintf(err, sizeof(err), "GPIO%d ist doppelt belegt", used[a]); return err; }
  if (s.batDiv < 10 || s.batDiv > 60) return "Teiler ungültig";
  return nullptr;
}

void load() {
  prefs.begin(NS, false);
  for (uint8_t i = 0; i < MAX_PORTS; i++) {
    PortCfg &p = settings.port[i];
    p.enabled = i == 0 ? true : prefs.getBool(key(i, "en").c_str(), false);
    p.name = prefs.getString(key(i, "nm").c_str(), "");
    p.rx = (int8_t)prefs.getChar(key(i, "rx").c_str(), DEF_PINS[i][0]);
    p.tx = (int8_t)prefs.getChar(key(i, "tx").c_str(), DEF_PINS[i][1]);
    loadSerial(i);
  }

  settings.apSsid   = prefs.getString("apSsid", "");
  settings.apPass   = prefs.getString("apPass", "");
  settings.staSsid  = prefs.getString("staSsid", "");
  settings.staPass  = prefs.getString("staPass", "");
  settings.staAuth  = prefs.getUChar("staAuth", 0);
  settings.staIdentity = prefs.getString("staId", "");
  settings.staUser  = prefs.getString("staUser", "");
  settings.staCaCheck  = prefs.getBool("staCa", true);
  settings.staPhase2   = prefs.getUChar("staP2", 0);
  settings.httpsEnabled = prefs.getBool("https", false);
  settings.httpsLanOnly = prefs.getBool("httpsLan", false);
  settings.httpsCert    = prefs.getUChar("httpsCrt", 0);
  settings.hostname = prefs.getString("host", DEFAULT_HOSTNAME);
  settings.webPass  = prefs.getString("webPass", "");
  settings.oledType = prefs.getUChar("oled", 0);
  settings.oledFlip = prefs.getBool("flip", false);
  settings.displayTimeout = prefs.getUShort("dispTo", 60);
  settings.oledBrightness = prefs.getUChar("oledBr", 255);
  settings.ledBrightness  = prefs.getUChar("led", 12);
  settings.tcpEnabled     = prefs.getBool("tcp", true);
  settings.tcpLan         = prefs.getBool("tcpLan", false);
  settings.apChannel      = prefs.getUChar("chan", AP_CHANNEL_DEFAULT);
  settings.txPower        = (int8_t)prefs.getUChar("txp", TX_POWER_DEFAULT);
  settings.batPin         = (int8_t)prefs.getChar("batPin", BAT_PIN_DEFAULT);
  settings.batLbo         = (int8_t)prefs.getChar("batLbo", -1);
  settings.batType        = prefs.getUChar("batType", BAT_TYPE_DEFAULT);
  settings.batDiv         = prefs.getUChar("batDiv", BAT_DIV_DEFAULT);
  if (settings.staAuth > 3) settings.staAuth = 0;
  if (settings.staPhase2 > 1) settings.staPhase2 = 0;
  if (settings.httpsCert > 1) settings.httpsCert = 0;
  if (settings.apChannel > 13) settings.apChannel = 0;
  if (settings.txPower != 34 && settings.txPower != 44 && settings.txPower != 60 && settings.txPower != 78)
    settings.txPower = TX_POWER_DEFAULT;
  if (settings.batType > 1) settings.batType = BAT_TYPE_DEFAULT;

  if (settings.hostname.isEmpty()) settings.hostname = DEFAULT_HOSTNAME;
  prefs.end();

  // swapped RX/TX needs an RX pin that can also send
  for (uint8_t i = 0; i < MAX_PORTS; i++)
    if (settings.port[i].serial.swap && !txPinOk(settings.port[i].rx)) settings.port[i].serial.swap = false;

  // broken pin setup (e.g. from a different board profile): back to the defaults
  if (checkPins(settings)) {
    for (uint8_t i = 0; i < MAX_PORTS; i++) {
      settings.port[i].enabled = i == 0;
      settings.port[i].rx = DEF_PINS[i][0];
      settings.port[i].tx = DEF_PINS[i][1];
    }
    settings.batPin = BAT_PIN_DEFAULT;
    settings.batDiv = BAT_DIV_DEFAULT;
  }
}

// Call after the radio is running: esp_random() is only a true RNG with RF enabled.
void ensureCredentials() {
  bool dirty = false;
  if (settings.apSsid.isEmpty()) { settings.apSsid = String(AP_SSID_PREFIX) + macSuffix(); dirty = true; }
#ifdef FIXED_AP_PASS
  if (settings.apPass.length() < 8) { settings.apPass = FIXED_AP_PASS; dirty = true; }
#else
  if (settings.apPass.length() < 8) { settings.apPass = randomPassword(); dirty = true; }
#endif
  if (dirty) save();
}

void save() {
  prefs.begin(NS, false);
  for (uint8_t i = 0; i < MAX_PORTS; i++) {
    const PortCfg &p = settings.port[i];
    if (i) prefs.putBool(key(i, "en").c_str(), p.enabled);
    prefs.putString(key(i, "nm").c_str(), p.name);
    prefs.putChar(key(i, "rx").c_str(), p.rx);
    prefs.putChar(key(i, "tx").c_str(), p.tx);
    putSerial(i);
  }
  prefs.putString("apSsid", settings.apSsid);
  prefs.putString("apPass", settings.apPass);
  prefs.putString("staSsid", settings.staSsid);
  prefs.putString("staPass", settings.staPass);
  prefs.putUChar("staAuth", settings.staAuth);
  prefs.putString("staId", settings.staIdentity);
  prefs.putString("staUser", settings.staUser);
  prefs.putBool("staCa", settings.staCaCheck);
  prefs.putUChar("staP2", settings.staPhase2);
  prefs.putBool("https", settings.httpsEnabled);
  prefs.putBool("httpsLan", settings.httpsLanOnly);
  prefs.putUChar("httpsCrt", settings.httpsCert);
  prefs.putString("host", settings.hostname);
  prefs.putString("webPass", settings.webPass);
  prefs.putUChar("oled", settings.oledType);
  prefs.putBool("flip", settings.oledFlip);
  prefs.putUShort("dispTo", settings.displayTimeout);
  prefs.putUChar("oledBr", settings.oledBrightness);
  prefs.putUChar("led", settings.ledBrightness);
  prefs.putBool("tcp", settings.tcpEnabled);
  prefs.putBool("tcpLan", settings.tcpLan);
  prefs.putUChar("chan", settings.apChannel);
  prefs.putUChar("txp", (uint8_t)settings.txPower);
  prefs.putChar("batPin", settings.batPin);
  prefs.putChar("batLbo", settings.batLbo);
  prefs.putUChar("batType", settings.batType);
  prefs.putUChar("batDiv", settings.batDiv);
  prefs.end();
}

void saveSerial(uint8_t port) {
  if (port >= MAX_PORTS) return;
  prefs.begin(NS, false);
  putSerial(port);
  prefs.end();
}

void factoryReset() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
}

bool validSerial(const SerialCfg &c) {
  return c.baud >= 300 && c.baud <= 1000000 &&
         c.bits >= 5 && c.bits <= 8 &&
         (c.parity == 'N' || c.parity == 'E' || c.parity == 'O') &&
         (c.stop == 1 || c.stop == 2);
}

uint32_t serialConfigValue(const SerialCfg &c) {
  // [bits-5][parity N/E/O][stop 1/2]
  static const uint32_t table[4][3][2] = {
    {{SERIAL_5N1, SERIAL_5N2}, {SERIAL_5E1, SERIAL_5E2}, {SERIAL_5O1, SERIAL_5O2}},
    {{SERIAL_6N1, SERIAL_6N2}, {SERIAL_6E1, SERIAL_6E2}, {SERIAL_6O1, SERIAL_6O2}},
    {{SERIAL_7N1, SERIAL_7N2}, {SERIAL_7E1, SERIAL_7E2}, {SERIAL_7O1, SERIAL_7O2}},
    {{SERIAL_8N1, SERIAL_8N2}, {SERIAL_8E1, SERIAL_8E2}, {SERIAL_8O1, SERIAL_8O2}},
  };
  int b = constrain(c.bits, 5, 8) - 5;
  int p = c.parity == 'E' ? 1 : (c.parity == 'O' ? 2 : 0);
  int s = c.stop == 2 ? 1 : 0;
  return table[b][p][s];
}

String serialLabel(const SerialCfg &c) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu %u%c%u", (unsigned long)c.baud, c.bits, c.parity, c.stop);
  return String(buf);
}

}  // namespace Store
