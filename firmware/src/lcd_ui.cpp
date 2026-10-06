#include "display.h"
#include "config.h"

// Colour touch front end for boards with a small SPI LCD (Waveshare
// ESP32-S3-Touch-LCD-2: ST7789T3 240x320, CST816D touch, QMI8658 accelerometer).
// It implements the same Display interface as the OLED code in display.cpp, so
// the rest of the firmware does not care which of the two is built in.
//
//   swipe / arrows   change the screen: Status, Terminal, WLAN, Web-UI, Bluetooth, Info
//   drag up / down   scrolls a page that is longer than the screen
//   Terminal         live view of the serial line plus a few keys
//   accelerometer    turns the picture to whichever edge is up, wakes it on movement
//
// How it is built - the serial bridge must never wait for pixels:
//
//   main loop task   owns the firmware state. Every 100 ms it copies what the
//                    screens show into a plain snapshot, feeds the terminal grid
//                    and carries out the commands the user tapped.
//   UI task          owns the panel, the touch controller and the accelerometer.
//                    It draws from the snapshot only, into an off-screen canvas,
//                    and sends just the horizontal bands that changed.
//
// The two meet in the snapshot (mutex), a command queue and a few request flags.
// Nothing else crosses the task boundary, so no String is ever read while the
// other side changes it.
#if HAS_SPI_LCD
#include "settings.h"
#include "power.h"
#include "net.h"
#include "serial_bridge.h"
#include "sdcard.h"
#include "ble.h"
#include "configs.h"
#include "player.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include "esp_freertos_hooks.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#if ARDUINO_USB_CDC_ON_BOOT
#include "hal/usb_serial_jtag_ll.h"
#endif

namespace Display {

// Pins and panel options follow LovyanGFX's own configuration for this board
// (lgfx_user/LGFX_ESP32_S3_Touch_LCD_2.h): panel inverted, no reset line, no
// touch interrupt line. Different on purpose: SPI2 instead of SPI3, because
// that is the bus LovyanGFX shares with Arduino's SPI object and thus with SD.
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
      cfg.freq_write = LCD_SPI_HZ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;              // the panel answers on MOSI; MISO belongs to the SD card
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
      cfg.readable = false;              // nothing is read back from the panel
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = true;             // the SD slot sits on the same SPI lines
      panel.config(cfg);
    }
    {
      auto cfg = light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;                  // well above anything the eye or a camera sees
      cfg.pwm_channel = 7;
      light.config(cfg);
      panel.setLight(&light);
    }
    {
      auto cfg = touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x15;
      cfg.pin_sda = PIN_I2C_SDA;
      cfg.pin_scl = PIN_I2C_SCL;
      cfg.pin_int = -1;
      cfg.pin_rst = -1;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
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

enum Screen : uint8_t { S_STATUS = 0, S_TERM, S_SCRIPTS, S_WIFI, S_URL, S_BT, S_SYSTEM, S_INFO, S_SETUP, S_COUNT };
static const char *const TITLES[S_COUNT] = {"Status", "Terminal", "Skripte", "WLAN", "Web-UI", "Bluetooth",
                                            "System", "Info", "Setup"};
static const int MAX_CFG = 16, MAX_NETS = 12;

// 7.8 pixels are a millimetre on this panel: buttons are 6 mm high, list rows 4 mm.
static const int HEAD_H = 30, NAV_H = 12, BTN_H = 46, BTN_GAP = 4;
static const int TERM_MAX_COLS = 53, TERM_MAX_ROWS = 30, TERM_CW = 6, TERM_CH = 8;

// ============================================================ shared between the tasks

// Everything the screens show, as plain data.
struct Snapshot {
  bool    portOn[MAX_PORTS];
  char    portName[MAX_PORTS][20];
  char    serial[MAX_PORTS][16];
  bool    autobaud[MAX_PORTS], rxActive[MAX_PORTS], txActive[MAX_PORTS], recording[MAX_PORTS];
  uint32_t rx[MAX_PORTS], tx[MAX_PORTS];
  uint8_t ports;
  char    apSsid[33], apPass[65], apIp[16], staIp[16], hostname[33];
  bool    staConfigured, staConnected, tcpEnabled, tcpConnected;
  uint8_t webClients, apStations;
  uint8_t bleState;
  uint32_t blePin;
  bool    batMeasured, batPresent, batLow;
  uint8_t batCharge;                 // Power::Charge
  uint8_t batPct;
  uint16_t batMv;
  bool    saver;                     // running from the battery: dim early, poll slowly
  uint8_t cpuMhz;
  bool    sdMounted;
  char    sdType[16];
  uint32_t sdUsedMb, sdTotalMb;
  uint8_t brightness;
  uint16_t timeoutS;
  bool    flip;
  // System page
  uint8_t cpu[2];                    // load in percent, per core
  uint32_t heapFree, heapTotal, heapMin, heapBlock, psramFree, psramTotal;
  int8_t  tempC;
  uint16_t loopMs, drawMs;           // longest main loop pause / drawing time in the last second
  uint8_t tasks;
  // Skripte page
  uint8_t cfgCount;
  char    cfg[MAX_CFG][28];
  uint8_t playState, playPort;
  uint16_t playLine, playLines;
  char    playName[28], playResult[48];
  // Setup page
  char    staSsid[33];
  uint8_t scanState;                 // 0 = no result, 1 = scanning, 2 = list valid
  uint8_t netCount;
  struct { char ssid[33]; int8_t rssi; bool locked; } net[MAX_NETS];
};

static SemaphoreHandle_t stateMux = nullptr;       // snapshot, terminal grid, message text
static SemaphoreHandle_t busMux = nullptr;         // the SPI lines shared by LCD and SD card
static QueueHandle_t cmdQueue = nullptr;           // UI task -> main loop: command << 16 | argument << 8 | port
static char joinSsid[33], joinPass[64];            // handed over with B_JOIN, under stateMux

static Snapshot shared;
static char termShared[TERM_MAX_ROWS][TERM_MAX_COLS + 1];
static int termX = 0, termY = 0;                   // cursor in termShared
static volatile uint32_t termVersion = 0;
static char msgShared1[40], msgShared2[40];
static volatile uint32_t msgUntil = 0;
static volatile uint32_t msgVersion = 0;

// main loop -> UI task
static volatile bool reqWake = false, reqOff = false, reqNext = false, reqBrightness = false, reqShot = false;
static volatile int16_t reqTapX = -1, reqTapY = -1, reqShotFrom = 0, reqShotTo = 999;
static volatile int8_t reqScreen = -1, reqRot = -1;
static volatile int16_t reqDrag[4];                // simulated finger: from x,y to x,y
static volatile bool reqDragGo = false;
static volatile bool reqHalt = false, uiHalted = false;   // power-off: the UI task puts panel and sensor to sleep, then stops

// UI task -> main loop (what it currently shows)
static volatile bool uiOn = false;
static volatile uint8_t uiScreen = S_STATUS, uiPort = 0, uiRot = 3;
static volatile uint8_t uiTermCols = TERM_MAX_COLS, uiTermRows = 19;
static volatile uint32_t uiGeometry = 0;           // bumped when the terminal size changes

// health figures for the debug console
static volatile uint32_t statFrames = 0, statPushes = 0, statBands = 0, statTouches = 0;
static volatile uint32_t statLoopGapMax = 0, statUiGapMax = 0, statTouchMsMax = 0, statDrawMsMax = 0;
static volatile uint16_t recentLoopMs = 0, recentDrawMs = 0;   // the same, per second, for the System page
static volatile uint32_t loopBeat = 0, uiBeat = 0;
static volatile int16_t lastTouchX = -1, lastTouchY = -1;
static volatile int16_t imuX = 0, imuY = 0, imuZ = 0;
static volatile uint16_t litAt = 0xFFFF;           // backlight level the panel has now (0xFFFF: unknown, set it)
static bool imuFound = false;
static bool ready = false;

struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(SemaphoreHandle_t mux) : m(mux) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m); }
};

void busLock() {
  if (!busMux) busMux = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(busMux, portMAX_DELAY);
}
void busUnlock() { xSemaphoreGiveRecursive(busMux); }

// ============================================================ main loop side

enum Cmd : uint8_t {
  B_NONE = 0,
  // handled in the UI task
  B_PREV, B_NEXT, B_PORT, B_BACK, B_LIST_PREV, B_LIST_NEXT, B_CFG_ROW, B_CFG_CANCEL, B_PLAY_ACK, B_NET_OPEN, B_NET_ROW,
  B_ASK_FORGET, B_ASK_OFF, B_CONFIRM,
  K_CHAR, K_SHIFT, K_SYM, K_BKSP, K_OK,
  // carried out by the main loop
  B_BAUD, B_AUTO, B_BREAK, B_ENTER, B_CTRLC, B_REC, B_BT, B_SD, B_PLAY, B_STOP, B_SCAN, B_JOIN, B_FORGET,
  B_BRIGHT_DOWN, B_BRIGHT_UP, B_TIMEOUT, B_POWEROFF
};
static const uint8_t FIRST_LOOP_CMD = B_BAUD;

static const uint32_t BAUD_PRESETS[] = {9600, 19200, 38400, 57600, 115200};

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

static void copyStr(char *dst, size_t size, const String &s) { toAscii(s.c_str(), dst, size); }

// ---- CPU load: the idle task of each core tells how much of the time it got.
// It runs once per tick when the core has nothing to do, so any gap longer than
// a tick was somebody's work. Coarse (work shorter than a tick hides in it), but
// free of the run-time statistics this build of FreeRTOS does not have.
static volatile uint32_t idleUs[2] = {0, 0};
static int64_t idleLast[2] = {0, 0};
static bool idleTick(int core) {
  int64_t t = esp_timer_get_time(), d = t - idleLast[core];
  idleLast[core] = t;
  idleUs[core] = idleUs[core] + (uint32_t)(d > 1000 ? 1000 : d);
  return true;                                     // nothing more to do: the core may sleep
}
static bool idleHook0() { return idleTick(0); }
static bool idleHook1() { return idleTick(1); }

static uint8_t cpuLoad[2] = {0, 0};
static int8_t chipTemp = 0;
static uint16_t loopMsShown = 0, drawMsShown = 0;

static void sampleSystem(uint32_t now) {           // once a second
  static uint32_t last = 0;
  static int64_t lastUs = 0;
  if (now - last < 1000) return;
  last = now;
  int64_t t = esp_timer_get_time();
  uint32_t span = (uint32_t)(t - lastUs);
  lastUs = t;
  for (int c = 0; c < 2; c++) {
    uint32_t idle = idleUs[c];
    idleUs[c] = 0;
    uint32_t pct = span ? (uint64_t)idle * 100 / span : 100;
    cpuLoad[c] = pct >= 100 ? 0 : 100 - pct;
  }
  chipTemp = (int8_t)temperatureRead();
  loopMsShown = recentLoopMs; recentLoopMs = 0;
  drawMsShown = recentDrawMs; recentDrawMs = 0;
}

