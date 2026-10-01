#include "display.h"
#include "config.h"      // HAS_PANEL must be known before the guard below

#if !HAS_PANEL   // boards with an RGB panel use gui.cpp instead of this OLED code
#include "settings.h"
#include "power.h"
#include "net.h"
#include "serial_bridge.h"

#include <U8g2lib.h>
#include <Wire.h>
#include "qrcode.h"          // esp_qrcode (ESP-IDF component shipped with Arduino-ESP32)

namespace Display {

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oledSsd(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static U8G2_SH1106_128X64_NONAME_F_HW_I2C  oledSh(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static U8G2 *u8 = nullptr;

static bool on = false;
static uint8_t curPage = PAGE_STATUS;
static uint32_t lastActivity = 0;
static uint32_t lastDraw = 0;
static char msg1[22], msg2[22];
static uint32_t msgUntil = 0;

// ------------------------------------------------------------------ helpers
static String fmtBytes(uint32_t b) {
  char buf[12];
  if (b < 10000) snprintf(buf, sizeof(buf), "%lu", (unsigned long)b);
  else if (b < 10000000) snprintf(buf, sizeof(buf), "%luk", (unsigned long)(b / 1000));
  else snprintf(buf, sizeof(buf), "%luM", (unsigned long)(b / 1000000));
  return String(buf);
}

static String fmtUptime(uint32_t s) {
  char buf[16];
  if (s < 3600) snprintf(buf, sizeof(buf), "%lum %02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else snprintf(buf, sizeof(buf), "%luh %02lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  return String(buf);
}

static String clip(const String &s, size_t n) {
  return s.length() <= n ? s : s.substring(0, n - 1) + "~";
}

// WiFi QR payload needs \ ; , : " escaped
static String qrEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

// ------------------------------------------------------------------ QR code
static int qrX = 0, qrY = 0, qrBox = 64;

static void qrRender(esp_qrcode_handle_t q) {
  int size = esp_qrcode_get_size(q);
  int scale = qrBox / (size + 2);
  if (scale < 1) scale = 1;
  int px = size * scale;
  int ox = qrX + (qrBox - px) / 2;
  int oy = qrY + (qrBox - px) / 2;
  u8->setDrawColor(1);
  u8->drawBox(qrX, qrY, qrBox, qrBox);          // lit background = quiet zone
  u8->setDrawColor(0);
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++)
      if (esp_qrcode_get_module(q, x, y)) u8->drawBox(ox + x * scale, oy + y * scale, scale, scale);
  u8->setDrawColor(1);
}

static void drawQr(const String &text, int x, int y, int box) {
  qrX = x; qrY = y; qrBox = box;
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrRender;
  cfg.max_qrcode_version = 6;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, text.c_str());
}

// ------------------------------------------------------------------ widgets
static void drawBattery(int x, int y) {
  u8->setFont(u8g2_font_5x7_tf);
  if (!Power::measured()) return;
  if (!Power::present()) {
    u8->drawUTF8(x + 3, y + 7, "USB");
    return;
  }
  bool blinkOff = Power::low() && (millis() / 500) % 2;
  if (!blinkOff) {
    u8->drawFrame(x, y, 16, 9);
    u8->drawBox(x + 16, y + 2, 2, 5);
    int w = (Power::pct() * 12 + 50) / 100;
    if (w > 0) u8->drawBox(x + 2, y + 2, w, 5);
  }
  char buf[6];
  snprintf(buf, sizeof(buf), "%u%%", Power::pct());
  u8->drawUTF8(x - 2 - u8->getUTF8Width(buf), y + 8, buf);
}

static void drawHeader(const char *title) {
  u8->setFont(u8g2_font_6x10_tf);
  u8->drawUTF8(0, 8, title);
  drawBattery(108, 0);
  u8->drawHLine(0, 11, 128);
}

// ------------------------------------------------------------------ pages
static void pageStatus() {
  drawHeader("RS232");
  u8->setFont(u8g2_font_6x10_tf);
  String l;
  l = "WLAN " + clip(settings.apSsid, 16);
  u8->drawUTF8(0, 21, l.c_str());
  l = "IP   " + Net::apIp();
  u8->drawUTF8(0, 31, l.c_str());

  u8->setFont(u8g2_font_7x13B_tf);
  String ser = Bridge::autobaudRunning(0) ? String("Auto-Baud..") : Store::serialLabel(settings.port[0].serial);
  if (Bridge::enabledCount() > 1) ser = "1:" + ser;
  u8->drawUTF8(0, 44, ser.c_str());
  u8->setFont(u8g2_font_6x10_tf);
  char cl[12];
  snprintf(cl, sizeof(cl), "W%u T%u", Net::webClients(), Net::tcpConnected() ? 1 : 0);
  u8->drawUTF8(128 - u8->getUTF8Width(cl), 43, cl);

  if (settings.staSsid.length()) l = Net::staConnected() ? "LAN  " + Net::staIp() : String("LAN  verbinde...");
  else l = "LAN  aus";
  u8->drawUTF8(0, 53, l.c_str());

  uint32_t now = millis();
  bool rx = false, tx = false;
  uint32_t rxSum = 0, txSum = 0;
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    rx |= now - Bridge::lastRxMs[p] < 150;
    tx |= now - Bridge::lastTxMs[p] < 150;
    rxSum += Bridge::rxBytes[p];
    txSum += Bridge::txBytes[p];
  }
  l = "RX " + fmtBytes(rxSum);
  u8->drawUTF8(8, 63, l.c_str());
  l = "TX " + fmtBytes(txSum);
  u8->drawUTF8(72, 63, l.c_str());
  if (rx) u8->drawBox(0, 56, 5, 6); else u8->drawFrame(0, 56, 5, 6);
  if (tx) u8->drawBox(64, 56, 5, 6); else u8->drawFrame(64, 56, 5, 6);
}

// one line per serial port: activity, number, name, settings
static void pagePorts() {
  drawHeader("Ports");
  u8->setFont(u8g2_font_5x7_tf);
  uint32_t now = millis();
  int y = 21;
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    bool act = now - Bridge::lastRxMs[p] < 150 || now - Bridge::lastTxMs[p] < 150;
    if (act) u8->drawBox(0, y - 6, 5, 6); else u8->drawFrame(0, y - 6, 5, 6);
    String l = String(p + 1) + " " + clip(Store::portName(p), 10);
    u8->drawUTF8(8, y, l.c_str());
    String ser = Bridge::autobaudRunning(p) ? String("Auto-Baud") : Store::serialLabel(settings.port[p].serial);
    u8->drawUTF8(128 - u8->getUTF8Width(ser.c_str()), y, ser.c_str());
    y += 11;
  }
}

