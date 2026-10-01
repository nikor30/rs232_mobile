#pragma once
#include <Arduino.h>

// 128x64 I2C OLED (SSD1306 0.96" or SH1106 1.3"). Boards with an SPI colour LCD
// implement the same interface in lcd_ui.cpp (their own screens, touch operated).
// Pages: status -> ports (only with 2+ ports) -> WiFi QR (join AP) -> URL QR -> device info
namespace Display {
  enum Page : uint8_t { PAGE_STATUS = 0, PAGE_PORTS, PAGE_WIFI_QR, PAGE_URL_QR, PAGE_INFO, PAGE_COUNT };

  bool begin();                  // false if no OLED found (everything else keeps working)
  void loop();
  bool isOn();
  void wake();                   // switch on + restart timeout
  void off();
  void nextPage();
  uint8_t page();
  void applyBrightness();        // push settings.oledBrightness to the panel (contrast, 0..255)
  void message(const char *line1, const char *line2, uint32_t ms);   // overlay box
  // Boards whose SD card sits on the display's SPI lines: whoever talks on that
  // bus holds this lock (recursive). Defined in lcd_ui.cpp only.
  void busLock();
  void busUnlock();
}