// ---- stored configurations (Skripte page). The directory is read when the page
// is opened and every few seconds while it is shown.
static char cfgNames[MAX_CFG][Configs::MAX_NAME + 1];
static uint8_t cfgCount = 0;
static void loadConfigs() {
  cfgCount = 0;
  JsonDocument d;
  if (deserializeJson(d, Configs::listJson())) return;
  for (JsonObjectConst o : d["configs"].as<JsonArrayConst>()) {
    if (cfgCount == MAX_CFG) break;
    strlcpy(cfgNames[cfgCount++], o["name"] | "", sizeof(cfgNames[0]));
  }
}

// ---- WLAN scan for the Setup page
struct NetEntry { char ssid[33]; int8_t rssi; bool locked; };
static NetEntry nets[MAX_NETS];
static uint8_t netCount = 0, scanState = 0;

static void startScan() {
  if (scanState == 1) return;
  if (Ble::state() != Ble::OFF) WiFi.setSleep(true);       // station + Bluetooth without modem sleep aborts
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);   // scanning needs the station interface
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) { scanState = 2; netCount = 0; return; }
  scanState = 1;
}

static void pollScan() {
  if (scanState != 1) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  netCount = 0;
  for (int i = 0; i < n && netCount < MAX_NETS; i++) {       // strongest first; one line per name
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (uint8_t k = 0; k < netCount; k++) dup |= ssid == nets[k].ssid;
    if (dup) continue;
    strlcpy(nets[netCount].ssid, ssid.c_str(), sizeof(nets[0].ssid));
    nets[netCount].rssi = WiFi.RSSI(i);
    nets[netCount].locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    netCount++;
  }
  WiFi.scanDelete();
  scanState = 2;
}

struct Snapshot;
static void fillSystem(Snapshot &s);
static void fillScripts(Snapshot &s);
static void fillNets(Snapshot &s);

static void buildSnapshot() {
  static Snapshot s;                               // static: too big for comfort on the stack
  memset(&s, 0, sizeof(s));
  uint32_t now = millis();
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    s.portOn[p] = Bridge::enabled(p);
    if (!s.portOn[p]) continue;
    s.ports++;
    copyStr(s.portName[p], sizeof(s.portName[p]), Store::portName(p));
    copyStr(s.serial[p], sizeof(s.serial[p]), Store::serialLabel(settings.port[p].serial));
    s.autobaud[p] = Bridge::autobaudRunning(p);
    s.rxActive[p] = now - Bridge::lastRxMs[p] < 150;
    s.txActive[p] = now - Bridge::lastTxMs[p] < 150;
    s.recording[p] = Sd::logging(p);
    s.rx[p] = Bridge::rxBytes[p];
    s.tx[p] = Bridge::txBytes[p];
  }
  copyStr(s.apSsid, sizeof(s.apSsid), settings.apSsid);
  strlcpy(s.apPass, settings.apPass.c_str(), sizeof(s.apPass));      // QR payload: keep it byte exact
  copyStr(s.apIp, sizeof(s.apIp), Net::apIp());
  copyStr(s.hostname, sizeof(s.hostname), settings.hostname);
  s.staConfigured = settings.staSsid.length() > 0;
  s.staConnected = Net::staConnected();
  if (s.staConnected) copyStr(s.staIp, sizeof(s.staIp), Net::staIp());
  s.tcpEnabled = settings.tcpEnabled;
  s.tcpConnected = Net::tcpConnected();
  s.webClients = Net::webClients();
  s.apStations = Net::apStations();
  s.bleState = Ble::state();
  s.blePin = Ble::passkey();
  s.batMeasured = Power::measured();
  s.batPresent = Power::present();
  s.batLow = Power::low();
  s.batCharge = Power::charge();
  s.batPct = Power::pct();
  s.batMv = Power::mv();
  s.saver = Power::saver();
  s.cpuMhz = (uint8_t)getCpuFrequencyMhz();
  s.sdMounted = Sd::mounted();
  strlcpy(s.sdType, Sd::typeName(), sizeof(s.sdType));
  s.sdUsedMb = Sd::usedMb();
  s.sdTotalMb = Sd::totalMb();
  s.brightness = settings.oledBrightness;
  fillSystem(s);
  fillScripts(s);
  fillNets(s);
  s.timeoutS = settings.displayTimeout;
  s.flip = settings.oledFlip;
  Lock l(stateMux);
  shared = s;
}

static void fillSystem(Snapshot &s) {
  s.cpu[0] = cpuLoad[0];
  s.cpu[1] = cpuLoad[1];
  s.heapFree = ESP.getFreeHeap();
  s.heapTotal = ESP.getHeapSize();
  s.heapMin = ESP.getMinFreeHeap();
  s.heapBlock = ESP.getMaxAllocHeap();
  s.psramFree = ESP.getFreePsram();
  s.psramTotal = ESP.getPsramSize();
  s.tempC = chipTemp;
  s.loopMs = loopMsShown;
  s.drawMs = drawMsShown;
  s.tasks = uxTaskGetNumberOfTasks();
}

static void fillScripts(Snapshot &s) {
  s.cfgCount = cfgCount;
  for (uint8_t i = 0; i < cfgCount; i++) toAscii(cfgNames[i], s.cfg[i], sizeof(s.cfg[i]));
  s.playState = Player::state();
  s.playPort = Player::port();
  s.playLine = Player::line();
  s.playLines = Player::lines();
  toAscii(Player::name(), s.playName, sizeof(s.playName));
  toAscii(Player::result(), s.playResult, sizeof(s.playResult));
}

static void fillNets(Snapshot &s) {
  copyStr(s.staSsid, sizeof(s.staSsid), settings.staSsid);
  s.scanState = scanState;
  s.netCount = netCount;
  for (uint8_t i = 0; i < netCount; i++) {
    strlcpy(s.net[i].ssid, nets[i].ssid, sizeof(s.net[i].ssid));     // byte exact: it is handed back to join
    s.net[i].rssi = nets[i].rssi;
    s.net[i].locked = nets[i].locked;
  }
}

// ---- terminal grid, fed from the port's replay ring so nothing has to be hooked
// into the serial path. Escape sequences are dropped, not interpreted.
static uint32_t termSeq = 0, termGeometry = 0xFFFFFFFF;
static uint8_t termPort = 0xFF, termEsc = 0;
static int termCols = TERM_MAX_COLS, termRows = 19;

static void termReset() {
  memset(termShared, 0, sizeof(termShared));
  termX = termY = 0;
  termEsc = 0;
  termPort = uiPort;
  termGeometry = uiGeometry;
  termCols = uiTermCols;
  termRows = uiTermRows;
  uint32_t now = Bridge::seqNow(termPort), start = Bridge::ringStartSeq(termPort);
  termSeq = now - start > 4096 ? now - 4096 : start;     // replay the last screens only
  termVersion = termVersion + 1;
}

static void termNewline() {
  if (termY < termRows - 1) { termY++; return; }
  memmove(termShared[0], termShared[1], (size_t)(termRows - 1) * sizeof(termShared[0]));
  memset(termShared[termRows - 1], 0, sizeof(termShared[0]));
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
  termShared[termY][termX++] = c;
}

static void termFeed() {
  if (uiScreen != S_TERM) return;                  // caught up again when the screen is opened
  uint8_t buf[256];
  Lock l(stateMux);
  if (termPort != uiPort || termGeometry != uiGeometry) termReset();
  uint32_t start = Bridge::ringStartSeq(termPort);
  if ((int32_t)(start - termSeq) > 0) termSeq = start;   // fell behind the ring
  size_t n = Bridge::copyFrom(termPort, termSeq, buf, sizeof(buf));
  if (!n) return;
  termSeq += n;
  for (size_t i = 0; i < n; i++) termPut(buf[i]);
  termVersion = termVersion + 1;
}

static uint32_t rebootAt = 0, offAt = 0;
static const char *offReason = "";

static void saveAndReboot(const char *what) {
  Store::save();
  message(what, "Neustart ...", 3000);
  rebootAt = millis() + 1500;
}

static void runCommand(uint8_t id, uint8_t arg, uint8_t port) {
  if (port >= MAX_PORTS) return;
  static const uint16_t TIMEOUTS[] = {15, 60, 300, 0};
  switch (id) {
    case B_PLAY:
      if (arg < cfgCount && !Player::start(cfgNames[arg], port)) message("Konfiguration", Player::result(), 2500);
      break;
    case B_STOP: Player::stop(); break;
    case B_SCAN: startScan(); break;
    case B_JOIN: {
      Lock l(stateMux);
      settings.staSsid = joinSsid;
      settings.staPass = joinPass;
      settings.staAuth = 0;                // WPA2/WPA3 with a key; 802.1X is set up in the web UI
      memset(joinPass, 0, sizeof(joinPass));
    }
      saveAndReboot("WLAN gespeichert");
      break;
    case B_FORGET:
      settings.staSsid = "";
      settings.staPass = "";
      settings.staAuth = 0;
      saveAndReboot("WLAN-Client aus");
      break;
    case B_BRIGHT_DOWN:
    case B_BRIGHT_UP: {
      int v = settings.oledBrightness + (id == B_BRIGHT_UP ? 32 : -32);
      settings.oledBrightness = constrain(v, 0, 255);
      Store::save();
      Net::markDirty();
      break;
    }
    case B_TIMEOUT: {
      size_t i = 0, n = sizeof(TIMEOUTS) / sizeof(TIMEOUTS[0]);
      while (i < n && TIMEOUTS[i] != settings.displayTimeout) i++;
      settings.displayTimeout = TIMEOUTS[i < n ? (i + 1) % n : 1];
      Store::save();
      Net::markDirty();
      break;
    }
    case B_POWEROFF:
      offReason = "Taste";
      message("Ausschalten ...", "Einschalten: BOOT-Taste", 4000);
      offAt = millis() + 1800;
      break;
    case B_BAUD: {
      const size_t n = sizeof(BAUD_PRESETS) / sizeof(BAUD_PRESETS[0]);
      SerialCfg c = settings.port[port].serial;
      size_t i = 0;
      while (i < n && BAUD_PRESETS[i] != c.baud) i++;
      c.baud = BAUD_PRESETS[i < n ? (i + 1) % n : 0];
      Bridge::apply(port, c);
      Net::markDirty();
      break;
    }
    case B_AUTO:  Bridge::startAutobaud(port); break;
    case B_BREAK: Bridge::sendBreak(port); message("BREAK gesendet", "", 800); break;
    case B_ENTER: Bridge::write(port, (const uint8_t *)"\r", 1); break;
    case B_CTRLC: Bridge::write(port, (const uint8_t *)"\x03", 1); break;
    case B_REC:
      if (Sd::logging(port)) { Sd::logStop(port); message("Mitschnitt beendet", "", 1200); }
      else if (Sd::logStart(port)) message("Mitschnitt", Sd::logName(port).c_str(), 1500);
      else message("SD-Karte:", "Datei nicht angelegt", 1500);
      break;
    case B_BT: Ble::setEnabled(Ble::state() == Ble::OFF); break;
    case B_SD:                           // a card put in after start-up; the attempt can take a few seconds
      message("SD-Karte", "wird gesucht...", 6000);
      delay(150);                        // let the UI task show that before the bus is taken for seconds
      if (Sd::begin()) message("SD-Karte", "eingebunden", 2000);
      else message("SD-Karte", "nicht gefunden", 2000);
      break;
  }
}

