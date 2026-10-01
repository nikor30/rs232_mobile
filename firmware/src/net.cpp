#include "net.h"
#include "config.h"
#include "settings.h"
#include "serial_bridge.h"
#include "power.h"
#include "display.h"
#include "gui.h"
#include "status_led.h"
#include "web_assets.h"
#include "captive_dns.h"
#include "xfer.h"
#include "configs.h"
#include "certs.h"
#include "https.h"

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include "compat_eap.h"   // 802.1X names differ between Arduino core 2.x and 3.x
#include <lwip/sockets.h>   // non-blocking send() for the raw TCP path

namespace Net {

// The stock (synchronous) WebServer serves one connection at a time. After
// accepting a socket it waits up to 5 s for the request - browsers (Safari on
// iPhone/iPad in particular) open spare "preconnect" sockets that never carry a
// request, and while the server waits on one of them nobody else is served.
// This drops such an idle socket as soon as another connection is waiting.
class ConsoleWebServer : public WebServer {
 public:
  explicit ConsoleWebServer(int port) : WebServer(port) {}
  void dropIdleClient() {
    if (_currentStatus != HC_WAIT_READ || millis() - _statusChange < 400) return;
    if (_currentClient.available() || !_server.hasClient()) return;
#if DEBUG_NET_LOG
    Serial.printf("[HTTP] %s: leere Verbindung verworfen (nächste wartet)\n",
                  _currentClient.remoteIP().toString().c_str());
#endif
    _currentClient.stop();
    _currentClient = WiFiClient();
    _currentStatus = HC_NONE;
  }
};

static ConsoleWebServer http(HTTP_PORT);
static WebSocketsServer ws(WS_PORT);
static WiFiServer *tcpServer[MAX_PORTS];          // raw TCP 2000 + port index
static WiFiClient tcpClient[MAX_PORTS];

// One port serves two kinds of client and tells them apart by their very first
// byte, because a telnet client always opens with option negotiation (IAC ...):
//   0xFF  -> telnet (PuTTY "Telnet", the `telnet` command). We answer the way a
//            terminal server does - "I echo, I suppress go-ahead" - which is what
//            switches off the client's local echo and its line-at-a-time mode.
//            Without that, every typed line shows up twice or with a blank line.
//   other -> raw (PuTTY "Raw", nc). Not a single byte is touched in either
//            direction, so the pipe stays 8-bit clean for binary traffic.
static const uint8_t T_IAC = 255, T_SE = 240, T_SB = 250,
                     T_WILL = 251, T_WONT = 252, T_DO = 253, T_DONT = 254,
                     T_ECHO = 1, T_SGA = 3;

class TelnetSession {
 public:
  void reset() { mode_ = UNDECIDED; state_ = 0; cmd_ = 0; rspLen_ = 0; greeted_ = false; }
  bool isTelnet() const { return mode_ == TELNET; }

  // client data -> UART: strips negotiation, keeps payload (in place)
  size_t filter(uint8_t *buf, size_t n) {
    if (!n) return 0;
    if (mode_ == UNDECIDED) mode_ = buf[0] == T_IAC ? TELNET : RAW;
    if (mode_ == RAW) return n;                        // byte for byte, untouched
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
      uint8_t b = buf[i];
      switch (state_) {
        case 0:
          if (b == T_IAC) state_ = 1; else buf[out++] = b;
          break;
        case 1:                                        // just saw IAC
          if (b == T_IAC) { buf[out++] = 0xFF; state_ = 0; }   // IAC IAC = literal 0xFF
          else if (b == T_SB) state_ = 2;
          else if (b == T_WILL || b == T_WONT || b == T_DO || b == T_DONT) { cmd_ = b; state_ = 3; }
          else state_ = 0;                             // 1-byte command (NOP, GA, AYT ...)
          break;
        case 2:                                        // inside IAC SB ... IAC SE
          if (b == T_IAC) state_ = 4;
          break;
        case 4:
          state_ = (b == T_SE) ? 0 : 2;
          break;
        default:                                       // state 3: the option byte
          answer(cmd_, b);
          state_ = 0;
          break;
      }
    }
    return out;
  }

  // what we owe the client: the opening offer plus answers to its own requests
  size_t pending(uint8_t *out, size_t max) {
    if (mode_ == TELNET && !greeted_) {
      greeted_ = true;
      put(T_IAC, T_WILL, T_ECHO);                      // "local echo off, I do it"
      put(T_IAC, T_WILL, T_SGA);                       // character at a time
    }
    size_t n = rspLen_ < max ? rspLen_ : max;
    memcpy(out, rsp_, n);
    rspLen_ = 0;
    return n;
  }

 private:
  void put(uint8_t a, uint8_t b, uint8_t c) {
    if (rspLen_ + 3 > sizeof(rsp_)) return;
    rsp_[rspLen_++] = a; rsp_[rspLen_++] = b; rsp_[rspLen_++] = c;
  }
  // decline everything we did not offer ourselves, so the client stops asking
  void answer(uint8_t cmd, uint8_t opt) {
    if (cmd == T_DO) { if (opt != T_ECHO && opt != T_SGA) put(T_IAC, T_WONT, opt); }
    else if (cmd == T_WILL) put(T_IAC, T_DONT, opt);
  }
  enum Mode : uint8_t { UNDECIDED, RAW, TELNET };
  Mode mode_ = UNDECIDED;
  uint8_t state_ = 0, cmd_ = 0;
  bool greeted_ = false;
  uint8_t rsp_[30];
  size_t rspLen_ = 0;
};
static TelnetSession tcpSession[MAX_PORTS];

// Outbound queue per port. WiFiClient::write() blocks for up to 10 s when the
// socket is congested and then silently drops whatever it could not place - on a
// weak link that is exactly "characters go missing". So nothing is handed to the
// socket directly any more: serial data lands here and tcpFlush() moves out only
// as much as lwIP accepts right now, without ever blocking the main loop.
static uint8_t *tcpOut[MAX_PORTS];
static uint16_t tcpOutHead[MAX_PORTS], tcpOutTail[MAX_PORTS];
static uint32_t tcpLostMs[MAX_PORTS];

static size_t tcpOutLen(uint8_t p) {
  return (uint16_t)(tcpOutHead[p] + TCP_OUT_QUEUE - tcpOutTail[p]) % TCP_OUT_QUEUE;
}

