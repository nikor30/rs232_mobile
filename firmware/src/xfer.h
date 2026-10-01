#pragma once
#include <Arduino.h>

// File upload to a serial device with XMODEM (128 byte blocks, checksum or CRC),
// XMODEM-1K and YMODEM (batch header with name + size, 1K blocks).
//
// The browser streams the file over the WebSocket into a buffer here (flow
// control via the "taken" counter in the progress messages); the protocol
// itself (blocks, ACK/NAK, retries, timeouts) runs on the ESP32, so WiFi
// latency never delays an ACK/next-block turnaround on the serial line.
//
// Typical receivers: Cisco ROMmon "xmodem -c <file>", IOS "copy xmodem: flash:",
// U-Boot "loadx" / "loady", lrzsz "rx" / "rb".
namespace Xfer {
  enum Proto : uint8_t { XMODEM = 0, XMODEM_1K = 1, YMODEM = 2 };
  typedef void (*Notify)(const String &json);

  void begin(Notify notify);
  void loop();

  // WebSocket side
  bool start(uint8_t port, Proto proto, const String &name, uint32_t size, uint8_t owner, String &err);
  bool resume(uint8_t client, uint32_t id);   // browser reconnected: continue feeding
  void abort(const String &why);                // user: cancel (sends CAN to the receiver)
  void clientGone(uint8_t client);
  size_t feed(uint8_t client, const uint8_t *data, size_t len);
  String statusJson();
  bool active();

  // serial side (called by Bridge)
  bool on(uint8_t port);            // transfer attached to this port (waiting or running)
  bool blocksInput(uint8_t port);   // handshake done: terminal input would corrupt the transfer
  size_t onRx(uint8_t port, const uint8_t *data, size_t len); // bytes consumed by the engine (rest -> terminal)
}