// ---- debug console on the USB serial port: lets a developer (or a script) see
// and drive the display without standing in front of it.
//   ?            status of both tasks
//   shot [A B]   the current picture (or rows A..B), run-length coded RGB565
//   tap X Y      a touch at that position
//   screen N     go to screen N,  rot N  force a rotation (rot auto: back to the sensor),  wake / off
//   bat          battery: raw pin voltage, result, what the charge state rests on
//   drag X Y X Y a finger moved from the first to the second position
//   saver N      power saving: 1 on, 0 off, -1 automatic (on battery)
//   poweroff N   switch off, and on again after N seconds (without N: BOOT button only)
static void printStatus() {
  uint32_t now = millis();
  Serial.printf("[DIAG] up %lu s, Heap %u kB (min %u kB), Reset-Grund %d\n", (unsigned long)(now / 1000),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), (int)esp_reset_reason());
  Serial.printf("[DIAG] Hauptschleife: laengste Pause %lu ms | UI-Task: laengste Pause %lu ms, letzter Lauf vor %lu ms\n",
                (unsigned long)statLoopGapMax, (unsigned long)statUiGapMax, (unsigned long)(now - uiBeat));
  Serial.printf("[DIAG] Anzeige: %s, Licht %u, Seite %u, Port %u, Drehung %u, Puffer %u Bit | Bilder %lu, gesendet %lu (%lu Streifen), Zeichnen max %lu ms\n",
                uiOn ? "an" : "aus", (unsigned)litAt, uiScreen, uiPort + 1, uiRot, canvasBits, (unsigned long)statFrames,
                (unsigned long)statPushes, (unsigned long)statBands, (unsigned long)statDrawMsMax);
  Serial.printf("[DIAG] Touch: %lu Beruehrungen, letzte %d,%d, Abfrage max %lu ms | Lage %s x=%d y=%d z=%d\n",
                (unsigned long)statTouches, lastTouchX, lastTouchY, (unsigned long)statTouchMsMax,
                imuFound ? "ok" : "fehlt", imuX, imuY, imuZ);
  Serial.printf("[DIAG] SD: %s | %s\n", Sd::typeName(), Sd::history().c_str());
  Serial.printf("[DIAG] CPU %u%% / %u%%, Chip %d C, Skript-Zustand %d (%s)\n", cpuLoad[0], cpuLoad[1], chipTemp,
                (int)Player::state(), Player::result());
  Serial.printf("[DIAG] Akku: %s\n", Power::diag().c_str());
  Serial.printf("[DIAG] Bluetooth: Zustand %d\n", (int)Ble::state());
  statLoopGapMax = statUiGapMax = statTouchMsMax = statDrawMsMax = 0;
}

static void console() {
  static char line[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    len = 0;
    int a = 0, b = 0, a2 = 0, b2 = 0;
    if (!strcmp(line, "?")) printStatus();
    else if (!strncmp(line, "shot", 4)) {            // "shot" or "shot FIRST LAST" for single rows
      bool range = sscanf(line, "shot %d %d", &a, &b) == 2;
      reqShotFrom = range ? a : 0;
      reqShotTo = range ? b : 999;
      reqShot = true;
    }
    else if (sscanf(line, "tap %d %d", &a, &b) == 2) { reqTapX = a; reqTapY = b; }
    else if (sscanf(line, "screen %d", &a) == 1) reqScreen = a;
    else if (!strcmp(line, "rot auto")) reqRot = 4;
    else if (sscanf(line, "rot %d", &a) == 1) reqRot = a & 3;
    else if (!strcmp(line, "cfgtest")) {             // a small configuration to try the Skripte page with
      const char *err = Configs::save("Demo", "show version\n@pause 1\nshow clock\n", "");
      Serial.printf("[DIAG] Konfiguration \"Demo\": %s\n", err ? err : "gespeichert");
    }
    else if (!strcmp(line, "bat")) Serial.printf("[DIAG] Akku: %s\n", Power::diag().c_str());
    else if (sscanf(line, "drag %d %d %d %d", &a, &b, &a2, &b2) == 4) {
      reqDrag[0] = a; reqDrag[1] = b; reqDrag[2] = a2; reqDrag[3] = b2;
      reqDragGo = true;
    }
    else if (sscanf(line, "saver %d", &a) == 1) Power::forceSaver(a < 0 ? -1 : a ? 1 : 0);
    else if (!strncmp(line, "poweroff", 8)) powerOff("Debug-Konsole", sscanf(line, "poweroff %d", &a) == 1 ? a : 0);
    else if (!strcmp(line, "wake")) reqWake = true;
    else if (!strcmp(line, "off")) reqOff = true;
  }
}

// ============================================================ UI task side
// From here on everything runs in the UI task and reads only its own copies.

static Snapshot S;                       // the snapshot this frame is drawn from
static char grid[TERM_MAX_ROWS][TERM_MAX_COLS + 1];
static int gridX = 0, gridY = 0;
static char msg1[40], msg2[40];

static bool on = false;
static uint8_t screen = S_STATUS, selPort = 0, rot = 3;
static int W = 320, H = 240;
static int termColsUi = TERM_MAX_COLS, termRowsUi = 19;
static uint32_t lastActivity = 0;
static bool dirty = true;

// sub-states of the pages with lists and the keyboard
enum SetupMode : uint8_t { SETUP_MAIN = 0, SETUP_NETS, SETUP_PASS, SETUP_CONFIRM };
static uint8_t confirmId = 0;            // what "Ja" on the confirmation page carries out
static const char *confirm1 = "", *confirm2 = "";
static uint8_t setupMode = SETUP_MAIN;
static uint8_t listPage = 0;             // page of the list shown (scripts or networks)
static int8_t cfgSel = -1;               // script picked, waiting for "Abspielen"
static bool playAcked = true;            // the result of the last playback has been dismissed
static char netSsid[33];                 // network being joined
static char kbText[64];                  // what has been typed
static uint8_t kbLayer = 0;              // 0 lower, 1 upper, 2 symbols

static bool modal() { return screen == S_SETUP && setupMode != SETUP_MAIN; }

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

// WiFi QR payload needs \ ; , : " escaped
static String qrEscape(const char *s) {
  String o;
  for (; *s; s++) {
    if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"') o += '\\';
    o += *s;
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
    if (S.portOn[p]) { selPort = p; break; }
  }
  uiPort = selPort;
}

static void uiWake() {
  lastActivity = millis();
  if (on) return;
  busLock();
  lcd.wakeup();
  busUnlock();
  on = true;
  uiOn = true;
  dirty = true;
  reqBrightness = true;
}


static void uiOff() {
  if (!on) return;
  lcd.setBrightness(0);
  litAt = 0;
  busLock();
  lcd.sleep();
  busUnlock();
  on = false;
  uiOn = false;
}

static int scrollY;                      // how far the page content is scrolled (see "scrolling" below)

// ---- canvas, rotation, sending only what changed
static const int BAND_H = 16;
static uint32_t bandHash[320 / BAND_H + 1];
static bool forceFull = true;

static void setRotation(uint8_t r) {
  rot = r & 3;
  uiRot = rot;
  busLock();
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
  busUnlock();
  int top = HEAD_H + 2, bottom = H - NAV_H - BTN_H - 4;
  termColsUi = min((W - 4) / TERM_CW, TERM_MAX_COLS);
  termRowsUi = min((bottom - top) / TERM_CH, TERM_MAX_ROWS);
  uiTermCols = termColsUi;
  uiTermRows = termRowsUi;
  uiGeometry = uiGeometry + 1;           // the main loop rebuilds the terminal for the new size
  scrollY = 0;
  forceFull = true;
  dirty = true;
}

// Compare the canvas band by band with what the panel already shows and send the
// difference. A status screen then costs a few hundred pixels per update, not 76800.
static void flush() {
  if (g != &canvas) return;              // direct mode has drawn on the panel already
  const uint8_t *buf = (const uint8_t *)canvas.getBuffer();
  size_t stride = canvas.bufferLength() / H;
  int bands = (H + BAND_H - 1) / BAND_H;
  bool changed[320 / BAND_H + 1];
  bool any = false;
  for (int b = 0; b < bands; b++) {
    int rows = min(BAND_H, H - b * BAND_H);
    const uint32_t *p = (const uint32_t *)(buf + (size_t)b * BAND_H * stride);
    size_t words = rows * stride / 4;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u;
    changed[b] = forceFull || h != bandHash[b];
    bandHash[b] = h;
    any |= changed[b];
  }
  forceFull = false;
  if (!any) return;
  busLock();
  for (int b = 0; b < bands; b++) {
    if (!changed[b]) continue;
    int first = b;
    while (b + 1 < bands && changed[b + 1]) b++;
    int y = first * BAND_H, h = min((b + 1) * BAND_H, H) - y;
    lcd.setClipRect(0, y, W, h);
    canvas.pushSprite(0, 0);
    statBands = statBands + (b - first + 1);
  }
  lcd.clearClipRect();
  busUnlock();
  statPushes = statPushes + 1;
}

// ---- accelerometer (QMI8658)
static uint8_t imuAddr = 0;
static int16_t ax = 0, ay = 0, az = 0;
static uint32_t lastImu = 0;
static uint8_t rotCandidate = 0xFF, rotVotes = 0;
static bool rotForced = false;           // debug console: keep the rotation it asked for

static bool imuBegin() {
  for (uint8_t a : {0x6B, 0x6A}) {
    auto who = lgfx::i2c::readRegister8(0, a, 0x00);
    if (who.has_value() && who.value() == 0x05) { imuAddr = a; break; }
  }
  if (!imuAddr) return false;
  // After powerOff() the sensor's oscillator is stopped and it keeps its supply
  // while the processor sleeps: switch it on again first, or the reset is not
  // carried out and every reading comes back as FFFF.
  lgfx::i2c::writeRegister8(0, imuAddr, 0x02, 0x40);
  delay(5);
  lgfx::i2c::writeRegister8(0, imuAddr, 0x60, 0xB0);     // soft reset
  delay(50);
  lgfx::i2c::writeRegister8(0, imuAddr, 0x02, 0x40);     // CTRL1: register auto increment, little endian
  lgfx::i2c::writeRegister8(0, imuAddr, 0x03, 0x23);     // CTRL2: +-8 g (4096 LSB/g)
  lgfx::i2c::writeRegister8(0, imuAddr, 0x04, 0x43);     // CTRL3: gyroscope range/rate
  lgfx::i2c::writeRegister8(0, imuAddr, 0x08, 0x03);     // CTRL7: accelerometer + gyroscope on
  return true;
}

static void imuPoll(uint32_t now) {
  if (!imuAddr || now - lastImu < 200) return;
  lastImu = now;
  uint8_t d[6];
  if (!lgfx::i2c::readRegister(0, imuAddr, 0x35, d, 6).has_value()) return;
  int16_t x = d[0] | d[1] << 8, y = d[2] | d[3] << 8, z = d[4] | d[5] << 8;
  bool moved = abs(x - ax) + abs(y - ay) + abs(z - az) > 1200;      // ~0.3 g between two samples
  ax = x; ay = y; az = z;
  imuX = x; imuY = y; imuZ = z;
  if (moved && !on) uiWake();
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
  if (want == 0xFF || want == rot || rotForced) { rotCandidate = 0xFF; return; }
  if (want != rotCandidate) { rotCandidate = want; rotVotes = 0; }
  if (++rotVotes < 3) return;
  rotCandidate = 0xFF;
  Serial.printf("[IMU]  x=%d y=%d z=%d -> Drehung %u\n", x, y, z, want);
  setRotation(want);
  uiWake();
}

