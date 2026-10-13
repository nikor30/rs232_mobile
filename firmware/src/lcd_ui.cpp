#include "display.h"
#include "config.h"
#include "lang.h"

// Colour touch front end on the Waveshare ESP32-S3-Touch-LCD-2 (ST7789T3
// 240x320, CST816D touch, QMI8658 accelerometer). It implements the Display
// interface from display.h.
//
//   swipe / arrows   change the screen: Status, Terminal, Skripte, WLAN, Web-UI, Bluetooth, ...
//                    While the finger drags sideways, the name of the page it leads
//                    to comes in from that edge and follows it.
//   title            opens the overview: every page as a tile, one tap away
//   drag up / down   scrolls a page that is longer than the screen. The page keeps
//                    the finger's speed when it lets go, gives a little at its ends
//                    and settles back; a touch stops it where it is.
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
#include "settings.h"
#include "power.h"
#include "bat_curve.h"
#include "net.h"
#include "serial_bridge.h"
#include "sdcard.h"
#include "ble.h"
#include "configs.h"
#include "player.h"
#include "c2.h"
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

// colours are RGB888 (uint32_t), LovyanGFX converts them. The same family as
// the two web interfaces: black ground, grey surfaces in two steps, one tint
// (C_TINT) for what leads somewhere, and green, amber and red only for state.
// A pressed button takes the tint: on a small panel the answer to a touch has
// to be seen past the finger.
static const uint32_t C_BG = 0x000000, C_PANEL = 0x2C2C2E, C_LINE = 0x48484A, C_TEXT = 0xF5F5F7,
                      C_DIM = 0x98989D, C_ACCENT = 0x64D2FF, C_GREEN = 0x30D158, C_AMBER = 0xFF9F0A,
                      C_RED = 0xFF453A, C_BLUE = 0x0A84FF, C_BTN = 0x3A3A3C, C_BTN_DOWN = 0x0A84FF,
                      C_TINT = 0x0A84FF, C_TERM = 0x9AE6B4;

// The order is the order of use: is the port ready, how does one get onto the
// device, then the tools. "System" is for whoever looks after the device and
// only shows with the administrator switch of the web interface (shown()).
enum Screen : uint8_t { S_STATUS = 0, S_WIFI, S_TERM, S_SCRIPTS, S_BT, S_INFO, S_SYSTEM, S_SETUP, S_COUNT };
static const char *const TITLES[S_COUNT] = {TR("Status", "Status", "Estado"), TR("Network", "Netz", "Rede"), "Terminal", TR("Scripts", "Skripte", "Scripts"), "Bluetooth",
                                            TR("Info", "Info", "Info"), TR("System", "System", "Sistema"), TR("Setup", "Setup", "Ajustes")};
static const int MAX_CFG = 16, MAX_NETS = 12;

// 7.8 pixels are a millimetre on this panel: buttons are 6 mm high, list rows 4 mm.
// 7.8 pixels are a millimetre on this panel. The device is used on a shop
// floor, with big thumbs and sometimes gloves: buttons are 7 mm high, the arrows
// in the title bar have 10 mm each.
static const int HEAD_H = 40, NAV_H = 12, BTN_H = 56, BTN_GAP = 4, HEAD_HIT = 78;
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
  char    c2[32];              // command-and-control server: state, or the code to confirm
  char    c2Code[8];           // the code alone while the server waits for it, else empty
  bool    adminUi;             // the administrator's pages are shown (settings.adminUi)
  bool    stick, stickHost, stickEjected;   // the card is lent to the USB port as a memory stick
  uint32_t stickReadKb, stickWrittenKb;
  uint8_t conState;            // remote console: 0 none, 1 the server asks, 2 open
  uint16_t conMinutes;
  char    conPort[20], conLeft[12];
  bool    staConfigured, staConnected, tcpEnabled, tcpConnected;
  uint8_t webClients, apStations;
  uint8_t bleState;
  uint32_t blePin;
  bool    batMeasured, batPresent, batLow;
  uint8_t batCharge;                 // Power::Charge
  uint8_t batPct;
  uint16_t batMv;
  uint16_t batEmptyMv, batFullMv;    // the voltages shown as 0 % and 100 %
  bool    batCal;                    // ... set by a calibration, not the curve's own
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
static volatile int16_t reqSplash = 0;   // seconds for which the start-up picture is to be shown again
static volatile int16_t reqDrag[4];                // simulated finger: from x,y to x,y
static volatile bool reqDragGo = false, reqDragHold = false, reqLift = false;   // hold: the finger stays down at the end
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
static volatile int16_t statScroll = 0, statScrollMax = 0, statScrollMin = 0, statScrollPeak = 0;   // where the page is, and how far it went
static volatile bool statMenu = false;
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
  B_ASK_FORGET, B_ASK_OFF, B_ASK_CAL_FULL, B_ASK_CAL_EMPTY, B_ASK_CAL_RESET, B_CONFIRM, B_CON_MENU, B_CON_BACK,
  B_MENU, B_GOTO,
  K_CHAR, K_SHIFT, K_SYM, K_BKSP, K_OK,
  // carried out by the main loop
  B_BAUD, B_AUTO, B_BREAK, B_ENTER, B_CTRLC, B_REC, B_BT, B_SD, B_PLAY, B_STOP, B_SCAN, B_JOIN, B_FORGET,
  B_BRIGHT_DOWN, B_BRIGHT_UP, B_TIMEOUT, B_POWEROFF, B_CAL_FULL, B_CAL_EMPTY, B_CAL_RESET,
  B_CON_ALLOW, B_CON_DENY, B_CON_END, B_STICK, B_STICK_END
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
        // Portuguese: the letter without its accent reads well enough
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: r = "a"; break;
        case 0x80: case 0x81: case 0x82: case 0x83: r = "A"; break;
        case 0xA7: r = "c"; break; case 0x87: r = "C"; break;
        case 0xA9: case 0xAA: r = "e"; break; case 0x89: case 0x8A: r = "E"; break;
        case 0xAD: r = "i"; break; case 0x8D: r = "I"; break;
        case 0xB3: case 0xB4: case 0xB5: r = "o"; break;
        case 0x93: case 0x94: case 0x95: r = "O"; break;
        case 0xBA: r = "u"; break; case 0x9A: r = "U"; break;
      }
    } else if (p[0] == 0xC2 && p[1] == 0xB7) r = "-";
    else if (p[0] == 0xE2 && p[1] == 0x80) {           // typographic dashes and quotes
      switch (p[2]) {
        case 0x93: case 0x94: r = "-"; break;
        case 0x9C: case 0x9D: case 0x9E: r = "\""; break;
        case 0x98: case 0x99: r = "'"; break;
      }
    } else if (p[0] == 0xE2 && p[1] == 0x86 && p[2] == 0x92) r = "->";
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

static uint32_t stickDemoUntil = 0;      // show the stick dialog without lending the card (debug console)
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
  copyStr(s.c2, sizeof(s.c2), C2::line());
  {
    C2::Info ci = C2::info();
    copyStr(s.c2Code, sizeof(s.c2Code), ci.state == C2::PENDING ? ci.code : "");
  }
  C2::Console rc = C2::console();
  s.conState = rc.state;
  if (rc.state) {
    s.conMinutes = rc.minutes;
    copyStr(s.conPort, sizeof(s.conPort), Store::portName(rc.port));
    if (rc.leftS >= 120) snprintf(s.conLeft, sizeof(s.conLeft), "%lu min", (unsigned long)((rc.leftS + 59) / 60));
    else snprintf(s.conLeft, sizeof(s.conLeft), "%lu s", (unsigned long)rc.leftS);
  }
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
  s.batCal = Power::calEmptyMv() || Power::calFullMv();
  s.batEmptyMv = Power::calEmptyMv() ? Power::calEmptyMv() : BatCurve::defaultEmptyMv(settings.batType);
  s.batFullMv = Power::calFullMv() ? Power::calFullMv() : BatCurve::defaultFullMv(settings.batType);
  s.saver = Power::saver();
  s.cpuMhz = (uint8_t)getCpuFrequencyMhz();
  s.adminUi = settings.adminUi;
  s.sdMounted = Sd::mounted();
  Sd::Stick st = Sd::stick();
  if (stickDemoUntil && (int32_t)(millis() - stickDemoUntil) < 0) st = {true, true, false, 5321, 48};   // debug: "stickui N"
  s.stick = st.on;
  s.stickHost = st.host;
  s.stickEjected = st.ejected;
  s.stickReadKb = st.readKb;
  s.stickWrittenKb = st.writtenKb;
  strlcpy(s.sdType, Sd::typeName(), sizeof(s.sdType));
  s.sdUsedMb = Sd::usedMb();
  s.sdTotalMb = Sd::totalMb();
  s.brightness = settings.displayBrightness;
  fillSystem(s);
  fillScripts(s);
  fillNets(s);
  s.timeoutS = settings.displayTimeout;
  s.flip = settings.displayFlip;
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
  message(what, TR("Restarting ...", "Neustart ...", "Reiniciando ..."), 3000);
  rebootAt = millis() + 1500;
}