static void pageWifiQr() {
  String payload = "WIFI:T:WPA;S:" + qrEscape(settings.apSsid) + ";P:" + qrEscape(settings.apPass) + ";;";
  drawQr(payload, 0, 0, 64);
  u8->setFont(u8g2_font_6x10_tf);
  u8->drawUTF8(68, 9, "WLAN");
  u8->setFont(u8g2_font_5x7_tf);
  u8->drawUTF8(68, 18, "scannen:");
  u8->drawUTF8(68, 30, clip(settings.apSsid, 12).c_str());
  u8->drawUTF8(68, 42, "Passwort:");
  u8->drawUTF8(68, 51, clip(settings.apPass, 12).c_str());
  u8->drawUTF8(68, 63, "Taste=weiter");
}

static void pageUrlQr() {
  String url = "http://" + Net::apIp() + "/";
  drawQr(url, 0, 0, 64);
  u8->setFont(u8g2_font_6x10_tf);
  u8->drawUTF8(68, 9, "Web-UI");
  u8->setFont(u8g2_font_5x7_tf);
  u8->drawUTF8(68, 22, Net::apIp().c_str());
  String mdns = clip(settings.hostname, 6) + ".local";
  u8->drawUTF8(68, 32, mdns.c_str());
  if (Net::staConnected()) {
    u8->drawUTF8(68, 44, "LAN:");
    u8->drawUTF8(68, 53, Net::staIp().c_str());
  }
  u8->drawUTF8(68, 63, "Taste=weiter");
}

static void pageInfo() {
  drawHeader("Info");
  u8->setFont(u8g2_font_5x7_tf);
  String l;
  l = String("FW ") + FW_VERSION + "  up " + fmtUptime(millis() / 1000);
  u8->drawUTF8(0, 21, l.c_str());
  l = "Host " + settings.hostname + ".local";
  u8->drawUTF8(0, 30, clip(l, 25).c_str());
  l = "AP-Clients " + String(Net::apStations()) + "  Web " + String(Net::webClients());
  u8->drawUTF8(0, 39, l.c_str());
  if (Power::present()) l = "Akku " + String(Power::mv()) + " mV  " + String(Power::pct()) + "%";
  else if (Power::measured()) l = "Akku: keiner (USB)";
  else l = "Akku: keine Messung";
  u8->drawUTF8(0, 48, l.c_str());
  if (!settings.tcpEnabled) l = "TCP aus";
  else if (Bridge::enabledCount() > 1) l = "TCP :" + String(RAW_TCP_PORT) + "-" + String(RAW_TCP_PORT + MAX_PORTS - 1);
  else l = "TCP :" + String(RAW_TCP_PORT);
  l += "  RAM " + String(ESP.getFreeHeap() / 1024) + "k";
  u8->drawUTF8(0, 57, l.c_str());
}

