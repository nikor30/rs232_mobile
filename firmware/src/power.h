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

  // Calibration of the percentage: the voltage this board measures on an empty
  // and on a full battery (0 = not calibrated, the curve's own 0 % / 100 %).
  // Takes effect at once and is stored; returns nullptr or an error text.
  uint16_t calEmptyMv();
  uint16_t calFullMv();
  const char *setCal(uint16_t emptyMv, uint16_t fullMv);
  const char *calibrateNow(bool full);   // the voltage measured right now is 100 % (true) or 0 % (false)

  // Charging, on boards with a charger but no status line to the processor
  // (HAS_CHARGER). Derived from what can be observed: a USB host on the port and
  // the battery voltage. See power.cpp for what that can and cannot tell.
  enum Charge : uint8_t { ON_BATTERY = 0, CHARGING, FULL };
  Charge charge();
  bool usbHost();      // a computer on the USB port is sending frames right now
  const char *chargeName();   // "", "laedt", "voll"
  String diag();       // one line with the raw values behind all of the above

  // Running from the battery (steady for a few seconds): the callers save power
  // then - slower CPU clock, dimmed backlight, slower polling.
  bool saver();
  void forceSaver(int8_t mode);   // debug console: 0 = off, 1 = on, -1 = automatic
  bool empty();        // on battery and below BAT_OFF_MV for BAT_OFF_S: time to switch off
}