// ---- touch + buttons
struct Btn { int16_t x, y, w, h; uint8_t id, arg; };
static Btn btns[64];
static uint8_t btnCount = 0;

static bool touchDown = false, touchSwallow = false;
static int touchX0 = 0, touchY0 = 0, touchX = 0, touchY = 0;
static uint32_t lastTouchSeen = 0, touchStart = 0;
// The CST816 does not answer every poll while a finger rests on it. Without this
// hold time one touch falls apart into several taps.
static const uint32_t TOUCH_RELEASE_MS = 120;

// ---- scrolling. A page draws its content between cTop and cBot, shifted up by
// scrollY and clipped to that window, and reports how long it turned out to be.
// Whatever does not fit is reached by dragging; nothing is cut off any more.
static int cTop = 0, cBot = 0;
static int contentH = 0;
static bool inContent = false;
static bool scrolling = false;           // this touch has become a drag
static int scroll0 = 0;                  // scrollY when the finger came down
static bool marquee = false;             // a text on this frame is moving: keep redrawing
static int simStep = -1, simX0 = 0, simY0 = 0, simX1 = 0, simY1 = 0;   // finger simulated by the debug console
static const int SIM_STEPS = 10;

static int maxScroll() { return max(0, contentH - (cBot - cTop)); }

static bool inside(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void addHotspot(int x, int y, int w, int h, uint8_t id, uint8_t arg = 0) {
  if (inContent) {                       // of a button scrolled half out of sight only the visible part counts
    int y1 = max(y, cTop), y2 = min(y + h, cBot);
    if (y2 - y1 < 12) return;
    y = y1;
    h = y2 - y1;
  }
  if (btnCount < sizeof(btns) / sizeof(btns[0]))
    btns[btnCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id, arg};
}

static bool isDown(int x, int y, int w, int h) {
  Btn b = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, 0, 0};
  return touchDown && !touchSwallow && !scrolling && inside(b, touchX0, touchY0) && inside(b, touchX, touchY);
}

static void button(int x, int y, int w, int h, const char *label, uint8_t id, uint32_t textColor = C_TEXT,
                   uint8_t arg = 0, const lgfx::IFont *font = &fonts::DejaVu12) {
  bool down = isDown(x, y, w, h);
  g->fillRoundRect(x, y, w, h, 6, down ? C_BTN_DOWN : C_BTN);
  g->setFont(font);
  if (g->textWidth(label) > w - 6) g->setFont(&fonts::DejaVu9);      // a long label on a narrow button
  g->setTextDatum(lgfx::middle_center);
  g->setTextColor(down ? C_TEXT : textColor);
  g->drawString(fit(label, w - 4).c_str(), x + w / 2, y + h / 2);
  addHotspot(x, y, w, h, id, arg);
}

struct BtnDef { const char *label; uint8_t id; uint32_t color; uint8_t arg; };

// The buttons at the bottom of a page. They go on two rows when their labels do
// not fit side by side (upright, the screen is 240 pixels wide). Returns the top
// of the area they take, which is where the page content has to end.
static int buttonRow(const BtnDef *defs, int n, int maxRows = 2) {
  int bottom = H - (modal() ? 0 : NAV_H) - 2;
  g->setFont(&fonts::DejaVu12);
  int w = (W - BTN_GAP * (n + 1)) / n;
  bool fits = true;
  for (int i = 0; i < n; i++) fits &= g->textWidth(defs[i].label) + 8 <= w;
  int rows = maxRows > 1 && n > 1 && !fits ? 2 : 1;
  int top = bottom - rows * BTN_H - (rows - 1) * BTN_GAP;
  int first = rows == 2 ? (n + 1) / 2 : n;
  for (int r = 0; r < rows; r++) {
    int from = r ? first : 0, cnt = r ? n - first : first;
    w = (W - BTN_GAP * (cnt + 1)) / cnt;
    for (int i = 0; i < cnt; i++)
      button(BTN_GAP + i * (w + BTN_GAP), top + r * (BTN_H + BTN_GAP), w, BTN_H, defs[from + i].label, defs[from + i].id,
             defs[from + i].color, defs[from + i].arg);
  }
  return top;
}

static void gotoScreen(uint8_t s) {
  screen = s % S_COUNT;
  uiScreen = screen;
  setupMode = SETUP_MAIN;
  listPage = 0;
  cfgSel = -1;
}

static void sendCmd(uint8_t id, uint8_t arg = 0) {
  uint32_t cmd = (uint32_t)id << 16 | (uint32_t)arg << 8 | selPort;
  xQueueSend(cmdQueue, &cmd, 0);
}

static void pressed(uint8_t id, uint8_t arg) {
  switch (id) {
    case B_PREV: gotoScreen(screen + S_COUNT - 1); return;
    case B_NEXT: gotoScreen(screen + 1); return;
    case B_PORT: nextPort(); return;
    case B_LIST_PREV: if (listPage) listPage--; return;
    case B_LIST_NEXT: listPage++; return;            // the drawing code keeps it in range
    case B_CFG_ROW: cfgSel = arg; return;
    case B_CFG_CANCEL: cfgSel = -1; return;
    case B_PLAY_ACK: playAcked = true; return;
    case B_PLAY: playAcked = false; cfgSel = -1; break;
    case B_NET_OPEN: setupMode = SETUP_NETS; listPage = 0; sendCmd(B_SCAN); return;
    case B_BACK:
      setupMode = setupMode == SETUP_PASS || (setupMode == SETUP_CONFIRM && confirmId == B_JOIN) ? SETUP_NETS : SETUP_MAIN;
      return;
    case B_NET_ROW:
      if (arg >= S.netCount) return;
      strlcpy(netSsid, S.net[arg].ssid, sizeof(netSsid));
      kbText[0] = 0;
      kbLayer = 0;
      if (S.net[arg].locked) { setupMode = SETUP_PASS; return; }
      // open network: nothing to type, but never joined by a single (stray) tap
      confirmId = B_JOIN;
      confirm1 = "Offenes Netz ohne Verschluesselung.";
      confirm2 = "Verbinden? Das Geraet startet neu.";
      setupMode = SETUP_CONFIRM;
      return;
    case B_ASK_FORGET:
      confirmId = B_FORGET;
      confirm1 = "WLAN-Client ausschalten?";
      confirm2 = "Das Geraet startet neu.";
      setupMode = SETUP_CONFIRM;
      return;
    case B_ASK_OFF:
      confirmId = B_POWEROFF;
      confirm1 = "Geraet ausschalten?";
      confirm2 = "Einschalten mit der BOOT-Taste.";
      setupMode = SETUP_CONFIRM;
      return;
    case B_CONFIRM:
      id = confirmId;
      setupMode = SETUP_MAIN;
      break;
    case K_CHAR: {
      size_t n = strlen(kbText);
      if (n < sizeof(kbText) - 1) { kbText[n] = arg; kbText[n + 1] = 0; }
      return;
    }
    case K_BKSP: { size_t n = strlen(kbText); if (n) kbText[n - 1] = 0; return; }
    case K_SHIFT: kbLayer = kbLayer == 1 ? 0 : 1; return;
    case K_SYM: kbLayer = kbLayer == 2 ? 0 : 2; return;
    case K_OK:
      if (strlen(kbText) < 8) return;                // a WPA key has 8 to 63 characters
      id = B_JOIN;
      break;
  }
  if (id == B_JOIN) {
    Lock l(stateMux);
    strlcpy(joinSsid, netSsid, sizeof(joinSsid));
    strlcpy(joinPass, kbText, sizeof(joinPass));
    memset(kbText, 0, sizeof(kbText));
    setupMode = SETUP_MAIN;
  }
  if (id >= FIRST_LOOP_CMD) sendCmd(id, arg);        // everything else belongs to the main loop
}

static void press(uint8_t id, uint8_t arg = 0) {
  uint8_t s = screen, m = setupMode;
  dirty = true;
  pressed(id, arg);
  if (s != screen || m != setupMode) scrollY = 0;    // a different page starts at its top
}

static void tapAt(int x, int y) {
  for (int i = btnCount - 1; i >= 0; i--)            // drawn last = on top
    if (inside(btns[i], x, y)) { press(btns[i].id, btns[i].arg); return; }
}

static void touchPoll(uint32_t now) {
  int32_t x = 0, y = 0;
  uint32_t t0 = millis();
  bool raw = lcd.getTouch(&x, &y) > 0;
  uint32_t took = millis() - t0;
  if (took > statTouchMsMax) statTouchMsMax = took;
  if (simStep >= 0) {                                // the debug console's finger takes the same path as a real one
    raw = true;
    x = simX0 + (simX1 - simX0) * simStep / SIM_STEPS;
    y = simY0 + (simY1 - simY0) * simStep / SIM_STEPS;
    if (++simStep > SIM_STEPS) simStep = -1;
  }
  if (raw) {
    lastTouchSeen = now;
    if (!touchDown) {
      touchDown = true;
      touchStart = now;
      touchX0 = x; touchY0 = y;
      touchSwallow = !on;                // the first touch on a dark screen only wakes it
      scrolling = false;
      scroll0 = scrollY;
      dirty = true;
    }
    touchX = x; touchY = y;
    int dx = x - touchX0, dy = y - touchY0;
    if (!scrolling && !touchSwallow && abs(dy) > 10 && abs(dy) > abs(dx) && touchY0 >= cTop && touchY0 < cBot && maxScroll())
      scrolling = true;
    if (scrolling) {
      int to = constrain(scroll0 - dy, 0, maxScroll());
      if (to != scrollY) { scrollY = to; dirty = true; }
    }
    uiWake();
    return;
  }
  if (!touchDown || now - lastTouchSeen < TOUCH_RELEASE_MS) return;
  touchDown = false;
  dirty = true;
  statTouches = statTouches + 1;
  lastTouchX = touchX0; lastTouchY = touchY0;
  if (touchSwallow) return;
  if (scrolling) { scrolling = false; return; }
  int dx = touchX - touchX0, dy = touchY - touchY0;
  if (abs(dx) > 50 && abs(dx) > abs(dy) && !modal()) press(dx < 0 ? B_NEXT : B_PREV);   // swipe left = next screen
  else if (abs(dx) < 16 && abs(dy) < 16) tapAt(touchX0, touchY0);
}

// ---- text that is longer than its place
static const int LINE_H = 15;            // line pitch of DejaVu12

// Break a text into lines of at most maxW pixels in the current font: at spaces
// where there are any, else in the middle of a word. With words = false every
// character is kept (a password must show its blanks).
static int wrap(const char *text, int maxW, String *out, int maxLines, bool words = true) {
  int n = 0;
  String cur;
  for (const char *p = text; *p && n < maxLines; p++) {
    cur += *p;
    if (cur.length() < 2 || g->textWidth(cur.c_str()) <= maxW) continue;
    int sp = words ? cur.lastIndexOf(' ') : -1;
    if (sp > 0) {
      out[n++] = cur.substring(0, sp);
      cur = cur.substring(sp + 1);
    } else {
      out[n++] = cur.substring(0, cur.length() - 1);
      cur = cur.substring(cur.length() - 1);
    }
  }
  if (cur.length() && n < maxLines) out[n++] = cur;
  return n;
}