static void runCommand(uint8_t id, uint8_t arg, uint8_t port) {
  if (port >= MAX_PORTS) return;
  static const uint16_t TIMEOUTS[] = {15, 60, 300, 0};
  switch (id) {
    case B_PLAY:
      if (arg < cfgCount && !Player::start(cfgNames[arg], port)) message(TR("Configuration", "Konfiguration", "Configuracao"), Player::result(), 2500);
      break;
    case B_STOP: Player::stop(); break;
    case B_CON_ALLOW: C2::consoleAnswer(true); break;
    case B_CON_DENY: C2::consoleAnswer(false); break;
    case B_CON_END: C2::consoleEnd(); break;
    case B_SCAN: startScan(); break;
    case B_JOIN: {
      Lock l(stateMux);
      settings.staSsid = joinSsid;
      settings.staPass = joinPass;
      settings.staAuth = 0;                // WPA2/WPA3 with a key; 802.1X is set up in the web UI
      memset(joinPass, 0, sizeof(joinPass));
    }
      saveAndReboot(TR("Wi-Fi saved", "WLAN gespeichert", "Wi-Fi salvo"));
      break;
    case B_FORGET:
      settings.staSsid = "";
      settings.staPass = "";
      settings.staAuth = 0;
      saveAndReboot(TR("Wi-Fi client off", "WLAN-Client aus", "Cliente Wi-Fi desligado"));
      break;
    case B_BRIGHT_DOWN:
    case B_BRIGHT_UP: {
      int v = settings.displayBrightness + (id == B_BRIGHT_UP ? 32 : -32);
      settings.displayBrightness = constrain(v, 0, 255);
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
    case B_CAL_FULL:
    case B_CAL_EMPTY:
    case B_CAL_RESET: {
      const char *err = id == B_CAL_RESET ? Power::setCal(0, 0) : Power::calibrateNow(id == B_CAL_FULL);
      String now = String(Power::mv()) + " mV = " + String(Power::pct()) + " %";
      if (err) message(TR("Battery: not saved", "Akku: nicht gespeichert", "Bateria: nao salvo"), err, 4000);
      else message(id == B_CAL_RESET ? TR("Calibration cleared", "Kalibrierung geloescht", "Calibracao apagada") : TR("Battery calibrated", "Akku kalibriert", "Bateria calibrada"), now.c_str(), 2500);
      Net::markDirty();
      break;
    }
    case B_POWEROFF:
      offReason = TR("button", "Taste", "botao");
      message(TR("Switching off ...", "Ausschalten ...", "Desligando ..."), TR("Switch on: BOOT button", "Einschalten: BOOT-Taste", "Ligar: botao BOOT"), 4000);
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
    case B_BREAK: Bridge::sendBreak(port); message(TR("BREAK sent", "BREAK gesendet", "BREAK enviado"), "", 800); break;
    case B_ENTER: Bridge::write(port, (const uint8_t *)"\r", 1); break;
    case B_CTRLC: Bridge::write(port, (const uint8_t *)"\x03", 1); break;
    case B_REC:
      if (Sd::logging(port)) { Sd::logStop(port); message(TR("Recording stopped", "Mitschnitt beendet", "Gravacao encerrada"), "", 1200); }
      else if (Sd::logStart(port)) message(TR("Recording", "Mitschnitt", "Gravacao"), Sd::logName(port).c_str(), 1500);
      else message(TR("SD card:", "SD-Karte:", "Cartao SD:"), TR("file not created", "Datei nicht angelegt", "arquivo nao criado"), 1500);
      break;
    case B_BT: Ble::setEnabled(Ble::state() == Ble::OFF); break;
    case B_SD:                           // a card put in after start-up; the attempt can take a few seconds
      message(TR("SD card", "SD-Karte", "Cartao SD"), TR("looking for it...", "wird gesucht...", "procurando..."), 6000);
      delay(150);                        // let the UI task show that before the bus is taken for seconds
      if (Sd::begin()) message(TR("SD card", "SD-Karte", "Cartao SD"), TR("mounted", "eingebunden", "montado"), 2000);
      else message(TR("SD card", "SD-Karte", "Cartao SD"), TR("not found", "nicht gefunden", "nao encontrado"), 2000);
      break;
    case B_STICK: {
      const char *err = Sd::stickStart();
      if (err) message(TR("USB stick", "USB-Stick", "Pendrive USB"), err, 2500);
      break;
    }
    case B_STICK_END:                    // the USB port only comes back with a restart
      message(TR("Restarting ...", "Neustart ...", "Reiniciando ..."), "", 4000);
      rebootAt = millis() + 600;
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
//   batcal ...   calibrate the percentage: full / empty (the voltage right now), E F (in mV), reset
//   drag X Y X Y a finger moved from the first to the second position
//   saver N      power saving: 1 on, 0 off, -1 automatic (on battery)
//   poweroff N   switch off, and on again after N seconds (without N: BOOT button only)
//   stick        the SD card as a USB memory stick (this console ends; back: restart),  stickui N  only its dialog for N s
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
  Serial.printf("[DIAG] Blaettern: Stand %d von %d, seit der letzten Abfrage %d bis %d | Uebersicht %s\n", statScroll, statScrollMax,
                statScrollMin, statScrollPeak, statMenu ? "offen" : "zu");
  statScrollMin = statScrollPeak = statScroll;
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
    else if (sscanf(line, "splash %d", &a) == 1) reqSplash = constrain(a, 1, 60);
    else if (!strcmp(line, "rot auto")) reqRot = 4;
    else if (sscanf(line, "rot %d", &a) == 1) reqRot = a & 3;
    else if (!strcmp(line, "cfgtest")) {             // a small configuration to try the Skripte page with
      const char *err = Configs::save("Demo", "show version\n@pause 1\nshow clock\n", "");
      Serial.printf("[DIAG] Konfiguration \"Demo\": %s\n", err ? err : "gespeichert");
    }
    else if (!strcmp(line, "bat")) Serial.printf("[DIAG] Akku: %s\n", Power::diag().c_str());
    else if (!strcmp(line, "c2")) {
      C2::Info c = C2::info();
      Serial.printf("[DIAG] Leitstelle: %s%s%s | Server %s | ID %s | %lu Abfragen, letzte vor %ld s | Handshake %lu ms (davon Rechnen %lu ms) | Stack frei %lu B (ML-KEM) / %lu B (Netz), Heap %u kB (min %u kB, groesster Block %u kB)%s%s\n",
                    C2::stateName(c.state), c.code[0] ? ", Code " : "", c.code, c.url[0] ? c.url : "-", c.id[0] ? c.id : "-",
                    (unsigned long)c.polls, c.lastOkMs ? (long)((millis() - c.lastOkMs) / 1000) : -1L,
                    (unsigned long)c.handshakeMs, (unsigned long)c.cryptoMs, (unsigned long)c.stackFree, (unsigned long)c.taskStackFree,
                    (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024),
                    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                    c.error[0] ? " | Fehler: " : "", c.error);
    }
    else if (!strncmp(line, "batcal ", 7)) {
      const char *err = !strcmp(line + 7, "full") ? Power::calibrateNow(true)
                        : !strcmp(line + 7, "empty") ? Power::calibrateNow(false)
                        : !strcmp(line + 7, "reset") ? Power::setCal(0, 0)
                        : sscanf(line + 7, "%d %d", &a, &b) == 2 && a >= 0 && b >= 0 && a < 65536 && b < 65536 ? Power::setCal(a, b)
                        : "full, empty, reset oder zwei Werte in mV";
      Serial.printf("[DIAG] Akku-Kalibrierung: %s | %s\n", err ? err : "gespeichert", Power::diag().c_str());
    }
    else if (sscanf(line, "drag %d %d %d %d", &a, &b, &a2, &b2) == 4 || sscanf(line, "hold %d %d %d %d", &a, &b, &a2, &b2) == 4) {
      reqDrag[0] = a; reqDrag[1] = b; reqDrag[2] = a2; reqDrag[3] = b2;
      reqDragHold = line[0] == 'h';                  // "hold": the finger stays where it arrived, until "lift"
      reqDragGo = true;
    }
    else if (!strcmp(line, "lift")) reqLift = true;
    else if (sscanf(line, "saver %d", &a) == 1) Power::forceSaver(a < 0 ? -1 : a ? 1 : 0);
    else if (!strncmp(line, "poweroff", 8)) powerOff("Debug-Konsole", sscanf(line, "poweroff %d", &a) == 1 ? a : 0);
    else if (sscanf(line, "stickui %d", &a) == 1) stickDemoUntil = (millis() + constrain(a, 1, 60) * 1000UL) | 1;
    else if (!strcmp(line, "stick")) {               // the card as a USB memory stick; this console ends with it
      const char *err = Sd::stickStart();
      if (err) Serial.printf("[DIAG] USB-Stick: %s\n", err);
    }
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
static bool conMenu = false;             // "close the remote console?" is on the screen
// A question about the remote console lies over the page: nothing else reacts.
static bool overlay() { return S.stick || S.conState == 1 || (S.conState == 2 && conMenu); }
static bool menu = false;                // the overview of all pages is on the screen

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
static void scrollReset();

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
  scrollReset();
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
//
// The page behaves like a thing with weight. Under the finger it follows one to
// one; let go, it keeps the finger's speed and slows down by itself; at its
// ends it gives - less the further it is pulled - and a spring without bounce
// brings it back. A touch stops it wherever it is: nothing has to be waited for.
static int cTop = 0, cBot = 0;
static int contentH = 0;
static bool inContent = false;
static bool scrolling = false;           // this touch has become a drag
static float scroll0 = 0;                // scroll position when the finger came down
static float scrollPos = 0, scrollVel = 0;   // position (scrollY is this, rounded) and speed in pixels per second
enum ScrollAnim : uint8_t { A_NONE = 0, A_COAST, A_SPRING };
static uint8_t scrollAnim = A_NONE;
static float springTo = 0;
static uint32_t animAt = 0;
static bool stoppedByTouch = false;      // this touch caught a moving page: it is not a tap
// the finger's last positions, to know its speed when it lets go
struct Sample { uint32_t t; int16_t x, y; };
static Sample trail[6];
static uint8_t trailN = 0;
// a sideways drag that may become a change of page
static bool swiping = false;
static int swipeDx = 0;
static const int SWIPE_COMMIT = 50;
static bool marquee = false;             // a text on this frame is moving: keep redrawing
static int simStep = -1, simX0 = 0, simY0 = 0, simX1 = 0, simY1 = 0;   // finger simulated by the debug console
static const int SIM_STEPS = 10;
static bool simHold = false;

static int maxScroll() { return max(0, contentH - (cBot - cTop)); }

static void scrollReset() {
  scrollY = 0;
  scrollPos = scrollVel = 0;
  scrollAnim = A_NONE;
}

// Past its end the page follows less and less: there is nothing more here, but
// the touch still counts (Apple's formula; c = how willing it is).
static float rubber(float over, float dim, float c = 0.55f) { return over * dim * c / (dim + c * fabsf(over)); }

// Speed of the finger over its last ~120 ms, in pixels per second.
static void fingerSpeed(float &vx, float &vy) {
  vx = vy = 0;
  if (trailN < 2) return;
  const Sample &b = trail[trailN - 1];
  int i = trailN - 2;
  while (i > 0 && b.t - trail[i].t < 60) i--;
  const Sample &a = trail[i];
  if (b.t == a.t || b.t - a.t > 250) return;        // it rested before it let go
  vx = (b.x - a.x) * 1000.0f / (b.t - a.t);
  vy = (b.y - a.y) * 1000.0f / (b.t - a.t);
}

// One step of the page's own movement. Both laws are solved exactly for the
// time that has passed, so a slow frame changes how often the page is drawn,
// not where it ends up.
static void scrollTick(uint32_t now) {
  if (scrollAnim == A_NONE) return;
  float dt = (now - animAt) / 1000.0f;
  if (dt <= 0) return;
  animAt = now;
  float most = maxScroll();
  if (scrollAnim == A_COAST) {
    const float K = 2.6f;                            // friction: the speed falls to a tenth in ~0.9 s
    float f = expf(-K * dt);
    scrollPos += scrollVel * (1 - f) / K;
    scrollVel *= f;
    if (scrollPos < 0 || scrollPos > most) {         // ran into an end: the spring takes it from here, speed and all
      scrollAnim = A_SPRING;
      springTo = scrollPos < 0 ? 0 : most;
    } else if (fabsf(scrollVel) < 12) scrollAnim = A_NONE;
  } else {
    const float OMEGA = 2 * 3.14159f / 0.38f;        // critically damped, response 0.38 s: back without a bounce
    float d = scrollPos - springTo, c = scrollVel + OMEGA * d, f = expf(-OMEGA * dt);
    scrollPos = springTo + (d + c * dt) * f;
    scrollVel = (scrollVel - c * OMEGA * dt) * f;
    if (fabsf(scrollPos - springTo) < 0.5f && fabsf(scrollVel) < 8) {
      scrollPos = springTo;
      scrollVel = 0;
      scrollAnim = A_NONE;
    }
  }
  int y = lroundf(scrollPos);
  if (y != scrollY) { scrollY = y; dirty = true; }
}

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
  return touchDown && !touchSwallow && !scrolling && !swiping && !stoppedByTouch && inside(b, touchX0, touchY0) && inside(b, touchX, touchY);
}

static void button(int x, int y, int w, int h, const char *label, uint8_t id, uint32_t textColor = C_TEXT,
                   uint8_t arg = 0, const lgfx::IFont *font = &fonts::DejaVu18) {
  bool down = isDown(x, y, w, h);
  // under the finger it takes the tint and gives way a little - at once, not on release
  if (down) g->fillRoundRect(x + 2, y + 1, w - 4, h - 2, 10, C_BTN_DOWN);
  else g->fillRoundRect(x, y, w, h, 10, C_BTN);
  g->setFont(font);                      // as large as the label allows
  if (g->textWidth(label) > w - 8) g->setFont(&fonts::DejaVu12);
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
    // one size for the whole row: the largest in which every label fits
    const lgfx::IFont *font = &fonts::DejaVu18;
    g->setFont(font);
    for (int i = 0; i < cnt; i++)
      if (g->textWidth(defs[from + i].label) > w - 8) font = &fonts::DejaVu12;
    for (int i = 0; i < cnt; i++)
      button(BTN_GAP + i * (w + BTN_GAP), top + r * (BTN_H + BTN_GAP), w, BTN_H, defs[from + i].label, defs[from + i].id,
             defs[from + i].color, defs[from + i].arg, font);
  }
  return top;
}

static bool shown(int i) { return i != S_SYSTEM || S.adminUi; }
static int stepScreen(int from, bool next) {
  int s = from;
  do s = (s + (next ? 1 : S_COUNT - 1)) % S_COUNT; while (!shown(s) && s != from);
  return s;
}

static void gotoScreen(uint8_t s) {
  screen = s % S_COUNT;
  uiScreen = screen;
  menu = false;
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
    case B_PREV: gotoScreen(stepScreen(screen, false)); return;
    case B_NEXT: gotoScreen(stepScreen(screen, true)); return;
    case B_PORT: nextPort(); return;
    case B_MENU: menu = !menu; return;
    case B_GOTO: gotoScreen(arg); return;
    case B_LIST_PREV: if (listPage) listPage--; return;
    case B_LIST_NEXT: listPage++; return;            // the drawing code keeps it in range
    case B_CFG_ROW: cfgSel = arg; return;
    case B_CFG_CANCEL: cfgSel = -1; return;
    case B_PLAY_ACK: playAcked = true; return;
    case B_CON_MENU: conMenu = true; return;
    case B_CON_BACK: conMenu = false; return;
    case B_CON_END: conMenu = false; break;
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
      confirm1 = TR("Open network without encryption.", "Offenes Netz ohne Verschluesselung.", "Rede aberta, sem criptografia.");
      confirm2 = TR("Connect? The device restarts.", "Verbinden? Das Geraet startet neu.", "Conectar? O dispositivo reinicia.");
      setupMode = SETUP_CONFIRM;
      return;
    case B_ASK_FORGET:
      confirmId = B_FORGET;
      confirm1 = TR("Switch the Wi-Fi client off?", "WLAN-Client ausschalten?", "Desligar o cliente Wi-Fi?");
      confirm2 = TR("The device restarts.", "Das Geraet startet neu.", "O dispositivo reinicia.");
      setupMode = SETUP_CONFIRM;
      return;
    case B_ASK_OFF:
      confirmId = B_POWEROFF;
      confirm1 = TR("Switch the device off?", "Geraet ausschalten?", "Desligar o dispositivo?");
      confirm2 = TR("Switch on with the BOOT button.", "Einschalten mit der BOOT-Taste.", "Ligar com o botao BOOT.");
      setupMode = SETUP_CONFIRM;
      return;
    case B_ASK_CAL_FULL:
    case B_ASK_CAL_EMPTY: {
      static char ask[40];
      snprintf(ask, sizeof(ask), TR("Save %u mV as %s?", "%u mV als %s speichern?", "Salvar %u mV como %s?"), S.batMv, id == B_ASK_CAL_FULL ? "100 %" : "0 %");
      confirmId = id == B_ASK_CAL_FULL ? B_CAL_FULL : B_CAL_EMPTY;
      confirm1 = ask;
      confirm2 = id == B_ASK_CAL_FULL ? TR("Battery charged, charger off for a minute?", "Akku geladen, Ladegeraet seit einer Minute ab?", "Bateria carregada, carregador fora ha um minuto?") : TR("Only the percentage shown changes.", "Nur die Prozentanzeige aendert sich.", "So muda a porcentagem mostrada.");
      setupMode = SETUP_CONFIRM;
      return;
    }
    case B_ASK_CAL_RESET:
      confirmId = B_CAL_RESET;
      confirm1 = TR("Clear the battery calibration?", "Akku-Kalibrierung loeschen?", "Apagar a calibracao da bateria?");
      confirm2 = TR("The built-in curve applies again.", "Es gilt wieder die Kennlinie.", "Volta a valer a curva padrao.");
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
  bool wasMenu = menu;
  dirty = true;
  pressed(id, arg);
  if (s != screen || m != setupMode || wasMenu != menu) scrollReset();   // a different page starts at its top
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
    if (++simStep > SIM_STEPS) simStep = simHold ? SIM_STEPS : -1;
  }
  if (raw) {
    lastTouchSeen = now;
    if (!touchDown) {
      touchDown = true;
      touchStart = now;
      touchX0 = x; touchY0 = y;
      touchSwallow = !on;                // the first touch on a dark screen only wakes it
      // A page on its way is caught by the finger: it stops here, and goes on
      // from here if the finger moves. Such a touch presses nothing.
      stoppedByTouch = scrollAnim != A_NONE;
      scrollAnim = A_NONE;
      scrollVel = 0;
      scrolling = swiping = false;
      swipeDx = 0;
      scroll0 = scrollPos;
      trailN = 0;
      dirty = true;
    }
    if (trailN == sizeof(trail) / sizeof(trail[0])) memmove(trail, trail + 1, sizeof(trail) - sizeof(trail[0])), trailN--;
    trail[trailN++] = {now, (int16_t)x, (int16_t)y};
    touchX = x; touchY = y;
    int dx = x - touchX0, dy = y - touchY0;
    // Which gesture this is shows in its first ten pixels; from then on it stays that.
    if (!scrolling && !swiping && !touchSwallow) {
      if (abs(dy) > 10 && abs(dy) > abs(dx) && touchY0 >= cTop && touchY0 < cBot && (maxScroll() || stoppedByTouch)) scrolling = true;
      else if (abs(dx) > 10 && abs(dx) > abs(dy) && !modal() && !overlay() && !menu) swiping = true;
    }
    if (scrolling) {
      float to = scroll0 - dy, most = maxScroll(), vis = cBot - cTop;
      if (to < 0) to = -rubber(-to, vis);
      else if (to > most) to = most + rubber(to - most, vis);
      scrollPos = to;
      if (lroundf(to) != scrollY) { scrollY = lroundf(to); dirty = true; }
    }
    if (swiping && dx != swipeDx) { swipeDx = dx; dirty = true; }
    uiWake();
    return;
  }
  if (!touchDown || now - lastTouchSeen < TOUCH_RELEASE_MS) return;
  touchDown = false;
  dirty = true;
  statTouches = statTouches + 1;
  lastTouchX = touchX0; lastTouchY = touchY0;
  float vx, vy;
  fingerSpeed(vx, vy);
  bool wasSwipe = swiping, caught = stoppedByTouch;
  swiping = stoppedByTouch = false;
  if (touchSwallow) return;
  if (scrolling || caught) {
    // The page goes on with the speed the finger had. Outside its ends there
    // is nowhere to go on to: the spring brings it back.
    float most = maxScroll();
    animAt = now;
    scrollVel = scrolling ? -vy : 0;
    scrolling = false;
    if (scrollPos < 0 || scrollPos > most) {
      scrollAnim = A_SPRING;
      springTo = scrollPos < 0 ? 0 : most;
      scrollVel = 0;
    } else if (fabsf(scrollVel) > 60) scrollAnim = A_COAST;
    return;
  }
  int dx = touchX - touchX0, dy = touchY - touchY0;
  if (wasSwipe) {
    // Far enough, or thrown: what counts is where the movement was going. A
    // finger that turned back before letting go has taken it back.
    float going = dx + vx * 0.12f;
    bool back = (dx < 0 && vx > 120) || (dx > 0 && vx < -120);
    if (!back && (abs(dx) > SWIPE_COMMIT || fabsf(going) > SWIPE_COMMIT * 1.6f)) press(dx < 0 ? B_NEXT : B_PREV);   // swipe left = next screen
    return;
  }
  if (abs(dx) < 16 && abs(dy) < 16) tapAt(touchX0, touchY0);
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

// An arrowhead, drawn rather than typed: "<" from a font is thin and sits too low.
// dir: 0 left, 1 right, 2 down, 3 up; r = half its size.
static void chevron(int x, int y, uint8_t dir, uint32_t color, int r = 6) {
  for (int t = -1; t <= 1; t++) {                    // three lines side by side make the stroke
    switch (dir) {
      case 0: g->drawLine(x + r / 2 + t, y - r, x - r / 2 + t, y, color); g->drawLine(x - r / 2 + t, y, x + r / 2 + t, y + r, color); break;
      case 1: g->drawLine(x - r / 2 + t, y - r, x + r / 2 + t, y, color); g->drawLine(x + r / 2 + t, y, x - r / 2 + t, y + r, color); break;
      case 2: g->drawLine(x - r, y - r / 2 + t, x, y + r / 2 + t, color); g->drawLine(x, y + r / 2 + t, x + r, y - r / 2 + t, color); break;
      default: g->drawLine(x - r, y + r / 2 + t, x, y - r / 2 + t, color); g->drawLine(x, y - r / 2 + t, x + r, y + r / 2 + t, color); break;
    }
  }
}

// The product's mark, the same as in both web interfaces: the shell of a serial
// connector with a prompt in it, on a tile in the accent colour. s is the side.
static void drawMark(int x, int y, int s) {
  g->fillRoundRect(x, y, s, s, s * 15 / 64, C_TINT);
  // the shell: wider at the top than at the bottom, its corners rounded
  int top = y + s * 20 / 64, h = s * 24 / 64, r = max(2, s * 4 / 64);
  for (int i = 0; i < h; i++) {
    int inset = s * 11 / 64 + s * 5 * i / (64 * (h - 1));
    int d = i < r ? r - i : i >= h - r ? i - (h - 1 - r) : 0;        // how far into a corner
    if (d) inset += r - (int)sqrtf((float)(r * r - d * d));
    g->drawFastHLine(x + inset, top + i, s - 2 * inset, 0xFFFFFF);
  }
  // the prompt: ">" and the cursor waiting next to it
  int th = max(3, s * 7 / 128), cx = x + s * 25 / 64, cy = y + s / 2, a = max(3, s * 11 / 128);
  for (int t = -(th / 2); t <= th / 2; t++) {
    g->drawLine(cx - a / 2 + t, cy - a, cx + a / 2 + t, cy, C_TINT);
    g->drawLine(cx + a / 2 + t, cy, cx - a / 2 + t, cy + a, C_TINT);
  }
  g->fillRoundRect(x + s * 31 / 64, cy + a - th + 1, s * 12 / 64, th, th / 2, C_TINT);
}

// Mark and name together, centred on (cx, cy): what the display shows first.
static void drawSplash() {
  const int s = 84;
  drawMark((W - s) / 2, H / 2 - s / 2 - 26, s);
  g->setTextDatum(lgfx::middle_center);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString("RS232 Console", W / 2, H / 2 + s / 2 - 2);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  g->drawString("v" FW_VERSION, W / 2, H / 2 + s / 2 + 22);
}

static uint32_t splashUntil = 0;         // the start-up picture stays until then

static void drawHeader() {
  g->fillRect(0, 0, W, HEAD_H, C_PANEL);
  g->setFont(&fonts::DejaVu18);
  g->setTextDatum(lgfx::middle_center);
  bool backDown = isDown(0, 0, HEAD_HIT, HEAD_H + 6);
  chevron(15, HEAD_H / 2, 0, backDown ? C_TEXT : C_TINT);   // the arrows lead somewhere: they carry the tint
  if (modal()) {                         // a sub-page: the arrow leads back, nothing else leaves it
    g->setTextColor(C_TEXT);
    const char *title = setupMode == SETUP_NETS ? TR("Choose Wi-Fi", "WLAN waehlen", "Escolher Wi-Fi")
                      : setupMode == SETUP_CONFIRM && confirmId == B_FORGET ? TR("Wi-Fi client", "WLAN-Client", "Cliente Wi-Fi")
                      : setupMode == SETUP_CONFIRM && confirmId == B_POWEROFF ? TR("Switch off", "Ausschalten", "Desligar")
                      : setupMode == SETUP_CONFIRM && confirmId >= B_CAL_FULL && confirmId <= B_CAL_RESET ? TR("Calibrate battery", "Akku kalibrieren", "Calibrar bateria") : netSsid;
    if (g->textWidth(title) <= W - 88) g->drawString(title, W / 2, HEAD_H / 2);
    else drawMarquee(title, 44, HEAD_H / 2, W - 52);
    addHotspot(0, 0, HEAD_HIT, HEAD_H + 6, B_BACK);
    return;
  }
  chevron(W - 15, HEAD_H / 2, 1, isDown(W - HEAD_HIT, 0, HEAD_HIT, HEAD_H + 6) ? C_TEXT : C_TINT);
  // While the server has a console open, every page says so in place of its
  // title; a tap there leads to "Beenden". Otherwise the title opens the
  // overview of all pages, and the small arrowhead beside it says so.
  const char *title = S.conState == 2 ? TR("Remote console", "Fernkonsole", "Console remoto") : menu ? TR("Pages", "Seiten", "Paginas") : TITLES[screen];
  bool titleDown = isDown(HEAD_HIT, 0, W - 2 * HEAD_HIT, HEAD_H + 6);
  g->setTextColor(S.conState == 2 ? C_RED : titleDown ? C_TINT : C_TEXT);
  int tw = g->textWidth(title), mark = S.conState == 2 ? 0 : 14;
  g->drawString(title, W / 2 - mark / 2, HEAD_H / 2);
  if (mark) chevron(W / 2 - mark / 2 + tw / 2 + 9, HEAD_H / 2 + 1, menu ? 3 : 2, titleDown ? C_TINT : C_DIM, 4);
  int titleLeft = W / 2 - (tw + mark) / 2;
  addHotspot(0, 0, HEAD_HIT, HEAD_H + 6, B_PREV);
  addHotspot(W - HEAD_HIT, 0, HEAD_HIT, HEAD_H + 6, B_NEXT);
  addHotspot(HEAD_HIT, 0, W - 2 * HEAD_HIT, HEAD_H + 6, S.conState == 2 ? B_CON_MENU : B_MENU);

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

// The page a sideways drag is leading to, if it is far enough to count.
static int swipeTarget() {
  if (!swiping || abs(swipeDx) < SWIPE_COMMIT) return -1;
  return stepScreen(screen, swipeDx < 0);
}

static void drawNav() {
  if (modal()) return;
  int count = 0;
  for (int i = 0; i < S_COUNT; i++) count += shown(i);
  int y = H - NAV_H / 2, x = W / 2 - (count - 1) * 7, to = swipeTarget();
  for (int i = 0; i < S_COUNT; i++) {
    if (!shown(i)) continue;
    g->fillCircle(x, y, i == screen || i == to ? 3 : 2, i == to ? C_TINT : i == screen ? C_ACCENT : C_LINE);
    x += 14;
  }
}

// While the finger drags sideways, the name of the page it leads to comes in
// from that edge and stays under the finger. It takes the tint once letting go
// would change the page - so the answer is seen before the decision is made.
static void drawSwipe() {
  if (!swiping || !swipeDx) return;
  bool next = swipeDx < 0;
  const char *name = TITLES[stepScreen(screen, next)];
  g->setFont(&fonts::DejaVu18);
  int full = g->textWidth(name) + 44, pull = abs(swipeDx);
  int w = pull <= full ? pull : full + (int)rubber(pull - full, W / 2);   // past its size it only stretches
  int h = 52, y = (H - h) / 2;
  bool armed = pull >= SWIPE_COMMIT;
  uint32_t col = armed ? C_TINT : C_BTN;
  int x = next ? W - w : 0;
  g->fillRoundRect(next ? x : x - 16, y, w + 16, h, 16, col);   // the round end faces the page, the other is beyond the edge
  int32_t cx, cy, cw, ch;
  g->getClipRect(&cx, &cy, &cw, &ch);
  // The name is read from its first letter on, whichever side it comes from:
  // from the right it travels in with the edge, from the left it stands at the
  // screen's edge and is uncovered.
  g->setTextColor(armed ? C_TEXT : C_DIM);
  g->setTextDatum(lgfx::middle_left);
  if (next) {
    g->setClipRect(x, y, w, h);
    g->drawString(name, x + 28, y + h / 2);
    chevron(x + 13, y + h / 2, 0, armed ? C_TEXT : C_DIM);
  } else {
    g->setClipRect(0, y, max(0, w - 24), h);
    g->drawString(name, max(10, w - full + 16), y + h / 2);
    g->setClipRect(0, y, w, h);
    chevron(w - 13, y + h / 2, 1, armed ? C_TEXT : C_DIM);
  }
  g->setClipRect(cx, cy, cw, ch);
}

// Every page as a tile: its name and the one thing worth knowing before going
// there. Nine dots in a row tell how many pages there are, not which is where.
static void drawMenu() {
  static char hint[S_COUNT][28];
  snprintf(hint[S_STATUS], sizeof(hint[0]), "%s", S.autobaud[selPort] ? "Auto-Baud" : S.serial[selPort]);
  snprintf(hint[S_TERM], sizeof(hint[0]), "%s", S.recording[selPort] ? TR("recording", "zeichnet auf", "gravando") : S.portName[selPort]);
  if (S.playState == Player::RUNNING) snprintf(hint[S_SCRIPTS], sizeof(hint[0]), TR("running", "laeuft", "em execucao"));
  else if (S.cfgCount) snprintf(hint[S_SCRIPTS], sizeof(hint[0]), S.cfgCount == 1 ? TR("1 script", "1 Skript", "1 script") : TR("%u scripts", "%u Skripte", "%u scripts"), S.cfgCount);
  else snprintf(hint[S_SCRIPTS], sizeof(hint[0]), TR("play", "abspielen", "reproduzir"));   // the list is read when the page is opened
  snprintf(hint[S_WIFI], sizeof(hint[0]), "%s", S.staConnected ? S.staIp : S.apIp);
  snprintf(hint[S_BT], sizeof(hint[0]), "%s", S.bleState == Ble::OFF ? TR("off", "aus", "desligado") : S.bleState == Ble::CONNECTED ? TR("connected", "verbunden", "conectado") : TR("ready", "bereit", "pronto"));
  snprintf(hint[S_SYSTEM], sizeof(hint[0]), "%u MHz", S.cpuMhz);
  if (S.batPresent) snprintf(hint[S_INFO], sizeof(hint[0]), TR("Battery %u%%", "Akku %u%%", "Bateria %u%%"), S.batPct);
  else snprintf(hint[S_INFO], sizeof(hint[0]), "%s", S.c2);
  snprintf(hint[S_SETUP], sizeof(hint[0]), "%s", !S.staConfigured ? TR("no LAN", "kein LAN", "sem LAN") : S.staConnected ? TR("LAN up", "LAN an", "LAN ok") : TR("LAN searching", "LAN sucht", "LAN buscando"));

  int count = 0;
  for (int i = 0; i < S_COUNT; i++) count += shown(i);
  const int cols = W > H ? 4 : 2, rows = (count + cols - 1) / cols, gap = 5;
  int top = HEAD_H + gap, bottom = H - NAV_H - 2;
  int tw = (W - gap * (cols + 1)) / cols, th = (bottom - top - gap * (rows - 1)) / rows;
  for (int i = 0, k = 0; i < S_COUNT; i++) {
    if (!shown(i)) continue;
    int x = gap + (k % cols) * (tw + gap), y = top + (k / cols) * (th + gap);
    k++;
    bool down = isDown(x, y, tw, th), here = i == screen;
    if (down) g->fillRoundRect(x + 2, y + 1, tw - 4, th - 2, 12, C_BTN_DOWN);
    else g->fillRoundRect(x, y, tw, th, 12, here ? C_BTN : C_PANEL);
    if (here && !down) g->drawRoundRect(x, y, tw, th, 12, C_TINT);       // where one is
    g->setTextDatum(lgfx::middle_center);
    g->setFont(&fonts::DejaVu18);
    if (g->textWidth(TITLES[i]) > tw - 8) g->setFont(&fonts::DejaVu12);
    if (g->textWidth(TITLES[i]) > tw - 6) g->setFont(&fonts::DejaVu9);
    g->setTextColor(C_TEXT);
    g->drawString(fit(TITLES[i], tw - 6).c_str(), x + tw / 2, y + th / 2 - 9);
    g->setFont(&fonts::DejaVu9);
    g->setTextColor(down ? C_TEXT : i == S_BT && S.bleState == Ble::CONNECTED ? C_GREEN : C_DIM);
    g->drawString(fit(hint[i], tw - 8).c_str(), x + tw / 2, y + th / 2 + 12);
    addHotspot(x, y, tw, th, B_GOTO, i);
  }
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
  // The content got shorter while the page rests: back into range. A finger or
  // the page's own movement may hold it outside for a moment.
  if (!touchDown && scrollAnim == A_NONE && (scrollY > most || scrollY < 0)) { scrollY = constrain(scrollY, 0, most); scrollPos = scrollY; dirty = true; }
  if (!most) return;
  // There is more: say so, and where we are. Pulled past an end, the bar
  // shrinks against that end, as the page itself is pressed there.
  int over = scrollY < 0 ? -scrollY : scrollY > most ? scrollY - most : 0;
  int h = max(8, max(20, vis * vis / contentH) - over);
  int at = constrain((vis - h) * constrain(scrollY, 0, most) / most, 0, vis - h);
  if (scrollY > most) at = vis - h;
  g->fillRect(W - 4, cTop, 3, vis, C_PANEL);
  g->fillRect(W - 4, cTop + at, 3, h, C_ACCENT);
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
    if (S.batCharge == Power::CHARGING) s += TR("  charging", "  laedt", "  carregando");
    else if (S.batCharge == Power::FULL) s += TR("  full", "  voll", "  cheia");
    return s;
  }
  return S.batMeasured ? String(TR("none (USB)", "keiner (USB)", "nenhuma (USB)")) : String(TR("not measured", "keine Messung", "sem medicao"));
}

static String sdLine() {
  if (!S.sdMounted) return String(S.sdType);
  return String(S.sdType) + "  " + String(S.sdUsedMb) + " / " + String(S.sdTotalMb) + " MB";
}

static String lanLine() {
  if (!S.staConfigured) return String(TR("off", "aus", "desligado"));
  return S.staConnected ? String(S.staIp) : String(TR("connecting...", "verbinde...", "conectando..."));
}

// The first page answers one question: can I work on this port? One word, large
// enough to read from a step away, the line settings under it, and whether
// anything moves. Everything else has its own page.
static void screenStatus() {
  BtnDef defs[4] = {{"Baud", B_BAUD, C_TEXT}, {"Auto", B_AUTO, C_TEXT}, {"BREAK", B_BREAK, C_AMBER}};
  int n = 3;
  static char portLabel[8];
  if (S.ports > 1) {
    snprintf(portLabel, sizeof(portLabel), TR("Port %u", "Port %u", "Porta %u"), selPort + 1);
    defs[n++] = {portLabel, B_PORT, C_ACCENT};
  }
  int top = HEAD_H, bottom = buttonRow(defs, n) - 2;
  cTop = cBot = 0;

  const char *state = TR("Ready", "Bereit", "Pronto");
  uint32_t color = C_GREEN;
  if (S.conState == 2) { state = TR("Remote console", "Fernkonsole", "Console remoto"); color = C_RED; }
  else if (S.autobaud[selPort]) { state = "Auto-Baud ..."; color = C_AMBER; }
  else if (S.playState == Player::RUNNING) { state = TR("Script running", "Skript laeuft", "Script rodando"); color = C_AMBER; }
  else if (S.webClients || S.tcpConnected || S.bleState == Ble::CONNECTED) { state = TR("In use", "In Benutzung", "Em uso"); color = C_BLUE; }

  // three lines and the lamps, spread over what the buttons leave
  int room = bottom - top, y = top + max(4, (room - 124) / 2);
  g->setTextDatum(lgfx::top_center);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  g->drawString(fit(S.portName[selPort], W - 16).c_str(), W / 2, y);
  g->setFont(&fonts::DejaVu40);
  if (g->textWidth(state) > W - 12) g->setFont(&fonts::DejaVu24);
  g->setTextColor(color);
  g->drawString(state, W / 2, y + 18);
  g->setFont(&fonts::DejaVu24);
  g->setTextColor(C_TEXT);
  g->drawString(S.serial[selPort], W / 2, y + 64);

  y += 110;
  g->setFont(&fonts::DejaVu18);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_DIM);
  dot(20, y, S.rxActive[selPort], C_GREEN);
  g->drawString(("RX " + fmtBytes(S.rx[selPort])).c_str(), 34, y);
  dot(W / 2 + 12, y, S.txActive[selPort], C_AMBER);
  g->drawString(("TX " + fmtBytes(S.tx[selPort])).c_str(), W / 2 + 26, y);
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

// How one gets onto the device: the QR code joins the hotspot (the phone then
// opens the web interface by itself), beside it what somebody would type in or
// read out - network, key, the addresses.
static void screenNet() {
  contentBegin(HEAD_H, H - NAV_H);
  int top = rowY - 2, avail = cBot - cTop - 12;
  bool wide = W > H;
  int size = wide ? min(avail, W - 142) : min(W - 24, avail - 76);   // beside it, an IP address still fits on one line
  int qx = wide ? 8 : (W - size) / 2;
  String payload = "WIFI:T:WPA;S:" + qrEscape(S.apSsid) + ";P:" + qrEscape(S.apPass) + ";;";
  g->fillRoundRect(qx, top, size, size, 6, 0xFFFFFFu);
  g->qrcode(payload.c_str(), qx + 6, top + 6, size - 12, 1);

  const char *labels[4] = {TR("Wi-Fi", "WLAN", "Wi-Fi"), TR("Password", "Passwort", "Senha"), "IP", "LAN"};
  String values[4] = {S.apSsid, S.apPass, S.apIp, lanLine()};
  uint32_t colors[4] = {C_TEXT, C_TEXT, C_TEXT, !S.staConfigured ? C_DIM : S.staConnected ? C_GREEN : C_AMBER};
  int tx = wide ? qx + size + 10 : 8, ty = wide ? top + 2 : top + size + 8, tw = W - tx - 10;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  for (int i = 0; i < 4; i++) {
    g->setTextColor(C_DIM);
    g->drawString(labels[i], tx, ty);
    g->setTextColor(colors[i]);
    ty += LINE_H + wrapped(values[i].c_str(), tx, ty + LINE_H, tw, false) + 6;   // every character: it gets typed in
  }
  rowY = max(top + size, ty);
  contentEnd();
}

static void screenBt() {
  uint8_t s = S.bleState;
  const char *text = s == Ble::OFF ? TR("Off", "Aus", "Desligado") : s == Ble::ADVERTISING ? TR("Ready, waiting for a connection", "Bereit, wartet auf Verbindung", "Pronto, aguardando conexao")
                   : s == Ble::PAIRING ? TR("Pairing: enter the PIN on the phone", "Kopplung: PIN am Handy eingeben", "Pareamento: digite o PIN no celular") : TR("Connected", "Verbunden", "Conectado");
  uint32_t color = s == Ble::OFF ? C_DIM : s == Ble::CONNECTED ? C_BLUE : s == Ble::PAIRING ? C_AMBER : C_GREEN;
  BtnDef def = {s == Ble::OFF ? TR("Switch Bluetooth on", "Bluetooth einschalten", "Ligar o Bluetooth") : TR("Switch Bluetooth off", "Bluetooth ausschalten", "Desligar o Bluetooth"), B_BT, C_TEXT};
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
  row(TR("Name", "Name", "Nome"), S.apSsid);
  row(TR("Service", "Dienst", "Servico"), "Nordic UART (BLE)");
  row(TR("Port", "Port", "Porta"), String(S.portName[0]) + ", " + S.serial[0]);
  contentEnd();
}

static void screenInfo() {
  int bottom = H - NAV_H;
  {
    BtnDef def = {TR("Mount SD card", "SD-Karte einbinden", "Montar cartao SD"), B_SD, C_TEXT};
    if (S.sdMounted) def = {TR("SD card as USB stick", "SD-Karte als USB-Stick", "Cartao SD como pendrive"), B_STICK, C_TEXT};
    bottom = buttonRow(&def, 1) - 2;
  }
  contentBegin(HEAD_H, bottom);
  String tcp = !S.tcpEnabled ? String(TR("off", "aus", "desligado"))
             : S.ports > 1 ? ":" + String(RAW_TCP_PORT) + "-" + String(RAW_TCP_PORT + MAX_PORTS - 1)
             : ":" + String(RAW_TCP_PORT);
  char imu[40];
  if (imuAddr) snprintf(imu, sizeof(imu), TR("rotation %u", "Drehung %u", "rotacao %u"), rot);
  else strlcpy(imu, TR("sensor not found", "Sensor nicht gefunden", "sensor nao encontrado"), sizeof(imu));
  if (S.c2Code[0]) {
    // The code somebody types in at the control center: large, three and three,
    // to be read from a step away and in bad light.
    char spaced[10];
    snprintf(spaced, sizeof(spaced), "%.3s %s", S.c2Code, S.c2Code + 3);
    g->fillRoundRect(6, rowY, W - 12, 78, 12, C_PANEL);
    g->setTextDatum(lgfx::middle_center);
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    g->drawString(TR("Code for the control center", "Code fuer die Leitstelle", "Codigo para a central"), W / 2, rowY + 15);
    g->setFont(&fonts::DejaVu40);
    g->setTextColor(C_TEXT);
    g->drawString(spaced, W / 2, rowY + 50);
    rowY += 86;
  }
  // who this is: mark and name as on the web interfaces
  drawMark(8, rowY, 34);
  g->setTextDatum(lgfx::middle_left);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString("RS232 Console", 52, rowY + 17);
  rowY += 44;
  row("Firmware", String(FW_VERSION) + "  up " + fmtUptime(millis() / 1000));
  row("Host", String(S.hostname) + ".local");
  row("Hotspot", String(S.apStations) + TR(" devices", " Geraete", " dispositivos"));
  row("Clients", clientsLine());
  row("Raw-TCP", tcp);
  row(TR("Center", "Leitstelle", "Central"), S.c2);
  row(TR("Battery", "Akku", "Bateria"), batteryLine());
  row("SD", sdLine());
  row(TR("Position", "Lage", "Posicao"), imu);
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
  g->fillRoundRect(x, rowY, w, 16, 7, C_PANEL);
  if (pct) g->fillRoundRect(x, rowY, max(16, w * pct / 100), 16, 7, color);
  g->setTextColor(C_TEXT);
  g->setTextDatum(lgfx::middle_center);
  g->drawString(fit(text, w - 4).c_str(), x + w / 2, rowY + 9);
  rowY += 22;
}

static uint32_t loadColor(int pct) { return pct >= 85 ? C_RED : pct >= 60 ? C_AMBER : C_GREEN; }

static void screenSystem() {
  contentBegin(HEAD_H, H - NAV_H);
  bar("CPU 0", S.cpu[0], String(S.cpu[0]) + TR(" %  radio + display", " %  Funk + Anzeige", " %  radio + tela"), loadColor(S.cpu[0]));
  bar("CPU 1", S.cpu[1], String(S.cpu[1]) + TR(" %  bridge", " %  Bruecke", " %  ponte"), loadColor(S.cpu[1]));
  int ram = S.heapTotal ? 100 - (int)((uint64_t)S.heapFree * 100 / S.heapTotal) : 0;
  bar("RAM", ram, String((S.heapTotal - S.heapFree) / 1024) + TR(" of ", " von ", " de ") + String(S.heapTotal / 1024) + " kB", loadColor(ram));
  if (S.psramTotal) {
    int ps = 100 - (int)((uint64_t)S.psramFree * 100 / S.psramTotal);
    bar("PSRAM", ps, String((S.psramTotal - S.psramFree) / 1024) + TR(" of ", " von ", " de ") + String(S.psramTotal / 1024) + " kB", C_BLUE);
  }
  rowY += 2;
  row(TR("RAM free", "RAM frei", "RAM livre"), String(S.heapFree / 1024) + " kB, min. " + String(S.heapMin / 1024) + " kB");
  row("Block", String(S.heapBlock / 1024) + TR(" kB in one piece", " kB am Stueck", " kB contiguos"));
  row(TR("Timing", "Takt", "Ciclo"), TR("pause ", "Pause ", "pausa ") + String(S.loopMs) + TR(" ms, frame ", " ms, Bild ", " ms, quadro ") + String(S.drawMs) + " ms");
  row("CPU", String(S.cpuMhz) + " MHz" + (S.saver ? TR(", power saving (battery)", ", Sparmodus (Akku)", ", economia (bateria)") : ""));
  row("Chip", String(S.tempC) + " C,  " + String(S.tasks) + " Tasks");
  row(TR("Uptime", "Laufzeit", "Ligado ha"), fmtUptime(millis() / 1000));
  contentEnd();
}

// ---- lists (stored configurations, WLAN networks): rows to tap, paged
static const int LIST_ROW_H = 44;

static int listRows(int top, int bottom) { return max(1, (bottom - top) / LIST_ROW_H); }

static void listRow(int index, int y, const char *text, const char *right, bool selected, uint8_t id) {
  bool down = isDown(4, y, W - 8, LIST_ROW_H - 2);
  g->fillRoundRect(4, y, W - 8, LIST_ROW_H - 2, 8, selected || down ? C_BTN_DOWN : C_PANEL);
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
    BtnDef def = running ? BtnDef{TR("Stop", "Stopp", "Parar"), B_STOP, C_RED, 0} : BtnDef{"OK", B_PLAY_ACK, C_TEXT, 0};
    contentBegin(HEAD_H, buttonRow(&def, 1) - 2);
    g->setFont(&fonts::DejaVu18);
    g->setTextColor(C_TEXT);
    drawMarquee(S.playName, 8, rowY + 10, W - 20);
    rowY += 26;
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    rowY += wrapped((String(TR("on ", "auf ", "em ")) + S.portName[S.playPort] + TR(",  line ", ",  Zeile ", ",  linha ") + S.playLine + TR(" of ", " von ", " de ") + S.playLines).c_str(),
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
    centered(TR("No configurations stored", "Keine Konfigurationen gespeichert", "Nenhuma configuracao salva"), TR("Create them: web interface, tab Config", "Anlegen: Weboberflaeche, Reiter Konfig", "Criar: interface web, aba Config"), (HEAD_H + H - NAV_H) / 2);
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
    snprintf(label, sizeof(label), TR("Send to port %u", "Senden an Port %u", "Enviar para a porta %u"), selPort + 1);
    BtnDef defs[2] = {{label, B_PLAY, C_AMBER, (uint8_t)cfgSel}, {TR("Cancel", "Abbrechen", "Cancelar"), B_CFG_CANCEL, C_TEXT, 0}};
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
    snprintf(label, sizeof(label), TR("Port %u", "Port %u", "Porta %u"), selPort + 1);
    defs[n++] = {label, B_PORT, C_ACCENT, 0};
  }
  if (n) buttonRow(defs, n, 1);
  else centered(TR("Tap one, then send", "Antippen, dann senden", "Toque em um e envie"), nullptr, H - NAV_H - BTN_H / 2 - 2);
}

// ---- on-screen keyboard (WLAN key). QWERTZ, three layers, ten keys per row.
static const char *const KB_LAYERS[3][4] = {
    {"1234567890", TR("qwertyuiop", "qwertzuiop", "qwertyuiop"), "asdfghjkl-", TR("zxcvbnm._@", "yxcvbnm._@", "zxcvbnm._@")},
    {"!\"#$%&/()=", TR("QWERTYUIOP", "QWERTZUIOP", "QWERTYUIOP"), "ASDFGHJKL+", TR("ZXCVBNM,;:", "YXCVBNM,;:", "ZXCVBNM,;:")},
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
  button(x0 + kw * 3 + 1, y, kw * 3 - 2, h, TR("Space", "Leer", "Espaco"), K_CHAR, C_DIM, ' ');
  button(x0 + kw * 6 + 1, y, kw * 2 - 2, h, "<-", K_BKSP, C_AMBER);
  button(x0 + kw * 8 + 1, y, kw * 2 - 2, h, "OK", K_OK, strlen(kbText) >= 8 ? C_GREEN : C_LINE);
}

static const char *signalText(int rssi) { return rssi >= -60 ? TR("strong", "stark", "forte") : rssi >= -75 ? TR("medium", "mittel", "medio") : TR("weak", "schwach", "fraco"); }

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
      g->drawString(fit(TR("Wi-Fi key (8-63 characters)", "WLAN-Schluessel (8-63 Zeichen)", "Senha do Wi-Fi (8-63 caracteres)"), W - 24).c_str(), 10, top + 12);
    }
    keyboard(top + 28);
    return;
  }

  if (setupMode == SETUP_CONFIRM) {
    BtnDef defs[2] = {{TR("Yes", "Ja", "Sim"), B_CONFIRM, C_AMBER, 0}, {TR("Cancel", "Abbrechen", "Cancelar"), B_BACK, C_TEXT, 0}};
    centered(confirm1, confirm2, (HEAD_H + buttonRow(defs, 2, 1)) / 2);
    return;
  }

  if (setupMode == SETUP_NETS) {
    int top = HEAD_H + 6, bottom = H - BTN_H - 6;
    if (S.scanState == 1) centered(TR("Looking for networks ...", "Suche Netze ...", "Procurando redes ..."), nullptr, (top + bottom) / 2);
    else if (!S.netCount) centered(TR("No network found", "Kein Netz gefunden", "Nenhuma rede encontrada"), nullptr, (top + bottom) / 2);
    int rows = listRows(top, bottom), pages = max(1, (S.netCount + rows - 1) / rows);
    if (listPage >= pages) listPage = pages - 1;
    for (int i = 0; S.scanState == 2 && i < rows; i++) {
      int idx = listPage * rows + i;
      if (idx >= S.netCount) break;
      snprintf(right, sizeof(right), "%s%s", S.net[idx].locked ? "" : TR("open, ", "offen, ", "aberta, "), signalText(S.net[idx].rssi));
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
    defs[n++] = {TR("Search again", "Neu suchen", "Procurar de novo"), B_SCAN, C_TEXT, 0};
    buttonRow(defs, n, 1);
    return;
  }

  // main page: brightness, display timeout, WLAN client, power off. (The battery calibration is in
  // the web interface and on the debug console; nobody needs it standing at the device.) Everything
  // is part of the scrolling content, so each control can have a full-size button.
  contentBegin(HEAD_H, H - NAV_H);
  const int bw = 72, right0 = W - 10;
  int y = rowY;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_DIM);
  g->drawString(TR("Brightness", "Helligkeit", "Brilho"), 8, y);
  y += 18;
  button(8, y, bw, BTN_H, "-", B_BRIGHT_DOWN, C_TEXT, 0, &fonts::DejaVu24);
  button(right0 - bw, y, bw, BTN_H, "+", B_BRIGHT_UP, C_TEXT, 0, &fonts::DejaVu24);
  int bx = 8 + bw + 8, bwid = right0 - bw - 8 - bx;
  g->fillRoundRect(bx, y + BTN_H / 2 - 7, bwid, 14, 6, C_PANEL);
  g->fillRoundRect(bx, y + BTN_H / 2 - 7, max(14, bwid * (S.brightness + 1) / 256), 14, 6, C_TINT);

  y += BTN_H + 10;
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_DIM);
  g->drawString(TR("Display off", "Display aus", "Apagar tela"), 8, y + BTN_H / 2);
  static char timeout[16];
  if (!S.timeoutS) strlcpy(timeout, TR("never", "nie", "nunca"), sizeof(timeout));
  else if (S.timeoutS < 120) snprintf(timeout, sizeof(timeout), TR("after %u s", "nach %u s", "apos %u s"), S.timeoutS);
  else snprintf(timeout, sizeof(timeout), TR("after %u min", "nach %u min", "apos %u min"), S.timeoutS / 60);
  int tw = min(150, W - 118);
  button(right0 - tw, y, tw, BTN_H, timeout, B_TIMEOUT);

  y += BTN_H + 10;
  g->drawFastHLine(8, y, right0 - 8, C_LINE);
  rowY = y + 8;
  row(TR("Wi-Fi", "WLAN", "Wi-Fi"), !S.staConfigured ? String(TR("off", "aus", "desligado")) : String(S.staSsid));
  if (S.staConfigured)
    row(TR("Status", "Status", "Estado"), S.staConnected ? String(TR("connected, ", "verbunden, ", "conectado, ")) + S.staIp : String(TR("connecting ...", "verbinde ...", "conectando ...")), S.staConnected ? C_GREEN : C_AMBER);
  y = rowY + 4;
  if (S.staConfigured) {
    int w = (right0 - 8 - BTN_GAP) / 2;
    button(8, y, w, BTN_H, TR("Choose network", "Netz waehlen", "Escolher rede"), B_NET_OPEN);
    button(8 + w + BTN_GAP, y, w, BTN_H, TR("Wi-Fi client off", "WLAN-Client aus", "Cliente Wi-Fi desligado"), B_ASK_FORGET, C_AMBER);
  } else {
    button(8, y, right0 - 8, BTN_H, TR("Choose network", "Netz waehlen", "Escolher rede"), B_NET_OPEN);
  }
  y += BTN_H + 10;
  g->drawFastHLine(8, y, right0 - 8, C_LINE);
  y += 10;
  button(8, y, right0 - 8, BTN_H, TR("Switch off", "Ausschalten", "Desligar"), B_ASK_OFF, C_RED);
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
  g->fillRoundRect(x, y, w, h, 12, C_PANEL);
  g->drawRoundRect(x, y, w, h, 12, C_LINE);
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

