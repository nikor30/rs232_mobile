#include "display.h"
#include "config.h"
#include "lang.h"

// Colour touch front end on the Waveshare ESP32-S3-Touch-LCD-2 (ST7789T3
// 240x320, CST816D touch, QMI8658 accelerometer). It implements the Display
// interface from display.h. The pages lay themselves out from the width and
// height they find.
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
#include <esp_wifi.h>
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
#define LCD_CANVAS_BITS 16
// One pattern for every screen: black ground; what can be pressed and what
// belongs together sits on C_PANEL with rounded corners and lightens to C_BTN
// under the finger; sheets and dialogs are C_SHEET. One colour (C_TINT) for what
// leads somewhere; green, amber and red only say how things are.
static const uint32_t C_BG = 0x000000, C_SHEET = 0x1C1C1E, C_PANEL = 0x2C2C2E, C_BTN = 0x3A3A3C, C_LINE = 0x48484A,
                      C_TEXT = 0xF5F5F7, C_DIM = 0x98989D, C_TINT = 0x0A84FF, C_BLUE = 0x0A84FF,
                      C_GREEN = 0x30D158, C_AMBER = 0xFF9F0A, C_RED = 0xFF453A;
static const int PAD = 12;               // from the edge of the screen to anything

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
static const int HEAD_H = 30, NAV_H = 0, BTN_H = 56, BTN_GAP = 8, HEAD_HIT = 78;
// The terminal grid (TERM_MAX_COLS x TERM_MAX_ROWS characters of TERM_CW x TERM_CH
// pixels in TERM_FONT) is the board's: config.h.
static const int LCD_MAX_SIDE = LCD_W > LCD_H ? LCD_W : LCD_H;

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
  bool    c2Down;              // enrolled, but the server is not reached right now
  bool    adminUi;             // the administrator's pages are shown (settings.adminUi)
  bool    stick, stickHost, stickEjected;   // the card is lent to the USB port as a memory stick
  uint32_t stickReadKb, stickWrittenKb;
  uint8_t conState;            // remote console: 0 none, 1 the server asks, 2 open
  uint16_t conMinutes;
  char    conPort[20], conLeft[12];
  char    conCode[8];          // end to end: "123 456" to compare with the browser, empty without
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
  bool    sdRaw;                         // a card without a usable file system: still offered as a USB stick
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
static volatile int reqBench = 0;        // debug console: draw this many pictures in a row and time them
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
  B_PORT, B_BACK, B_LIST_PREV, B_LIST_NEXT, B_CFG_ROW, B_CFG_CANCEL, B_PLAY_ACK, B_NET_OPEN, B_NET_ROW,
  B_ASK_FORGET, B_ASK_OFF, B_ASK_CAL_FULL, B_ASK_CAL_EMPTY, B_ASK_CAL_RESET, B_CONFIRM, B_CON_MENU, B_CON_BACK,
  B_GOTO, B_SH_ACT, B_SH_CON, B_SH_CLOSE, B_RULER, B_HOLD,
  K_CHAR, K_SHIFT, K_SYM, K_BKSP, K_OK,
  // carried out by the main loop
  B_BAUD, B_AUTO, B_BREAK, B_ENTER, B_CTRLC, B_REC, B_BT, B_SD, B_PLAY, B_STOP, B_SCAN, B_JOIN, B_FORGET,
  B_BRIGHT_DOWN, B_BRIGHT_UP, B_TIMEOUT, B_POWEROFF, B_CAL_FULL, B_CAL_EMPTY, B_CAL_RESET,
  B_CON_ALLOW, B_CON_DENY, B_CON_END, B_STICK, B_STICK_END, B_C2_RETRY, B_TAB, B_UP, B_SPACE
};
static const uint8_t FIRST_LOOP_CMD = B_BAUD;

static const uint32_t BAUD_PRESETS[] = {9600, 19200, 38400, 57600, 115200};
static const uint32_t RULER_RATES[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};   // the baud ruler of the home screen
static const int RULER_N = sizeof(RULER_RATES) / sizeof(RULER_RATES[0]);

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

// A station that keeps trying to join (wrong password, 802.1X refused, network
// gone) leaves the radio no room for a scan: every attempt fails and the list
// stays empty - just when somebody wants to pick another network. So the
// attempts are stopped for the scan and taken up again when it is done.
static bool scanPaused = false;          // we stopped the station's attempts
static uint32_t scanTryUntil = 0;        // the scan has not started yet: try until then

static void startScan() {
  if (scanState == 1) return;
  if (Ble::state() != Ble::OFF) WiFi.setSleep(true);       // station + Bluetooth without modem sleep aborts
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);   // scanning needs the station interface
  if (settings.staSsid.length() && WiFi.status() != WL_CONNECTED) {
    WiFi.setAutoReconnect(false);
    esp_wifi_disconnect();
    scanPaused = true;
  }
  WiFi.scanDelete();
  netCount = 0;
  scanState = 1;
  scanTryUntil = millis() + 4000;
  if (WiFi.scanNetworks(true) != WIFI_SCAN_FAILED) scanTryUntil = 0;
}

static void scanResume() {
  if (!scanPaused) return;
  scanPaused = false;
  WiFi.setAutoReconnect(true);
  esp_wifi_connect();
}

static void pollScan() {
  if (scanState != 1) return;
  if (scanTryUntil) {                    // the radio was still busy with the last attempt to join
    static uint32_t triedAt = 0;
    if (millis() - triedAt < 250) return;
    triedAt = millis();
    if (WiFi.scanNetworks(true) != WIFI_SCAN_FAILED) scanTryUntil = 0;
    else if ((int32_t)(millis() - scanTryUntil) > 0) { scanTryUntil = 0; scanState = 2; scanResume(); }
    return;
  }
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
  scanResume();
}

struct Snapshot;
static void fillSystem(Snapshot &s);
static void fillScripts(Snapshot &s);
static void fillNets(Snapshot &s);

static uint32_t stickDemoUntil = 0;      // show the stick dialog without lending the card (debug console)
static uint32_t conDemoUntil = 0;        // the same for the remote console: asked (1) or open (2)
static uint8_t conDemoState = 1;
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
    s.c2Down = C2::down();
  }
  C2::Console rc = C2::console();
  s.conState = rc.state;
  if (rc.state) {
    s.conMinutes = rc.minutes;
    copyStr(s.conPort, sizeof(s.conPort), Store::portName(rc.port));
    if (rc.code[0]) snprintf(s.conCode, sizeof(s.conCode), "%.3s %s", rc.code, rc.code + 3);
    else s.conCode[0] = 0;
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
  s.sdRaw = Sd::unreadable();
  Sd::Stick st = Sd::stick();
  if (conDemoUntil && (int32_t)(millis() - conDemoUntil) < 0) {      // debug: "conui STATE N"
    s.conState = conDemoState;
    s.conMinutes = 15;
    strlcpy(s.conPort, "Port 1", sizeof(s.conPort));
    strlcpy(s.conLeft, "14 min", sizeof(s.conLeft));
    strlcpy(s.conCode, "482 913", sizeof(s.conCode));
  }
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
      c.baud = arg ? RULER_RATES[min<int>(arg, RULER_N) - 1] : BAUD_PRESETS[i < n ? (i + 1) % n : 0];   // arg: the rate itself, from the ruler
      Bridge::apply(port, c);
      Net::markDirty();
      break;
    }
    case B_AUTO:  Bridge::startAutobaud(port); break;
    case B_BREAK: Bridge::sendBreak(port); message(TR("BREAK sent", "BREAK gesendet", "BREAK enviado"), "", 800); break;
    case B_ENTER: Bridge::write(port, (const uint8_t *)"\r", 1); break;
    case B_CTRLC: Bridge::write(port, (const uint8_t *)"\x03", 1); break;
    case B_TAB:   Bridge::write(port, (const uint8_t *)"\t", 1); break;
    case B_UP:    Bridge::write(port, (const uint8_t *)"\x1b[A", 3); break;     // the last command again
    case B_SPACE: Bridge::write(port, (const uint8_t *)" ", 1); break;           // "--More--"
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
    case B_C2_RETRY:
      C2::reconnect();
      message(TR("Control center", "Leitstelle", "Central"), TR("connecting ...", "verbindet ...", "conectando ..."), 2500);
      break;
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
  Serial.printf("[DIAG] up %lu s, Heap %u kB (min %u kB, groesster Block %u kB), Reset-Grund %d\n", (unsigned long)(now / 1000),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024), (int)esp_reset_reason());
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
    else if (sscanf(line, "bench %d", &a) == 1) reqBench = constrain(a, 1, 1000);
    else if (!strcmp(line, "rot auto")) reqRot = 4;
    else if (sscanf(line, "rot %d", &a) == 1) reqRot = a & 3;
    else if (!strcmp(line, "cfgtest")) {             // a small configuration to try the Skripte page with
      const char *err = Configs::save("Demo", "show version\n@pause 1\nshow clock\n", "");
      Serial.printf("[DIAG] Konfiguration \"Demo\": %s\n", err ? err : "gespeichert");
    }
    else if (!strcmp(line, "bat")) Serial.printf("[DIAG] Akku: %s\n", Power::diag().c_str());
    else if (!strcmp(line, "eap")) Serial.printf("[DIAG] EAP: %s\n", Net::eapDiag().c_str());
    else if (!strcmp(line, "pins")) Bridge::requestPinProbe(0);
    else if (sscanf(line, "pins %d", &a) == 1) Bridge::requestPinProbe(a - 1);
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
    else if (sscanf(line, "conui %d %d", &a, &b) == 2) { conDemoState = constrain(a, 1, 2); conDemoUntil = (millis() + constrain(b, 1, 60) * 1000UL) | 1; }
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
static uint32_t bandHash[LCD_MAX_SIDE / BAND_H + 1];
static bool forceFull = true;