static void tcpQueue(uint8_t p, const uint8_t *d, size_t n) {
  if (!tcpOut[p]) return;
  for (size_t i = 0; i < n; i++) {
    uint16_t next = (tcpOutHead[p] + 1) % TCP_OUT_QUEUE;
    if (next == tcpOutTail[p]) {                       // full: say so instead of losing it quietly
      if (millis() - tcpLostMs[p] > 5000) {
        tcpLostMs[p] = millis();
        Serial.printf("[TCP]  Port %u: Sendepuffer voll, %u Byte verworfen\n", p + 1, (unsigned)(n - i));
        onMessage(Bridge::prefix(p) + "TCP-Verbindung zu langsam: " + String((unsigned)(n - i)) + " Byte verworfen");
      }
      return;
    }
    tcpOut[p][tcpOutHead[p]] = d[i];
    tcpOutHead[p] = next;
  }
}

static void tcpFlush(uint8_t p) {
  if (!tcpOut[p] || !tcpConnected(p)) return;
  int fd = tcpClient[p].fd();
  if (fd < 0) return;
  while (size_t len = tcpOutLen(p)) {
    // one contiguous run of the ring at a time
    size_t run = min(len, (size_t)(TCP_OUT_QUEUE - tcpOutTail[p]));
    int w = ::send(fd, tcpOut[p] + tcpOutTail[p], run, MSG_DONTWAIT);
    if (w <= 0) break;                                 // EAGAIN: the rest goes out next round
    tcpOutTail[p] = (tcpOutTail[p] + w) % TCP_OUT_QUEUE;
    if ((size_t)w < run) break;
  }
}

static bool ready[WEBSOCKETS_SERVER_CLIENT_MAX];   // client finished replay -> gets live data
static bool connectedWs[WEBSOCKETS_SERVER_CLIENT_MAX];
static bool tcpWasConnected[MAX_PORTS];
static uint32_t bootId = 0;
static String wsToken;
static uint8_t frameBuf[1 + 2048];                 // [port][data...] for binary WebSocket frames

static const uint8_t XFER_TAG = 0x7F;              // binary frame from the browser = file data

static bool dirty = true;
static uint32_t lastStatusMs = 0;
static uint32_t rebootAt = 0;
static bool otaOk = false;
static uint8_t activeChannel = 6;
static String otaErr;
static uint8_t staReason = 0;                      // last WiFi disconnect reason
static bool staIpChanged = true;                   // HTTPS certificate covers the current address?
static uint8_t *tlsBuf = nullptr;                  // certificate upload
static size_t tlsLen = 0;
static bool tlsTooBig = false;
static const size_t TLS_UPLOAD_MAX = 24 * 1024;

// ---------------------------------------------------------------- helpers
uint8_t webClients() {
  uint8_t n = 0;
  for (auto c : connectedWs) n += c;
  return n;
}
bool tcpConnected(uint8_t p) { return p < MAX_PORTS && tcpClient[p] && tcpClient[p].connected(); }
bool tcpConnected() {
  for (uint8_t p = 0; p < MAX_PORTS; p++)
    if (tcpConnected(p)) return true;
  return false;
}
uint8_t apStations() { return WiFi.softAPgetStationNum(); }
bool staConnected() { return settings.staSsid.length() && WiFi.status() == WL_CONNECTED; }
String apIp() { return WiFi.softAPIP().toString(); }
String staIp() { return staConnected() ? WiFi.localIP().toString() : String(""); }
void markDirty() { dirty = true; }

static const char *authName(uint8_t a) {
  switch (a) {
    case 1: return "802.1X EAP-TLS";
    case 2: return "802.1X PEAP-MSCHAPv2";
    case 3: return "802.1X EAP-TTLS";
    default: return "WPA2/WPA3-PSK";
  }
}

// wifi_err_reason_t -> short German text (the important ones for 802.1X)
static String reasonText(uint8_t r) {
  const char *t;
  switch (r) {
    case 1: t = "nicht angegeben"; break;
    case 2: t = "Authentifizierung abgelaufen"; break;
    case 4: t = "Verbindung abgelaufen"; break;
    case 15: t = "Schlüsselaustausch fehlgeschlagen (Passwort falsch?)"; break;
    case 16: t = "Gruppenschlüssel-Timeout"; break;
    case 23: t = "802.1X abgelehnt (Zertifikat/Identität prüfen)"; break;
    case 24: t = "802.1X: Cipher abgelehnt"; break;
    case 201: t = "Netz nicht gefunden"; break;
    case 202: t = "Anmeldung abgelehnt"; break;
    case 203: t = "Association abgelehnt"; break;
    case 204: t = "Handshake-Timeout"; break;
    case 205: t = "Verbindung verloren"; break;
    default: t = "";
  }
  return String("Grund ") + r + (strlen(t) ? String(": ") + t : String());
}

static bool inApSubnet(const IPAddress &ip) {
  IPAddress ap = WiFi.softAPIP();
  return ip[0] == ap[0] && ip[1] == ap[1] && ip[2] == ap[2];
}