// Draws wrapped text in the current font and colour, returns the height used.
static int wrapped(const char *text, int x, int y, int maxW, bool words = true) {
  String lines[8];
  int n = wrap(text, maxW, lines, 8, words);
  g->setTextDatum(lgfx::top_left);
  for (int i = 0; i < n; i++) g->drawString(lines[i].c_str(), x, y + i * LINE_H);
  return max(1, n) * LINE_H;
}

// One line that must stay one line (a list entry, a title): if it is too long
// it travels to its end and back, slowly enough to read.
static void drawMarquee(const char *text, int x, int yMid, int maxW) {
  g->setTextDatum(lgfx::middle_left);
  int over = g->textWidth(text) - maxW;
  if (over <= 0) { g->drawString(text, x, yMid); return; }
  marquee = true;
  const uint32_t REST = 1500, PER_PX = 33;
  uint32_t t = millis() % (2 * REST + 2 * over * PER_PX);
  int off = t < REST ? 0 : t < REST + over * PER_PX ? (t - REST) / PER_PX
          : t < 2 * REST + over * PER_PX ? over : over - (t - 2 * REST - over * PER_PX) / PER_PX;
  int32_t cx, cy, cw, ch;
  g->getClipRect(&cx, &cy, &cw, &ch);
  g->setClipRect(x, yMid - 12, maxW, 24);
  g->drawString(text, x - off, yMid);
  g->setClipRect(cx, cy, cw, ch);
}

// ---- drawing
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
  if (modal()) {                         // a sub-page: the arrow leads back, nothing else leaves it
    g->setTextColor(C_TEXT);
    const char *title = setupMode == SETUP_NETS ? "WLAN waehlen"
                      : setupMode == SETUP_CONFIRM && confirmId == B_FORGET ? "WLAN-Client"
                      : setupMode == SETUP_CONFIRM && confirmId == B_POWEROFF ? "Ausschalten" : netSsid;
    if (g->textWidth(title) <= W - 88) g->drawString(title, W / 2, HEAD_H / 2);
    else drawMarquee(title, 44, HEAD_H / 2, W - 52);
    addHotspot(0, 0, 64, HEAD_H + 6, B_BACK);
    return;
  }
  g->drawString(">", W - 16, HEAD_H / 2);
  g->setTextColor(C_TEXT);
  g->drawString(TITLES[screen], W / 2, HEAD_H / 2);
  int titleLeft = W / 2 - g->textWidth(TITLES[screen]) / 2;
  addHotspot(0, 0, 64, HEAD_H + 6, B_PREV);
  addHotspot(W - 64, 0, 64, HEAD_H + 6, B_NEXT);

  // link lamps: web/TCP client, Bluetooth, recording
  int x = W - 44;
  if (S.recording[selPort]) { g->fillCircle(x, HEAD_H / 2, 4, C_RED); x -= 13; }
  if (S.bleState != Ble::OFF) { g->fillCircle(x, HEAD_H / 2, 4, S.bleState == Ble::CONNECTED ? C_BLUE : C_LINE); x -= 13; }
  if (S.webClients || S.tcpConnected) g->fillCircle(x, HEAD_H / 2, 4, C_GREEN);

  // battery, left of the title: symbol with the level, a bolt while charging,
  // and the percentage where the title leaves room for it
  if (S.batMeasured && S.batPresent) {
    bool ext = S.batCharge != Power::ON_BATTERY;
    uint32_t col = S.batLow && !ext ? C_RED : ext ? C_GREEN : C_DIM;
    int bx = 32, by = HEAD_H / 2 - 6;
    g->drawRect(bx, by, 22, 12, col);
    g->fillRect(bx + 22, by + 3, 2, 6, col);
    g->fillRect(bx + 2, by + 2, (S.batPct * 18 + 50) / 100, 8, col);
    if (S.batCharge == Power::CHARGING) {
      g->fillTriangle(bx + 13, by + 1, bx + 7, by + 7, bx + 11, by + 7, C_TEXT);
      g->fillTriangle(bx + 9, by + 11, bx + 15, by + 5, bx + 11, by + 5, C_TEXT);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", S.batPct);
    g->setFont(&fonts::DejaVu9);
    if (bx + 28 + g->textWidth(buf) + 4 <= titleLeft) {
      g->setTextDatum(lgfx::middle_left);
      g->setTextColor(col);
      g->drawString(buf, bx + 28, HEAD_H / 2);
    }
  }
}

static void drawNav() {
  if (modal()) return;
  int y = H - NAV_H / 2, x0 = W / 2 - (S_COUNT - 1) * 7;
  for (int i = 0; i < S_COUNT; i++) g->fillCircle(x0 + i * 14, y, i == screen ? 3 : 2, i == screen ? C_ACCENT : C_LINE);
}

// ---- page content: label/value rows and whatever else a page draws at rowY
static int rowY = 0;

static void contentBegin(int top, int bottom) {
  cTop = top;
  cBot = bottom;
  inContent = true;
  g->setClipRect(0, top, W, bottom - top);
  rowY = top + 8 - scrollY;
}

static void contentEnd() {
  contentH = rowY + scrollY - cTop + 4;
  inContent = false;
  g->clearClipRect();
  int vis = cBot - cTop, most = maxScroll();
  if (scrollY > most) { scrollY = most; dirty = true; }       // the content got shorter
  if (!most) return;
  int h = max(20, vis * vis / contentH);                      // there is more: say so, and where we are
  g->fillRect(W - 4, cTop, 3, vis, C_PANEL);
  g->fillRect(W - 4, cTop + (vis - h) * scrollY / most, 3, h, C_ACCENT);
}

// A long value continues on the next line instead of being cut off.
static bool row(const char *label, const String &value, uint32_t color = C_TEXT) {
  g->setFont(&fonts::DejaVu12);
  String lines[8];
  int n = max(1, wrap(value.c_str(), W - 80 - 10, lines, 8));
  if (rowY + n * LINE_H > cTop && rowY < cBot) {
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    g->drawString(label, 8, rowY);
    g->setTextColor(color);
    for (int i = 0; i < n; i++) g->drawString(lines[i].c_str(), 80, rowY + i * LINE_H);
  }
  rowY += n * LINE_H + 4;
  return true;
}

static String clientsLine() {
  String s = "Web " + String(S.webClients) + "  TCP " + String(S.tcpConnected ? 1 : 0);
  if (S.bleState != Ble::OFF) s += String("  BT ") + (S.bleState == Ble::CONNECTED ? "1" : "0");
  return s;
}

static String batteryLine() {
  if (S.batPresent) {
    String s = String(S.batPct) + "%  " + String(S.batMv) + " mV";
    if (S.batCharge == Power::CHARGING) s += "  laedt";
    else if (S.batCharge == Power::FULL) s += "  voll";
    return s;
  }
  return S.batMeasured ? String("keiner (USB)") : String("keine Messung");
}

static String sdLine() {
  if (!S.sdMounted) return String(S.sdType);
  return String(S.sdType) + "  " + String(S.sdUsedMb) + " / " + String(S.sdTotalMb) + " MB";
}

static String lanLine() {
  if (!S.staConfigured) return String("aus");
  return S.staConnected ? String(S.staIp) : String("verbinde...");
}

static void screenStatus() {
  BtnDef defs[4] = {{"Baud", B_BAUD, C_TEXT}, {"Auto-Baud", B_AUTO, C_TEXT}, {"BREAK", B_BREAK, C_AMBER}};
  int n = 3;
  static char portLabel[8];
  if (S.ports > 1) {
    snprintf(portLabel, sizeof(portLabel), "Port %u", selPort + 1);
    defs[n++] = {portLabel, B_PORT, C_ACCENT};
  }
  contentBegin(HEAD_H, buttonRow(defs, n) - 2);

  int y = rowY - 2;
  g->setTextDatum(lgfx::top_left);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  g->drawString(fit(S.portName[selPort], W - 16).c_str(), 8, y);

  g->setFont(&fonts::DejaVu24);
  if (S.autobaud[selPort]) {
    g->setTextColor(C_AMBER);
    g->drawString("Auto-Baud...", 8, y + 16);
  } else {
    g->setTextColor(C_ACCENT);
    g->drawString(S.serial[selPort], 8, y + 16);
  }

  y += 56;
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->setTextDatum(lgfx::middle_left);
  dot(14, y, S.rxActive[selPort], C_GREEN);
  g->drawString(("RX " + fmtBytes(S.rx[selPort])).c_str(), 26, y);
  dot(W / 2 + 6, y, S.txActive[selPort], C_AMBER);
  g->drawString(("TX " + fmtBytes(S.tx[selPort])).c_str(), W / 2 + 18, y);
  g->drawFastHLine(8, y + 16, W - 16, C_LINE);

  rowY = y + 24;
  row("WLAN", S.apSsid);
  row("IP", S.apIp);
  row("LAN", lanLine());
  row("Clients", clientsLine());
  row("Akku", batteryLine());
  row("SD", sdLine());
  contentEnd();
}

static void screenTerm() {
  BtnDef defs[5] = {{"Enter", B_ENTER, C_TEXT}, {"Ctrl-C", B_CTRLC, C_TEXT}, {"BREAK", B_BREAK, C_AMBER}};
  int n = 3;
  if (S.sdMounted) defs[n++] = {S.recording[selPort] ? "Stop" : "REC", B_REC, C_RED};
  static char portLabel[8];
  if (S.ports > 1) {
    snprintf(portLabel, sizeof(portLabel), "P%u", selPort + 1);
    defs[n++] = {portLabel, B_PORT, C_ACCENT};
  }
  int top = HEAD_H + 2, bottom = buttonRow(defs, n) - 2;

  // the buttons may take one row or two: the terminal gets whatever is left
  int rows = constrain((bottom - top) / TERM_CH, 1, TERM_MAX_ROWS);
  if (rows != termRowsUi) {
    termRowsUi = rows;
    uiTermRows = rows;
    uiGeometry = uiGeometry + 1;         // the main loop rebuilds the grid for the new size
  }
  g->setFont(&fonts::Font0);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_TERM);
  for (int r = 0; r < termRowsUi; r++)
    if (grid[r][0]) g->drawString(grid[r], 2, top + r * TERM_CH);
  if ((millis() / 500) % 2 && gridY < termRowsUi)
    g->fillRect(2 + min(gridX, termColsUi - 1) * TERM_CW, top + gridY * TERM_CH, TERM_CW, TERM_CH, C_TERM);
}

