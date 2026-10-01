#include "display.h"
#include "config.h"

// Colour touch front end for boards with a small SPI LCD (Waveshare
// ESP32-S3-Touch-LCD-2: ST7789T3 240x320, CST816D touch, QMI8658 accelerometer).
// It implements the same Display interface as the OLED code in display.cpp, so
// the rest of the firmware does not care which of the two is built in.
//
//   swipe / arrows   change the screen: Status, Terminal, WLAN, Web-UI, Bluetooth, Info
//   Terminal         live view of the serial line plus a few keys
//   accelerometer    turns the picture to whichever edge is up, wakes it on movement
#if HAS_SPI_LCD
#include "settings.h"
#include "power.h"
#include "net.h"
#include "serial_bridge.h"
#include "sdcard.h"
#include "ble.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace Display {

// Pins and panel options follow the LovyanGFX board configuration published for
// this board (bus SPI2, panel inverted, no reset line, touch on I2C port 0).
class Panel : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_CST816S touch;

public:
  Panel() {
    {
      auto cfg = bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      bus.config(cfg);
      panel.setBus(&bus);
    }
    {
      auto cfg = panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.readable = false;              // the panel does not answer on MISO
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = true;             // the SD slot sits on the same SPI lines
      panel.config(cfg);
    }
    {
      auto cfg = light.config();
      cfg.pin_bl = PIN_LCD_BL;
      light.config(cfg);
      panel.setLight(&light);
    }
    {
      auto cfg = touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x15;
      cfg.pin_sda = PIN_I2C_SDA;
      cfg.pin_scl = PIN_I2C_SCL;
      cfg.freq = 400000;
      touch.config(cfg);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};

static Panel lcd;
static LGFX_Sprite canvas(&lcd);
static LovyanGFX *g = &lcd;              // the off-screen canvas, or the panel itself if there is no RAM for one
static uint8_t canvasBits = 0;           // 16 = PSRAM, 8 = internal RAM, 0 = no canvas

// colours are RGB888 (uint32_t), LovyanGFX converts them
static const uint32_t C_BG = 0x0B0F14, C_PANEL = 0x1C2733, C_LINE = 0x2E3D4D, C_TEXT = 0xF2F5F8,
                      C_DIM = 0x8FA1B3, C_ACCENT = 0x22D3EE, C_GREEN = 0x34D399, C_AMBER = 0xFBBF24,
                      C_RED = 0xF87171, C_BLUE = 0x60A5FA, C_BTN = 0x263545, C_BTN_DOWN = 0x3B82F6,
                      C_TERM = 0x9AE6B4;

enum Screen : uint8_t { S_STATUS = 0, S_TERM, S_WIFI, S_URL, S_BT, S_INFO, S_COUNT };
static const char *const TITLES[S_COUNT] = {"Status", "Terminal", "WLAN", "Web-UI", "Bluetooth", "Info"};

static const int HEAD_H = 30, NAV_H = 12, BTN_H = 38;

static bool ready = false, on = false;
static uint8_t screen = S_STATUS;
static uint8_t selPort = 0;              // port shown on Status and Terminal
static uint8_t rot = 3;
static int W = 320, H = 240;
static uint32_t lastActivity = 0, lastDraw = 0;
static bool dirty = true;
static char msg1[40], msg2[40];
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

// The built-in fonts are ASCII only; texts from other modules carry umlauts.
static void toAscii(const char *in, char *out, size_t size) {
  size_t o = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && o + 2 < size; p++) {
    if (*p < 0x80) { out[o++] = *p; continue; }
    const char *r = "";
    if (p[0] == 0xC3) {
      switch (p[1]) {
        case 0xA4: r = "ae"; break; case 0xB6: r = "oe"; break; case 0xBC: r = "ue"; break;
        case 0x84: r = "Ae"; break; case 0x96: r = "Oe"; break; case 0x9C: r = "Ue"; break;
        case 0x9F: r = "ss"; break;
      }
    } else if (p[0] == 0xC2 && p[1] == 0xB7) r = "-";
    while (*r && o + 1 < size) out[o++] = *r++;
    while ((p[1] & 0xC0) == 0x80) p++;   // skip the rest of the UTF-8 sequence
  }
  out[o] = 0;
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

static String fit(const String &s, int maxWidth) {
  if (g->textWidth(s.c_str()) <= maxWidth) return s;
  String t = s;
  while (t.length() > 1 && g->textWidth((t + "..").c_str()) > maxWidth) t.remove(t.length() - 1);
  return t + "..";
}

static void nextPort() {
  for (uint8_t i = 1; i <= MAX_PORTS; i++) {
    uint8_t p = (selPort + i) % MAX_PORTS;
    if (Bridge::enabled(p)) { selPort = p; return; }
  }
}

// ------------------------------------------------------------------ terminal view
// A small screen buffer fed from the port's replay ring, so nothing has to be
// hooked into the serial path. Escape sequences are dropped, not interpreted.
static const int TERM_MAX_COLS = 53, TERM_MAX_ROWS = 30, TERM_CW = 6, TERM_CH = 8;
static char termGrid[TERM_MAX_ROWS][TERM_MAX_COLS + 1];
static int termCols = TERM_MAX_COLS, termRows = 19, termX = 0, termY = 0;
static uint32_t termSeq = 0;
static uint8_t termPort = 0xFF, termEsc = 0;
static bool termDirty = false;

static void termReset() {
  for (auto &row : termGrid) memset(row, 0, sizeof(row));
  termX = termY = 0;
  termEsc = 0;
  termPort = selPort;
  uint32_t now = Bridge::seqNow(termPort), start = Bridge::ringStartSeq(termPort);
  termSeq = now - start > 4096 ? now - 4096 : start;     // replay the last screens only
  termDirty = true;
}

static void termNewline() {
  if (termY < termRows - 1) { termY++; return; }
  memmove(termGrid[0], termGrid[1], (size_t)(termRows - 1) * sizeof(termGrid[0]));
  memset(termGrid[termRows - 1], 0, sizeof(termGrid[0]));
}

static void termPut(uint8_t c) {
  if (termEsc == 1) { termEsc = c == '[' ? 2 : 0; return; }
  if (termEsc == 2) { if (c >= 0x40 && c <= 0x7E) termEsc = 0; return; }
  switch (c) {
    case 0x1B: termEsc = 1; return;
    case '\r': termX = 0; return;
    case '\n': termNewline(); return;
    case '\b': if (termX > 0) termX--; return;
    case '\t': do { termPut(' '); } while (termX % 8 && termX < termCols); return;
  }
  if (c < 0x20 || c > 0x7E) return;
  if (termX >= termCols) { termX = 0; termNewline(); }
  termGrid[termY][termX++] = c;
}

static void termFeed() {
  if (termPort != selPort) termReset();
  uint32_t start = Bridge::ringStartSeq(termPort);
  if ((int32_t)(start - termSeq) > 0) termSeq = start;   // fell behind the ring
  uint8_t buf[256];
  size_t n = Bridge::copyFrom(termPort, termSeq, buf, sizeof(buf));
  if (!n) return;
  termSeq += n;
  for (size_t i = 0; i < n; i++) termPut(buf[i]);
  termDirty = true;
}

// ------------------------------------------------------------------ accelerometer (QMI8658)
static uint8_t imuAddr = 0;
static int16_t ax = 0, ay = 0, az = 0;
static uint32_t lastImu = 0;
static uint8_t rotCandidate = 0xFF, rotVotes = 0;

static bool imuBegin() {
  for (uint8_t a : {0x6B, 0x6A}) {
    auto who = lgfx::i2c::readRegister8(0, a, 0x00);
    if (who.has_value() && who.value() == 0x05) { imuAddr = a; break; }
  }
  if (!imuAddr) return false;
  lgfx::i2c::writeRegister8(0, imuAddr, 0x60, 0xB0);     // soft reset
  delay(20);
  lgfx::i2c::writeRegister8(0, imuAddr, 0x02, 0x40);     // CTRL1: register auto increment, little endian
  lgfx::i2c::writeRegister8(0, imuAddr, 0x03, 0x23);     // CTRL2: +-8 g (4096 LSB/g)
  lgfx::i2c::writeRegister8(0, imuAddr, 0x04, 0x43);     // CTRL3: gyroscope range/rate
  lgfx::i2c::writeRegister8(0, imuAddr, 0x08, 0x03);     // CTRL7: accelerometer + gyroscope on
  return true;
}

static void setRotation(uint8_t r);

static void imuPoll(uint32_t now) {
  if (!imuAddr || now - lastImu < 200) return;
  lastImu = now;
  uint8_t d[6];
  if (!lgfx::i2c::readRegister(0, imuAddr, 0x35, d, 6).has_value()) return;
  int16_t x = d[0] | d[1] << 8, y = d[2] | d[3] << 8, z = d[4] | d[5] << 8;
  bool moved = abs(x - ax) + abs(y - ay) + abs(z - az) > 1200;      // ~0.3 g between two samples
  ax = x; ay = y; az = z;
  if (moved && !on) wake();
  static uint8_t samples = 0;            // one settled reading for the boot log (the first is empty)
  if (samples < 5 && ++samples == 5) {
    Serial.printf("[IMU]  Lage x=%d y=%d z=%d (4096 = 1 g), Drehung %u\n", x, y, z, rot);
  }

  // Which edge points up? Only decided when the board is clearly tilted; lying
  // flat keeps the last orientation. IMU_ROT_* map the axes to the panel.
  const int16_t TILT = 2400;                                         // ~0.6 g
  uint8_t want = 0xFF;
  if (abs(x) > TILT && abs(x) > abs(y)) want = x > 0 ? IMU_ROT_X_POS : IMU_ROT_X_POS ^ 2;
  else if (abs(y) > TILT) want = y > 0 ? IMU_ROT_Y_POS : IMU_ROT_Y_POS ^ 2;
  if (want == 0xFF || want == rot) { rotCandidate = 0xFF; return; }
  if (want != rotCandidate) { rotCandidate = want; rotVotes = 0; }
  if (++rotVotes < 3) return;
  rotCandidate = 0xFF;
  Serial.printf("[IMU]  x=%d y=%d z=%d -> Drehung %u\n", x, y, z, want);
  setRotation(want);
  wake();
}

// ------------------------------------------------------------------ canvas / rotation
static void setRotation(uint8_t r) {
  rot = r & 3;
  lcd.setRotation(rot);
  W = lcd.width();
  H = lcd.height();
  canvas.deleteSprite();
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  g = &canvas;
  canvasBits = 16;
  if (!canvas.createSprite(W, H)) {
    canvas.setPsram(false);
    canvas.setColorDepth(8);
    canvasBits = 8;
    if (!canvas.createSprite(W, H)) {
      g = &lcd;                          // slower and with some flicker, but it works
      canvasBits = 0;
      lcd.fillScreen(C_BG);
    }
  }
  int top = HEAD_H + 2, bottom = H - NAV_H - BTN_H - 4;
  termCols = min((W - 4) / TERM_CW, TERM_MAX_COLS);
  termRows = min((bottom - top) / TERM_CH, TERM_MAX_ROWS);
  termPort = 0xFF;                       // rebuild the terminal for the new size
  dirty = true;
}

// ------------------------------------------------------------------ touch + buttons
enum BtnId : uint8_t { B_NONE = 0, B_PREV, B_NEXT, B_BAUD, B_AUTO, B_BREAK, B_ENTER, B_CTRLC, B_PORT, B_REC, B_BT };
struct Btn { int16_t x, y, w, h; uint8_t id; };
static Btn btns[10];
static uint8_t btnCount = 0;

static bool touchDown = false, touchSwallow = false;
static int touchX0 = 0, touchY0 = 0, touchX = 0, touchY = 0;
static uint32_t lastTouchPoll = 0;

static bool inside(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void addHotspot(int x, int y, int w, int h, uint8_t id) {
  if (btnCount < sizeof(btns) / sizeof(btns[0])) btns[btnCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id};
}

static void button(int x, int y, int w, int h, const char *label, uint8_t id, uint32_t textColor = C_TEXT) {
  Btn b = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id};
  bool down = touchDown && !touchSwallow && inside(b, touchX0, touchY0) && inside(b, touchX, touchY);
  g->fillRoundRect(x, y, w, h, 6, down ? C_BTN_DOWN : C_BTN);
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_center);
  g->setTextColor(down ? C_TEXT : textColor);
  g->drawString(label, x + w / 2, y + h / 2);
  addHotspot(x, y, w, h, id);
}