static String fmtKb(uint32_t kb) {
  char b[16];
  if (kb < 1024) snprintf(b, sizeof(b), "%lu kB", (unsigned long)kb);
  else snprintf(b, sizeof(b), "%lu.%lu MB", (unsigned long)(kb / 1024), (unsigned long)(kb % 1024 * 10 / 1024));
  return String(b);
}

// The card is lent to the USB port: say so, show that something moves, and
// offer the one way back.
static void drawStickDialog() {
  btnCount = 0;
  cTop = cBot = 0;
  const int w = W - 16, x = 8;
  uint32_t color = S.stickEjected ? C_GREEN : S.stickHost ? C_BLUE : C_AMBER;
  String moved = String(TR("read ", "gelesen ", "lido ")) + fmtKb(S.stickReadKb) + TR(", written ", ", geschrieben ", ", gravado ") + fmtKb(S.stickWrittenKb);
  String a[2], b[4];
  g->setFont(&fonts::DejaVu12);
  int na = wrap(moved.c_str(), w - 20, a, 2);
  int nb = wrap(TR("The SD card belongs to the USB port now. Eject it there before ending.", "Die SD-Karte gehoert jetzt dem USB-Port. Dort auswerfen, bevor hier beendet wird.", "O cartao SD pertence agora a porta USB. Ejete-o la antes de encerrar."), w - 20, b, 4);
  int h = 36 + (1 + na + nb) * LINE_H + 16 + BTN_H + 8, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 12, C_PANEL);
  g->drawRoundRect(x, y, w, h, 12, color);
  g->setTextDatum(lgfx::top_center);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString(TR("USB stick", "USB-Stick", "Pendrive USB"), W / 2, y + 8);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(color);
  int ty = y + 36;
  g->drawString(S.stickEjected ? TR("Ejected - safe to unplug", "Ausgeworfen - kann abgezogen werden", "Ejetado - pode desconectar")
                : S.stickHost ? TR("Connected", "Verbunden", "Conectado")
                : TR("Waiting for the USB port", "Wartet auf den USB-Port", "Aguardando a porta USB"), W / 2, ty);
  ty += LINE_H + 2;
  g->setTextColor(C_DIM);
  for (int i = 0; i < na; i++, ty += LINE_H) g->drawString(a[i].c_str(), W / 2, ty);
  ty += 6;
  for (int i = 0; i < nb; i++, ty += LINE_H) g->drawString(b[i].c_str(), W / 2, ty);
  button(x + BTN_GAP, y + h - BTN_H - 8, w - 2 * BTN_GAP, BTN_H, TR("End (restart)", "Beenden (Neustart)", "Encerrar (reiniciar)"), B_STICK_END);
}