static void setRotation(uint8_t r) {
  rot = r & 3;
  uiRot = rot;
  busLock();
  lcd.setRotation(rot);
  W = lcd.width();
  H = lcd.height();
  canvas.deleteSprite();
  {
  canvas.setPsram(true);
  canvas.setColorDepth(LCD_CANVAS_BITS);   // the panel's own depth: sending is a plain copy then
  g = &canvas;
  canvasBits = LCD_CANVAS_BITS;
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
  static bool changed[LCD_MAX_SIDE / BAND_H + 1];
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
static bool shDrag = false, rulerDrag = false;   // home screen: a sheet or the baud ruler is under the finger
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
  return touchDown && !touchSwallow && !scrolling && !swiping && !shDrag && !rulerDrag && !stoppedByTouch && inside(b, touchX0, touchY0) && inside(b, touchX, touchY);
}

static void button(int x, int y, int w, int h, const char *label, uint8_t id, uint32_t textColor = C_TEXT,
                   uint8_t arg = 0, const lgfx::IFont *font = &fonts::DejaVu18) {
  bool down = isDown(x, y, w, h);
  int r = min(12, min(w, h) / 3);
  // under the finger it lightens and gives way a little - at once, not on release
  if (down) g->fillRoundRect(x + 2, y + 1, w - 4, h - 2, r, C_BTN);
  else g->fillRoundRect(x, y, w, h, r, C_PANEL);
  g->setFont(font);                      // as large as the label allows
  if (g->textWidth(label) > w - 8) g->setFont(&fonts::DejaVu12);
  if (g->textWidth(label) > w - 6) g->setFont(&fonts::DejaVu9);      // a long label on a narrow button
  g->setTextDatum(lgfx::middle_center);
  g->setTextColor(textColor);
  g->drawString(fit(label, w - 4).c_str(), x + w / 2, y + h / 2);
  addHotspot(x, y, w, h, id, arg);
}

struct BtnDef { const char *label; uint8_t id; uint32_t color; uint8_t arg; };

// The buttons at the bottom of a page. They go on two rows when their labels do
// not fit side by side (upright, the screen is 240 pixels wide). Returns the top
// of the area they take, which is where the page content has to end.
static int buttonRow(const BtnDef *defs, int n, int maxRows = 2) {
  int bottom = H - 6;
  g->setFont(&fonts::DejaVu12);
  int w = (W - 2 * PAD - BTN_GAP * (n - 1)) / n;
  bool fits = true;
  for (int i = 0; i < n; i++) fits &= g->textWidth(defs[i].label) + 8 <= w;
  int rows = maxRows > 1 && n > 1 && !fits ? 2 : 1;
  int top = bottom - rows * BTN_H - (rows - 1) * BTN_GAP;
  int first = rows == 2 ? (n + 1) / 2 : n;
  for (int r = 0; r < rows; r++) {
    int from = r ? first : 0, cnt = r ? n - first : first;
    w = (W - 2 * PAD - BTN_GAP * (cnt - 1)) / cnt;
    // one size for the whole row: the largest in which every label fits
    const lgfx::IFont *font = &fonts::DejaVu18;
    g->setFont(font);
    for (int i = 0; i < cnt; i++)
      if (g->textWidth(defs[from + i].label) > w - 8) font = &fonts::DejaVu12;
    for (int i = 0; i < cnt; i++)
      button(PAD + i * (w + BTN_GAP), top + r * (BTN_H + BTN_GAP), w, BTN_H, defs[from + i].label, defs[from + i].id,
             defs[from + i].color, defs[from + i].arg, font);
  }
  return top;
}

static bool shown(int i) { return i != S_SYSTEM || S.adminUi; }
enum Sheet : uint8_t { SH_NONE = 0, SH_ACT, SH_CON };   // the home screen's sheets, see below
static void sheetAway();
static void sheetGo(uint8_t which, float to, float v);
static void gotoScreen(uint8_t s) {
  screen = s % S_COUNT;
  sheetAway();
  if (screen == S_WIFI) { screen = S_STATUS; sheetGo(SH_CON, 1, 0); }   // how to get in is the home screen's sheet from above
  uiScreen = screen;
  setupMode = SETUP_MAIN;
  listPage = 0;
  cfgSel = -1;
}

// ---- home: one screen per port, a sheet from below to do things and one from
// above to get a phone connected. Everything here follows the finger one to
// one, keeps its speed when let go, and lands where the movement was going.
static const int HOME_BAR = 28, RULER_PITCH = 56;
static uint8_t sheet = SH_NONE;
static float shPos = 0, shVel = 0, shTo = 0, sh0 = 0;      // 0 away .. 1 fully out
static bool shAnim = false;
static float rulerPos = 3, rulerVel = 0, rulerTo = 3, ruler0 = 0;   // index into RULER_RATES under the marker
static bool rulerAnim = false;
static uint32_t rulerSetAt = 0;          // the finger chose a rate: the port needs a moment to report it
static float pageOff = 0, pageVel = 0;   // the port screens, pixels away from rest
static bool pageAnim = false;
static float holdP = 0;                  // BREAK: how far the hold has come, 0..1
static bool holdFired = false;
struct HoldKey { int16_t x, y, w, h; uint8_t cmd, arg; };
static HoldKey holds[4];                 // the keys of this frame that are held, not tapped
static uint8_t holdN = 0;
static uint8_t holdCmd = 0, holdArg = 0; // the one being filled, and what it does when full
static uint32_t homeAt = 0;

static bool home() { return screen == S_STATUS; }
static int shTravel() { return W >= 300 ? 212 : 276; }   // upright the keys stand in two columns: a taller sheet

// A spring, solved exactly for the time that passed (zeta 1: no bounce).
static void springStep(float &x, float &v, float to, float dt, float response, float zeta) {
  float w = 2 * 3.14159f / response, d = x - to;
  if (zeta >= 1) {
    float c = v + w * d, f = expf(-w * dt);
    x = to + (d + c * dt) * f;
    v = (c - w * (d + c * dt)) * f;
  } else {
    float wd = w * sqrtf(1 - zeta * zeta), b = (v + zeta * w * d) / wd, f = expf(-zeta * w * dt), co = cosf(wd * dt), si = sinf(wd * dt);
    x = to + f * (d * co + b * si);
    v = f * ((b * wd - zeta * w * d) * co - (d * wd + zeta * w * b) * si);
  }
}
static float project(float v) { return v * 0.099f; }                 // where a flick comes to rest (Apple's decay, rate 0.99)
static float banded(float x, float lo, float hi, float dim) { return x < lo ? lo - rubber(lo - x, dim) : x > hi ? hi + rubber(x - hi, dim) : x; }

static int baudIndex(int port) {
  uint32_t baud = strtoul(S.serial[port], nullptr, 10);
  for (int i = 0; i < RULER_N; i++) if (RULER_RATES[i] == baud) return i;
  return -1;
}
static void rulerSync() {
  int i = baudIndex(selPort);
  rulerPos = rulerTo = i < 0 ? 3 : i;
  rulerVel = 0;
  rulerAnim = false;
}
static void sheetGo(uint8_t which, float to, float v) {
  if (to > 0 && sheet == SH_NONE) { sheet = which; if (which == SH_ACT) rulerSync(); }
  shTo = to;
  shVel = v;
  shAnim = true;
  dirty = true;
}
static void sheetAway() { sheet = SH_NONE; shPos = shTo = shVel = 0; shAnim = false; }
static void rulerGo(int to, float v) {
  rulerTo = constrain(to, 0, RULER_N - 1);
  rulerVel = v;
  rulerAnim = true;
  rulerSetAt = millis();
  dirty = true;
}
static uint8_t portStep(int dir) {
  for (uint8_t i = 1; i <= MAX_PORTS; i++) {
    uint8_t p = (selPort + MAX_PORTS + dir * i) % MAX_PORTS;
    if (S.portOn[p]) return p;
  }
  return selPort;
}
// Sideways there is only somewhere to go with a second port; without, the screen gives a little.
static float homeOff(int dx) { return portStep(dx < 0 ? 1 : -1) != selPort ? dx : dx < 0 ? -rubber(-dx, W) : rubber(dx, W); }

static void sendCmd(uint8_t id, uint8_t arg = 0) {
  uint32_t cmd = (uint32_t)id << 16 | (uint32_t)arg << 8 | selPort;
  xQueueSend(cmdQueue, &cmd, 0);
}

static void pressed(uint8_t id, uint8_t arg) {
  switch (id) {
    case B_PORT: nextPort(); return;
    case B_SH_ACT: sheetGo(SH_ACT, 1, 0); return;
    case B_SH_CON: sheetGo(SH_CON, 1, 0); return;
    case B_SH_CLOSE: sheetGo(sheet, 0, 0); return;
    case B_HOLD: return;                             // held, not tapped: homeTick() counts
    case B_RULER: rulerGo(lroundf(rulerTo) + (arg ? 1 : -1), 0); id = B_BAUD; arg = lroundf(rulerTo) + 1; break;
    case B_STICK: sheetAway(); break;
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
  dirty = true;
  pressed(id, arg);
  if (s != screen || m != setupMode) scrollReset();   // a different page starts at its top
}

// What moves by itself on the home screen, and the key that is held.
static void homeTick(uint32_t now) {
  float dt = (now - homeAt) / 1000.0f;
  homeAt = now;
  if (dt <= 0 || dt > 0.5f) return;
  if (shAnim) {
    springStep(shPos, shVel, shTo, dt, 0.32f, 0.9f);
    if (fabsf(shPos - shTo) < 0.004f && fabsf(shVel) < 0.05f) {
      shPos = shTo;
      shVel = 0;
      shAnim = false;
      if (shTo == 0) sheet = SH_NONE;
    }
    dirty = true;
  }
  if (rulerAnim) {
    springStep(rulerPos, rulerVel, rulerTo, dt, 0.3f, 0.8f);
    if (fabsf(rulerPos - rulerTo) < 0.01f && fabsf(rulerVel) < 0.1f) { rulerPos = rulerTo; rulerAnim = false; }
    dirty = true;
  }
  if (pageAnim) {
    springStep(pageOff, pageVel, 0, dt, 0.34f, 0.9f);
    if (fabsf(pageOff) < 0.6f && fabsf(pageVel) < 10) { pageOff = 0; pageAnim = false; }
    dirty = true;
  }
  // What cannot be taken back is held, not tapped: the key fills while the finger
  // stays on it and acts when full. Letting go early takes it back.
  const HoldKey *key = nullptr;
  for (int i = 0; i < holdN && touchDown && !touchSwallow && !shDrag && !rulerDrag; i++) {
    Btn b = {holds[i].x, holds[i].y, holds[i].w, holds[i].h, 0, 0};
    if (inside(b, touchX0, touchY0) && inside(b, touchX, touchY)) key = &holds[i];
  }
  bool holding = key != nullptr;
  if (holding && !holdFired && holdCmd != key->cmd) { holdCmd = key->cmd; holdP = 0; }
  if (holding) holdArg = key->arg;
  if (!touchDown) holdFired = false;
  if (holding && !holdFired) {
    holdP += dt / 0.7f;
    // the finger is still down when the key has done its work: what appears under it must not take the release as a tap
    if (holdP >= 1) { holdP = 1; holdFired = true; touchSwallow = true; press(holdCmd, holdArg); }
    dirty = true;
  } else if (holdP > 0 && !(holding && holdFired)) {
    holdP = max(0.0f, holdP - dt / 0.2f);
    dirty = true;
  }
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
      shDrag = rulerDrag = false;
      sh0 = shPos;
      ruler0 = rulerPos;
      if (shAnim || pageAnim) stoppedByTouch = true;       // caught in flight: it stops here and presses nothing
      shAnim = pageAnim = false;
      shTo = shPos;
      if (rulerAnim) { rulerAnim = false; rulerTo = rulerPos; }
      dirty = true;
    }
    if (trailN == sizeof(trail) / sizeof(trail[0])) memmove(trail, trail + 1, sizeof(trail) - sizeof(trail[0])), trailN--;
    trail[trailN++] = {now, (int16_t)x, (int16_t)y};
    touchX = x; touchY = y;
    int dx = x - touchX0, dy = y - touchY0;
    // Which gesture this is shows in its first ten pixels; from then on it stays that.
    bool hm = home() && !overlay();
    if (!scrolling && !swiping && !shDrag && !rulerDrag && !touchSwallow) {
      if (hm && sheet == SH_ACT && sh0 > 0.9f && abs(dx) > 10 && abs(dx) > abs(dy) && touchY0 >= H - shTravel() + 8 && touchY0 < H - shTravel() + 76) rulerDrag = true;
      else if (hm && abs(dy) > 10 && abs(dy) > abs(dx)) {
        if (sheet == SH_NONE) { sheet = dy < 0 ? SH_ACT : SH_CON; if (sheet == SH_ACT) rulerSync(); }
        shDrag = true;
      } else if (hm && sheet != SH_NONE) { /* sideways on a sheet outside the ruler: nothing */ }
      else if (abs(dy) > 10 && abs(dy) > abs(dx) && touchY0 >= cTop && touchY0 < cBot && (maxScroll() || stoppedByTouch)) scrolling = true;
      else if (hm && abs(dx) > 10 && abs(dx) > abs(dy)) swiping = true;   // sideways there is only the next port
    }
    if (scrolling) {
      float to = scroll0 - dy, most = maxScroll(), vis = cBot - cTop;
      if (to < 0) to = -rubber(-to, vis);
      else if (to > most) to = most + rubber(to - most, vis);
      scrollPos = to;
      if (lroundf(to) != scrollY) { scrollY = lroundf(to); dirty = true; }
    }
    if (swiping && dx != swipeDx) { swipeDx = dx; dirty = true; }
    if (shDrag) {
      float to = banded(sh0 + (sheet == SH_ACT ? -dy : dy) / (float)shTravel(), 0, 1, 1);
      if (to != shPos) { shPos = shTo = to; dirty = true; }
    }
    if (rulerDrag) {
      float to = banded(ruler0 - dx / (float)RULER_PITCH, 0, RULER_N - 1, 3);
      if (to != rulerPos) { rulerPos = rulerTo = to; dirty = true; }
    }
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
  if (shDrag) {                          // it lands where the movement was going, speed and all
    shDrag = false;
    float v = (sheet == SH_ACT ? -vy : vy) / shTravel();
    sheetGo(sheet, shPos + project(v) > 0.5f ? 1 : 0, v);
    return;
  }
  if (rulerDrag) {
    rulerDrag = false;
    float v = -vx / RULER_PITCH;
    rulerGo(lroundf(rulerPos + project(v)), v);
    sendCmd(B_BAUD, lroundf(rulerTo) + 1);
    return;
  }
  if (home() && wasSwipe) {              // the next port, or back to this one
    int dxh = touchX - touchX0;
    uint8_t nb = portStep(dxh < 0 ? 1 : -1);
    bool back = (dxh < 0 && vx > 120) || (dxh > 0 && vx < -120);
    pageOff = homeOff(dxh);
    if (nb != selPort && !back && (abs(dxh) > SWIPE_COMMIT || fabsf(dxh + vx * 0.12f) > SWIPE_COMMIT * 1.6f)) {
      selPort = nb;
      uiPort = selPort;
      pageOff += dxh < 0 ? W : -W;       // the same picture, counted from the new port
    }
    pageVel = vx;
    pageAnim = true;
    return;
  }
  if (home() && sheet != SH_NONE && shPos > 0.001f && shPos < 0.999f) { sheetGo(sheet, shPos > 0.5f ? 1 : 0, 0); return; }
  if (home() && caught) return;
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

// The battery: its level, a bolt while charging. Returns the colour it was drawn in.
static uint32_t batterySymbol(int bx, int yMid) {
  bool ext = S.batCharge != Power::ON_BATTERY;
  uint32_t col = S.batLow && !ext ? C_RED : ext ? C_GREEN : C_DIM;
  int by = yMid - 6;
  g->drawRect(bx, by, 22, 12, col);
  g->fillRect(bx + 22, by + 3, 2, 6, col);
  g->fillRect(bx + 2, by + 2, (S.batPct * 18 + 50) / 100, 8, col);
  if (S.batCharge == Power::CHARGING) {
    g->fillTriangle(bx + 13, by + 1, bx + 7, by + 7, bx + 11, by + 7, C_TEXT);
    g->fillTriangle(bx + 9, by + 11, bx + 15, by + 5, bx + 11, by + 5, C_TEXT);
  }
  return col;
}

// Top right on every screen: who is connected, whether it records, the battery.
static void statusCorner(int yMid) {
  int x = W - 88;                        // left of the battery and its percentage
  if (S.recording[selPort]) { g->fillCircle(x, yMid, 4, C_RED); x -= 13; }
  if (S.bleState != Ble::OFF) { g->fillCircle(x, yMid, 4, S.bleState == Ble::CONNECTED ? C_BLUE : C_LINE); x -= 13; }
  if (S.webClients || S.tcpConnected) g->fillCircle(x, yMid, 4, C_GREEN);
  if (!S.batMeasured || !S.batPresent) return;
  uint32_t col = batterySymbol(W - 38, yMid);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", S.batPct);
  g->setFont(&fonts::DejaVu9);
  g->setTextDatum(lgfx::middle_right);
  g->setTextColor(col);
  g->drawString(buf, W - 43, yMid);
}

// The top line of every page but the home screen and the terminal: the arrow
// leads back to where the page was opened from, beside it what the page is.
static void drawHeader() {
  bool sub = modal();
  chevron(14, HEAD_H / 2, 0, isDown(0, 0, 96, HEAD_H + 6) ? C_TEXT : C_TINT);
  const char *title = sub ? (setupMode == SETUP_NETS ? TR("Choose Wi-Fi", "WLAN waehlen", "Escolher Wi-Fi")
                      : setupMode == SETUP_CONFIRM && confirmId == B_FORGET ? TR("Wi-Fi client", "WLAN-Client", "Cliente Wi-Fi")
                      : setupMode == SETUP_CONFIRM && confirmId == B_POWEROFF ? TR("Switch off", "Ausschalten", "Desligar")
                      : setupMode == SETUP_CONFIRM && confirmId >= B_CAL_FULL && confirmId <= B_CAL_RESET ? TR("Calibrate battery", "Akku kalibrieren", "Calibrar bateria") : netSsid)
                    : S.conState == 2 ? TR("Remote console", "Fernkonsole", "Console remoto") : TITLES[screen];
  g->setFont(&fonts::DejaVu18);
  if (g->textWidth(title) > W - 118) g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(S.conState == 2 && !sub ? C_RED : C_TEXT);
  g->drawString(fit(title, sub ? W - 36 : W - 118).c_str(), 28, HEAD_H / 2 + 1);
  if (sub) { addHotspot(0, 0, W, HEAD_H + 6, B_BACK); return; }
  if (S.conState == 2) addHotspot(96, 0, W - 96, HEAD_H + 6, B_CON_MENU);     // a tap there leads to ending it
  addHotspot(0, 0, 96, HEAD_H + 6, B_GOTO, screen == S_BT || screen == S_INFO || screen == S_SYSTEM ? S_SETUP : S_STATUS);
  statusCorner(HEAD_H / 2);
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
  g->fillRoundRect(W - 5, cTop + at, 3, h, 1, C_LINE);
}

// What belongs together stands on one card. Its rows run twice: once to learn
// how tall the card is, once to draw on it - so rows draw nothing while measuring.
static bool measuring = false;
template <typename F> static void card(F rows) {
  int y0 = rowY;
  measuring = true;
  rowY = y0 + 10;
  rows();
  int y1 = rowY + 4;
  measuring = false;
  if (y1 > cTop && y0 < cBot) g->fillRoundRect(PAD, y0, W - 2 * PAD, y1 - y0, 12, C_PANEL);
  rowY = y0 + 10;
  rows();
  rowY = y1 + 8;
}

// A name and its value. A long value continues on the next line instead of being cut off.
static bool row(const char *label, const String &value, uint32_t color = C_TEXT) {
  g->setFont(&fonts::DejaVu12);
  String lines[8];
  int lx = PAD + 10, vx = PAD + 84, n = max(1, wrap(value.c_str(), W - PAD - 10 - vx, lines, 8));
  if (!measuring && rowY + n * LINE_H > cTop && rowY < cBot) {
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    g->drawString(label, lx, rowY);
    g->setTextColor(color);
    for (int i = 0; i < n; i++) g->drawString(lines[i].c_str(), vx, rowY + i * LINE_H);
  }
  rowY += n * LINE_H + 6;
  return true;
}

// A small word over what follows.
static void caption(const char *text) {
  g->setFont(&fonts::DejaVu9);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_DIM);
  g->drawString(text, PAD + 2, rowY);
  rowY += 14;
}

// A number to read from a step away: what it is, and the digits, on a card.
static void bigCode(const char *what, const char *digits) {
  if (rowY + 78 > cTop && rowY < cBot) {
    g->fillRoundRect(PAD, rowY, W - 2 * PAD, 78, 12, C_PANEL);
    g->setTextDatum(lgfx::middle_center);
    g->setFont(&fonts::DejaVu12);
    g->setTextColor(C_DIM);
    g->drawString(what, W / 2, rowY + 15);
    g->setFont(&fonts::DejaVu40);
    g->setTextColor(C_TEXT);
    g->drawString(digits, W / 2, rowY + 50);
  }
  rowY += 86;
}

// A thin bar: how full something is, 0..100.
static void meter(int x, int y, int w, int pct, uint32_t color) {
  g->fillRoundRect(x, y, w, 6, 3, C_BTN);
  if (pct > 0) g->fillRoundRect(x, y, max(6, w * constrain(pct, 0, 100) / 100), 6, 3, color);
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

// ---- the trace: what the port did in the last eight seconds, one bar per tenth
// of a second. Received bytes rise, sent bytes fall. It moves in steps, so it
// reads the same at any frame rate, and only its own bands are sent to the panel.
static const int TRACE_N = 80;
static uint8_t traceRx[MAX_PORTS][TRACE_N], traceTx[MAX_PORTS][TRACE_N];   // 0..255 of a busy line
static uint32_t traceSeenRx[MAX_PORTS], traceSeenTx[MAX_PORTS], traceAt = 0;
static bool traceLive[MAX_PORTS], traceBegun = false;

static uint8_t traceLevel(uint32_t bytes, uint32_t baud) {
  if (!bytes) return 0;
  float full = max(1.0f, baud / 100.0f);                 // what a busy line carries in a tenth of a second
  return (uint8_t)constrain(lroundf(255 * sqrtf(min(1.0f, bytes / full))), 24L, 255L);   // the root: one key press shows
}

// Every 100 ms: one more bar for each port. True if the selected port's picture moved.
static bool traceTick(uint32_t now) {
  if (now - traceAt < 100) return false;
  traceAt = now;
  bool moved = false;
  for (int p = 0; p < S.ports && p < MAX_PORTS; p++) {
    uint32_t baud = strtoul(S.serial[p], nullptr, 10);
    // counters start again when a port is opened anew: no bar for that
    uint32_t dr = traceBegun && S.rx[p] >= traceSeenRx[p] ? S.rx[p] - traceSeenRx[p] : 0;
    uint32_t dt = traceBegun && S.tx[p] >= traceSeenTx[p] ? S.tx[p] - traceSeenTx[p] : 0;
    traceSeenRx[p] = S.rx[p];
    traceSeenTx[p] = S.tx[p];
    if (!dr && !dt && !traceLive[p]) continue;           // a flat line stays as it is
    memmove(traceRx[p], traceRx[p] + 1, TRACE_N - 1);
    memmove(traceTx[p], traceTx[p] + 1, TRACE_N - 1);
    traceRx[p][TRACE_N - 1] = traceLevel(dr, baud);
    traceTx[p][TRACE_N - 1] = traceLevel(dt, baud);
    bool live = false;
    for (int i = 0; i < TRACE_N && !live; i++) live = traceRx[p][i] || traceTx[p][i];
    traceLive[p] = live;
    moved |= p == selPort;
  }
  traceBegun = true;
  return moved;
}

// The home screen answers one question: can I work on this port? One word, large
// enough to read from a step away, the line settings, the counters, and the
// trace of what the port is doing. Each port has its own; a sideways drag
// brings the next.
static void homePort(int p, int ox) {
  const char *state = "Ready";             // the same word in every language: the owner finds it punchier
  uint32_t color = C_GREEN;
  String sub = S.serial[p];
  if (S.conState == 2) { state = TR("Remote", "Fernkonsole", "Remoto"); color = C_AMBER; sub = String(S.conPort) + "  " + S.conLeft; }
  else if (S.autobaud[p]) { state = TR("Listening", "Lauscht", "Ouvindo"); color = C_AMBER; }
  else if (S.playState == Player::RUNNING) { state = TR("Playing", "Skript", "Script"); color = C_AMBER; }
  else if (S.webClients || S.tcpConnected || S.bleState == Ble::CONNECTED) { state = TR("In use", "In Benutzung", "Em uso"); color = C_BLUE; }

  bool wide = W >= 300;
  g->fillCircle(ox + 16, HOME_BAR / 2, 4, color);
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_TEXT);
  g->drawString(fit(S.portName[p], W - 150).c_str(), ox + 28, HOME_BAR / 2 + 1);

  g->setTextDatum(lgfx::top_left);
  g->setFont(&fonts::DejaVu40);
  if (g->textWidth(state) > (wide ? W - 124 : W - 20)) g->setFont(&fonts::DejaVu24);
  g->setTextColor(color);
  g->drawString(state, ox + 12, 36);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString(sub.c_str(), ox + 13, 80);

  g->setFont(&fonts::DejaVu12);
  for (int k = 0; k < 2; k++) {
    bool lit = k ? S.txActive[p] : S.rxActive[p];
    String val = fmtBytes(k ? S.tx[p] : S.rx[p]);
    int x = wide ? ox + W - 112 : ox + 16 + k * (W / 2 - 8), y = wide ? 46 + k * 22 : 116;   // beside the word, or in a row under it
    g->fillCircle(x, y, 3, lit ? (k ? C_BLUE : C_GREEN) : C_LINE);
    g->setTextDatum(lgfx::middle_left);
    g->setTextColor(C_DIM);
    g->drawString(k ? "TX" : "RX", x + 8, y);
    g->setTextColor(C_TEXT);
    if (wide) { g->setTextDatum(lgfx::middle_right); g->drawString(val.c_str(), ox + W - 12, y); }
    else g->drawString(val.c_str(), x + 34, y);
  }
}

// The trace of one port at a horizontal offset.
static void homeTrace(int p, int ox) {
  bool wide = W >= 300;
  int ty = wide ? 108 : 136, th = H - ty - 36;
  if (th < 24) return;
  static const uint32_t RXC[3] = {0x10451D, 0x208A3A, C_GREEN}, TXC[3] = {0x042B55, 0x0757AA, C_BLUE};   // the oldest bars fade out
  int half = (th - 2) / 2, axis = ty + half, pitch = max(2, W / TRACE_N), x0 = ox + W - TRACE_N * pitch;
  for (int i = 0; i < TRACE_N; i++) {
    int x = x0 + i * pitch;
    if (x < -pitch || x >= W) continue;
    int r = traceRx[p][i] * half / 255, t = traceTx[p][i] * half / 255, age = i < 6 ? 0 : i < 12 ? 1 : 2;
    g->fillRect(x, axis - 1 - r, pitch - 1, r + 1, r ? RXC[age] : C_PANEL);
    if (t) g->fillRect(x, axis + 1, pitch - 1, t, TXC[age]);
  }
}

// A key that is held: it fills from the left while the finger stays on it.
static void holdAdd(int x, int y, int w, int h, uint8_t cmd, uint8_t arg) {
  if (holdN < sizeof(holds) / sizeof(holds[0])) holds[holdN++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, cmd, arg};
  addHotspot(x, y, w, h, B_HOLD);
}
static void holdFill(int x, int y, int w, int h, int r, uint8_t cmd, uint32_t color) {
  if (holdP <= 0 || holdCmd != cmd) return;
  g->setClipRect(x, y, lroundf(w * holdP), h);
  g->fillRoundRect(x, y, w, h, r, color);
  g->clearClipRect();
}
static void holdKey(int x, int y, int w, int h, const char *label, uint8_t cmd, uint8_t arg = 0, uint32_t fill = 0x8A2A24u) {
  g->fillRoundRect(x, y, w, h, 12, C_PANEL);
  holdFill(x, y, w, h, 12, cmd, fill);
  g->setFont(&fonts::DejaVu18);
  if (g->textWidth(label) > w - 8) g->setFont(&fonts::DejaVu12);
  if (g->textWidth(label) > w - 6) g->setFont(&fonts::DejaVu9);
  g->setTextDatum(lgfx::middle_center);
  g->setTextColor(C_TEXT);
  g->drawString(fit(label, w - 4).c_str(), x + w / 2, y + h / 2);
  holdAdd(x, y, w, h, cmd, arg);
}

// A key of the sheet: a small drawing and a word. With hold, it is held instead of tapped.
static void tile(int x, int y, int w, int kind, const char *label, uint32_t color, uint8_t id, uint8_t arg = 0, bool hold = false) {
  const int h = 56;
  bool down = !hold && isDown(x, y, w, h);
  g->fillRoundRect(x, y, w, h, 12, down ? C_BTN : C_PANEL);
  if (hold) holdFill(x, y, w, h, 12, id, 0x7A4D05u);
  int ix = x + 10, iy = y + 9;             // an 18 px drawing, in lines
  switch (kind) {
    case 0: g->drawArc(ix + 9, iy + 10, 8, 7, 150, 390, color); g->drawLine(ix + 9, iy + 10, ix + 13, iy + 4, color); break;   // auto-baud: a dial
    case 1: g->drawFastHLine(ix, iy + 9, 6, color); g->drawFastHLine(ix + 12, iy + 9, 6, color);                              // BREAK: a line with a gap
            g->drawFastVLine(ix + 6, iy + 5, 9, color); g->drawFastVLine(ix + 12, iy + 5, 9, color); break;
    case 2: if (arg) g->fillCircle(ix + 9, iy + 9, 6, C_RED); else g->drawCircle(ix + 9, iy + 9, 6, color); break;           // record
    case 3: g->drawTriangle(ix + 5, iy + 3, ix + 15, iy + 9, ix + 5, iy + 15, color); break;                                 // scripts
    case 4: g->drawRoundRect(ix + 1, iy + 5, 11, 8, 2, color); g->drawRect(ix + 12, iy + 7, 5, 4, color); break;             // USB stick
    default: g->drawFastHLine(ix + 1, iy + 5, 16, color); g->drawFastHLine(ix + 1, iy + 13, 16, color);                      // settings
             g->fillCircle(ix + 12, iy + 5, 2, color); g->fillCircle(ix + 6, iy + 13, 2, color); break;
  }
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(id == B_BREAK ? C_AMBER : C_TEXT);
  g->drawString(fit(label, w - 14).c_str(), x + 10, y + 36);
  if (hold) {
    g->setFont(&fonts::DejaVu9);
    g->setTextDatum(lgfx::top_right);
    g->setTextColor(C_DIM);
    g->drawString(TR("hold", "halten", "segurar"), x + w - 8, y + 12);
    if (shPos > 0.95f) holdAdd(x, y, w, h, id, 0); else addHotspot(x, y, w, h, B_HOLD);
  } else addHotspot(x, y, w, h, id, id == B_GOTO ? arg : 0);
}

// From below: what can be done to this port.
static void sheetActions() {
  int top = H - lroundf(shPos * shTravel());
  addHotspot(0, 0, W, H, B_SH_CLOSE);                // beside the sheet: away with it
  g->fillRoundRect(0, top, W, H - top + 20, 16, C_SHEET);
  addHotspot(0, top, W, H, B_HOLD);                  // the sheet itself swallows what misses a key
  g->fillRoundRect(W / 2 - 18, top + 4, 36, 4, 2, C_BTN);

  // the port reports another rate than the ruler shows (auto-baud found one): follow it
  int now = baudIndex(selPort);
  if (now >= 0 && now != lroundf(rulerTo) && !rulerDrag && millis() - rulerSetAt > 1500) rulerGo(now, 0);

  int near = constrain((int)lroundf(rulerPos), 0, RULER_N - 1);
  char num[12];
  snprintf(num, sizeof(num), "%lu", (unsigned long)RULER_RATES[near]);
  g->setFont(&fonts::DejaVu24);
  g->setTextDatum(lgfx::top_center);
  g->setTextColor(C_TEXT);
  g->drawString(num, W / 2, top + 13);
  int nw = g->textWidth(num);
  g->setFont(&fonts::DejaVu9);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(S.autobaud[selPort] ? C_AMBER : C_DIM);
  g->drawString(S.autobaud[selPort] ? TR("listening", "lauscht", "ouvindo") : "baud", W / 2 + nw / 2 + 6, top + 26);
  if (top + 42 < H) {
    g->setClipRect(0, top + 42, W, min(34, H - top - 42));
    g->setTextDatum(lgfx::top_center);
    for (int i = 0; i < RULER_N; i++) {
      int x = W / 2 + lroundf((i - rulerPos) * RULER_PITCH);
      if (x < -30 || x > W + 30) continue;
      g->fillRect(x, top + 46, 1, 8, i == near ? C_TEXT : C_DIM);
      if (i < RULER_N - 1) for (int m = 1; m < 4; m++) g->fillRect(x + m * RULER_PITCH / 4, top + 48, 1, 4, C_LINE);
      snprintf(num, sizeof(num), "%lu", (unsigned long)RULER_RATES[i]);
      g->setTextColor(i == near ? C_TEXT : C_DIM);
      g->drawString(num, x, top + 60);
    }
    g->clearClipRect();
  }
  g->fillTriangle(W / 2 - 5, top + 42, W / 2 + 5, top + 42, W / 2, top + 47, C_BLUE);
  addHotspot(0, top + 8, W / 2 - 28, 68, B_RULER, 0);
  addHotspot(W / 2 + 28, top + 8, W / 2 - 28, 68, B_RULER, 1);

  int cols = W >= 300 ? 3 : 2, w = (W - 28 - (cols - 1) * 8) / cols;
  bool rec = S.recording[selPort], card = S.sdMounted || S.sdRaw;
  struct { int kind; const char *label; uint32_t color; uint8_t id, arg; bool hold; } t[6] = {
    {0, "Auto-baud", S.autobaud[selPort] ? C_AMBER : C_DIM, B_AUTO, 0, false},
    {1, "BREAK", C_AMBER, B_BREAK, 0, true},
    {2, rec ? "Stop" : TR("Record", "Mitschnitt", "Gravar"), S.sdMounted ? C_DIM : C_LINE, (uint8_t)(S.sdMounted ? B_REC : B_SH_CLOSE), rec, false},
    {3, TR("Scripts", "Skripte", "Scripts"), C_DIM, B_GOTO, S_SCRIPTS, false},
    {4, TR("USB stick", "USB-Stick", "Pendrive"), card ? C_DIM : C_LINE, (uint8_t)(card ? B_STICK : B_SH_CLOSE), 0, card},   // the way back is a restart: held
    {5, TR("Settings", "Setup", "Ajustes"), C_DIM, B_GOTO, S_SETUP, false}};
  for (int i = 0; i < 6; i++) tile(14 + (i % cols) * (w + 8), top + 80 + (i / cols) * 64, w, t[i].kind, t[i].label, t[i].color, t[i].id, t[i].arg, t[i].hold);
}

// From above: what a phone needs to get in.
static void sheetConnect() {
  int oy = lroundf(shPos * shTravel()) - shTravel();
  addHotspot(0, 0, W, H, B_SH_CLOSE);
  g->fillRoundRect(0, oy - 20, W, shTravel() + 20, 16, C_SHEET);
  addHotspot(0, 0, W, oy + shTravel(), B_HOLD);
  g->fillRoundRect(W / 2 - 18, oy + shTravel() - 8, 36, 4, 2, C_BTN);
  bool wide = W >= 300;
  int qs = 128, qx = wide ? 16 : (W - qs) / 2, qy = oy + 14;
  if (qy + qs > 0) {
    String payload = "WIFI:T:WPA;S:" + qrEscape(S.apSsid) + ";P:" + qrEscape(S.apPass) + ";;";
    // Working the code out takes 25 ms: once per hotspot name and password, not once per picture
    static lgfx::LGFX_Sprite code;
    static String codeFor;
    if (payload != codeFor) {
      if (!code.getBuffer()) { code.setPsram(true); code.setColorDepth(LCD_CANVAS_BITS); code.createSprite(qs - 12, qs - 12); }
      code.fillSprite(0xFFFFFFu);
      code.qrcode(payload.c_str(), 0, 0, qs - 12, 1);
      codeFor = payload;
    }
    g->fillRoundRect(qx, qy, qs, qs, 8, 0xFFFFFFu);
    if (code.getBuffer()) code.pushSprite(g, qx + 6, qy + 6);
    else g->qrcode(payload.c_str(), qx + 6, qy + 6, qs - 12, 1);
  }
  g->setTextDatum(lgfx::top_left);
  const char *labels[3] = {"Hotspot", TR("Password", "Passwort", "Senha"), TR("Then open", "Dann oeffnen", "Depois abrir")};
  const char *values[3] = {S.apSsid, S.apPass, S.apIp};
  if (wide) {
    for (int i = 0; i < 3; i++) {
      int y = oy + 16 + i * 42;
      g->setFont(&fonts::DejaVu9);
      g->setTextColor(C_DIM);
      g->drawString(labels[i], 160, y);
      g->setFont(&fonts::DejaVu12);
      g->setTextColor(C_TEXT);
      g->drawString(fit(values[i], W - 172).c_str(), 160, y + 14);
    }
  } else {
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::top_center);
    g->setTextColor(C_TEXT);
    for (int i = 0; i < 3; i++) g->drawString(fit(values[i], W - 16).c_str(), W / 2, qy + qs + 6 + i * 16);
  }
  static char pin[16];
  snprintf(pin, sizeof(pin), "PIN %06lu", (unsigned long)S.blePin);
  String right[3] = {lanLine(), S.bleState == Ble::OFF ? String(TR("off", "aus", "desligado")) : String(pin), String(S.c2)};
  const char *left[3] = {"LAN", "Bluetooth", TR("Control center", "Leitstelle", "Central")};
  uint32_t cols[3] = {!S.staConfigured ? C_LINE : S.staConnected ? C_GREEN : C_AMBER, S.bleState == Ble::OFF ? C_LINE : C_BLUE, S.c2Down ? C_AMBER : C_GREEN};
  g->setFont(&fonts::DejaVu9);
  for (int i = 0; i < 3; i++) {
    int y = oy + (wide ? 152 : 208) + i * 15;
    g->fillCircle(20, y + 5, 3, cols[i]);
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    g->drawString(left[i], 30, y);
    g->setTextDatum(lgfx::top_right);
    g->setTextColor(C_TEXT);
    g->drawString(fit(right[i], W - 150).c_str(), W - 16, y);
  }
}

