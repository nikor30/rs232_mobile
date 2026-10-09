#pragma once
#include <Arduino.h>

// The 2" colour touch LCD; implemented in lcd_ui.cpp. The rest of the firmware
// only sees this interface.
namespace Display {
  enum Page : uint8_t { PAGE_STATUS = 0, PAGE_PORTS, PAGE_WIFI_QR, PAGE_URL_QR, PAGE_INFO, PAGE_COUNT };

  bool begin();                  // false if the panel did not start (everything else keeps working)
  void loop();
  bool isOn();
  void wake();                   // switch on + restart timeout
  void off();
  void nextPage();
  uint8_t page();
  void applyBrightness();        // push settings.displayBrightness to the backlight (0..255)
  void message(const char *line1, const char *line2, uint32_t ms);   // overlay box
  // The SD card sits on the display's SPI lines: whoever talks on that bus
  // holds this lock (recursive).
  void busLock();
  void busUnlock();
  // Switch the device off (deep sleep; the BOOT button switches it on again).
  // wakeAfterS > 0 also wakes it by timer - for testing without a hand on the
  // board.
  void powerOff(const char *reason, uint32_t wakeAfterS = 0);
}