struct BtnDef { const char *label; uint8_t id; uint32_t color; };

static void buttonRow(const BtnDef *defs, int n) {
  const int gap = 4, y = H - NAV_H - BTN_H - 2;
  int w = (W - gap * (n + 1)) / n;
  for (int i = 0; i < n; i++) button(gap + i * (w + gap), y, w, BTN_H, defs[i].label, defs[i].id, defs[i].color);
}

static const uint32_t BAUD_PRESETS[] = {9600, 19200, 38400, 57600, 115200};

static void press(uint8_t id) {
  switch (id) {
    case B_PREV: screen = (screen + S_COUNT - 1) % S_COUNT; break;
    case B_NEXT: screen = (screen + 1) % S_COUNT; break;
    case B_BAUD: {
      const size_t n = sizeof(BAUD_PRESETS) / sizeof(BAUD_PRESETS[0]);
      SerialCfg c = settings.port[selPort].serial;
      size_t i = 0;
      while (i < n && BAUD_PRESETS[i] != c.baud) i++;
      c.baud = BAUD_PRESETS[i < n ? (i + 1) % n : 0];
      Bridge::apply(selPort, c);
      Net::markDirty();
      break;
    }
    case B_AUTO:  Bridge::startAutobaud(selPort); break;
    case B_BREAK: Bridge::sendBreak(selPort); message("BREAK gesendet", "", 800); break;
    case B_ENTER: Bridge::write(selPort, (const uint8_t *)"\r", 1); break;
    case B_CTRLC: Bridge::write(selPort, (const uint8_t *)"\x03", 1); break;
    case B_PORT:  nextPort(); break;
    case B_REC:
      if (Sd::logging(selPort)) { Sd::logStop(selPort); message("Mitschnitt beendet", "", 1200); }
      else if (Sd::logStart(selPort)) message("Mitschnitt", Sd::logName(selPort).c_str(), 1500);
      else message("SD-Karte:", "Datei nicht angelegt", 1500);
      break;
    case B_BT: Ble::setEnabled(Ble::state() == Ble::OFF); break;
  }
  dirty = true;
}

