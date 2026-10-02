// ============================================================================
//  RS232 Web Console - LilyGO T-RSS3 (ESP32-S3)
//
//  Phone/laptop  --WiFi-->  web terminal (xterm.js) / raw TCP  --> RS232 port
//
//  Button (IO5 on board, optional panel button on IO6):
//    short press : wake display / next page (status, WiFi QR, URL QR, info)
//    long press  : on status page -> next baud rate (9600 ... 115200)
//    hold at power-on for 5 s -> factory reset (new WiFi password)
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"
#include "serial_bridge.h"
#include "net.h"
#include "display.h"
#include "power.h"
#include "status_led.h"
#include "xfer.h"
#include "configs.h"
#include "certs.h"
#include "gui.h"
#include "sdcard.h"
#include "ble.h"
#include "player.h"

// Certificate parsing (PKCS#12, key check) runs in the loop task and needs more
// than the 8 kB Arduino default.
SET_LOOP_TASK_STACK_SIZE(14 * 1024);

// ------------------------------------------------------------------ buttons
struct Button {
  int pin;
  bool raw, pressed, longFired;
  uint32_t changedAt, downAt;
  explicit Button(int p) : pin(p), raw(false), pressed(false), longFired(false), changedAt(0), downAt(0) {}
};
static Button buttons[] = {Button(PIN_KEY), Button(PIN_EXT_KEY)};

static const uint32_t BAUD_PRESETS[] = {9600, 19200, 38400, 57600, 115200};

static void onShortPress() {
  if (!Display::isOn()) {        // first press only wakes the display
    Display::wake();
    return;
  }
  Display::wake();
  Display::nextPage();
}

static void onLongPress() {
  bool wasOn = Display::isOn();
  Display::wake();
  if (!wasOn) return;
  if (Display::page() != Display::PAGE_STATUS) {
    while (Display::page() != Display::PAGE_STATUS) Display::nextPage();
    return;
  }
  const size_t n = sizeof(BAUD_PRESETS) / sizeof(BAUD_PRESETS[0]);
  size_t i = 0;
  while (i < n && BAUD_PRESETS[i] != settings.port[0].serial.baud) i++;
  SerialCfg c = settings.port[0].serial;
  c.baud = BAUD_PRESETS[i < n ? (i + 1) % n : 0];
  Bridge::apply(0, c);
  Net::markDirty();
}

static void pollButton(Button &b) {
  if (b.pin < 0) return;
  bool raw = digitalRead(b.pin) == LOW;
  uint32_t now = millis();
  if (raw != b.raw) {
    b.raw = raw;
    b.changedAt = now;
  }
  if (now - b.changedAt < 30) return;              // debounce
  if (raw && !b.pressed) {
    b.pressed = true;
    b.longFired = false;
    b.downAt = now;
  } else if (raw && b.pressed && !b.longFired && now - b.downAt > 1000) {
    b.longFired = true;
    onLongPress();
  } else if (!raw && b.pressed) {
    b.pressed = false;
    if (!b.longFired) onShortPress();
  }
}

// Hold a button (on-board IO5 or panel button IO6) while powering on -> factory reset after 5 s
static bool anyButtonDown() {
  for (auto &b : buttons)
    if (b.pin >= 0 && digitalRead(b.pin) == LOW) return true;
  return false;
}

static void checkFactoryReset() {
  if (!anyButtonDown()) return;
  uint32_t t0 = millis();
  while (anyButtonDown()) {
    uint32_t held = millis() - t0;
    if (held >= 5000) {
      Store::factoryReset();
      Certs::wipe();                   // private keys never survive a factory reset
      Display::message("Werksreset", "OK - Neustart", 2000);
      Display::loop();
      Led::set(60, 0, 0);
      delay(2000);
      ESP.restart();
    }
    char buf[22];
    snprintf(buf, sizeof(buf), "halten: %lu s", (unsigned long)(5 - held / 1000));
    Display::message("Werksreset?", buf, 500);
    Display::loop();
    Led::set((millis() / 200) % 2 ? 40 : 0, 0, 0);
    delay(50);
  }
  Display::message("Abgebrochen", "", 1000);
}