// The question the server's request for a console puts to whoever stands at the
// device, and the way to end a console that is open.
static void drawConsoleDialog() {
  bool ask = S.conState == 1;
  btnCount = 0;                          // nothing underneath can be touched
  cTop = cBot = 0;
  int w = W - 16, h = 156, x = 8, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 12, C_PANEL);
  g->drawRoundRect(x, y, w, h, 12, ask ? C_AMBER : C_RED);
  g->setTextDatum(lgfx::top_center);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString(TR("Remote console", "Fernkonsole", "Console remoto"), W / 2, y + 8);
  char text[112];
  if (ask) snprintf(text, sizeof(text), TR("The control center wants to operate %s for %u min. Allow?", "Die Leitstelle moechte %s fuer %u min bedienen. Erlauben?", "A central quer operar %s por %u min. Permitir?"), S.conPort, S.conMinutes);
  else snprintf(text, sizeof(text), TR("The control center is operating %s, %s left.", "Die Leitstelle bedient %s, noch %s.", "A central esta operando %s, restam %s."), S.conPort, S.conLeft);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(C_DIM);
  String lines[3];
  int n = wrap(text, w - 20, lines, 3);
  for (int i = 0; i < n; i++) g->drawString(lines[i].c_str(), W / 2, y + 38 + i * LINE_H);
  int bw = (w - 3 * BTN_GAP) / 2, by = y + h - BTN_H - 8;
  button(x + BTN_GAP, by, bw, BTN_H, ask ? TR("Refuse", "Ablehnen", "Recusar") : TR("Back", "Zurueck", "Voltar"), ask ? B_CON_DENY : B_CON_BACK);
  button(x + 2 * BTN_GAP + bw, by, bw, BTN_H, ask ? TR("Allow", "Erlauben", "Permitir") : TR("End", "Beenden", "Encerrar"), ask ? B_CON_ALLOW : B_CON_END, ask ? C_GREEN : C_RED);
}