static void touchPoll(uint32_t now) {
  if (now - lastTouchPoll < 15) return;
  lastTouchPoll = now;
  int32_t x, y;
  bool t = lcd.getTouch(&x, &y) > 0;
  if (t) {
    if (!touchDown) {
      touchDown = true;
      touchX0 = x; touchY0 = y;
      touchSwallow = !on;                // the first touch on a dark screen only wakes it
      dirty = true;
    }
    touchX = x; touchY = y;
    wake();
    return;
  }
  if (!touchDown) return;
  touchDown = false;
  dirty = true;
  if (touchSwallow) return;
  int dx = touchX - touchX0, dy = touchY - touchY0;
  if (abs(dx) > 50 && abs(dx) > abs(dy)) {            // swipe left = next screen
    press(dx < 0 ? B_NEXT : B_PREV);
    return;
  }
  for (uint8_t i = 0; i < btnCount; i++)
    if (inside(btns[i], touchX0, touchY0) && inside(btns[i], touchX, touchY)) { press(btns[i].id); return; }
}

// ------------------------------------------------------------------ drawing
static void dot(int x, int y, bool lit, uint32_t color) {
  if (lit) g->fillCircle(x, y, 5, color);
  else g->drawCircle(x, y, 5, C_LINE);
}

static void drawHeader() {
  g->fillRect(0, 0, W, HEAD_H, C_PANEL);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_DIM);
  g->setTextDatum(lgfx::middle_center);
  g->drawString("<", 16, HEAD_H / 2);
  g->drawString(">", W - 16, HEAD_H / 2);
  g->setTextColor(C_TEXT);
  g->drawString(TITLES[screen], W / 2, HEAD_H / 2);
  addHotspot(0, 0, 56, HEAD_H + 6, B_PREV);
  addHotspot(W - 56, 0, 56, HEAD_H + 6, B_NEXT);

  // link lamps: web/TCP client, Bluetooth, recording
  int x = W - 44;
  if (Sd::logging(selPort)) { g->fillCircle(x, HEAD_H / 2, 4, C_RED); x -= 13; }
  Ble::State bs = Ble::state();
  if (bs != Ble::OFF) { g->fillCircle(x, HEAD_H / 2, 4, bs == Ble::CONNECTED ? C_BLUE : C_LINE); x -= 13; }
  if (Net::webClients() || Net::tcpConnected()) g->fillCircle(x, HEAD_H / 2, 4, C_GREEN);

  // battery, left of the title
  if (Power::measured() && Power::present()) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", Power::pct());
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::middle_left);
    g->setTextColor(Power::low() ? C_RED : C_DIM);
    g->drawString(buf, 34, HEAD_H / 2);
  }
}