// ---- request log on the USB serial console (diagnostics) ----
static void logReq() {
  static const char *M[] = {"ANY", "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
  int m = (int)http.method();
  Serial.printf("[HTTP] %s %s http://%s%s\n", http.client().remoteIP().toString().c_str(),
                (m >= 0 && m < 8) ? M[m] : "?", http.hostHeader().c_str(), http.uri().c_str());
}

// ---- captive portal ------------------------------------------------------------
// Phones probe e.g. captive.apple.com/hotspot-detect.html or
// connectivitycheck.gstatic.com/generate_204. Our DNS answers every name with the
// AP address; any request for a foreign host name is redirected to the web UI, so
// the phone opens the console page by itself right after joining the hotspot.
static bool isOwnHost(String host) {
  int c = host.indexOf(':');
  if (c >= 0) host = host.substring(0, c);
  host.toLowerCase();
  if (host.isEmpty() || host == apIp()) return true;
  if (staConnected() && host == staIp()) return true;
  String hn = settings.hostname;
  hn.toLowerCase();
  return host == hn || host == hn + ".local";
}

static bool captiveRedirect() {
  if (Https::fromProxy(http.client().remoteIP())) return false;      // came in over HTTPS
  if (isOwnHost(http.hostHeader())) return false;
  String url = String("http://") + http.client().localIP().toString() + "/";
  http.sendHeader("Location", url, true);
  http.sendHeader("Cache-Control", "no-cache, no-store");
  http.send(302, "text/html", "<a href=\"" + url + "\">RS232 Console</a>");
  Serial.printf("[HTTP]   -> captive redirect to %s\n", url.c_str());
  return true;
}

static void serialJson(JsonObject s, const SerialCfg &c) {
  s["baud"] = c.baud;
  s["bits"] = c.bits;
  s["parity"] = String(c.parity);
  s["stop"] = c.stop;
  s["label"] = Store::serialLabel(c);
  s["swap"] = c.swap;
}

static String statusJson() {
  JsonDocument d;
  d["type"] = "status";
  d["fw"] = FW_VERSION;
  d["board"] = BOARD_NAME;
  d["host"] = settings.hostname;
  d["boot"] = bootId;
  JsonArray ps = d["ports"].to<JsonArray>();
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    JsonObject o = ps.add<JsonObject>();
    o["id"] = p;
    o["name"] = Store::portName(p);
    o["hw"] = Bridge::isHardware(p);
    o["maxBaud"] = Bridge::maxBaud(p);
    o["swapOk"] = Bridge::swapOk(p);
    o["rxPin"] = Bridge::rxPin(p);
    o["txPin"] = Bridge::txPin(p);
    serialJson(o["serial"].to<JsonObject>(), settings.port[p].serial);
    o["autobaud"] = Bridge::autobaudRunning(p);
    o["rx"] = Bridge::rxBytes[p];
    o["tx"] = Bridge::txBytes[p];
    o["tcp"] = tcpConnected(p);
    o["tcpPort"] = RAW_TCP_PORT + p;
  }
  d["xfer"] = Xfer::active();
  JsonObject b = d["bat"].to<JsonObject>();
  b["measured"] = Power::measured();
  b["present"] = Power::present();
  b["mv"] = Power::mv();
  b["pct"] = Power::pct();
  b["low"] = Power::low();
  b["type"] = Power::typeName();
  d["clients"] = webClients();
  d["tcp"] = tcpConnected();
  d["tcpEnabled"] = settings.tcpEnabled;
  JsonObject ap = d["ap"].to<JsonObject>();
  ap["ssid"] = settings.apSsid;
  ap["ip"] = apIp();
  ap["stations"] = apStations();
  ap["channel"] = activeChannel;
  ap["txPower"] = settings.txPower;
  JsonObject sta = d["sta"].to<JsonObject>();
  sta["ssid"] = settings.staSsid;
  sta["connected"] = staConnected();
  sta["ip"] = staIp();
  sta["rssi"] = staConnected() ? WiFi.RSSI() : 0;
  sta["auth"] = authName(settings.staAuth);
  if (!staConnected() && staReason) sta["reason"] = reasonText(staReason);
  JsonObject tls = d["https"].to<JsonObject>();
  tls["on"] = settings.httpsEnabled;
  tls["running"] = Https::running();
  tls["port"] = Https::port();
  tls["lanOnly"] = settings.httpsLanOnly;
  tls["sessions"] = Https::sessions();
  tls["cert"] = Https::certName();
  tls["busy"] = Certs::busy();
  if (Https::lastError().length()) tls["err"] = Https::lastError();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  String out;
  serializeJson(d, out);
  return out;
}

static void sendText(uint8_t num, const String &s) {
  String copy = s;
  ws.sendTXT(num, copy);
}

// ---------------------------------------------------------------- HTTP
// plain HTTP from the LAN although HTTPS is required there?
static bool lanPlain() {
  if (!settings.httpsLanOnly || !Https::running()) return false;
  IPAddress ip = http.client().remoteIP();
  return !Https::fromProxy(ip) && !inApSubnet(ip);
}

static bool guard() {
  if (lanPlain()) {                                  // send the browser to HTTPS before anything else
    String host = http.hostHeader();
    int c = host.indexOf(':');
    if (c >= 0) host = host.substring(0, c);
    if (host.isEmpty()) host = staIp();
    String url = "https://" + host + http.uri();
    http.sendHeader("Location", url, true);
    http.sendHeader("Cache-Control", "no-store");
    http.send(301, "text/html", "<a href=\"" + url + "\">HTTPS</a>");
    return false;
  }
  if (settings.webPass.isEmpty()) return true;
  if (http.authenticate("admin", settings.webPass.c_str())) return true;
  http.requestAuthentication(BASIC_AUTH, settings.hostname.c_str());
  return false;
}

static void sendAsset(const WebAsset *a) {
  logReq();
  if (captiveRedirect()) return;
  if (!guard()) return;
  if (http.header("If-None-Match") == a->etag) {
    http.send(304);
    Serial.println("[HTTP]   -> 304 (Browser-Cache)");
    return;
  }
  http.sendHeader("Content-Encoding", "gzip");
  http.sendHeader("Cache-Control", "no-cache");
  http.sendHeader("ETag", a->etag);
  uint32_t t0 = millis();
  http.send_P(200, a->mime, (const char *)a->data, a->len);
  Serial.printf("[HTTP]   -> 200 %u Bytes in %lu ms\n", (unsigned)a->len, (unsigned long)(millis() - t0));
}

static void sendJson(int code, const String &body) {
  http.sendHeader("Cache-Control", "no-store");
  http.send(code, "application/json", body);
}

static void handleGetSettings() {
  if (!guard()) return;
  JsonDocument d;
  d["apSsid"] = settings.apSsid;
  d["staSsid"] = settings.staSsid;
  d["staAuth"] = settings.staAuth;
  d["staIdentity"] = settings.staIdentity;
  d["staUser"] = settings.staUser;
  d["staPassSet"] = !settings.staPass.isEmpty();
  d["staCaCheck"] = settings.staCaCheck;
  d["staPhase2"] = settings.staPhase2;
  d["httpsEnabled"] = settings.httpsEnabled;
  d["httpsLanOnly"] = settings.httpsLanOnly;
  d["httpsCert"] = settings.httpsCert;
  d["hostname"] = settings.hostname;
  d["webPassSet"] = !settings.webPass.isEmpty();
  d["oledType"] = settings.oledType;
  d["oledFlip"] = settings.oledFlip;
  d["displayTimeout"] = settings.displayTimeout;
  d["oledBrightness"] = settings.oledBrightness;
  d["ledBrightness"] = settings.ledBrightness;
  d["tcpEnabled"] = settings.tcpEnabled;
  d["tcpLan"] = settings.tcpLan;
  d["tcpPort"] = RAW_TCP_PORT;
  d["apChannel"] = settings.apChannel;
  d["txPower"] = settings.txPower;
  JsonArray ps = d["ports"].to<JsonArray>();
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    JsonObject o = ps.add<JsonObject>();
    o["enabled"] = settings.port[p].enabled;
    o["name"] = settings.port[p].name;
    o["rx"] = settings.port[p].rx;
    o["tx"] = settings.port[p].tx;
    o["hw"] = p < HW_PORTS;
  }
  static const int8_t RXP[] = PINS_RX, TXP[] = PINS_TX, ADCP[] = PINS_ADC;
  JsonArray a;
  a = d["pinsRx"].to<JsonArray>();
  for (int8_t v : RXP) a.add(v);
  a = d["pinsTx"].to<JsonArray>();
  for (int8_t v : TXP) a.add(v);
  a = d["pinsAdc"].to<JsonArray>();
  for (int8_t v : ADCP) a.add(v);
  d["pinNames"] = PIN_NAMES;
  d["pinPrefix"] = PIN_PREFIX;
  d["hwPorts"] = HW_PORTS;
  d["swMaxBaud"] = SW_MAX_BAUD;
  d["batPin"] = settings.batPin;
  d["batLbo"] = settings.batLbo;
  d["batType"] = settings.batType;
  d["batDiv"] = settings.batDiv;
  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

