#pragma once
#include <Arduino.h>
#include "config.h"

// Serial console over Bluetooth LE (Nordic UART Service) for port 1. A phone app
// such as "Serial Bluetooth Terminal" then works without joining the hotspot.
// The link must be paired with the six-digit PIN shown on the display; without
// an encrypted, authenticated link no serial byte goes either way.
namespace Ble {
  enum State : uint8_t { OFF = 0, ADVERTISING, PAIRING, CONNECTED };

  void begin();
  void loop();
  void onSerialData(uint8_t port, const uint8_t *data, size_t len);   // UART -> phone
  void setEnabled(bool on);          // stored in the settings
  State state();
  uint32_t passkey();                // pairing PIN, new on every boot
}
