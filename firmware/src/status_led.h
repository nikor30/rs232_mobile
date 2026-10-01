#pragma once
#include <Arduino.h>

// WS2812 status LED
//  blue   = ready, no client        green  = web/TCP client connected
//  white  = flash on serial traffic  yellow = auto-baud running
//  red blink = battery low           magenta flash = BREAK sent
namespace Led {
  void begin();
  void loop(uint8_t clients, bool batLow, bool autobaud);
  void flash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);
  void off();
  void set(uint8_t r, uint8_t g, uint8_t b);   // immediate, bypasses the state logic
}