static bool validHostname(const String &h) {
  if (h.isEmpty() || h.length() > 31) return false;
  for (char c : h)
    if (!(isalnum((unsigned char)c) || c == '-')) return false;
  return true;
}

static void handlePostSettings() {
  if (!guard()) return;
  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) return sendJson(400, "{\"ok\":false,\"error\":\"JSON ungültig\"}");

  Settings n = settings;
  if (d["apSsid"].is<const char *>()) n.apSsid = d["apSsid"].as<String>();
  if (d["apPass"].is<const char *>() && d["apPass"].as<String>().length()) n.apPass = d["apPass"].as<String>();
  if (d["staSsid"].is<const char *>()) n.staSsid = d["staSsid"].as<String>();
  if (d["staPass"].is<const char *>() && d["staPass"].as<String>().length()) n.staPass = d["staPass"].as<String>();
  if (d["staClear"] | false) { n.staSsid = ""; n.staPass = ""; n.staAuth = 0; }
  if (d["staAuth"].is<int>()) n.staAuth = constrain(d["staAuth"].as<int>(), 0, 3);
  if (d["staIdentity"].is<const char *>()) n.staIdentity = d["staIdentity"].as<String>();
  if (d["staUser"].is<const char *>()) n.staUser = d["staUser"].as<String>();
  if (d["staCaCheck"].is<bool>()) n.staCaCheck = d["staCaCheck"];
  if (d["staPhase2"].is<int>()) n.staPhase2 = constrain(d["staPhase2"].as<int>(), 0, 1);
  if (d["httpsEnabled"].is<bool>()) n.httpsEnabled = d["httpsEnabled"];
  if (d["httpsLanOnly"].is<bool>()) n.httpsLanOnly = d["httpsLanOnly"];
  if (d["httpsCert"].is<int>()) n.httpsCert = constrain(d["httpsCert"].as<int>(), 0, 1);
  if (d["hostname"].is<const char *>()) n.hostname = d["hostname"].as<String>();
  if (d["webPass"].is<const char *>() && d["webPass"].as<String>().length()) n.webPass = d["webPass"].as<String>();
  if (d["webPassClear"] | false) n.webPass = "";
  if (d["oledType"].is<int>()) n.oledType = constrain(d["oledType"].as<int>(), 0, 1);
  if (d["oledFlip"].is<bool>()) n.oledFlip = d["oledFlip"];
  if (d["displayTimeout"].is<int>()) n.displayTimeout = constrain(d["displayTimeout"].as<int>(), 0, 3600);
  if (d["oledBrightness"].is<int>()) n.oledBrightness = constrain(d["oledBrightness"].as<int>(), 0, 255);
  if (d["ledBrightness"].is<int>()) n.ledBrightness = constrain(d["ledBrightness"].as<int>(), 0, 255);
  if (d["tcpEnabled"].is<bool>()) n.tcpEnabled = d["tcpEnabled"];
  if (d["tcpLan"].is<bool>()) n.tcpLan = d["tcpLan"];
  if (d["apChannel"].is<int>()) n.apChannel = constrain(d["apChannel"].as<int>(), 0, 13);
  if (d["txPower"].is<int>()) {
    int p = d["txPower"].as<int>();
    if (p == 34 || p == 44 || p == 60 || p == 78) n.txPower = p;
  }
  if (d["ports"].is<JsonArrayConst>()) {
    JsonArrayConst ps = d["ports"].as<JsonArrayConst>();
    for (uint8_t p = 0; p < MAX_PORTS && p < ps.size(); p++) {
      JsonObjectConst o = ps[p];
      if (o["enabled"].is<bool>()) n.port[p].enabled = p == 0 ? true : o["enabled"].as<bool>();
      if (o["name"].is<const char *>()) {
        String nm = o["name"].as<String>();
        nm.trim();
        n.port[p].name = nm.substring(0, 24);
      }
      if (o["rx"].is<int>()) n.port[p].rx = o["rx"].as<int>();
      if (o["tx"].is<int>()) n.port[p].tx = o["tx"].as<int>();
      // software UART: keep the baud rate in its range
      if (p >= HW_PORTS && n.port[p].serial.baud > SW_MAX_BAUD) n.port[p].serial.baud = DEFAULT_BAUD;
    }
  }
  if (d["batPin"].is<int>()) n.batPin = d["batPin"].as<int>();
  if (d["batLbo"].is<int>()) n.batLbo = d["batLbo"].as<int>();
  if (d["batType"].is<int>()) n.batType = constrain(d["batType"].as<int>(), 0, 1);
  if (d["batDiv"].is<int>()) n.batDiv = d["batDiv"].as<int>();

  const char *err = nullptr;
  if (n.apSsid.isEmpty() || n.apSsid.length() > 32) err = "AP-SSID: 1-32 Zeichen";
  else if (n.apPass.length() < 8 || n.apPass.length() > 63) err = "AP-Passwort: 8-63 Zeichen";
  else if (n.staSsid.length() > 32) err = "WLAN-SSID zu lang";
  else if (n.staAuth == 0 && n.staPass.length() > 63) err = "WLAN-Passwort zu lang";
  else if (n.staAuth == 0 && n.staPass.length() && n.staPass.length() < 8) err = "WLAN-Passwort: 8-63 Zeichen";
  else if (n.staIdentity.length() > 64 || n.staUser.length() > 64) err = "Identität/Benutzer: max. 64 Zeichen";
  else if (n.staAuth > 1 && n.staUser.isEmpty() && n.staSsid.length()) err = "Benutzername fehlt (PEAP/TTLS)";
  else if (n.staAuth > 1 && n.staPass.isEmpty() && n.staSsid.length()) err = "Passwort fehlt (PEAP/TTLS)";
  else if (n.staAuth == 1 && n.staSsid.length() && !Certs::ready(Certs::CLIENT))
    err = "EAP-TLS: erst ein Client-Zertifikat mit Schlüssel hochladen (Tab Netz)";
  else if (!validHostname(n.hostname)) err = "Hostname: a-z, 0-9, '-' (max. 31)";
  else err = Store::checkPins(n);
  if (err) return sendJson(400, String("{\"ok\":false,\"error\":\"") + err + "\"}");

  settings = n;
  Store::save();
  sendJson(200, "{\"ok\":true,\"reboot\":true}");
  Display::message("Einstellungen", "Neustart ...", 3000);
  rebootAt = millis() + 1500;
}