static void screenStatus() {
  cTop = cBot = 0;
  float off = swiping ? homeOff(swipeDx) : pageOff;
  int ox = lroundf(off);
  homePort(selPort, ox);
  homeTrace(selPort, ox);
  if (ox) {                                // the neighbour comes into view beside it
    uint8_t nb = portStep(ox < 0 ? 1 : -1);
    if (nb != selPort) { homePort(nb, ox + (ox < 0 ? W : -W)); homeTrace(nb, ox + (ox < 0 ? W : -W)); }
  }

  // what stays in place: link lamps, battery, which port of how many, and the handle of the sheet
  if (ox) g->fillRect(W - 96, 0, 96, HOME_BAR, C_BG);    // a port name sliding past stays out of it
  statusCorner(HOME_BAR / 2);
  int n = 0, at = 0;
  for (int p = 0; p < MAX_PORTS; p++) if (S.portOn[p]) { if (p == selPort) at = n; n++; }
  for (int k = 0; k < n && n > 1; k++) g->fillCircle(W / 2 + (2 * k - (n - 1)) * 6, H - 24, k == at ? 3 : 2, k == at ? C_TEXT : C_DIM);
  g->fillRoundRect(W / 2 - 18, H - 12, 36, 4, 2, C_BTN);

  int ty = W >= 300 ? 108 : 136;
  addHotspot(0, ty, W, H - ty - 40, B_GOTO, S_TERM);                  // the trace opens the terminal
  if (S.conState == 2) addHotspot(0, HOME_BAR + 2, W, ty - HOME_BAR - 2, B_CON_MENU);
  addHotspot(0, 0, W, HOME_BAR + 4, B_SH_CON);
  addHotspot(0, H - 40, W, 40, B_SH_ACT);
  if (sheet == SH_ACT) sheetActions();
  else if (sheet == SH_CON) sheetConnect();
}