// QR code with two labelled lines: beside it in landscape, below it in portrait
static void screenQr(const String &payload, const char *l1, const String &v1, const char *l2, const String &v2) {
  contentBegin(HEAD_H, H - NAV_H);
  int top = rowY - 2, avail = cBot - cTop - 12;
  bool wide = W > H;
  int size = wide ? min(avail, W - 142) : min(W - 24, avail - 76);   // beside it, an IP address still fits on one line
  int qx = wide ? 8 : (W - size) / 2;
  g->fillRoundRect(qx, top, size, size, 6, 0xFFFFFFu);
  g->qrcode(payload.c_str(), qx + 6, top + 6, size - 12, 1);

  int tx = wide ? qx + size + 10 : 8, ty = wide ? top + 4 : top + size + 8, tw = W - tx - 10;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  for (int i = 0; i < 2; i++) {
    g->setTextColor(C_DIM);
    g->drawString(i ? l2 : l1, tx, ty);
    g->setTextColor(C_TEXT);
    ty += LINE_H + wrapped((i ? v2 : v1).c_str(), tx, ty + LINE_H, tw, false) + 6;   // every character: it gets typed in
  }
  rowY = max(top + size, ty);
  contentEnd();
}

static void screenBt() {
  uint8_t s = S.bleState;
  const char *text = s == Ble::OFF ? "Aus" : s == Ble::ADVERTISING ? "Bereit, wartet auf Verbindung"
                   : s == Ble::PAIRING ? "Kopplung: PIN am Handy eingeben" : "Verbunden";
  uint32_t color = s == Ble::OFF ? C_DIM : s == Ble::CONNECTED ? C_BLUE : s == Ble::PAIRING ? C_AMBER : C_GREEN;
  BtnDef def = {s == Ble::OFF ? "Bluetooth einschalten" : "Bluetooth ausschalten", B_BT, C_TEXT};
  contentBegin(HEAD_H, buttonRow(&def, 1) - 2);

  g->setFont(&fonts::DejaVu12);
  g->setTextColor(color);
  rowY += wrapped(text, 8, rowY, W - 20) + 6;

  if (s != Ble::OFF) {
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)S.blePin);
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    g->drawString("PIN", 8, rowY + 6);
    g->setFont(&fonts::DejaVu24);
    g->setTextColor(C_TEXT);
    g->drawString(pin, 80, rowY);
    rowY += 34;
  }
  row("Name", S.apSsid);
  row("Dienst", "Nordic UART (BLE)");
  row("Port", String(S.portName[0]) + ", " + S.serial[0]);
  contentEnd();
}

static void screenInfo() {
  int bottom = H - NAV_H;
  if (HAS_SDCARD && !S.sdMounted) {
    BtnDef def = {"SD-Karte einbinden", B_SD, C_TEXT};
    bottom = buttonRow(&def, 1) - 2;
  }
  contentBegin(HEAD_H, bottom);
  String tcp = !S.tcpEnabled ? String("aus")
             : S.ports > 1 ? ":" + String(RAW_TCP_PORT) + "-" + String(RAW_TCP_PORT + MAX_PORTS - 1)
             : ":" + String(RAW_TCP_PORT);
  char imu[40];
  if (imuAddr) snprintf(imu, sizeof(imu), "Drehung %u", rot);
  else strlcpy(imu, "Sensor nicht gefunden", sizeof(imu));
  row("Firmware", String(FW_VERSION) + "  up " + fmtUptime(millis() / 1000));
  row("Host", String(S.hostname) + ".local");
  row("Hotspot", String(S.apStations) + " Geraete");
  row("Clients", clientsLine());
  row("Raw-TCP", tcp);
  row("Akku", batteryLine());
  row("SD", sdLine());
  row("Lage", imu);
  row("Board", BOARD_NAME);
  contentEnd();
}

// one bar with a caption, e.g. "RAM   [#####     ]  201 von 320 kB"
static void bar(const char *label, int pct, const String &text, uint32_t color) {
  pct = constrain(pct, 0, 100);
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_DIM);
  g->drawString(label, 8, rowY + 2);
  int x = 64, w = W - x - 10;
  g->fillRoundRect(x, rowY, w, 16, 4, C_PANEL);
  if (pct) g->fillRoundRect(x, rowY, max(8, w * pct / 100), 16, 4, color);
  g->setTextColor(C_TEXT);
  g->setTextDatum(lgfx::middle_center);
  g->drawString(fit(text, w - 4).c_str(), x + w / 2, rowY + 9);
  rowY += 22;
}

static uint32_t loadColor(int pct) { return pct >= 85 ? C_RED : pct >= 60 ? C_AMBER : C_GREEN; }

static void screenSystem() {
  contentBegin(HEAD_H, H - NAV_H);
  bar("CPU 0", S.cpu[0], String(S.cpu[0]) + " %  Funk + Anzeige", loadColor(S.cpu[0]));
  bar("CPU 1", S.cpu[1], String(S.cpu[1]) + " %  Bruecke", loadColor(S.cpu[1]));
  int ram = S.heapTotal ? 100 - (int)((uint64_t)S.heapFree * 100 / S.heapTotal) : 0;
  bar("RAM", ram, String((S.heapTotal - S.heapFree) / 1024) + " von " + String(S.heapTotal / 1024) + " kB", loadColor(ram));
  if (S.psramTotal) {
    int ps = 100 - (int)((uint64_t)S.psramFree * 100 / S.psramTotal);
    bar("PSRAM", ps, String((S.psramTotal - S.psramFree) / 1024) + " von " + String(S.psramTotal / 1024) + " kB", C_BLUE);
  }
  rowY += 2;
  row("RAM frei", String(S.heapFree / 1024) + " kB, min. " + String(S.heapMin / 1024) + " kB");
  row("Block", String(S.heapBlock / 1024) + " kB am Stueck");
  row("Takt", "Pause " + String(S.loopMs) + " ms, Bild " + String(S.drawMs) + " ms");
  row("CPU", String(S.cpuMhz) + " MHz" + (S.saver ? ", Sparmodus (Akku)" : ""));
  row("Chip", String(S.tempC) + " C,  " + String(S.tasks) + " Tasks");
  row("Laufzeit", fmtUptime(millis() / 1000));
  contentEnd();
}

// ---- lists (stored configurations, WLAN networks): rows to tap, paged
static const int LIST_ROW_H = 34;

static int listRows(int top, int bottom) { return max(1, (bottom - top) / LIST_ROW_H); }

static void listRow(int index, int y, const char *text, const char *right, bool selected, uint8_t id) {
  bool down = isDown(4, y, W - 8, LIST_ROW_H - 2);
  g->fillRoundRect(4, y, W - 8, LIST_ROW_H - 2, 5, selected || down ? C_BTN_DOWN : C_PANEL);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_TEXT);
  int rw = right ? g->textWidth(right) + 10 : 0;
  drawMarquee(text, 12, y + LIST_ROW_H / 2 - 1, W - 24 - rw);
  if (right) {
    g->setTextDatum(lgfx::middle_right);
    g->setTextColor(selected || down ? C_TEXT : C_DIM);
    g->drawString(right, W - 12, y + LIST_ROW_H / 2 - 1);
  }
  addHotspot(4, y, W - 8, LIST_ROW_H - 2, id, index);
}

// one or two texts in the middle of the page, wrapped where the screen is narrow
static void centered(const char *l1, const char *l2, int yMid) {
  g->setFont(&fonts::DejaVu12);
  String a[4], b[4];
  int na = wrap(l1, W - 16, a, 4), nb = l2 ? wrap(l2, W - 16, b, 4) : 0;
  int y = yMid - ((na + nb) * LINE_H + (nb ? 4 : 0)) / 2;
  g->setTextDatum(lgfx::top_center);
  g->setTextColor(C_TEXT);
  for (int i = 0; i < na; i++, y += LINE_H) g->drawString(a[i].c_str(), W / 2, y);
  y += 4;
  g->setTextColor(C_DIM);
  for (int i = 0; i < nb; i++, y += LINE_H) g->drawString(b[i].c_str(), W / 2, y);
}

static void screenScripts() {
  static char label[24], pager[12];

  // a playback is running, or its result has not been dismissed yet
  if (S.playState == Player::RUNNING || (S.playState != Player::IDLE && !playAcked)) {
    bool running = S.playState == Player::RUNNING;
    BtnDef def = running ? BtnDef{"Stopp", B_STOP, C_RED, 0} : BtnDef{"OK", B_PLAY_ACK, C_TEXT, 0};
    contentBegin(HEAD_H, buttonRow(&def, 1) - 2);
    g->setFont(&fonts::DejaVu18);
    g->setTextColor(C_TEXT);
    drawMarquee(S.playName, 8, rowY + 10, W - 20);
    rowY += 26;
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    rowY += wrapped((String("auf ") + S.portName[S.playPort] + ",  Zeile " + S.playLine + " von " + S.playLines).c_str(),
                    8, rowY, W - 20) + 6;
    int pct = S.playLines ? S.playLine * 100 / S.playLines : 0;
    g->fillRoundRect(8, rowY, W - 20, 14, 4, C_PANEL);
    if (pct) g->fillRoundRect(8, rowY, max(8, (W - 20) * pct / 100), 14, 4,
                              running ? C_ACCENT : S.playState == Player::DONE ? C_GREEN : C_RED);
    rowY += 22;
    g->setTextColor(running ? C_TEXT : S.playState == Player::DONE ? C_GREEN : C_RED);
    rowY += wrapped(S.playResult, 8, rowY, W - 20);
    contentEnd();
    return;
  }

  int top = HEAD_H + 6, bottom = H - NAV_H - BTN_H - 6;
  if (!S.cfgCount) {
    centered("Keine Konfigurationen gespeichert", "Anlegen: Weboberflaeche, Reiter Konfig", (HEAD_H + H - NAV_H) / 2);
    return;
  }
  int rows = listRows(top, bottom), pages = (S.cfgCount + rows - 1) / rows;
  if (listPage >= pages) listPage = pages - 1;
  for (int i = 0; i < rows; i++) {
    int idx = listPage * rows + i;
    if (idx >= S.cfgCount) break;
    listRow(idx, top + i * LIST_ROW_H, S.cfg[idx], nullptr, idx == cfgSel, B_CFG_ROW);
  }
  if (cfgSel >= 0 && cfgSel < S.cfgCount) {          // picked: one more tap sends it, on purpose
    snprintf(label, sizeof(label), "Senden an Port %u", selPort + 1);
    BtnDef defs[2] = {{label, B_PLAY, C_AMBER, (uint8_t)cfgSel}, {"Abbrechen", B_CFG_CANCEL, C_TEXT, 0}};
    buttonRow(defs, 2, 1);
    return;
  }
  BtnDef defs[4];
  int n = 0;
  if (pages > 1) {
    snprintf(pager, sizeof(pager), "%u / %u", listPage + 1, pages);
    defs[n++] = {"<", B_LIST_PREV, C_TEXT, 0};
    defs[n++] = {pager, B_NONE, C_DIM, 0};
    defs[n++] = {">", B_LIST_NEXT, C_TEXT, 0};
  }
  if (S.ports > 1) {
    snprintf(label, sizeof(label), "Port %u", selPort + 1);
    defs[n++] = {label, B_PORT, C_ACCENT, 0};
  }
  if (n) buttonRow(defs, n, 1);
  else centered("Antippen, dann senden", nullptr, H - NAV_H - BTN_H / 2 - 2);
}

// ---- on-screen keyboard (WLAN key). QWERTZ, three layers, ten keys per row.
static const char *const KB_LAYERS[3][4] = {
    {"1234567890", "qwertzuiop", "asdfghjkl-", "yxcvbnm._@"},
    {"!\"#$%&/()=", "QWERTZUIOP", "ASDFGHJKL+", "YXCVBNM,;:"},
    {"1234567890", "!\"#$%&/()=", "?+*~'<>|\\^", "{}[]_-.,;:"},
};