static void handleOtaDone() {
  if (!guard()) return;
  bool ok = otaOk && !Update.hasError();
  if (ok) {
    sendJson(200, "{\"ok\":true}");
    Display::message("Firmware OK", "Neustart ...", 3000);
    rebootAt = millis() + 1500;
  } else {
    sendJson(500, String("{\"ok\":false,\"error\":\"") + (otaErr.length() ? otaErr : String("Update fehlgeschlagen")) + "\"}");
    Display::message("Update", "fehlgeschlagen", 4000);
  }
}

static void handleOtaUpload() {
  if (!settings.webPass.isEmpty() && !http.authenticate("admin", settings.webPass.c_str())) return;
  HTTPUpload &up = http.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaOk = false;
    otaErr = "";
    Display::message("Firmware", "wird geschrieben", 60000);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaErr.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaErr.isEmpty()) {
      if (Update.end(true)) otaOk = true;
      else otaErr = Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaErr = "Upload abgebrochen";
  }
}

// ---- stored configurations (tab "Konfig") ----
static void handleConfigs() {
  if (!guard()) return;
  sendJson(200, Configs::listJson());
}

static void handleConfigGet() {
  if (!guard()) return;
  String text;
  if (!Configs::read(http.arg("name"), text)) return sendJson(404, "{\"ok\":false,\"error\":\"nicht gefunden\"}");
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "text/plain; charset=utf-8", text);
}

// POST /api/config?name=<name>&old=<previous name>, body = the text (no JSON: saves RAM)
static void handleConfigSave() {
  if (!guard()) return;
  // too little RAM for the body -> "plain" arrives empty/short: never store that
  if ((int)http.arg("plain").length() != http.clientContentLength())
    return sendJson(400, "{\"ok\":false,\"error\":\"Text nicht vollständig angekommen (zu wenig Speicher?)\"}");
  const char *err = Configs::save(http.arg("name"), http.arg("plain"), http.arg("old"));
  if (err) return sendJson(400, String("{\"ok\":false,\"error\":\"") + err + "\"}");
  sendJson(200, "{\"ok\":true}");
}

static void handleConfigDelete() {
  if (!guard()) return;
  JsonDocument d;
  deserializeJson(d, http.arg("plain"));
  bool ok = Configs::remove(d["name"] | "");
  sendJson(ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"nicht gefunden\"}");
}

// ---- certificates (tab "Netz") ----
static Certs::Slot slotFromName(const String &n) {
  if (n == "ca") return Certs::CA;
  if (n == "https") return Certs::HTTPS;
  return Certs::CLIENT;
}

static void freeTlsBuf() {
  if (tlsBuf) {
    memset(tlsBuf, 0, TLS_UPLOAD_MAX);         // key material does not stay in the heap
    free(tlsBuf);
    tlsBuf = nullptr;
  }
  tlsLen = 0;
  tlsTooBig = false;
}

static void handleTlsUpload() {
  if (!settings.webPass.isEmpty() && !http.authenticate("admin", settings.webPass.c_str())) return;
  if (lanPlain()) return;
  HTTPUpload &up = http.upload();
  if (up.status == UPLOAD_FILE_START) {
    freeTlsBuf();
    tlsBuf = (uint8_t *)malloc(TLS_UPLOAD_MAX);
    tlsTooBig = !tlsBuf;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    freeTlsBuf();                              // cancelled upload: never keep the buffer
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (tlsBuf && tlsLen + up.currentSize <= TLS_UPLOAD_MAX) {
      memcpy(tlsBuf + tlsLen, up.buf, up.currentSize);
      tlsLen += up.currentSize;
    } else {
      tlsTooBig = true;
    }
  }
}

static void handleTlsUploadDone() {
  if (!guard()) {
    freeTlsBuf();
    return;
  }
  String msg;
  bool ok = false;
  Certs::Slot slot = slotFromName(http.arg("slot"));
  if (!tlsBuf || tlsTooBig) {
    msg = tlsBuf ? "Datei zu groß (max. 24 kB)" : "Zu wenig Speicher";
  } else {
    ok = Certs::upload(slot, tlsBuf, tlsLen, http.arg("pass"), msg);
  }
  freeTlsBuf();
  // the WLAN supplicant keeps the certificates it got at boot
  bool needReboot = ok && settings.staAuth > 0 && settings.staSsid.length() &&
                    (slot == Certs::CLIENT || slot == Certs::CA);
  if (needReboot) msg += ". Neustart nötig, damit sich das WLAN damit anmeldet";
  JsonDocument d;
  d["ok"] = ok;
  d["reboot"] = needReboot;
  d[ok ? "msg" : "error"] = msg;
  String out;
  serializeJson(d, out);
  sendJson(ok ? 200 : 400, out);
  dirty = true;
}

static void handleTlsDownload() {
  if (!guard()) return;
  String what = http.arg("what");
  String pem, name;
  if (what == "csr") { pem = Certs::csrPem(); name = "antrag.csr"; }
  else if (what == "devca") { pem = Certs::deviceCaPem(); name = "geraete-ca.crt"; }
  else {
    Certs::Slot s = slotFromName(what);
    String key;
    Certs::copy(s, pem, key);                        // certificates only, never a private key
    name = what + ".crt";
  }
  if (pem.isEmpty()) return sendJson(404, "{\"ok\":false,\"error\":\"nicht vorhanden\"}");
  http.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "application/x-pem-file", pem);
}