static void drawMessage() {
  u8->setFont(u8g2_font_6x10_tf);
  int w1 = u8->getUTF8Width(msg1), w2 = u8->getUTF8Width(msg2);
  int w = max(w1, w2) + 12;
  if (w > 128) w = 128;
  int h = msg2[0] ? 30 : 18;
  int x = (128 - w) / 2, y = (64 - h) / 2;
  u8->setDrawColor(0);
  u8->drawBox(x, y, w, h);
  u8->setDrawColor(1);
  u8->drawFrame(x, y, w, h);
  u8->drawUTF8((128 - w1) / 2, y + 12, msg1);
  if (msg2[0]) u8->drawUTF8((128 - w2) / 2, y + 24, msg2);
}

// ------------------------------------------------------------------ public
bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  uint8_t addr = 0;
  const uint8_t candidates[] = {OLED_ADDR_1, OLED_ADDR_2};
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { addr = a; break; }
  }
  if (!addr) return false;

  u8 = settings.oledType == 1 ? static_cast<U8G2 *>(&oledSh) : static_cast<U8G2 *>(&oledSsd);
  u8->setI2CAddress(addr << 1);
  u8->setBusClock(400000);
  u8->begin();
  if (settings.oledFlip) u8->setDisplayRotation(U8G2_R2);
  u8->setFontMode(1);
  on = true;
  lastActivity = millis();
  applyBrightness();
  return true;
}

bool isOn() { return u8 && on; }
uint8_t page() { return curPage; }

// SSD1306/SH1106 have no backlight - "brightness" is the contrast register (0x81),
// which sets the segment drive current. 0 is still readable in a dark room, 255 is
// the default full drive, so the useful range is mostly at the lower end.
void applyBrightness() {
  if (u8) u8->setContrast(settings.oledBrightness);
}

void wake() {
  lastActivity = millis();
  if (u8 && !on) {
    u8->setPowerSave(0);
    on = true;
    lastDraw = 0;
    applyBrightness();          // some panels reset the contrast when leaving sleep
  }
}

void off() {
  if (u8 && on) {
    u8->setPowerSave(1);
    on = false;
  }
}

void nextPage() {
  curPage = (curPage + 1) % PAGE_COUNT;
  if (curPage == PAGE_PORTS && Bridge::enabledCount() < 2) curPage++;
  lastDraw = 0;
}

void message(const char *line1, const char *line2, uint32_t ms) {
  strlcpy(msg1, line1 ? line1 : "", sizeof(msg1));
  strlcpy(msg2, line2 ? line2 : "", sizeof(msg2));
  // trim leading space of second line ("Auto-Baud: 9600 ...")
  if (msg2[0] == ' ') memmove(msg2, msg2 + 1, strlen(msg2));
  msgUntil = millis() + ms;
  wake();
  lastDraw = 0;
}

void loop() {
  if (!u8) return;
  uint32_t now = millis();
  if (on && settings.displayTimeout && now - lastActivity > settings.displayTimeout * 1000UL &&
      (int32_t)(msgUntil - now) <= 0) {
    off();
  }
  if (!on) return;
  if (now - lastDraw < 250) return;
  lastDraw = now;

  u8->clearBuffer();
  switch (curPage) {
    case PAGE_PORTS:   pagePorts(); break;
    case PAGE_WIFI_QR: pageWifiQr(); break;
    case PAGE_URL_QR:  pageUrlQr(); break;
    case PAGE_INFO:    pageInfo(); break;
    default:           pageStatus(); break;
  }
  if ((int32_t)(msgUntil - now) > 0) drawMessage();
  u8->sendBuffer();
}

}  // namespace Display

#else   // ---------------------------------------------- board has an RGB panel

// The 5" panel board has no small OLED; these keep main.cpp and net.cpp free of
// #if noise. Messages land on the touch GUI via Gui::message().
namespace Display {
bool begin() { return false; }
void loop() {}
bool isOn() { return false; }
void wake() {}
void off() {}
void nextPage() {}
uint8_t page() { return 0; }
void applyBrightness() {}
void message(const char *, const char *, uint32_t) {}
}  // namespace Display

#endif