// The terminal: the port's own text in green, and the keys a waiting switch
// asks for. Everything else that can be done to the port is on the home
// screen's sheet; the arrow leads back there.
static void screenTerm() {
  chevron(14, HOME_BAR / 2, 0, isDown(0, 0, 90, HOME_BAR + 6) ? C_TEXT : C_TINT);
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_TEXT);
  g->drawString(fit(S.portName[selPort], W - 130).c_str(), 28, HOME_BAR / 2 + 1);
  g->setTextDatum(lgfx::middle_right);
  g->setTextColor(C_DIM);
  g->drawString(S.serial[selPort], W - 8, HOME_BAR / 2 + 1);
  if (S.recording[selPort]) g->fillCircle(W - 12 - g->textWidth(S.serial[selPort]) - 8, HOME_BAR / 2, 4, C_RED);
  addHotspot(0, 0, 90, HOME_BAR + 6, B_GOTO, S_STATUS);

  static const struct { const char *label; uint8_t id; } KEYS[5] = {{"Enter", B_ENTER}, {"Ctrl-C", B_CTRLC}, {"Tab", B_TAB}, {"^", B_UP}, {"Space", B_SPACE}};
  const int kh = 48, kw = (W - 6 * 4) / 5, ky = H - kh - 4;
  for (int i = 0; i < 5; i++) button(4 + i * (kw + 4), ky, kw, kh, KEYS[i].label, KEYS[i].id, C_TEXT, 0, &fonts::DejaVu12);
  int top = HOME_BAR + 2, bottom = ky - 2;

  int rows = constrain((bottom - top) / TERM_CH, 1, TERM_MAX_ROWS);
  if (rows != termRowsUi) {
    termRowsUi = rows;
    uiTermRows = rows;
    uiGeometry = uiGeometry + 1;         // the main loop rebuilds the grid for the new size
  }
  g->setFont(&TERM_FONT);
  g->setTextDatum(lgfx::top_left);
  g->setTextColor(C_GREEN);
  for (int r = 0; r < termRowsUi; r++)
    if (grid[r][0]) g->drawString(grid[r], 2, top + r * TERM_CH);
  if ((millis() / 500) % 2 && gridY < termRowsUi)
    g->fillRect(2 + min(gridX, termColsUi - 1) * TERM_CW, top + gridY * TERM_CH, TERM_CW, TERM_CH, C_GREEN);
}

