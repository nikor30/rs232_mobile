#pragma once
#include <Arduino.h>
#include "config.h"
#include "settings.h"

// UART <-> network bridge for up to MAX_PORTS serial ports, each with its own
// replay ring buffer. Ports 1..HW_PORTS use hardware UARTs, the others a
// software UART (interrupt driven, for console speeds).
namespace Bridge {
  typedef void (*DataSink)(uint8_t port, const uint8_t *data, size_t len);
  typedef void (*MsgSink)(const String &msg);

  void begin(DataSink onData, MsgSink onMsg);
  void loop();
  bool busy();                                // bytes waiting to be bit-banged (skip loop delay)

  bool enabled(uint8_t port);
  uint8_t enabledCount();
  bool isHardware(uint8_t port);
  int rxPin(uint8_t port);                    // actual GPIOs (after swap)
  int txPin(uint8_t port);

  void apply(uint8_t port, const SerialCfg &cfg);        // (re)open with new settings
  size_t write(uint8_t port, const uint8_t *data, size_t len);     // user input
  size_t writeRaw(uint8_t port, const uint8_t *data, size_t len);  // file transfer engine
  size_t txFree(uint8_t port);               // free space in the send queue
  bool txIdle(uint8_t port);                  // everything queued has left the TX pin
  bool swapOk(uint8_t port);                  // RX pin can also send (RX/TX swap possible)
  uint32_t maxBaud(uint8_t port);
  void sendBreak(uint8_t port, uint16_t ms = 300);
  void startAutobaud(uint8_t port);
  bool autobaudRunning(uint8_t port);
  bool autobaudRunning();                     // any port

  // Replay: bytes are numbered per port since boot ("seq").
  uint32_t seqNow(uint8_t port);
  uint32_t ringStartSeq(uint8_t port);
  size_t copyFrom(uint8_t port, uint32_t fromSeq, uint8_t *out, size_t max);

  String prefix(uint8_t port);                // "Name · " when several ports are active

  extern uint32_t rxBytes[MAX_PORTS], txBytes[MAX_PORTS];
  extern uint32_t lastRxMs[MAX_PORTS], lastTxMs[MAX_PORTS];
  extern uint32_t lastUserStartMs[MAX_PORTS];   // user typed 'C' or NAK (echo guard for XMODEM start)
}