// Everything that wants to see serial data: web/TCP clients, the panel GUI and
// the SD recording. Keeping the fan-out here means the bridge stays unaware of
// which front ends exist on a given board.
static void onSerial(uint8_t port, const uint8_t *data, size_t len) {
  Net::onSerialData(port, data, len);
  Gui::onSerialData(port, data, len);
  Sd::write(port, data, len);
  Ble::onSerialData(port, data, len);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  setCpuFrequencyMhz(CPU_MHZ);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Never wait for a USB host that is not reading. Not 0: the core's write loop
  // counts this value down and wraps around at 0, which blocks for good once
  // the port is plugged into a computer but not opened - no hotspot, no display.
  Serial.setTxTimeoutMs(5);
#endif

  for (auto &b : buttons)
    if (b.pin >= 0) pinMode(b.pin, INPUT_PULLUP);

  Store::load();
  Led::begin();

  Net::prepareRadio();               // credentials (true RNG with radio on) + channel scan

#if HAS_PANEL
  bool oled = Gui::begin();          // 800x480 touch panel instead of the small OLED
#else
  bool oled = Display::begin();
  if (oled) {
    Display::message(FW_NAME, "v" FW_VERSION, 1500);
    Display::loop();
  }
#endif
  checkFactoryReset();
  Sd::begin();

  Power::begin();
  Configs::begin();
  Certs::begin();
  Bridge::begin(onSerial, Net::onMessage);
  Net::begin();
  Ble::begin();

  Serial.printf("\n%s v%s (%s)\n", FW_NAME, FW_VERSION, BOARD_NAME);
#if HAS_PANEL
  Serial.printf("Panel  : %s\n", oled ? "ok" : "Fehler");
  Serial.printf("SD     : %s\n", Sd::typeName());
#else
  Serial.printf("%s : %s\n", HAS_SPI_LCD ? "LCD   " : "OLED  ", oled ? "ok" : "nicht gefunden");
#if HAS_SDCARD
  Serial.printf("SD     : %s\n", Sd::typeName());
#endif
#endif
  Serial.printf("WLAN   : %s  Passwort: %s\n", settings.apSsid.c_str(), settings.apPass.c_str());
  Serial.printf("Web-UI : http://%s/  (http://%s.local/)\n", Net::apIp().c_str(), settings.hostname.c_str());
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    if (!Bridge::enabled(p)) continue;
    char tcp[16];
    if (settings.tcpEnabled) snprintf(tcp, sizeof(tcp), "TCP :%u", RAW_TCP_PORT + p);
    else strlcpy(tcp, "TCP aus", sizeof(tcp));      // Menü -> Setup -> "Raw-TCP / Telnet"
    Serial.printf("Port %u : %-12s %s  RX=GPIO%d TX=GPIO%d  %s, %s\n", p + 1, Store::portName(p).c_str(),
                  Store::serialLabel(settings.port[p].serial).c_str(), Bridge::rxPin(p), Bridge::txPin(p),
                  Bridge::isHardware(p) ? "Hardware-UART" : "Software-UART", tcp);
  }
  if (settings.tcpEnabled)
    Serial.printf("Raw-TCP: %s\n", settings.tcpLan ? "Hotspot + LAN (ohne Anmeldung!)"
                                                   : "nur aus dem Hotspot-Netz");
  Serial.printf("Akku   : %s\n", Power::measured() ? Power::typeName() : "keine Messung");
  if (settings.staSsid.length())
    Serial.printf("WLAN-Client: %s (%s)\n", settings.staSsid.c_str(),
                  settings.staAuth == 0 ? "WPA2/WPA3-PSK" : "802.1X");
  if (settings.httpsEnabled) Serial.printf("HTTPS  : https://%s/\n", settings.hostname.c_str());
  Serial.printf("RAM    : %u kB frei\n", (unsigned)(ESP.getFreeHeap() / 1024));
}

void loop() {
  Bridge::loop();
  Xfer::loop();
  Net::loop();
  Power::loop();
  for (auto &b : buttons) pollButton(b);
  Display::loop();
  Gui::loop();
  Sd::loop();
  Ble::loop();
  Player::loop();
  Led::loop(Net::webClients() + (Net::tcpConnected() ? 1 : 0), Power::low(), Bridge::autobaudRunning());

  // one-time low battery notice (with hysteresis)
  static bool lowNotified = false;
  if (Power::low() && !lowNotified) {
    lowNotified = true;
    Net::onMessage("Akku schwach: noch " + String(Power::pct()) + "%");
  } else if (Power::pct() > BAT_LOW_PCT + 5) {
    lowNotified = false;
  }

  if (!Bridge::busy() && !Xfer::active()) delay(1);   // software UART / transfer: no pause
  else yield();
}
