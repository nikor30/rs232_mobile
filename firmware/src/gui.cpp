#include "gui.h"

#if HAS_PANEL

#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"

#include "serial_bridge.h"
#include "settings.h"
#include "net.h"
#include "sdcard.h"
#include "power.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace Gui {

// same palette as the web terminal, so both front ends look like one device
static const uint32_t C_BG = 0x0b0f14, C_BAR = 0x111a24, C_FG = 0xd6dde6,
                      C_ACCENT = 0x7ee787, C_DIM = 0x8b98a5, C_WARN = 0xe0a030,
                      C_LINE = 0x22303d;

// ---------------------------------------------------------------- text screen
// A console needs fixed cells, not flowing text: 8x16 pixels per character
// gives 96 columns on 800 px and 17 rows in the area between bar and input.
static const int T_COLS = 96;
static const int T_ROWS = 17;
static char cell[T_ROWS][T_COLS + 1];
static int curRow = 0, curCol = 0;
static uint8_t escState = 0;          // 0 = text, 1 = saw ESC, 2 = inside CSI
static bool textDirty = true;
static uint32_t lastDraw = 0;

static lv_obj_t *scrMain, *barLeft, *barMid, *barRight, *termLabel, *portTabs,
                *input, *keyboard, *keys, *toast, *scrMenu, *menuText, *logBtnLabel;
static bool menuOpen = false;
static uint32_t toastUntil = 0;
static bool dirty = true;

static void clearRow(int r) {
  memset(cell[r], ' ', T_COLS);
  cell[r][T_COLS] = 0;
}

static void termClear() {
  for (int r = 0; r < T_ROWS; r++) clearRow(r);
  curRow = curCol = 0;
  textDirty = true;
}

static void scrollUp() {
  for (int r = 0; r < T_ROWS - 1; r++) memcpy(cell[r], cell[r + 1], T_COLS + 1);
  clearRow(T_ROWS - 1);
}

static void newline() {
  if (curRow < T_ROWS - 1) curRow++;
  else scrollUp();
  curCol = 0;
}

// Minimal terminal behaviour: enough for router/switch consoles, which use CR,
// LF, backspace, tabs and colour escapes. Escape sequences are dropped rather
// than interpreted - colours would only cost readability on a 5" panel.
static void feed(uint8_t c) {
  switch (escState) {
    case 1:
      escState = (c == '[') ? 2 : 0;        // CSI ... or a short two-byte escape
      return;
    case 2:
      if (c >= 0x40 && c <= 0x7E) escState = 0;   // final byte ends the sequence
      return;
    default: break;
  }
  switch (c) {
    case 0x1B: escState = 1; return;
    case '\r': curCol = 0; return;
    case '\n': newline(); return;
    case '\b': if (curCol > 0) curCol--; return;
    case '\t': {
      int n = 8 - (curCol % 8);
      while (n-- > 0 && curCol < T_COLS) cell[curRow][curCol++] = ' ';
      return;
    }
    case 0x07: return;                       // bell
    default: break;
  }
  if (c < 0x20 || c > 0x7E) return;          // no UTF-8 on this font
  if (curCol >= T_COLS) newline();
  cell[curRow][curCol++] = (char)c;
}

static void renderTerm() {
  static String s;
  s = "";
  s.reserve((T_COLS + 1) * T_ROWS);
  for (int r = 0; r < T_ROWS; r++) {
    // trailing blanks cost nothing on screen but a lot in the string
    int end = T_COLS;
    while (end > 0 && cell[r][end - 1] == ' ') end--;
    s.concat(cell[r], end);
    if (r < T_ROWS - 1) s += '\n';
  }
  lv_label_set_text(termLabel, s.c_str());
}