static void drawNav() {
  int y = H - NAV_H / 2, x0 = W / 2 - (S_COUNT - 1) * 7;
  for (int i = 0; i < S_COUNT; i++) g->fillCircle(x0 + i * 14, y, i == screen ? 3 : 2, i == screen ? C_ACCENT : C_LINE);
}

// label/value rows; returns false when the screen is full
static int rowY = 0, rowLimit = 0;
static bool row(const char *label, const String &value, uint32_t color = C_TEXT) {
  if (rowY + 16 > rowLimit) return false;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_DIM);
  g->drawString(label, 8, rowY);
  g->setTextColor(color);
  g->drawString(fit(value, W - 78).c_str(), 70, rowY);
  rowY += 18;
  return true;
}

static String clientsLine() {
  String s = "Web " + String(Net::webClients()) + "  TCP " + String(Net::tcpConnected() ? 1 : 0);
  if (Ble::state() != Ble::OFF) s += String("  BT ") + (Ble::state() == Ble::CONNECTED ? "1" : "0");
  return s;
}

static String batteryLine() {
  if (Power::present()) return String(Power::pct()) + "%  " + String(Power::mv()) + " mV";
  return Power::measured() ? String("keiner (USB)") : String("keine Messung");
}

static String sdLine() {
  if (!Sd::mounted()) return String(Sd::typeName());
  return String((unsigned)Sd::usedMb()) + " / " + String((unsigned)Sd::totalMb()) + " MB";
}