static void screenBt() {
  uint8_t s = S.bleState;
  const char *text = s == Ble::OFF ? TR("Off", "Aus", "Desligado") : s == Ble::ADVERTISING ? TR("Ready, waiting for a connection", "Bereit, wartet auf Verbindung", "Pronto, aguardando conexao")
                   : s == Ble::PAIRING ? TR("Pairing: enter the PIN on the phone", "Kopplung: PIN am Handy eingeben", "Pareamento: digite o PIN no celular") : TR("Connected", "Verbunden", "Conectado");
  uint32_t color = s == Ble::OFF ? C_DIM : s == Ble::CONNECTED ? C_BLUE : s == Ble::PAIRING ? C_AMBER : C_GREEN;
  BtnDef def = {s == Ble::OFF ? TR("Switch Bluetooth on", "Bluetooth einschalten", "Ligar o Bluetooth") : TR("Switch Bluetooth off", "Bluetooth ausschalten", "Desligar o Bluetooth"), B_BT, C_TEXT};
  contentBegin(HEAD_H, buttonRow(&def, 1) - 4);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(color);
  rowY += wrapped(text, PAD + 2, rowY, W - 2 * PAD - 4) + 8;
  if (s != Ble::OFF) {
    char pin[10];
    snprintf(pin, sizeof(pin), "%03lu %03lu", (unsigned long)(S.blePin / 1000), (unsigned long)(S.blePin % 1000));
    bigCode("PIN", pin);
  }
  card([&] {
    row(TR("Name", "Name", "Nome"), S.apSsid);
    row(TR("Service", "Dienst", "Servico"), "Nordic UART (BLE)");
    row(TR("Port", "Port", "Porta"), String(S.portName[0]) + ", " + S.serial[0]);
  });
  contentEnd();
}

