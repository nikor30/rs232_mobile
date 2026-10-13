#pragma once
#include <Arduino.h>
#include "config.h"

// SD card (SPI). Everything here is a no-op when no
// card is present, so the console keeps working without one.
//
//   /logs/<port>-<nnn>.log   session recordings, one file per port and boot
//   /configs/*.txt           configurations that can be played back to a device
//   /xfer/*                  images for XMODEM/YMODEM transfers
namespace Sd {
  bool begin();                       // false = no card
  bool mounted();
  uint64_t totalMb();
  uint64_t usedMb();
  const char *typeName();
  String history();                   // diagnostic trail of mounts and failures (debug console)

  // ---- session log ----
  bool logStart(uint8_t port);        // opens the next free file for that port
  void logStop(uint8_t port);
  bool logging(uint8_t port);
  String logName(uint8_t port);       // "" when not logging
  uint32_t logBytes(uint8_t port);
  void write(uint8_t port, const uint8_t *data, size_t len);   // ignored unless logging
  void loop();                        // flushes at most every FLUSH_MS
  void end();                         // close the recordings and release the card (before power-off)

  // ---- a file from the control center ----
  // Written to the card's top folder, where a switch that gets the card as a
  // USB stick looks first. It arrives as "<name>.part" and gets its name only
  // when all of it is there.
  const char *putBegin(const char *name, uint32_t size);   // nullptr = go ahead, otherwise why not
  bool putWrite(const uint8_t *data, size_t len);
  bool putEnd(bool keep);             // keep = false throws the part away
  String rootJson(size_t room);       // the files of the top folder as [{"n":..,"s":..}], as many as fit

  // ---- USB memory stick ----
  // Hands the card to whatever the USB port is plugged into (a switch, a
  // firewall, a computer) as a mass-storage device. From then on the card
  // belongs to that host alone: mounted() is false, nothing is recorded, and the
  // USB port is no debug console any more. The way back is a restart.
  struct Stick {
    bool on;                          // the card is lent to the USB host
    bool host;                        // a host has taken the device
    bool ejected;                     // the host has let go of the card
    uint32_t readKb, writtenKb;       // what the host has moved so far
  };
  const char *stickStart();           // nullptr = started, otherwise why not
  Stick stick();
  void usbRestore();                  // first thing after a start: the USB port is the debug console again
}