static void keyboard(int top) {
  int kw = W / 10, x0 = (W - kw * 10) / 2;
  int kh = min(48, (H - top - 2) / 5);   // upright the keys are narrow, so at least make them tall
  char cap[2] = {0, 0};
  for (int r = 0; r < 4; r++) {
    const char *keys = KB_LAYERS[kbLayer][r];
    for (int c = 0; c < 10; c++) {
      cap[0] = keys[c];
      button(x0 + c * kw + 1, top + r * kh + 1, kw - 2, kh - 2, cap, K_CHAR, C_TEXT, keys[c], &fonts::DejaVu18);
    }
  }
  int y = top + 4 * kh + 1, h = kh - 2;
  button(x0 + 1, y, kw * 3 / 2 - 2, h, kbLayer == 1 ? "abc" : "ABC", K_SHIFT, C_ACCENT);
  button(x0 + kw * 3 / 2 + 1, y, kw * 3 / 2 - 2, h, kbLayer == 2 ? "abc" : "#+=", K_SYM, C_ACCENT);
  button(x0 + kw * 3 + 1, y, kw * 3 - 2, h, "Leer", K_CHAR, C_DIM, ' ');
  button(x0 + kw * 6 + 1, y, kw * 2 - 2, h, "<-", K_BKSP, C_AMBER);
  button(x0 + kw * 8 + 1, y, kw * 2 - 2, h, "OK", K_OK, strlen(kbText) >= 8 ? C_GREEN : C_LINE);
}

static const char *signalText(int rssi) { return rssi >= -60 ? "stark" : rssi >= -75 ? "mittel" : "schwach"; }

static void screenSetup() {
  static char pager[12], right[20];
  if (setupMode == SETUP_PASS) {
    int top = HEAD_H + 4;
    g->fillRoundRect(4, top, W - 8, 24, 4, C_PANEL);
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::middle_left);
    String shown = kbText;                           // long keys: show the end, that is where typing happens
    while (shown.length() > 1 && g->textWidth((shown + "_").c_str()) > W - 24) shown.remove(0, 1);
    if (kbText[0]) {
      g->setTextColor(C_TEXT);
      g->drawString((shown + "_").c_str(), 10, top + 12);
    } else {
      g->setTextColor(C_DIM);
      g->drawString(fit("WLAN-Schluessel (8-63 Zeichen)", W - 24).c_str(), 10, top + 12);
    }
    keyboard(top + 28);
    return;
  }

  if (setupMode == SETUP_CONFIRM) {
    BtnDef defs[2] = {{"Ja", B_CONFIRM, C_AMBER, 0}, {"Abbrechen", B_BACK, C_TEXT, 0}};
    centered(confirm1, confirm2, (HEAD_H + buttonRow(defs, 2, 1)) / 2);
    return;
  }

  if (setupMode == SETUP_NETS) {
    int top = HEAD_H + 6, bottom = H - BTN_H - 6;
    if (S.scanState == 1) centered("Suche Netze ...", nullptr, (top + bottom) / 2);
    else if (!S.netCount) centered("Kein Netz gefunden", nullptr, (top + bottom) / 2);
    int rows = listRows(top, bottom), pages = max(1, (S.netCount + rows - 1) / rows);
    if (listPage >= pages) listPage = pages - 1;
    for (int i = 0; S.scanState == 2 && i < rows; i++) {
      int idx = listPage * rows + i;
      if (idx >= S.netCount) break;
      snprintf(right, sizeof(right), "%s%s", S.net[idx].locked ? "" : "offen, ", signalText(S.net[idx].rssi));
      listRow(idx, top + i * LIST_ROW_H, S.net[idx].ssid, right, false, B_NET_ROW);
    }
    BtnDef defs[4];
    int n = 0;
    if (pages > 1) {
      snprintf(pager, sizeof(pager), "%u / %u", listPage + 1, pages);
      defs[n++] = {"<", B_LIST_PREV, C_TEXT, 0};
      defs[n++] = {pager, B_NONE, C_DIM, 0};
      defs[n++] = {">", B_LIST_NEXT, C_TEXT, 0};
    }
    defs[n++] = {"Neu suchen", B_SCAN, C_TEXT, 0};
    buttonRow(defs, n, 1);
    return;
  }

  // main page: brightness, display timeout, WLAN client, power off. Everything
  // is part of the scrolling content, so each control can have a full-size button.
  contentBegin(HEAD_H, H - NAV_H);
  const int bw = 58, right0 = W - 10;
  int y = rowY;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_DIM);
  g->drawString("Helligkeit", 8, y);
  y += 18;
  button(8, y, bw, BTN_H, "-", B_BRIGHT_DOWN, C_TEXT, 0, &fonts::DejaVu24);
  button(right0 - bw, y, bw, BTN_H, "+", B_BRIGHT_UP, C_TEXT, 0, &fonts::DejaVu24);
  int bx = 8 + bw + 8, bwid = right0 - bw - 8 - bx;
  g->fillRoundRect(bx, y + BTN_H / 2 - 7, bwid, 14, 4, C_PANEL);
  g->fillRoundRect(bx, y + BTN_H / 2 - 7, max(8, bwid * (S.brightness + 1) / 256), 14, 4, C_ACCENT);

  y += BTN_H + 10;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_DIM);
  g->drawString("Display aus", 8, y + BTN_H / 2);
  static char timeout[16];
  if (!S.timeoutS) strlcpy(timeout, "nie", sizeof(timeout));
  else if (S.timeoutS < 120) snprintf(timeout, sizeof(timeout), "nach %u s", S.timeoutS);
  else snprintf(timeout, sizeof(timeout), "nach %u min", S.timeoutS / 60);
  button(right0 - 120, y, 120, BTN_H, timeout, B_TIMEOUT);

  y += BTN_H + 10;
  g->drawFastHLine(8, y, right0 - 8, C_LINE);
  rowY = y + 8;
  row("WLAN", !S.staConfigured ? String("aus") : String(S.staSsid));
  if (S.staConfigured)
    row("Status", S.staConnected ? String("verbunden, ") + S.staIp : String("verbinde ..."), S.staConnected ? C_GREEN : C_AMBER);
  y = rowY + 4;
  if (S.staConfigured) {
    int w = (right0 - 8 - BTN_GAP) / 2;
    button(8, y, w, BTN_H, "Netz waehlen", B_NET_OPEN);
    button(8 + w + BTN_GAP, y, w, BTN_H, "WLAN-Client aus", B_ASK_FORGET, C_AMBER);
  } else {
    button(8, y, right0 - 8, BTN_H, "Netz waehlen", B_NET_OPEN);
  }
  y += BTN_H + 10;
  g->drawFastHLine(8, y, right0 - 8, C_LINE);
  y += 10;
  button(8, y, right0 - 8, BTN_H, "Ausschalten", B_ASK_OFF, C_RED);
  rowY = y + BTN_H;
  contentEnd();
}

// the overlay box; long texts wrap instead of running out of it
static void drawMessage() {
  const int maxW = W - 40;
  const lgfx::IFont *f1 = &fonts::DejaVu18;
  int h1 = 24;
  g->setFont(f1);
  if (g->textWidth(msg1) > maxW) { f1 = &fonts::DejaVu12; h1 = LINE_H; g->setFont(f1); }
  String a[3], b[3];
  int na = max(1, wrap(msg1, maxW, a, 3)), w = 0;
  for (int i = 0; i < na; i++) w = max(w, (int)g->textWidth(a[i].c_str()));
  g->setFont(&fonts::DejaVu12);
  int nb = msg2[0] ? wrap(msg2, maxW, b, 3) : 0;
  for (int i = 0; i < nb; i++) w = max(w, (int)g->textWidth(b[i].c_str()));
  w = min(w + 28, W - 8);
  int h = 20 + na * h1 + (nb ? 6 + nb * LINE_H : 0);
  int x = (W - w) / 2, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 8, C_PANEL);
  g->drawRoundRect(x, y, w, h, 8, C_ACCENT);
  g->setTextDatum(lgfx::top_center);
  g->setTextColor(C_TEXT);
  g->setFont(f1);
  y += 10;
  for (int i = 0; i < na; i++, y += h1) g->drawString(a[i].c_str(), W / 2, y);
  y += 6;
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  for (int i = 0; i < nb; i++, y += LINE_H) g->drawString(b[i].c_str(), W / 2, y);
}

static void draw(uint32_t now) {
  if (!S.portOn[selPort]) nextPort();
  btnCount = 0;
  marquee = false;
  contentH = 0;                          // a page without scrolling content leaves it at that
  if (g == &lcd) busLock();
  g->fillScreen(C_BG);
  drawHeader();
  switch (screen) {
    case S_TERM: screenTerm(); break;
    case S_WIFI:
      screenQr("WIFI:T:WPA;S:" + qrEscape(S.apSsid) + ";P:" + qrEscape(S.apPass) + ";;", "WLAN", S.apSsid, "Passwort",
               S.apPass);
      break;
    case S_URL:
      screenQr(String("http://") + S.apIp + "/", "Web-UI", S.apIp, S.staConnected ? "im LAN" : "Name",
               S.staConnected ? String(S.staIp) : String(S.hostname) + ".local");
      break;
    case S_BT:   screenBt(); break;
    case S_INFO: screenInfo(); break;
    case S_SYSTEM:  screenSystem(); break;
    case S_SCRIPTS: screenScripts(); break;
    case S_SETUP:   screenSetup(); break;
    default:     screenStatus(); break;
  }
  if (!contentH) { cTop = cBot = 0; scrollY = 0; }
  drawNav();
  if ((int32_t)(msgUntil - now) > 0) drawMessage();
  if (g == &lcd) busUnlock();
  flush();
  statFrames = statFrames + 1;
}

// Arduino's USB serial class drops data whenever it believes the host has gone
// (it is tuned for "never block"). The screenshot must arrive complete, so it
// feeds the USB FIFO directly and waits for room.
static void rawWrite(const char *data, size_t len) {
#if ARDUINO_USB_CDC_ON_BOOT
  uint32_t lastProgress = millis();
  while (len && millis() - lastProgress < 2000) {        // host really gone: give up
    if (!usb_serial_jtag_ll_txfifo_writable()) { vTaskDelay(1); continue; }
    size_t w = usb_serial_jtag_ll_write_txfifo((const uint8_t *)data, len);
    usb_serial_jtag_ll_txfifo_flush();
    data += w;
    len -= w;
    if (w) lastProgress = millis();
  }
#else
  Serial.write((const uint8_t *)data, len);
#endif
}