static void screenInfo() {
  int bottom = H - NAV_H;
  {
    BtnDef def[2] = {{TR("Mount SD card", "SD-Karte einbinden", "Montar cartao SD"), B_SD, C_TEXT},
                     {TR("Connect to center", "Leitstelle verbinden", "Conectar a central"), B_C2_RETRY, C_TINT}};
    if (S.sdMounted || S.sdRaw) def[0] = {TR("SD card as USB stick", "SD-Karte als USB-Stick", "Cartao SD como pendrive"), B_STICK, C_TEXT};
    // only while there is something to do about it: no contact with the server
    bottom = buttonRow(def, S.c2Down ? 2 : 1) - 4;
  }
  contentBegin(HEAD_H, bottom);
  String tcp = !S.tcpEnabled ? String(TR("off", "aus", "desligado"))
             : S.ports > 1 ? ":" + String(RAW_TCP_PORT) + "-" + String(RAW_TCP_PORT + MAX_PORTS - 1)
             : ":" + String(RAW_TCP_PORT);
  char imu[40];
  if (imuAddr) snprintf(imu, sizeof(imu), TR("rotation %u", "Drehung %u", "rotacao %u"), rot);
  else strlcpy(imu, TR("sensor not found", "Sensor nicht gefunden", "sensor nao encontrado"), sizeof(imu));
  if (S.c2Code[0]) {
    char spaced[10];                     // three and three, to be read in bad light
    snprintf(spaced, sizeof(spaced), "%.3s %s", S.c2Code, S.c2Code + 3);
    bigCode(TR("Code for the control center", "Code fuer die Leitstelle", "Codigo para a central"), spaced);
  }
  // who this is: mark and name as on the web interfaces
  drawMark(PAD, rowY, 34);
  g->setTextDatum(lgfx::middle_left);
  g->setFont(&fonts::DejaVu18);
  g->setTextColor(C_TEXT);
  g->drawString("RS232 Console", PAD + 44, rowY + 17);
  rowY += 44;
  card([&] {
    row("Firmware", String(FW_VERSION) + "  up " + fmtUptime(millis() / 1000));
    row("Host", String(S.hostname) + ".local");
    row(TR("Battery", "Akku", "Bateria"), batteryLine());
    row("SD", sdLine());
    row(TR("Position", "Lage", "Posicao"), imu);
    row("Board", BOARD_NAME);
  });
  card([&] {
    row("Hotspot", String(S.apStations) + TR(" devices", " Geraete", " dispositivos"));
    row("Clients", clientsLine());
    row("Raw-TCP", tcp);
    row(TR("Center", "Leitstelle", "Central"), S.c2);
  });
  contentEnd();
}

// How full something is: its name, the figures, and a thin bar under them.
static void bar(const char *label, int pct, const String &text, uint32_t color) {
  int x = PAD + 10, w = W - 2 * (PAD + 10);
  if (!measuring) {
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    g->drawString(label, x, rowY);
    g->setTextDatum(lgfx::top_right);
    g->setTextColor(C_TEXT);
    g->drawString(fit(text, w - 60).c_str(), x + w, rowY);
    meter(x, rowY + 17, w, pct, color);
  }
  rowY += 30;
}

static uint32_t loadColor(int pct) { return pct >= 85 ? C_RED : pct >= 60 ? C_AMBER : C_GREEN; }

static void screenSystem() {
  contentBegin(HEAD_H, H - NAV_H);
  card([&] {
    bar("CPU 0", S.cpu[0], String(S.cpu[0]) + TR(" %  radio + display", " %  Funk + Anzeige", " %  radio + tela"), loadColor(S.cpu[0]));
    bar("CPU 1", S.cpu[1], String(S.cpu[1]) + TR(" %  bridge", " %  Bruecke", " %  ponte"), loadColor(S.cpu[1]));
    int ram = S.heapTotal ? 100 - (int)((uint64_t)S.heapFree * 100 / S.heapTotal) : 0;
    bar("RAM", ram, String((S.heapTotal - S.heapFree) / 1024) + TR(" of ", " von ", " de ") + String(S.heapTotal / 1024) + " kB", loadColor(ram));
    if (S.psramTotal) {
      int ps = 100 - (int)((uint64_t)S.psramFree * 100 / S.psramTotal);
      bar("PSRAM", ps, String((S.psramTotal - S.psramFree) / 1024) + TR(" of ", " von ", " de ") + String(S.psramTotal / 1024) + " kB", C_TINT);
    }
  });
  card([&] {
    row(TR("RAM free", "RAM frei", "RAM livre"), String(S.heapFree / 1024) + " kB, min. " + String(S.heapMin / 1024) + " kB");
    row("Block", String(S.heapBlock / 1024) + TR(" kB in one piece", " kB am Stueck", " kB contiguos"));
    row(TR("Timing", "Takt", "Ciclo"), TR("pause ", "Pause ", "pausa ") + String(S.loopMs) + TR(" ms, frame ", " ms, Bild ", " ms, quadro ") + String(S.drawMs) + " ms");
    row("CPU", String(S.cpuMhz) + " MHz" + (S.saver ? TR(", power saving (battery)", ", Sparmodus (Akku)", ", economia (bateria)") : ""));
    row("Chip", String(S.tempC) + " C,  " + String(S.tasks) + " Tasks");
    row(TR("Uptime", "Laufzeit", "Ligado ha"), fmtUptime(millis() / 1000));
  });
  contentEnd();
}

// ---- lists (stored configurations, WLAN networks): rows to tap, paged
static const int LIST_ROW_H = 44;

static int listRows(int top, int bottom) { return max(1, (bottom - top) / LIST_ROW_H); }

