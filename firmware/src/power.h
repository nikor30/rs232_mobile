#pragma once
#include <Arduino.h>

namespace Power {
  void begin();
  void loop();
  bool measured();     // a battery divider pin is configured
  bool present();      // battery connected (divider reads a plausible voltage)
  uint16_t mv();       // filtered battery voltage
  uint8_t pct();       // 0..100
  bool low();          // empty: either by percentage or by the charger's LBO line
  bool hasLbo();       // a charger low-battery output is wired up
  bool lowSignal();    // that line is asserted right now
  const char *typeName();
}