static void screenStatus() {
  uint32_t now = millis();
  bool multi = Bridge::enabledCount() > 1;
  g->setTextDatum(lgfx::top_left);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  g->drawString(fit(Store::portName(selPort), W - 16).c_str(), 8, HEAD_H + 6);

  g->setFont(&fonts::DejaVu24);
  if (Bridge::autobaudRunning(selPort)) {
    g->setTextColor(C_AMBER);
    g->drawString("Auto-Baud...", 8, HEAD_H + 22);
  } else {
    g->setTextColor(C_ACCENT);
    g->drawString(Store::serialLabel(settings.port[selPort].serial).c_str(), 8, HEAD_H + 22);
  }

  int y = HEAD_H + 62;
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->setTextDatum(lgfx::middle_left);
  dot(14, y, now - Bridge::lastRxMs[selPort] < 150, C_GREEN);
  g->drawString(("RX " + fmtBytes(Bridge::rxBytes[selPort])).c_str(), 26, y);
  dot(W / 2 + 6, y, now - Bridge::lastTxMs[selPort] < 150, C_AMBER);
  g->drawString(("TX " + fmtBytes(Bridge::txBytes[selPort])).c_str(), W / 2 + 18, y);
  g->drawFastHLine(8, y + 16, W - 16, C_LINE);

  rowY = y + 24;
  rowLimit = H - NAV_H - BTN_H - 4;
  (void)(row("WLAN", settings.apSsid) && row("IP", Net::apIp()) &&
         row("LAN", settings.staSsid.length() ? (Net::staConnected() ? Net::staIp() : String("verbinde...")) : String("aus")) &&
         row("Clients", clientsLine()) && row("Akku", batteryLine()) && row("SD", sdLine()));

  BtnDef defs[4] = {{"Baud", B_BAUD, C_TEXT}, {"Auto-Baud", B_AUTO, C_TEXT}, {"BREAK", B_BREAK, C_AMBER}};
  int n = 3;
  static char portLabel[8];
  if (multi) {
    snprintf(portLabel, sizeof(portLabel), "Port %u", selPort + 1);
    defs[n++] = {portLabel, B_PORT, C_ACCENT};
  }
  buttonRow(defs, n);
}