static void listRow(int index, int y, const char *text, const char *right, bool selected, uint8_t id) {
  int w = W - 2 * PAD, h = LIST_ROW_H - 4;
  bool down = isDown(PAD, y, w, h);
  g->fillRoundRect(PAD, y, w, h, 12, selected || down ? C_BTN : C_PANEL);
  g->setFont(&fonts::DejaVu12);
  g->setTextColor(selected ? C_TINT : C_TEXT);          // the chosen one carries the tint
  int rw = right ? g->textWidth(right) + 10 : 0;
  drawMarquee(text, PAD + 12, y + h / 2, w - 24 - rw);
  if (right) {
    g->setTextDatum(lgfx::middle_right);
    g->setTextColor(C_DIM);
    g->drawString(right, W - PAD - 12, y + h / 2);
  }
  addHotspot(PAD, y, w, LIST_ROW_H, id, index);
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
    contentBegin(HEAD_H, buttonRow(&def, 1) - 4);
    int x = PAD + 10, w = W - 2 * (PAD + 10), y0 = rowY;
    uint32_t col = running ? C_TINT : S.playState == Player::DONE ? C_GREEN : C_RED;
    String where = String(TR("on ", "auf ", "em ")) + S.portName[S.playPort] + TR(",  line ", ",  Zeile ", ",  linha ") + S.playLine + TR(" of ", " von ", " de ") + S.playLines;
    g->setFont(&fonts::DejaVu12);
    String wl[3], rl[4];
    int nw = wrap(where.c_str(), w, wl, 3), nr = wrap(S.playResult, w, rl, 4);
    g->fillRoundRect(PAD, y0, W - 2 * PAD, 10 + 26 + nw * LINE_H + 8 + 14 + nr * LINE_H + 8, 12, C_PANEL);
    rowY = y0 + 10;
    g->setFont(&fonts::DejaVu18);
    g->setTextColor(C_TEXT);
    drawMarquee(S.playName, x, rowY + 10, w);
    rowY += 26;
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::top_left);
    g->setTextColor(C_DIM);
    for (int i = 0; i < nw; i++, rowY += LINE_H) g->drawString(wl[i].c_str(), x, rowY);
    rowY += 8;
    meter(x, rowY, w, S.playLines ? S.playLine * 100 / S.playLines : 0, col);
    rowY += 14;
    g->setTextColor(running ? C_TEXT : col);
    for (int i = 0; i < nr; i++, rowY += LINE_H) g->drawString(rl[i].c_str(), x, rowY);
    rowY += 8;
    contentEnd();
    return;
  }

  bool stacked = W < 300 && cfgSel >= 0;             // upright the two keys of a chosen script stand one over the other
  int top = HEAD_H + 6, bottom = H - 6 - (stacked ? 2 * BTN_H + BTN_GAP : BTN_H) - 6;
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
  if (cfgSel >= 0 && cfgSel < S.cfgCount) {          // picked: sending it cannot be taken back, so it is held
    static char send[44];
    snprintf(send, sizeof(send), TR("Hold: send to port %u", "Halten: an Port %u senden", "Segurar: enviar a porta %u"), selPort + 1);
    int y = H - 6 - BTN_H, wc = stacked ? W - 2 * PAD : 100, wh = stacked ? wc : W - 2 * PAD - BTN_GAP - wc;
    holdKey(PAD, stacked ? y - BTN_H - BTN_GAP : y, wh, BTN_H, send, B_PLAY, (uint8_t)cfgSel, 0x7A4D05u);
    button(stacked ? PAD : PAD + wh + BTN_GAP, y, wc, BTN_H, TR("Cancel", "Abbrechen", "Cancelar"), B_CFG_CANCEL, C_TEXT, 0, &fonts::DejaVu12);
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
    defs[n++] = {label, B_PORT, C_TINT, 0};
  }
  if (n) buttonRow(defs, n, 1);
  else centered(TR("Tap one, then send", "Antippen, dann senden", "Toque em um e envie"), nullptr, H - 6 - BTN_H / 2);
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
  button(x0 + 1, y, kw * 3 / 2 - 2, h, kbLayer == 1 ? "abc" : "ABC", K_SHIFT, C_TINT);
  button(x0 + kw * 3 / 2 + 1, y, kw * 3 / 2 - 2, h, kbLayer == 2 ? "abc" : "#+=", K_SYM, C_TINT);
  button(x0 + kw * 3 + 1, y, kw * 3 - 2, h, TR("Space", "Leer", "Espaco"), K_CHAR, C_DIM, ' ');
  button(x0 + kw * 6 + 1, y, kw * 2 - 2, h, "<-", K_BKSP, C_AMBER);
  button(x0 + kw * 8 + 1, y, kw * 2 - 2, h, "OK", K_OK, strlen(kbText) >= 8 ? C_GREEN : C_LINE);
}

static const char *signalText(int rssi) { return rssi >= -60 ? TR("strong", "stark", "forte") : rssi >= -75 ? TR("medium", "mittel", "medio") : TR("weak", "schwach", "fraco"); }

static void screenSetup() {
  static char pager[12], right[20];
  if (setupMode == SETUP_PASS) {
    int top = HEAD_H + 4;
    g->fillRoundRect(PAD, top, W - 2 * PAD, 24, 8, C_PANEL);
    g->setFont(&fonts::DejaVu12);
    g->setTextDatum(lgfx::middle_left);
    String shown = kbText;                           // long keys: show the end, that is where typing happens
    while (shown.length() > 1 && g->textWidth((shown + "_").c_str()) > W - 2 * PAD - 16) shown.remove(0, 1);
    if (kbText[0]) {
      g->setTextColor(C_TEXT);
      g->drawString((shown + "_").c_str(), PAD + 8, top + 12);
    } else {
      g->setTextColor(C_DIM);
      g->drawString(fit(TR("Wi-Fi key (8-63 characters)", "WLAN-Schluessel (8-63 Zeichen)", "Senha do Wi-Fi (8-63 caracteres)"), W - 2 * PAD - 16).c_str(), PAD + 8, top + 12);
    }
    keyboard(top + 28);
    return;
  }

  if (setupMode == SETUP_CONFIRM) {                 // each of these restarts or switches off: yes is held
    int y = H - 6 - BTN_H, w = (W - 2 * PAD - BTN_GAP) / 2;
    holdKey(PAD, y, w, BTN_H, TR("Hold: yes", "Halten: Ja", "Segurar: sim"), B_CONFIRM, 0, 0x7A4D05u);
    button(PAD + w + BTN_GAP, y, w, BTN_H, TR("Cancel", "Abbrechen", "Cancelar"), B_BACK);
    centered(confirm1, confirm2, (HEAD_H + y) / 2);
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
    defs[n++] = {TR("Scan", "Suchen", "Buscar"), B_SCAN, C_TINT, 0};
    buttonRow(defs, n, 1);
    return;
  }

  // main page: brightness, display timeout, WLAN client, power off. (The battery calibration is in
  // the web interface and on the debug console; nobody needs it standing at the device.) Everything
  // is part of the scrolling content, so each control can have a full-size button.
  contentBegin(HEAD_H, H - NAV_H);
  const int bw = 64, right0 = W - PAD;
  caption(TR("Brightness", "Helligkeit", "Brilho"));
  int y = rowY;
  button(PAD, y, bw, BTN_H, "-", B_BRIGHT_DOWN, C_TEXT, 0, &fonts::DejaVu24);
  button(right0 - bw, y, bw, BTN_H, "+", B_BRIGHT_UP, C_TEXT, 0, &fonts::DejaVu24);
  int bx = PAD + bw + 16, bwid = right0 - bw - 16 - bx, knob = bx + bwid * S.brightness / 255;
  meter(bx, y + BTN_H / 2 - 3, bwid, S.brightness * 100 / 255, C_TINT);
  g->fillCircle(knob, y + BTN_H / 2, 9, C_TEXT);

  y += BTN_H + 12;
  static char timeout[16];
  if (!S.timeoutS) strlcpy(timeout, TR("never", "nie", "nunca"), sizeof(timeout));
  else if (S.timeoutS < 120) snprintf(timeout, sizeof(timeout), TR("after %u s", "nach %u s", "apos %u s"), S.timeoutS);
  else snprintf(timeout, sizeof(timeout), TR("after %u min", "nach %u min", "apos %u min"), S.timeoutS / 60);
  button(PAD, y, right0 - PAD, BTN_H, "", B_TIMEOUT);            // one key: what it sets at the left, how it stands at the right
  g->setFont(&fonts::DejaVu12);
  g->setTextDatum(lgfx::middle_left);
  g->setTextColor(C_TEXT);
  g->drawString(TR("Display off", "Display aus", "Apagar tela"), PAD + 12, y + BTN_H / 2);
  g->setFont(&fonts::DejaVu18);
  g->setTextDatum(lgfx::middle_right);
  g->setTextColor(C_TINT);
  g->drawString(timeout, right0 - 12, y + BTN_H / 2);

  rowY = y + BTN_H + 12;
  card([&] {
    row(TR("Wi-Fi", "WLAN", "Wi-Fi"), !S.staConfigured ? String(TR("off", "aus", "desligado")) : String(S.staSsid));
    if (S.staConfigured)
      row(TR("Status", "Status", "Estado"), S.staConnected ? String(TR("connected, ", "verbunden, ", "conectado, ")) + S.staIp : String(TR("connecting ...", "verbinde ...", "conectando ...")),
          S.staConnected ? C_GREEN : C_AMBER);
  });
  y = rowY;
  if (S.staConfigured) {
    int w = (right0 - PAD - BTN_GAP) / 2;
    button(PAD, y, w, BTN_H, TR("Choose network", "Netz waehlen", "Escolher rede"), B_NET_OPEN, C_TEXT, 0, &fonts::DejaVu12);
    button(PAD + w + BTN_GAP, y, w, BTN_H, TR("Wi-Fi client off", "WLAN-Client aus", "Cliente Wi-Fi desligado"), B_ASK_FORGET, C_AMBER, 0, &fonts::DejaVu12);
  } else {
    button(PAD, y, right0 - PAD, BTN_H, TR("Choose network", "Netz waehlen", "Escolher rede"), B_NET_OPEN);
  }
  y += BTN_H + 16;
  {
    int n = S.adminUi ? 3 : 2, w = (right0 - PAD - (n - 1) * BTN_GAP) / n;
    static const uint8_t PAGES[3] = {S_BT, S_INFO, S_SYSTEM};
    for (int k = 0; k < n; k++) button(PAD + k * (w + BTN_GAP), y, w, BTN_H, TITLES[PAGES[k]], B_GOTO, C_TINT, PAGES[k], &fonts::DejaVu12);   // these lead somewhere: tint
  }
  y += BTN_H + 16;
  button(PAD, y, right0 - PAD, BTN_H, TR("Switch off", "Ausschalten", "Desligar"), B_ASK_OFF, C_RED);
  rowY = y + BTN_H + 4;
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
  g->fillRoundRect(x, y, w, h, 16, C_SHEET);
  g->drawRoundRect(x, y, w, h, 16, C_LINE);
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
  const int w = W - 2 * PAD, x = PAD;
  uint32_t color = S.stickEjected ? C_GREEN : S.stickHost ? C_BLUE : C_AMBER;
  String moved = String(TR("read ", "gelesen ", "lido ")) + fmtKb(S.stickReadKb) + TR(", written ", ", geschrieben ", ", gravado ") + fmtKb(S.stickWrittenKb);
  String a[2], b[4];
  g->setFont(&fonts::DejaVu12);
  int na = wrap(moved.c_str(), w - 20, a, 2);
  int nb = wrap(TR("The SD card belongs to the USB port now. Eject it there before ending.", "Die SD-Karte gehoert jetzt dem USB-Port. Dort auswerfen, bevor hier beendet wird.", "O cartao SD pertence agora a porta USB. Ejete-o la antes de encerrar."), w - 20, b, 4);
  int h = 36 + (1 + na + nb) * LINE_H + 16 + BTN_H + 8, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 16, C_SHEET);
  g->drawRoundRect(x, y, w, h, 16, C_LINE);
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
  holdKey(x + BTN_GAP, y + h - BTN_H - 8, w - 2 * BTN_GAP, BTN_H, TR("Hold to end (restarts)", "Halten: Beenden (Neustart)", "Segurar: encerrar (reinicia)"), B_STICK_END);
}