// ---------------------------------------------------------------- status bar
static void renderBar() {
  uint8_t p = 0;                              // active port = the one the tabs show
  p = (uint8_t)lv_btnmatrix_get_selected_btn(portTabs);
  if (p >= MAX_PORTS || !Bridge::enabled(p)) p = 0;

  String left = settings.hostname;
  String ip = Net::staIp();
  left += ip.length() ? "  " + ip : "  " + Net::apIp();
  lv_label_set_text(barLeft, left.c_str());

  String mid = Store::portName(p) + "  " + Store::serialLabel(settings.port[p].serial);
  if (Bridge::autobaudRunning(p)) mid = Store::portName(p) + "  Auto-Baud ...";
  lv_label_set_text(barMid, mid.c_str());

  String right;
  uint8_t web = Net::webClients();
  if (web) right += String(web) + (web == 1 ? " Browser" : " Browser");
  if (Net::tcpConnected(p)) right += right.length() ? " + TCP" : "TCP";
  if (!right.length()) right = "keine Sitzung";
  if (Sd::mounted()) right += Sd::logging(p) ? "  REC" : "  SD";
  lv_label_set_text(barRight, right.c_str());

  lv_label_set_text(logBtnLabel, Sd::logging(p) ? "Stop" : "Aufz.");
}

static uint8_t activePort() {
  uint32_t i = lv_btnmatrix_get_selected_btn(portTabs);
  return (i < MAX_PORTS && Bridge::enabled((uint8_t)i)) ? (uint8_t)i : 0;
}

// ---------------------------------------------------------------- callbacks
static void sendLine(const char *txt) {
  uint8_t p = activePort();
  size_t n = strlen(txt);
  if (n) Bridge::write(p, (const uint8_t *)txt, n);
  const uint8_t cr = '\r';
  Bridge::write(p, &cr, 1);
}

static void inputReady(lv_event_t *e) {
  lv_obj_t *ta = lv_event_get_target(e);
  sendLine(lv_textarea_get_text(ta));
  lv_textarea_set_text(ta, "");
}

static void showKeyboard(bool on) {
  if (on) lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void keysEvent(lv_event_t *e) {
  uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
  uint8_t p = activePort();
  const uint8_t esc = 0x1B, tab = '\t', ctrlC = 0x03;
  switch (id) {
    case 0: showKeyboard(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)); break;
    case 1: Bridge::write(p, &tab, 1); break;
    case 2: Bridge::write(p, &ctrlC, 1); break;          // Ctrl+C: stop a running command
    case 3: Bridge::write(p, &esc, 1); break;
    case 4: Bridge::sendBreak(p, 500); break;            // ROMmon / password recovery
    case 5: Bridge::startAutobaud(p); break;
    case 6:                                              // recording on the SD card
      if (Sd::logging(p)) { Sd::logStop(p); message("Mitschnitt beendet"); }
      else if (Sd::logStart(p)) message("Mitschnitt: " + Sd::logName(p));
      else message("SD-Karte fehlt");
      break;
    case 7:
      menuOpen = true;
      lv_obj_clear_flag(scrMenu, LV_OBJ_FLAG_HIDDEN);
      break;
    default: break;
  }
  dirty = true;
}

static void tabsEvent(lv_event_t *) {
  termClear();
  dirty = true;
}