// The picture as text, for a developer who cannot see the panel: one line per
// row, run-length coded as "RRRR*n" (RGB565 hex, repeat count), with a checksum
// so the receiver knows which rows arrived intact.
static void screenshot() {
  if (g != &canvas) { Serial.println("[SHOT] kein Zeichenpuffer"); return; }
  static uint16_t px[320];
  static char out[320 * 8 + 32];
  Serial.printf("[SHOT] %d %d\n", W, H);
  Serial.flush();
  vTaskDelay(pdMS_TO_TICKS(20));
  for (int y = max(0, (int)reqShotFrom); y < H && y <= reqShotTo; y++) {
    canvas.readRect(0, y, W, 1, px);
    size_t o = snprintf(out, sizeof(out), "%03d:", y);
    uint32_t sum = 0;
    for (int x = 0; x < W;) {
      int n = 1;
      while (x + n < W && px[x + n] == px[x]) n++;
      o += snprintf(out + o, sizeof(out) - o, "%04X*%X,", px[x], n);
      sum = sum * 31 + px[x] * 7 + n;
      x += n;
    }
    o += snprintf(out + o, sizeof(out) - o, "#%04X\n", (unsigned)(sum & 0xFFFF));
    rawWrite(out, o);
  }
  rawWrite("[SHOT] end\n", 11);
}

static void uiTask(void *) {
  uint32_t lastDraw = 0, lastTick = 0xFFFFFFFF, seenTerm = 0, seenMsg = 0, lastStallWarn = 0;
  for (;;) {
    uint32_t now = millis();
    if (uiBeat && (int32_t)(now - uiBeat) > (int32_t)statUiGapMax) statUiGapMax = now - uiBeat;
    uiBeat = now;

    // requests from the main loop
    if (reqWake) { reqWake = false; uiWake(); }
    if (reqOff) { reqOff = false; uiOff(); }
    if (reqNext) { reqNext = false; press(B_NEXT); }
    if (reqScreen >= 0) { gotoScreen(reqScreen); reqScreen = -1; dirty = true; }
    if (reqRot >= 0) {                     // 0..3 stays until "rot auto" hands it back to the accelerometer
      rotForced = reqRot < 4;
      if (rotForced) setRotation(reqRot);
      reqRot = -1;
    }
    if (reqDragGo) {
      reqDragGo = false;
      uiWake();
      simX0 = reqDrag[0]; simY0 = reqDrag[1]; simX1 = reqDrag[2]; simY1 = reqDrag[3];
      simStep = 0;
      Serial.printf("[DIAG] Drag %d,%d -> %d,%d\n", simX0, simY0, simX1, simY1);
    }
    if (reqHalt) {                         // power-off: dark panel, sensor asleep, and nothing more from this task
      uiOff();
      if (imuAddr) {
        lgfx::i2c::writeRegister8(0, imuAddr, 0x08, 0x00);   // CTRL7: accelerometer and gyroscope off
        lgfx::i2c::writeRegister8(0, imuAddr, 0x02, 0x41);   // CTRL1: oscillator off
      }
      uiHalted = true;
      vTaskDelete(nullptr);
    }
    if (reqTapX >= 0) {
      int x = reqTapX, y = reqTapY;
      reqTapX = -1;
      uiWake();
      Serial.printf("[DIAG] Tap %d,%d\n", x, y);
      tapAt(x, y);
    }

    touchPoll(now);
    imuPoll(now);

    // the main loop not coming round is the one thing this task can report
    if (loopBeat && (int32_t)(now - loopBeat) > 3000 && now - lastStallWarn > 5000) {
      lastStallWarn = now;
      Serial.printf("[DIAG] Hauptschleife steht seit %lu ms\n", (unsigned long)(now - loopBeat));
    }

    // new data? copy it under the lock, then draw without holding anything
    bool changed = false;
    {
      Lock l(stateMux);
      static Snapshot n;
      n = shared;
      // what the current page does not show is no reason to redraw it
      if (screen != S_INFO) n.apStations = 0;
      if (screen != S_SYSTEM) {
        n.cpu[0] = n.cpu[1] = 0;
        n.heapFree = n.heapMin = n.heapBlock = n.psramFree = 0;
        n.tempC = 0; n.loopMs = n.drawMs = 0; n.tasks = 0;
      }
      if (memcmp(&n, &S, sizeof(S))) { S = n; changed = true; }
      if (seenMsg != msgVersion) {
        seenMsg = msgVersion;
        memcpy(msg1, msgShared1, sizeof(msg1));
        memcpy(msg2, msgShared2, sizeof(msg2));
        changed = true;
      }
      if (screen == S_TERM && seenTerm != termVersion) {
        seenTerm = termVersion;
        memcpy(grid, termShared, sizeof(grid));
        gridX = termX; gridY = termY;
        changed = true;
      }
    }
    bool msgShown = (int32_t)(msgUntil - now) > 0;
    if (reqBrightness) { reqBrightness = false; litAt = 0xFFFF; }
    if (on) {
      // The setting was made for OLED contrast, where 0 is still readable; a
      // backlight at 0 is simply dark, so keep a floor. On battery the light is
      // the largest consumer after the radio: an untouched screen drops to a
      // quarter after DIM_MS and comes back with the next touch.
      const int32_t DIM_MS = 15000;
      uint16_t lit = 24 + (uint16_t)S.brightness * 231 / 255;
      if (S.saver && !msgShown && (int32_t)(now - lastActivity) > DIM_MS) lit = max(10, lit / 4);
      if (lit != litAt) { litAt = lit; lcd.setBrightness(lit); }
    }

    static bool msgWasShown = false;
    if (msgShown && !on) uiWake();
    if (msgShown != msgWasShown) { msgWasShown = msgShown; changed = true; }

    if (on && S.timeoutS && (int32_t)(now - lastActivity) > (int32_t)(S.timeoutS * 1000UL) && !msgShown) uiOff();

    if (on) {
      // things that change with time alone
      uint32_t tick = marquee ? now / 100 : screen == S_INFO || screen == S_SYSTEM ? now / 1000 : screen == S_TERM ? now / 500 : 0;
      if (tick != lastTick) { lastTick = tick; changed = true; }
      if (changed) dirty = true;
      if (dirty && now - lastDraw >= 40) {
        dirty = false;
        lastDraw = now;
        uint32_t t0 = millis();
        draw(now);
        uint32_t took = millis() - t0;
        if (took > statDrawMsMax) statDrawMsMax = took;
        if (took > recentDrawMs) recentDrawMs = took;
      }
    }
    if (reqShot) { reqShot = false; screenshot(); }
    // a dark screen only waits for a touch or a movement: on battery it looks less often
    vTaskDelay(pdMS_TO_TICKS(on ? 20 : S.saver ? 120 : 50));
  }
}

// ============================================================ public (called from the main loop task)
bool begin() {
  stateMux = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(8, sizeof(uint32_t));
  esp_register_freertos_idle_hook_for_cpu(idleHook0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
  busLock();
  busUnlock();
  // powerOff() froze these pins for the time asleep; they are ours again
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
  gpio_hold_dis((gpio_num_t)PIN_SD_CS);
  if (PIN_KEY >= 0) rtc_gpio_deinit((gpio_num_t)PIN_KEY);     // was the wake-up source
#if HAS_SDCARD
  // The card shares the LCD's SPI lines and has to be brought into SPI mode
  // before the first display traffic; main.cpp's later Sd::begin() is then a no-op.
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_LCD_CS, OUTPUT);
  digitalWrite(PIN_LCD_CS, HIGH);
  Sd::begin();
#endif
  if (!lcd.init()) return false;
  imuFound = imuBegin();
  Serial.printf("[LCD]  Lagesensor QMI8658: %s\n", imuFound ? "ok" : "nicht gefunden");
  setRotation(settings.oledFlip ? IMU_ROT_DEFAULT ^ 2 : IMU_ROT_DEFAULT);
  Serial.printf("[LCD]  Zeichenpuffer: %s\n", canvasBits == 0 ? "keiner (direkt)" : canvasBits == 16 ? "16 Bit" : "8 Bit");
  buildSnapshot();
  S = shared;
  ready = true;
  on = true;
  uiOn = true;
  lastActivity = millis();
  reqBrightness = true;
  // Core 0 next to the radio: drawing never takes time from the serial bridge,
  // which runs in the Arduino loop on core 1.
  xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 1, nullptr, 0);
  return true;
}

bool isOn() { return ready && uiOn; }
uint8_t page() { return uiScreen == S_STATUS ? PAGE_STATUS : PAGE_INFO; }

void applyBrightness() { reqBrightness = true; }
void wake() { reqWake = true; }
void off() { reqOff = true; }
void nextPage() { reqNext = true; }

void message(const char *line1, const char *line2, uint32_t ms) {
  if (!ready) return;
  Lock l(stateMux);
  toAscii(line1 ? line1 : "", msgShared1, sizeof(msgShared1));
  toAscii(line2 ? line2 : "", msgShared2, sizeof(msgShared2));
  if (msgShared2[0] == ' ') memmove(msgShared2, msgShared2 + 1, strlen(msgShared2));
  msgUntil = millis() + ms;
  msgVersion = msgVersion + 1;
}

// "Off" is deep sleep: the board has no power switch, and the charger keeps
// working on its own. What stays awake is the RTC domain watching the BOOT
// button. Everything that draws current next to the processor is put to sleep
// first - the panel, the backlight, the accelerometer, the card.
void powerOff(const char *reason, uint32_t wakeAfterS) {
  Serial.printf("[PWR]  Ausschalten (%s), Akku %u mV. Einschalten: BOOT-Taste%s\n", reason, Power::mv(),
                wakeAfterS ? " oder Zeitablauf" : "");
  Player::stop();
  Sd::end();
  if (ready) {
    reqHalt = true;
    for (int i = 0; i < 200 && !uiHalted; i++) delay(10);
  }
  WiFi.mode(WIFI_OFF);
  // The pads float while asleep. Two must not: the backlight stays off and the
  // card stays deselected. (LCD chip select is a strapping pin and is left alone.)
  ledcDetachPin(PIN_LCD_BL);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  digitalWrite(PIN_SD_CS, HIGH);
  gpio_hold_en((gpio_num_t)PIN_SD_CS);
  gpio_deep_sleep_hold_en();
  if (PIN_KEY >= 0) {
    rtc_gpio_pullup_en((gpio_num_t)PIN_KEY);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_KEY, 0);
  }
  if (wakeAfterS) esp_sleep_enable_timer_wakeup((uint64_t)wakeAfterS * 1000000ULL);
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

void loop() {
  if (!ready) return;
  uint32_t now = millis();
  if (loopBeat) {
    uint32_t pause = now - loopBeat;
    if (pause > statLoopGapMax) statLoopGapMax = pause;
    if (pause > recentLoopMs) recentLoopMs = pause > 65535 ? 65535 : pause;
  }
  loopBeat = now;

  uint32_t cmd;
  while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) runCommand(cmd >> 16, (cmd >> 8) & 0xFF, cmd & 0xFF);
  if (rebootAt && (int32_t)(now - rebootAt) > 0) ESP.restart();
  if (offAt && (int32_t)(now - offAt) > 0) powerOff(offReason);

  sampleSystem(now);
  pollScan();
  static uint32_t lastCfg = 0;
  static uint8_t lastScreen = 0xFF;
  if (uiScreen == S_SCRIPTS && (lastScreen != S_SCRIPTS || now - lastCfg > 4000)) {
    lastCfg = now;
    loadConfigs();
  }
  lastScreen = uiScreen;

  static uint32_t lastSnapshot = 0;
  if (now - lastSnapshot >= (uiOn ? 100u : 1000u)) {    // nobody looks at a dark screen
    lastSnapshot = now;
    buildSnapshot();
  }
  termFeed();
  console();
}

}  // namespace Display
#endif