// The question the server's request for a console puts to whoever stands at the
// device, and the way to end a console that is open.
static void drawConsoleDialog() {
  bool ask = S.conState == 1;
  btnCount = 0;                          // nothing underneath can be touched
  cTop = cBot = 0;
  int w = W - 2 * PAD, h = S.conCode[0] ? 178 : 156, x = PAD, y = (H - h) / 2;
  g->fillRoundRect(x, y, w, h, 16, C_SHEET);
  g->drawRoundRect(x, y, w, h, 16, C_LINE);
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
  int n = wrap(text, w - 20, lines, S.conCode[0] ? 2 : 3);
  for (int i = 0; i < n; i++) g->drawString(lines[i].c_str(), W / 2, y + 38 + i * LINE_H);
  if (S.conCode[0]) {
    // encrypted between the browser and this device: the same six digits there means nobody in between
    g->setFont(&fonts::DejaVu24);
    g->setTextColor(C_TEXT);
    g->drawString(S.conCode, W / 2, y + 44 + 2 * LINE_H);
  }
  int bw = (w - 3 * BTN_GAP) / 2, by = y + h - BTN_H - 8;
  button(x + BTN_GAP, by, bw, BTN_H, ask ? TR("Refuse", "Ablehnen", "Recusar") : TR("Back", "Zurueck", "Voltar"), ask ? B_CON_DENY : B_CON_BACK);
  if (ask) button(x + 2 * BTN_GAP + bw, by, bw, BTN_H, TR("Allow", "Erlauben", "Permitir"), B_CON_ALLOW, C_GREEN);
  else holdKey(x + 2 * BTN_GAP + bw, by, bw, BTN_H, TR("Hold to end", "Halten: Ende", "Segurar: fim"), B_CON_END);
}

static uint32_t drawOnlyUs = 0, fillOnlyUs = 0;   // the last picture: time to draw it (without sending), and of that the clearing
static void draw(uint32_t now) {
  uint32_t drawT0 = micros();
  if (!S.portOn[selPort]) nextPort();
  btnCount = 0;
  holdN = 0;
  marquee = false;
  contentH = 0;                          // a page without scrolling content leaves it at that
  if (g == &lcd) busLock();
  g->fillScreen(C_BG);
  fillOnlyUs = micros() - drawT0;
  if (splashUntil) {                     // just switched on: who this is, before anything else
    drawSplash();
    if (g == &lcd) busUnlock();
    flush();
    return;
  }
  bool hm = home(), bare = hm || screen == S_TERM;       // these two bring their own top line
  if (!bare) drawHeader();
  switch (screen) {
    case S_TERM: screenTerm(); break;
    case S_BT:   screenBt(); break;
    case S_INFO: screenInfo(); break;
    case S_SYSTEM:  screenSystem(); break;
    case S_SCRIPTS: screenScripts(); break;
    case S_SETUP:   screenSetup(); break;
    default:     screenStatus(); break;
  }
  if (!contentH) { cTop = cBot = 0; scrollReset(); }
  statScroll = scrollY;
  statScrollMax = maxScroll();
  if (scrollY < statScrollMin) statScrollMin = scrollY;
  if (scrollY > statScrollPeak) statScrollPeak = scrollY;
  statMenu = sheet != SH_NONE;
  if (S.conState == 2 && !bare) g->fillRect(0, HEAD_H - 2, W, 2, C_RED);
  if (S.stick) drawStickDialog();
  else if (overlay()) drawConsoleDialog();
  else if ((int32_t)(msgUntil - now) > 0) drawMessage();
  if (g == &lcd) busUnlock();
  drawOnlyUs = micros() - drawT0;
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
  static uint16_t px[LCD_MAX_SIDE];
  static char out[LCD_MAX_SIDE * 8 + 32];
  static const size_t OUT_SIZE = sizeof(out);
  Serial.printf("[SHOT] %d %d\n", W, H);
  Serial.flush();
  vTaskDelay(pdMS_TO_TICKS(20));
  for (int y = max(0, (int)reqShotFrom); y < H && y <= reqShotTo; y++) {
    canvas.readRect(0, y, W, 1, px);
    size_t o = snprintf(out, OUT_SIZE, "%03d:", y);
    uint32_t sum = 0;
    for (int x = 0; x < W;) {
      int n = 1;
      while (x + n < W && px[x + n] == px[x]) n++;
      o += snprintf(out + o, OUT_SIZE - o, "%04X*%X,", px[x], n);
      sum = sum * 31 + px[x] * 7 + n;
      x += n;
    }
    o += snprintf(out + o, OUT_SIZE - o, "#%04X\n", (unsigned)(sum & 0xFFFF));
    rawWrite(out, o);
  }
  rawWrite("[SHOT] end\n", 11);
}

static int benchLeft = 0, benchTotal = 0;
static uint32_t benchStart = 0, benchDrawUs = 0, benchFillUs = 0;

// Shortest time between two pictures. The SPI panel takes 40 ms for a full one
// anyway; the RGB panel shows a new one every 17 ms.
static const uint32_t DRAW_MIN_MS = 40;

static void uiTask(void *) {
  uint32_t lastDraw = 0, lastTick = 0xFFFFFFFF, seenTerm = 0, seenMsg = 0, lastStallWarn = 0;
  for (;;) {
    uint32_t now = millis();
    if (uiBeat && (int32_t)(now - uiBeat) > (int32_t)statUiGapMax) statUiGapMax = now - uiBeat;
    uiBeat = now;

    // requests from the main loop
    if (reqWake) { reqWake = false; uiWake(); }
    if (reqOff) { reqOff = false; uiOff(); }
    if (reqNext) { reqNext = false; if (home()) nextPort(); else gotoScreen(S_STATUS); scrollReset(); dirty = true; }   // the button: home, and from there the next port
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
    homeTick(now);
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
      if (traceTick(now) && screen == S_STATUS) changed = true;
      if (changed) dirty = true;
      if (reqBench > 0 && !benchLeft) {    // debug console "bench N": N pictures of this page, one after the other
        benchLeft = benchTotal = reqBench;
        reqBench = 0;
        benchStart = micros();
        benchDrawUs = 0;
        benchFillUs = 0;
      }
      if (benchLeft) { dirty = true; forceFull = true; }   // the whole picture each time: the worst case is what is measured
      if (dirty && (benchLeft || now - lastDraw >= DRAW_MIN_MS)) {
        dirty = false;
        lastDraw = now;
        uint32_t t0 = millis();
        drawOnlyUs = 0;
        draw(now);
        if (benchLeft) {
          benchDrawUs += drawOnlyUs;
          benchFillUs += fillOnlyUs;
          if (!--benchLeft) {
            uint32_t us = micros() - benchStart;
            Serial.printf("[BENCH] Seite %u: %u Bilder in %lu ms = %.1f Bilder/s, Zeichnen allein %.1f ms je Bild, davon Loeschen %.1f ms\n", screen, (unsigned)benchTotal,
                          (unsigned long)(us / 1000), benchTotal * 1e6 / us, benchDrawUs / 1000.0 / benchTotal, benchFillUs / 1000.0 / benchTotal);
          }
        }
        uint32_t took = millis() - t0;
        if (took > statDrawMsMax) statDrawMsMax = took;
        if (took > recentDrawMs) recentDrawMs = took;
      }
    }
    if (reqShot) { reqShot = false; screenshot(); }
    // a dark screen only waits for a touch or a movement: on battery it looks less often
    vTaskDelay(pdMS_TO_TICKS(benchLeft ? 1 : on ? (scrollAnim != A_NONE || touchDown || shAnim || rulerAnim || pageAnim || holdP > 0 ? 8 : 20) : S.saver ? 120 : 50));
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
  setRotation(settings.displayFlip ? LCD_ROT_DEFAULT ^ 2 : LCD_ROT_DEFAULT);
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
  Bridge::freezeTx();                    // and the serial lines stay at rest: no BREAK to what hangs on them
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