static void screenTerm() {
  int top = HEAD_H + 2;
  g->setFont(&fonts::Font0);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_TERM);
  for (int r = 0; r < termRows; r++)
    if (termGrid[r][0]) g->drawString(termGrid[r], 2, top + r * TERM_CH);
  if ((millis() / 500) % 2) g->fillRect(2 + min(termX, termCols - 1) * TERM_CW, top + termY * TERM_CH, TERM_CW, TERM_CH, C_TERM);

  BtnDef defs[5] = {{"Enter", B_ENTER, C_TEXT}, {"Ctrl-C", B_CTRLC, C_TEXT}, {"BREAK", B_BREAK, C_AMBER}};
  int n = 3;
  if (Sd::mounted()) defs[n++] = {Sd::logging(selPort) ? "Stop" : "REC", B_REC, C_RED};
  static char portLabel[8];
  if (Bridge::enabledCount() > 1) {
    snprintf(portLabel, sizeof(portLabel), "P%u", selPort + 1);
    defs[n++] = {portLabel, B_PORT, C_ACCENT};
  }
  buttonRow(defs, n);
}

// QR code with up to four text lines: beside it in landscape, below it in portrait
static void screenQr(const String &payload, const char *l1, const String &v1, const char *l2, const String &v2) {
  int top = HEAD_H + 6, avail = H - NAV_H - top - 6;
  bool wide = W > H;
  int size = wide ? avail : min(W - 24, avail - 76);
  int qx = wide ? 8 : (W - size) / 2;
  g->fillRoundRect(qx, top, size, size, 6, 0xFFFFFFu);
  g->qrcode(payload.c_str(), qx + 6, top + 6, size - 12, 1);

  int tx = wide ? qx + size + 10 : 8, ty = wide ? top + 4 : top + size + 8, tw = W - tx - 6;
  g->setTextDatum(lgfx::top_left);
  for (int i = 0; i < 2; i++) {
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    g->drawString(i ? l2 : l1, tx, ty);
    g->setTextColor(C_TEXT);
    g->drawString(fit(i ? v2 : v1, tw).c_str(), tx, ty + 15);
    ty += 36;
  }
}

static void screenBt() {
  Ble::State s = Ble::state();
  const char *text = s == Ble::OFF ? "Aus" : s == Ble::ADVERTISING ? "Bereit, wartet auf Verbindung"
                   : s == Ble::PAIRING ? "Kopplung: PIN am Handy eingeben" : "Verbunden";
  uint32_t color = s == Ble::OFF ? C_DIM : s == Ble::CONNECTED ? C_BLUE : s == Ble::PAIRING ? C_AMBER : C_GREEN;
  g->setTextDatum(lgfx::top_left);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(color);
  g->drawString(text, 8, HEAD_H + 8);

  if (s != Ble::OFF) {
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)Ble::passkey());
    g->setTextColor(C_DIM);
    g->drawString("PIN", 8, HEAD_H + 30);
    g->setFont(&fonts::DejaVu24);
    g->setTextColor(C_TEXT);
    g->drawString(pin, 70, HEAD_H + 26);
  }
  rowY = HEAD_H + 60;
  rowLimit = H - NAV_H - BTN_H - 4;
  (void)(row("Name", settings.apSsid) && row("Dienst", "Nordic UART (BLE)") &&
         row("Port", Store::portName(0) + ", " + Store::serialLabel(settings.port[0].serial)));

  BtnDef def = {s == Ble::OFF ? "Bluetooth einschalten" : "Bluetooth ausschalten", B_BT, C_TEXT};
  buttonRow(&def, 1);
}

static void screenInfo() {
  rowY = HEAD_H + 8;
  rowLimit = H - NAV_H - 2;
  String tcp = !settings.tcpEnabled ? String("aus")
             : Bridge::enabledCount() > 1 ? ":" + String(RAW_TCP_PORT) + "-" + String(RAW_TCP_PORT + MAX_PORTS - 1)
             : ":" + String(RAW_TCP_PORT);
  char imu[40];
  if (imuAddr) snprintf(imu, sizeof(imu), "%d %d %d  Dr. %u", ax, ay, az, rot);
  else strlcpy(imu, "nicht gefunden", sizeof(imu));
  (void)(row("Firmware", String(FW_VERSION) + "  up " + fmtUptime(millis() / 1000)) &&
         row("Host", settings.hostname + ".local") && row("Hotspot", String(Net::apStations()) + " Geraete") &&
         row("Clients", clientsLine()) && row("Raw-TCP", tcp) && row("Akku", batteryLine()) && row("SD", sdLine()) &&
         row("RAM", String(ESP.getFreeHeap() / 1024) + " kB frei") && row("Lage", imu) && row("Board", BOARD_NAME));
}