static void setupHttp() {
  static const char *hdrs[] = {"If-None-Match"};
  http.collectHeaders(hdrs, 1);
  http.enableDelay(false);                     // the main loop decides when to pause

  for (size_t i = 0; i < WEB_ASSETS_COUNT; i++) {
    const WebAsset *a = &WEB_ASSETS[i];
    http.on(a->path, HTTP_GET, [a]() { sendAsset(a); });
  }
  http.on("/index.html", HTTP_GET, []() { sendAsset(&WEB_ASSETS[0]); });

  http.on("/api/status", HTTP_GET, []() {
    if (guard()) sendJson(200, statusJson());
  });
  http.on("/api/session", HTTP_GET, []() {
    logReq();
    if (!guard()) return;
    sendJson(200, String("{\"token\":\"") + wsToken + "\",\"wsPort\":" + WS_PORT + ",\"boot\":" + bootId + "}");
  });
  // plain-text reachability check (no JavaScript, no gzip): http://192.168.4.1/ping
  http.on("/ping", HTTP_GET, []() {
    logReq();
    http.send(200, "text/plain", String("OK ") + FW_NAME + " v" + FW_VERSION + " (" + BOARD_NAME + "), up " +
                                     String(millis() / 1000) + " s, heap " + String(ESP.getFreeHeap()) +
                                     ", dns " + String(CaptiveDns::queries()) + "\n");
  });
  http.on("/api/configs", HTTP_GET, handleConfigs);
  http.on("/api/config", HTTP_GET, handleConfigGet);
  http.on("/api/config", HTTP_POST, handleConfigSave);
  http.on("/api/config/delete", HTTP_POST, handleConfigDelete);
  http.on("/api/tls", HTTP_GET, []() {
    if (guard()) sendJson(200, Certs::infoJson());
  });
  http.on("/api/tls/upload", HTTP_POST, handleTlsUploadDone, handleTlsUpload);
  http.on("/api/tls/download", HTTP_GET, handleTlsDownload);
  http.on("/api/tls/delete", HTTP_POST, []() {
    if (!guard()) return;
    JsonDocument d;
    deserializeJson(d, http.arg("plain"));
    Certs::remove(slotFromName(d["slot"] | "client"));
    sendJson(200, "{\"ok\":true}");
    dirty = true;
  });
  http.on("/api/tls/csr", HTTP_POST, []() {
    if (!guard()) return;
    JsonDocument d;
    if (deserializeJson(d, http.arg("plain"))) return sendJson(400, "{\"ok\":false,\"error\":\"JSON ungültig\"}");
    String err;
    bool ok = Certs::csrStart(d["subject"] | "", d["san"] | "", d["rsa"] | true, err);
    sendJson(ok ? 200 : 400, ok ? "{\"ok\":true}" : String("{\"ok\":false,\"error\":\"") + err + "\"}");
  });
  http.on("/api/settings", HTTP_GET, handleGetSettings);
  http.on("/api/settings", HTTP_POST, handlePostSettings);
  http.on("/api/reboot", HTTP_POST, []() {
    if (!guard()) return;
    sendJson(200, "{\"ok\":true}");
    rebootAt = millis() + 1000;
  });
  http.on("/api/factory", HTTP_POST, []() {
    if (!guard()) return;
    Store::factoryReset();
    Certs::wipe();
    sendJson(200, "{\"ok\":true}");
    Display::message("Werksreset", "Neustart ...", 3000);
    rebootAt = millis() + 1000;
  });
  http.on("/update", HTTP_POST, handleOtaDone, handleOtaUpload);
  http.onNotFound([]() {
    logReq();
    if (captiveRedirect()) return;
    http.send(404, "text/plain", "404");
  });
  http.begin();
}

// ---------------------------------------------------------------- WebSocket
// Binary frames: [port][bytes...] in both directions ([0x7F][bytes] = file data
// for a running XMODEM/YMODEM upload). Text frames: JSON commands / events.
static void sendBin(uint8_t num, uint8_t port, const uint8_t *data, size_t len) {
  frameBuf[0] = port;
  memcpy(frameBuf + 1, data, len);
  ws.sendBIN(num, frameBuf, len + 1);
}

static void replayPort(uint8_t num, uint8_t port, uint32_t clientSeq, bool fresh) {
  uint32_t now = Bridge::seqNow(port);
  uint32_t start = Bridge::ringStartSeq(port);
  uint32_t from = start, lost = 0;
  if (!fresh) {
    if (clientSeq > now) from = start;                        // should not happen
    else if (clientSeq < start) { lost = start - clientSeq; from = start; }
    else from = clientSeq;
  }
  JsonDocument d;
  d["type"] = "sync";
  d["port"] = port;
  d["seq"] = from;
  d["lost"] = lost;
  d["boot"] = bootId;
  String s;
  serializeJson(d, s);
  ws.sendTXT(num, s);
  while (from < now) {
    size_t n = Bridge::copyFrom(port, from, frameBuf + 1, sizeof(frameBuf) - 1);
    if (!n) break;
    frameBuf[0] = port;
    if (!ws.sendBIN(num, frameBuf, n + 1)) break;
    from += n;
  }
}

static void replay(uint8_t num, JsonVariantConst seqs, uint32_t clientBoot) {
  bool reboot = clientBoot != 0 && clientBoot != bootId;
  bool fresh = clientBoot == 0 || reboot;
  sendText(num, statusJson());                             // client learns the ports first
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    replayPort(num, p, seqs[p] | 0u, fresh);
  }
  JsonDocument d;
  d["type"] = "synced";
  d["reboot"] = reboot;
  d["boot"] = bootId;
  String s;
  serializeJson(d, s);
  ws.sendTXT(num, s);
  ready[num] = true;
  if (Xfer::active()) sendText(num, Xfer::statusJson());
}