static void menuClose(lv_event_t *) {
  menuOpen = false;
  lv_obj_add_flag(scrMenu, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- build ui
static lv_obj_t *mkLabel(lv_obj_t *par, const char *txt, uint32_t col, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(par);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_font(l, font, 0);
  return l;
}

static void buildMenu() {
  scrMenu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(scrMenu, 700, 420);
  lv_obj_center(scrMenu);
  lv_obj_set_style_bg_color(scrMenu, lv_color_hex(C_BAR), 0);
  lv_obj_set_style_border_color(scrMenu, lv_color_hex(C_LINE), 0);
  lv_obj_add_flag(scrMenu, LV_OBJ_FLAG_HIDDEN);

  menuText = mkLabel(scrMenu, "", C_FG, &lv_font_montserrat_16);
  lv_obj_align(menuText, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_long_mode(menuText, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(menuText, 380);

#if LV_USE_QRCODE
  // same idea as the OLED page: scan it to join the hotspot
  lv_obj_t *qr = lv_qrcode_create(scrMenu, 200, lv_color_hex(0x000000), lv_color_hex(0xffffff));
  String wifi = "WIFI:T:WPA;S:" + settings.apSsid + ";P:" + settings.apPass + ";;";
  lv_qrcode_update(qr, wifi.c_str(), wifi.length());
  lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -10, 10);
#endif

  lv_obj_t *btn = lv_btn_create(scrMenu);
  lv_obj_set_size(btn, 160, 56);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(btn, menuClose, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = mkLabel(btn, "Schliessen", 0xffffff, &lv_font_montserrat_16);
  lv_obj_center(bl);
}

static void refreshMenu() {
  String t = String(FW_NAME) + " v" + FW_VERSION + "\n" + BOARD_NAME + "\n\n";
  t += "Hotspot: " + settings.apSsid + "\nPasswort: " + settings.apPass + "\n";
  t += "Adresse: http://" + Net::apIp() + "/\n";
  String ip = Net::staIp();
  if (ip.length()) t += "LAN: " + ip + "\n";
  t += "\nSD-Karte: ";
  t += Sd::mounted() ? String(Sd::typeName()) + ", " + String((unsigned)Sd::usedMb()) + " / " +
                       String((unsigned)Sd::totalMb()) + " MB"
                     : String(Sd::typeName());
  t += "\nFreier Speicher: " + String((unsigned)(ESP.getFreeHeap() / 1024)) + " kB";
  t += "\nLaufzeit: " + String((unsigned)(millis() / 60000)) + " min";
  lv_label_set_text(menuText, t.c_str());
}

static const char *TAB_MAP[MAX_PORTS + 1];
static String tabNames[MAX_PORTS];

static void buildUi() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);

  // ---- status bar ----
  lv_obj_t *bar = lv_obj_create(scr);
  lv_obj_set_size(bar, 800, 44);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  barLeft = mkLabel(bar, "", C_ACCENT, &lv_font_montserrat_16);
  lv_obj_align(barLeft, LV_ALIGN_LEFT_MID, 0, 0);
  barMid = mkLabel(bar, "", C_FG, &lv_font_montserrat_16);
  lv_obj_align(barMid, LV_ALIGN_CENTER, 0, 0);
  barRight = mkLabel(bar, "", C_DIM, &lv_font_montserrat_16);
  lv_obj_align(barRight, LV_ALIGN_RIGHT_MID, 0, 0);

  // ---- port tabs ----
  uint8_t n = 0;
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    tabNames[n] = String(p + 1) + " " + Store::portName(p);
    TAB_MAP[n] = tabNames[n].c_str();
    n++;
  }
  if (!n) { tabNames[0] = "Port 1"; TAB_MAP[0] = tabNames[0].c_str(); n = 1; }
  TAB_MAP[n] = "";
  portTabs = lv_btnmatrix_create(scr);
  lv_btnmatrix_set_map(portTabs, TAB_MAP);
  lv_btnmatrix_set_btn_ctrl_all(portTabs, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(portTabs, true);
  lv_btnmatrix_set_btn_ctrl(portTabs, 0, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_set_size(portTabs, 800, 40);
  lv_obj_align(portTabs, LV_ALIGN_TOP_LEFT, 0, 44);
  lv_obj_set_style_bg_color(portTabs, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(portTabs, 0, 0);
  lv_obj_set_style_pad_all(portTabs, 2, 0);
  lv_obj_add_event_cb(portTabs, tabsEvent, LV_EVENT_VALUE_CHANGED, nullptr);

  // ---- terminal ----
  lv_obj_t *term = lv_obj_create(scr);
  lv_obj_set_size(term, 800, 284);
  lv_obj_align(term, LV_ALIGN_TOP_LEFT, 0, 84);
  lv_obj_set_style_bg_color(term, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(term, 0, 0);
  lv_obj_set_style_radius(term, 0, 0);
  lv_obj_set_style_pad_all(term, 4, 0);
  lv_obj_clear_flag(term, LV_OBJ_FLAG_SCROLLABLE);
  termLabel = mkLabel(term, "", C_FG, &lv_font_unscii_16);
  lv_obj_align(termLabel, LV_ALIGN_TOP_LEFT, 0, 0);

  // ---- input line ----
  input = lv_textarea_create(scr);
  lv_textarea_set_one_line(input, true);
  lv_textarea_set_placeholder_text(input, "Befehl eingeben ...");
  lv_obj_set_size(input, 800, 48);
  lv_obj_align(input, LV_ALIGN_TOP_LEFT, 0, 368);
  lv_obj_set_style_bg_color(input, lv_color_hex(C_BAR), 0);
  lv_obj_set_style_text_color(input, lv_color_hex(C_FG), 0);
  lv_obj_set_style_border_color(input, lv_color_hex(C_LINE), 0);
  lv_obj_add_event_cb(input, inputReady, LV_EVENT_READY, nullptr);

  // ---- button row ----
  static const char *KEY_MAP[] = {"Tastatur", "Tab", "Ctrl+C", "Esc",
                                  "Break", "Auto-Baud", "Aufz.", "Menue", ""};
  keys = lv_btnmatrix_create(scr);
  lv_btnmatrix_set_map(keys, KEY_MAP);
  lv_obj_set_size(keys, 800, 64);
  lv_obj_align(keys, LV_ALIGN_TOP_LEFT, 0, 416);
  lv_obj_set_style_bg_color(keys, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(keys, 0, 0);
  lv_obj_set_style_pad_all(keys, 2, 0);
  lv_obj_add_event_cb(keys, keysEvent, LV_EVENT_VALUE_CHANGED, nullptr);
  // the recording button changes its caption, so keep a handle on it
  logBtnLabel = mkLabel(scr, "Aufz.", C_BG, &lv_font_montserrat_14);
  lv_obj_add_flag(logBtnLabel, LV_OBJ_FLAG_HIDDEN);

  // ---- on-screen keyboard (hidden until asked for) ----
  keyboard = lv_keyboard_create(scr);
  lv_obj_set_size(keyboard, 800, 240);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(keyboard, input);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

  // ---- toast ----
  toast = lv_label_create(scr);
  lv_label_set_text(toast, "");
  lv_obj_set_style_bg_color(toast, lv_color_hex(C_WARN), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(toast, lv_color_hex(0x101010), 0);
  lv_obj_set_style_pad_all(toast, 8, 0);
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -80);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

  buildMenu();
}

// ---------------------------------------------------------------- public
bool begin() {
  termClear();

  Board *board = new Board();
  if (!board->init() || !board->begin()) {
    Serial.println("[GUI]  Panel konnte nicht initialisiert werden");
    return false;
  }
  lvgl_port_init(board->getLCD(), board->getTouch());

  lvgl_port_lock(-1);
  buildUi();
  renderTerm();
  renderBar();
  lvgl_port_unlock();

  Serial.printf("[GUI]  %dx%d, %d Spalten x %d Zeilen Konsole\n",
                800, 480, T_COLS, T_ROWS);
  return true;
}

void onSerialData(uint8_t port, const uint8_t *data, size_t len) {
  if (port != activePort()) return;         // other ports keep running, just unseen
  for (size_t i = 0; i < len; i++) feed(data[i]);
  textDirty = true;
}

void message(const String &text) {
  if (!toast) return;
  lvgl_port_lock(-1);
  lv_label_set_text(toast, text.c_str());
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lvgl_port_unlock();
  toastUntil = millis() + 3000;
}

void markDirty() { dirty = true; }

void loop() {
  if (!termLabel) return;
  uint32_t now = millis();

  // redrawing costs a full string rebuild, so cap it at ~12 fps
  if ((textDirty && now - lastDraw > 80) || dirty) {
    lvgl_port_lock(-1);
    if (textDirty) { renderTerm(); textDirty = false; lastDraw = now; }
    if (dirty) { renderBar(); if (menuOpen) refreshMenu(); dirty = false; }
    lvgl_port_unlock();
  }
  if (toastUntil && (int32_t)(now - toastUntil) > 0) {
    toastUntil = 0;
    lvgl_port_lock(-1);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
  }
  static uint32_t lastStatus = 0;
  if (now - lastStatus > 2000) { lastStatus = now; dirty = true; }
}

}  // namespace Gui

#else   // ------------------------------------------------------- no panel

namespace Gui {
bool begin() { return false; }
void loop() {}
void onSerialData(uint8_t, const uint8_t *, size_t) {}
void message(const String &) {}
void markDirty() {}
}  // namespace Gui

#endif