static void drawMessage() {
  g->setFont(&fonts::DejaVu18);
  int w1 = g->textWidth(msg1);
  g->setFont(&fonts::DejaVu12);
  int w2 = g->textWidth(msg2);
  int w = min(max(w1, w2) + 28, W - 8), h = msg2[0] ? 66 : 44;
  int x = (W - w) / 2, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 8, C_PANEL);
  g->drawRoundRect(x, y, w, h, 8, C_ACCENT);
  g->setTextDatum(lgfx::middle_center);
  g->setTextColor(C_TEXT);
  g->setFont(&fonts::DejaVu18);
  g->drawString(msg1, W / 2, y + 22);
  if (msg2[0]) {
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    g->drawString(msg2, W / 2, y + 48);
  }
}

static void draw(uint32_t now) {
  if (!Bridge::enabled(selPort)) nextPort();
  btnCount = 0;
  g->fillScreen(C_BG);
  drawHeader();
  switch (screen) {
    case S_TERM: screenTerm(); break;
    case S_WIFI:
      screenQr("WIFI:T:WPA;S:" + qrEscape(settings.apSsid) + ";P:" + qrEscape(settings.apPass) + ";;", "WLAN",
               settings.apSsid, "Passwort", settings.apPass);
      break;
    case S_URL:
      screenQr("http://" + Net::apIp() + "/", "Web-UI", Net::apIp(), Net::staConnected() ? "im LAN" : "Name",
               Net::staConnected() ? Net::staIp() : settings.hostname + ".local");
      break;
    case S_BT:   screenBt(); break;
    case S_INFO: screenInfo(); break;
    default:     screenStatus(); break;
  }
  drawNav();
  if ((int32_t)(msgUntil - now) > 0) drawMessage();
  if (g == &canvas) canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------------ public
bool begin() {
#if HAS_SDCARD
  pinMode(PIN_SD_CS, OUTPUT);            // a card on the shared bus must stay quiet while the LCD starts
  digitalWrite(PIN_SD_CS, HIGH);
#endif
  if (!lcd.init()) return false;
  ready = true;
  bool imu = imuBegin();
  Serial.printf("[LCD]  Lagesensor QMI8658: %s\n", imu ? "ok" : "nicht gefunden");
  setRotation(settings.oledFlip ? IMU_ROT_DEFAULT ^ 2 : IMU_ROT_DEFAULT);
  Serial.printf("[LCD]  Zeichenpuffer: %s\n", canvasBits == 0 ? "keiner (direkt)" : canvasBits == 16 ? "16 Bit" : "8 Bit");
  on = true;
  lastActivity = millis();
  applyBrightness();
  return true;
}

bool isOn() { return ready && on; }
uint8_t page() { return screen == S_STATUS ? PAGE_STATUS : PAGE_INFO; }

// The setting was made for OLED contrast, where 0 is still readable; a backlight
// at 0 is simply dark, so keep a floor.
void applyBrightness() {
  if (ready && on) lcd.setBrightness(24 + (uint16_t)settings.oledBrightness * 231 / 255);
}

void wake() {
  lastActivity = millis();
  if (ready && !on) {
    lcd.wakeup();
    on = true;
    dirty = true;
    applyBrightness();
  }
}

void off() {
  if (ready && on) {
    lcd.setBrightness(0);
    lcd.sleep();
    on = false;
  }
}

void nextPage() {
  screen = (screen + 1) % S_COUNT;
  dirty = true;
}

void message(const char *line1, const char *line2, uint32_t ms) {
  toAscii(line1 ? line1 : "", msg1, sizeof(msg1));
  toAscii(line2 ? line2 : "", msg2, sizeof(msg2));
  if (msg2[0] == ' ') memmove(msg2, msg2 + 1, strlen(msg2));
  msgUntil = millis() + ms;
  wake();
  dirty = true;
}

void loop() {
  if (!ready) return;
  uint32_t now = millis();
  touchPoll(now);
  imuPoll(now);
  if (on && settings.displayTimeout && now - lastActivity > settings.displayTimeout * 1000UL &&
      (int32_t)(msgUntil - now) <= 0) {
    off();
  }
  if (!on) return;

  if (screen == S_TERM) {
    termFeed();
    if (termDirty && now - lastDraw >= 100) dirty = true;
  }
  if (!dirty && now - lastDraw < (touchDown ? 60u : 250u)) return;
  dirty = false;
  termDirty = false;
  lastDraw = now;
  draw(now);
}

}  // namespace Display
#endif