static void handleCommand(uint8_t num, uint8_t *payload, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char *cmd = d["cmd"] | "";
  uint8_t port = d["port"] | 0;
  if (port >= MAX_PORTS) return;

  if (!strcmp(cmd, "hello")) {
    if (d["time"].is<uint32_t>()) Certs::setTime(d["time"]);
    replay(num, d["seq"], d["boot"] | 0u);
  } else if (!strcmp(cmd, "break")) {
    int ms = constrain((int)(d["ms"] | 300), 50, 2000);
    Led::flash(255, 0, 255, 300);
    Bridge::sendBreak(port, ms);
  } else if (!strcmp(cmd, "serial")) {
    SerialCfg c = settings.port[port].serial;
    c.baud = d["baud"] | c.baud;
    c.bits = d["bits"] | c.bits;
    const char *p = d["parity"] | "N";
    c.parity = toupper(p[0]);
    c.stop = d["stop"] | c.stop;
    c.swap = d["swap"] | c.swap;
    if (Store::validSerial(c)) Bridge::apply(port, c);
    else sendText(num, "{\"type\":\"msg\",\"text\":\"Ungültige serielle Einstellung\"}");
    dirty = true;
  } else if (!strcmp(cmd, "autobaud")) {
    Bridge::startAutobaud(port);
    dirty = true;
  } else if (!strcmp(cmd, "status")) {
    sendText(num, statusJson());
  } else if (!strcmp(cmd, "oledBrightness")) {
    // live preview while the slider moves: applied at once, stored only on "Speichern"
    settings.oledBrightness = constrain((int)(d["value"] | 255), 0, 255);
    Display::wake();
    Display::applyBrightness();
  } else if (!strcmp(cmd, "xfer")) {
    String err;
    const char *pr = d["proto"] | "x1k";
    Xfer::Proto proto = !strcmp(pr, "y") ? Xfer::YMODEM : (!strcmp(pr, "x") ? Xfer::XMODEM : Xfer::XMODEM_1K);
    if (!Xfer::start(port, proto, d["name"] | "datei.bin", d["size"] | 0u, num, err)) {
      JsonDocument e;
      e["type"] = "xfer";
      e["state"] = "error";
      e["msg"] = err;
      String s;
      serializeJson(e, s);
      sendText(num, s);
    }
    dirty = true;
  } else if (!strcmp(cmd, "xferAbort")) {
    Xfer::abort("abgebrochen");
  } else if (!strcmp(cmd, "xferResume")) {
    if (!Xfer::resume(num, d["id"] | 0u)) sendText(num, "{\"type\":\"xfer\",\"state\":\"idle\"}");
  }
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      // URL must carry the session token: ws://host:81/?t=<token>
      String url = String((const char *)payload);
      if (url.indexOf(wsToken) < 0) {
        ws.disconnect(num);
        return;
      }
      IPAddress rip = ws.remoteIP(num);
      if (settings.httpsLanOnly && Https::running() && !Https::fromProxy(rip) && !inApSubnet(rip)) {
        ws.disconnect(num);                          // plain WebSocket from the LAN: not allowed
        return;
      }
      connectedWs[num] = true;
      ready[num] = false;
      dirty = true;
      Display::wake();
      Serial.printf("[WS]   client %u connected (%s)\n", num, ws.remoteIP(num).toString().c_str());
      break;
    }
    case WStype_DISCONNECTED:
      if (connectedWs[num]) Serial.printf("[WS]   client %u disconnected\n", num);
      connectedWs[num] = false;
      ready[num] = false;
      Xfer::clientGone(num);
      dirty = true;
      break;
    case WStype_BIN:
      if (!connectedWs[num] || len < 2) break;
      if (payload[0] == XFER_TAG) Xfer::feed(num, payload + 1, len - 1);
      else if (payload[0] < MAX_PORTS) Bridge::write(payload[0], payload + 1, len - 1);
      break;
    case WStype_TEXT:
      if (connectedWs[num]) handleCommand(num, payload, len);
      break;
    default:
      break;
  }
}

static void xferNotify(const String &json) {
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
    if (ready[i]) {
      String copy = json;
      ws.sendTXT(i, copy);
    }
}

// ---------------------------------------------------------------- raw TCP
static void tcpLoop() {
  if (!settings.tcpEnabled) return;
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!tcpServer[p]) continue;
    if (tcpServer[p]->hasClient()) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      WiFiClient c = tcpServer[p]->accept();      // available() is the old name
#else
      WiFiClient c = tcpServer[p]->available();
#endif
      // Raw TCP has no authentication of its own. While the device also sits in a
      // LAN (station mode), clients from outside the hotspot are refused unless
      // "tcpLan" was switched on deliberately (Setup -> Raw-TCP aus dem LAN).
      if (staConnected() && !settings.tcpLan && !inApSubnet(c.remoteIP())) {
        Serial.printf("[TCP]  Port %u: %s abgelehnt (nur aus dem Hotspot-Netz %s erlaubt - "
                      "für LAN-Zugriff: Menü -> Setup -> \"auch aus dem LAN\")\n",
                      p + 1, c.remoteIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
        c.stop();
      } else {
        Serial.printf("[TCP]  Port %u: %s verbunden (:%u)\n", p + 1,
                      c.remoteIP().toString().c_str(), RAW_TCP_PORT + p);
        if (tcpConnected(p)) tcpClient[p].stop();      // newest connection wins
        tcpClient[p] = c;
        tcpClient[p].setNoDelay(true);
        tcpSession[p].reset();
        if (!tcpOut[p]) tcpOut[p] = (uint8_t *)malloc(TCP_OUT_QUEUE);
        tcpOutHead[p] = tcpOutTail[p] = 0;
        dirty = true;
        Display::wake();
      }
    }
    bool conn = tcpConnected(p);
    if (conn) {
      // take only what fits into the port's send queue: TCP flow control does the rest
      uint8_t buf[256];
      int avail;
      while ((avail = tcpClient[p].available()) > 0) {
        int room = min((int)Bridge::txFree(p), (int)sizeof(buf));
        if (room <= 0) break;
        int n = tcpClient[p].read(buf, min(avail, room));
        if (n <= 0) break;
        size_t m = tcpSession[p].filter(buf, n);       // telnet: strip negotiation, raw: untouched
        if (m) Bridge::write(p, buf, m);
      }
      uint8_t neg[32];
      size_t nn = tcpSession[p].pending(neg, sizeof(neg));    // our own telnet answers
      if (nn) tcpQueue(p, neg, nn);
      tcpFlush(p);
    }
    if (conn != tcpWasConnected[p]) {
      if (!conn) {
        Serial.printf("[TCP]  Port %u: Verbindung beendet\n", p + 1);
        tcpOutHead[p] = tcpOutTail[p] = 0;
      }
      tcpWasConnected[p] = conn;
      dirty = true;
    }
  }
}

// ---------------------------------------------------------------- public
void onSerialData(uint8_t port, const uint8_t *data, size_t len) {
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
    if (ready[i]) sendBin(i, port, data, len);
  if (tcpConnected(port)) { tcpQueue(port, data, len); tcpFlush(port); }
}

void onMessage(const String &msg) {
  JsonDocument d;
  d["type"] = "msg";
  d["text"] = msg;
  String s;
  serializeJson(d, s);
  ws.broadcastTXT(s);
  Gui::message(msg);
  int sp = msg.indexOf(':');
  if (sp > 0) Display::message(msg.substring(0, sp).c_str(), msg.substring(sp + 1).c_str(), 2500);
  else Display::message(msg.c_str(), "", 2500);
  dirty = true;
}

// Quietest of the non-overlapping 2.4 GHz channels 1/6/11 (weighted by signal strength)
static uint8_t pickChannel() {
  int n = WiFi.scanNetworks(false, true);
  long score[3] = {0, 0, 0};
  const uint8_t cand[3] = {1, 6, 11};
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    long w = max(0L, 100L + (long)WiFi.RSSI(i));      // -40 dBm -> 60, -90 dBm -> 10
    for (int c = 0; c < 3; c++) {
      int d = abs(ch - cand[c]);
      if (d <= 2) score[c] += w;
      else if (d <= 4) score[c] += w / 2;
    }
  }
  WiFi.scanDelete();
  int best = 1;                                        // prefer 6 on a tie
  for (int c = 0; c < 3; c++) if (score[c] < score[best]) best = c;
  Serial.printf("[WLAN] %d Netze gefunden, Belegung Kanal 1/6/11: %ld/%ld/%ld -> Kanal %u\n",
                n, score[0], score[1], score[2], cand[best]);
  return cand[best];
}

