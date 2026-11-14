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
  bool unreadable();                  // a card is there but holds no file system the board can use (a host may format it)
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
  const char *putBegin(const char *name, uint32_t size, const char *tag, uint32_t &have);   // nullptr = go ahead (have: bytes that are there already), otherwise why not
  bool putWrite(const uint8_t *data, size_t len);
  void putPause();                    // stop for now; the part stays and is continued
  const char *putFinish(uint32_t size, const uint8_t md5[16], void (*progress)(int pct));   // read back, compare, name it; nullptr = stored
  const char *removeRoot(const char *name);              // a file of the top folder; nullptr = gone
  String rootJson(size_t room);       // the files of the top folder as [{"n":..,"s":..}], as many as fit
  // A small file of the top folder in one piece (the encrypted backup,
  // sdbackup.cpp): written as "<name>.tmp", read back and compared, then named.
  const char *writeRoot(const char *name, const uint8_t *data, size_t len);   // nullptr = stored
  const char *readRoot(const char *name, uint8_t *&data, size_t &len, size_t max);   // data: external RAM, free() it

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
    uint32_t sizeMb;                  // what the host is shown (at most 4 GB, see stickSize)
    uint32_t retries, fails;
    uint32_t cmds[8];                 // SCSI opcodes beyond what TinyUSB handles itself, one bit each          // requests the card refused at first / after every try
  };
  const char *stickStart(bool ram = false, uint32_t ramMb = 0);   // nullptr = started, otherwise why not; ram: a medium in RAM instead of the card, 4 MB backed, ramMb the size reported (diagnosis)
  void stickSetDelay(uint32_t first, uint32_t next);   // measuring: how long a request / the chunks that follow it take at least (sdcard.cpp: stickIo)
  bool stickReplug();                 // diagnosis: unplug and plug the stick again for the host
  bool stickRamLoad(uint32_t off, const uint8_t *data, size_t n);   // diagnosis: fill the RAM medium before stick mode
  Stick stick();
  String stickSectors();              // JSON: the last single-sector writes, in hex
  String stickTrace();                // the host's last requests, one per line (sdcard.cpp: StickReq)
  void usbStart(bool debug);          // early in setup(): the debug console (serial/JTAG unit), or a dead port until stick mode
  bool usbOwn();                      // the USB port is not the serial/JTAG unit: dead, or the stick
  bool usbHostSeen();                 // stick mode, and a host has taken the stick
}
