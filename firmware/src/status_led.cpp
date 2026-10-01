#include "status_led.h"
#include "config.h"
#include "settings.h"
#include "serial_bridge.h"

namespace Led {

static uint32_t lastUpdate = 0;
static uint32_t flashUntil = 0;
static uint8_t fr, fg, fb;
static uint8_t cr = 255, cg = 255, cb = 255;   // last written colour

void set(uint8_t r, uint8_t g, uint8_t b) {
  if (r == cr && g == cg && b == cb) return;
  cr = r; cg = g; cb = b;
#if LED_IS_WS2812
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_LED, r, g, b);          // core 2 called this neopixelWrite()
#else
  neopixelWrite(PIN_LED, r, g, b);
#endif
#else
  digitalWrite(PIN_LED, (r | g | b) ? HIGH : LOW);
#endif
}

__attribute__((unused)) static uint8_t scale(uint8_t v) {
  return (uint16_t)v * settings.ledBrightness / 255;
}

void begin() {
#if !LED_IS_WS2812
  pinMode(PIN_LED, OUTPUT);
#endif
  set(0, 0, 0);
}

void off() { set(0, 0, 0); }

void flash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms) {
  fr = r; fg = g; fb = b;
  flashUntil = millis() + ms;
}

void loop(uint8_t clients, bool batLow, bool autobaud) {
  uint32_t now = millis();
  if (now - lastUpdate < 20) return;
  lastUpdate = now;

  if (settings.ledBrightness == 0) { set(0, 0, 0); return; }
  bool traffic = false;
  for (uint8_t p = 0; p < MAX_PORTS; p++)
    traffic |= Bridge::enabled(p) && (now - Bridge::lastRxMs[p] < 40 || now - Bridge::lastTxMs[p] < 40);

#if LED_IS_WS2812
  uint8_t r, g, b;
  if ((int32_t)(flashUntil - now) > 0) {
    r = fr; g = fg; b = fb;
  } else if (autobaud) {
    bool on = (now / 150) % 2;
    r = on ? 255 : 0; g = on ? 160 : 0; b = 0;
  } else if (batLow && (now / 500) % 2) {
    r = 255; g = 0; b = 0;
  } else if (traffic) {
    r = 200; g = 200; b = 200;
  } else if (clients > 0) {
    r = 0; g = 255; b = 40;
  } else {
    r = 0; g = 60; b = 255;
  }
  set(scale(r), scale(g), scale(b));
#else
  // single LED:  short blip every 2 s = ready, steady = client connected,
  // flicker = traffic, fast blink = auto-baud, long flash = BREAK
  (void)batLow;
  bool on;
  if ((int32_t)(flashUntil - now) > 0) on = true;
  else if (autobaud) on = (now / 120) % 2;
  else if (clients > 0) on = !traffic;
  else on = (now % 2000) < 60 || traffic;
  set(on ? 255 : 0, 0, 0);
#endif
}

}  // namespace Led