// WLAN client: WPA2/WPA3-PSK or 802.1X (EAP-TLS with certificate, PEAP or TTLS).
// The PEM buffers are handed to the supplicant including the terminating NUL
// (mbedtls only recognises PEM that way) and must stay alive while connected.
static void startSta() {
  if (settings.staAuth == 0) {
    WiFi.begin(settings.staSsid.c_str(), settings.staPass.c_str());
    return;
  }
  static String caPem, crtPem, keyPem, ident;
  esp_eap_client_clear_ca_cert();
  esp_eap_client_clear_certificate_and_key();
  esp_eap_client_clear_identity();
  esp_eap_client_clear_username();
  esp_eap_client_clear_password();
  esp_eap_client_set_disable_time_check(true);     // the board has no clock
  String unused;
  if (settings.staCaCheck && Certs::hasCert(Certs::CA)) {
    Certs::copy(Certs::CA, caPem, unused);
    esp_eap_client_set_ca_cert((const uint8_t *)caPem.c_str(), caPem.length() + 1);
  } else {
    Serial.println("[WLAN] 802.1X ohne Prüfung des RADIUS-Zertifikats (kein CA-Zertifikat)");
  }
  ident = settings.staIdentity;
  if (settings.staAuth == 1) {
    if (!Certs::ready(Certs::CLIENT)) {
      Serial.println("[WLAN] EAP-TLS: Client-Zertifikat fehlt - keine Anmeldung");
      return;
    }
    Certs::copy(Certs::CLIENT, crtPem, keyPem);
    esp_eap_client_set_certificate_and_key((const uint8_t *)crtPem.c_str(), crtPem.length() + 1,
                                       (const uint8_t *)keyPem.c_str(), keyPem.length() + 1, nullptr, 0);
    if (ident.isEmpty()) ident = Certs::subjectCN(Certs::CLIENT);
    if (ident.length() > 64) ident = ident.substring(0, 64);      // the supplicant rejects longer ones
  } else {
    if (settings.staAuth == 3)
      esp_eap_client_set_ttls_phase2_method(settings.staPhase2 ? ESP_EAP_TTLS_PHASE2_PAP
                                                                      : ESP_EAP_TTLS_PHASE2_MSCHAPV2);
    esp_eap_client_set_username((const uint8_t *)settings.staUser.c_str(), settings.staUser.length());
    esp_eap_client_set_password((const uint8_t *)settings.staPass.c_str(), settings.staPass.length());
    if (ident.isEmpty()) ident = settings.staUser;
  }
  if (ident.length()) esp_eap_client_set_identity((const uint8_t *)ident.c_str(), ident.length());
  esp_err_t e = esp_wifi_sta_enterprise_enable();
  Serial.printf("[WLAN] %s als \"%s\"%s\n", authName(settings.staAuth), ident.c_str(),
                e == ESP_OK ? "" : " - Fehler beim Aktivieren");
  WiFi.begin(settings.staSsid.c_str());
}

void prepareRadio() {
  WiFi.persistent(false);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.mode(WIFI_STA);                                 // radio on: true RNG + scan
  Store::ensureCredentials();
  activeChannel = settings.apChannel ? settings.apChannel : pickChannel();
  WiFi.mode(WIFI_OFF);                                 // clean restart of the driver as AP
  delay(50);
}

void begin() {
  bootId = esp_random() | 1;
  char tok[17];
  snprintf(tok, sizeof(tok), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  wsToken = tok;

  bool sta = settings.staSsid.length() > 0;
  WiFi.persistent(false);
  WiFi.setHostname(settings.hostname.c_str());     // must be set before STA starts
  WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);    // 20 MHz: robust next to office WLANs
  WiFi.softAP(settings.apSsid.c_str(), settings.apPass.c_str(), activeChannel, 0, AP_MAX_CLIENTS);
  WiFi.setSleep(false);                                // no modem sleep: lowest latency
  WiFi.setTxPower((wifi_power_t)settings.txPower);
  Serial.printf("[WLAN] Hotspot %s auf Kanal %u, Sendeleistung %.1f dBm\n", settings.apSsid.c_str(),
                activeChannel, settings.txPower / 4.0f);
  if (sta) {
    WiFi.setAutoReconnect(true);
    startSta();
  }

  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED) Serial.println("[WLAN] Client verbunden");
    else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println("[WLAN] Client getrennt");
    else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
      Serial.printf("[WLAN] Client hat IP %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
    else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[WLAN] LAN-IP %s\n", WiFi.localIP().toString().c_str());
      staReason = 0;
      staIpChanged = true;
      dirty = true;
    } else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      staReason = info.wifi_sta_disconnected.reason;
      Serial.printf("[WLAN] getrennt: %s\n", reasonText(staReason).c_str());
      dirty = true;
    }
  });

  CaptiveDns::begin(WiFi.softAPIP());         // captive portal: every name -> 192.168.4.1

  if (MDNS.begin(settings.hostname.c_str())) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    if (settings.httpsEnabled) MDNS.addService("https", "tcp", 443);
  }

  setupHttp();
  Https::begin();
  ws.begin();
  ws.onEvent(wsEvent);
  ws.enableHeartbeat(15000, 5000, 2);   // drop dead phone connections
  Xfer::begin(xferNotify);
  if (settings.tcpEnabled) {
    for (uint8_t p = 0; p < MAX_PORTS; p++) {
      if (!Bridge::enabled(p)) continue;
      tcpServer[p] = new WiFiServer(RAW_TCP_PORT + p);
      tcpServer[p]->begin();
      tcpServer[p]->setNoDelay(true);
    }
  }
}

void loop() {
  Certs::loop();
  // HTTPS with a certificate of the device itself: keep it valid for the current addresses
  if (settings.httpsEnabled && staIpChanged && !Certs::busy()) {
    staIpChanged = false;
    bool ownCert = settings.httpsCert == 1 && Certs::serverUsable(Certs::CLIENT);
    if (!ownCert) Certs::ensureAutoHttps(settings.hostname, settings.apSsid, staIp());
  }
  CaptiveDns::loop();
  http.dropIdleClient();
  http.handleClient();
  ws.loop();
  tcpLoop();

  uint32_t now = millis();
  if (webClients() && (now - lastStatusMs > 3000 || (dirty && now - lastStatusMs > 250))) {
    lastStatusMs = now;
    dirty = false;
    String s = statusJson();
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
      if (ready[i]) ws.sendTXT(i, s);
  }
  if (rebootAt && (int32_t)(now - rebootAt) > 0) {
    ws.disconnect();
    delay(100);
    ESP.restart();
  }
}

}  // namespace Net