static void draw(uint32_t now) {
  if (!S.portOn[selPort]) nextPort();
  btnCount = 0;
  marquee = false;
  contentH = 0;                          // a page without scrolling content leaves it at that
  if (g == &lcd) busLock();
  g->fillScreen(C_BG);
  if (splashUntil) {                     // just switched on: who this is, before anything else
    drawSplash();
    if (g == &lcd) busUnlock();
    flush();
    return;
  }
  drawHeader();
  if (menu) {
    drawMenu();
  } else switch (screen) {
    case S_TERM: screenTerm(); break;
    case S_WIFI: screenNet(); break;
    case S_BT:   screenBt(); break;
    case S_INFO: screenInfo(); break;
    case S_SYSTEM:  screenSystem(); break;
    case S_SCRIPTS: screenScripts(); break;
    case S_SETUP:   screenSetup(); break;
    default:     screenStatus(); break;
  }
  if (!contentH) { cTop = cBot = 0; scrollReset(); }
  drawNav();
  drawSwipe();
  statScroll = scrollY;
  statScrollMax = maxScroll();
  if (scrollY < statScrollMin) statScrollMin = scrollY;
  if (scrollY > statScrollPeak) statScrollPeak = scrollY;
  statMenu = menu;
  if (S.conState == 2) g->fillRect(0, HEAD_H - 2, W, 2, C_RED);
  if (S.stick) drawStickDialog();
  else if (overlay()) drawConsoleDialog();
  else if ((int32_t)(msgUntil - now) > 0) drawMessage();
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
    if (reqSplash) { splashUntil = (now + reqSplash * 1000UL) | 1; reqSplash = 0; dirty = true; }
    if (splashUntil && (int32_t)(now - splashUntil) >= 0) { splashUntil = 0; dirty = true; }
    if (reqScreen >= 0) { gotoScreen(reqScreen); scrollReset(); reqScreen = -1; dirty = true; }
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
      simHold = reqDragHold;
      Serial.printf("[DIAG] %s %d,%d -> %d,%d\n", simHold ? "Hold" : "Drag", simX0, simY0, simX1, simY1);
    }
    if (reqLift) { reqLift = false; simHold = false; simStep = -1; }
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
    scrollTick(now);
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
      // A backlight at 0 is simply dark, so keep a floor. On battery the light is
      // the largest consumer after the radio: an untouched screen drops to a
      // quarter after DIM_MS and comes back with the next touch.
      const int32_t DIM_MS = 15000;
      uint16_t lit = 24 + (uint16_t)S.brightness * 231 / 255;
      if (S.saver && !msgShown && (int32_t)(now - lastActivity) > DIM_MS) lit = max(10, lit / 4);
      if (lit != litAt) { litAt = lit; lcd.setBrightness(lit); }
    }

    static bool msgWasShown = false;
    if (S.conState != 2) conMenu = false;
    if (S.conState == 1) msgShown = true;  // the question stays lit until it is answered
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
    vTaskDelay(pdMS_TO_TICKS(on ? (scrollAnim != A_NONE || touchDown ? 8 : 20) : S.saver ? 120 : 50));
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
  // The card shares the LCD's SPI lines and has to be brought into SPI mode
  // before the first display traffic; main.cpp's later Sd::begin() is then a no-op.
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_LCD_CS, OUTPUT);
  digitalWrite(PIN_LCD_CS, HIGH);
  Sd::begin();
  if (!lcd.init()) return false;
  imuFound = imuBegin();
  Serial.printf("[LCD]  Lagesensor QMI8658: %s\n", imuFound ? "ok" : "nicht gefunden");
  setRotation(settings.displayFlip ? IMU_ROT_DEFAULT ^ 2 : IMU_ROT_DEFAULT);
  Serial.printf("[LCD]  Zeichenpuffer: %s\n", canvasBits == 0 ? "keiner (direkt)" : canvasBits == 16 ? "16 Bit" : "8 Bit");
  buildSnapshot();
  S = shared;
  ready = true;
  on = true;
  uiOn = true;
  lastActivity = millis();
  splashUntil = (millis() + 1500) | 1;   // never 0, which means "none"
  dirty = true;
  reqBrightness = true;
  // Core 0 next to the radio: drawing never takes time from the serial bridge,
  // which runs in the Arduino loop on core 1.
  xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 1, nullptr, 0);
  return true;
}

bool isOn() { return ready && uiOn; }
bool usable() { return ready; }
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
                wakeAfterS ? TR(" or timer", " oder Zeitablauf", " ou temporizador") : "");
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
