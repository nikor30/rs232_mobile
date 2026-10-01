#pragma once
#include <Arduino.h>
#include "config.h"

// Touch GUI for boards with an RGB panel (currently the VIEWE 5", 800x480).
// It is a second, equal front end next to the web terminal: same ports, same
// bridge, same settings. Everything is a no-op on boards without a panel.
//
//   status bar   host, address, serial settings, clients, SD card
//   port tabs    one per active port
//   terminal     17 rows x 96 columns, monospace
//   input line   on-screen keyboard, sent line by line like the web UI
//   buttons      special keys, BREAK, auto baud, recording, settings
namespace Gui {
  bool begin();                       // false = no panel on this board / init failed
  void loop();
  void onSerialData(uint8_t port, const uint8_t *data, size_t len);
  void message(const String &text);   // short overlay, same texts as the OLED gets
  void markDirty();                   // status changed, refresh the bar
}
