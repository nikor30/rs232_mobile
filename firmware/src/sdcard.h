#pragma once
#include <Arduino.h>
#include "config.h"

// SD card (SPI) on boards that have a slot. Everything here is a no-op when no
// card is present, so the console keeps working without one.
//
//   /logs/<port>-<nnn>.log   session recordings, one file per port and boot
//   /configs/*.txt           configurations that can be played back to a device
//   /xfer/*                  images for XMODEM/YMODEM transfers
namespace Sd {
  bool begin();                       // false = no card (or no slot on this board)
  bool mounted();
  uint64_t totalMb();
  uint64_t usedMb();
  const char *typeName();

  // ---- session log ----
  bool logStart(uint8_t port);        // opens the next free file for that port
  void logStop(uint8_t port);
  bool logging(uint8_t port);
  String logName(uint8_t port);       // "" when not logging
  uint32_t logBytes(uint8_t port);
  void write(uint8_t port, const uint8_t *data, size_t len);   // ignored unless logging
  void loop();                        // flushes at most every FLUSH_MS
}
