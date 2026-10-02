#pragma once
#include <Arduino.h>
#include "config.h"

// Plays a stored configuration (configs.h) to a serial port from the device
// itself - what the web UI's "Abspielen" does in the browser, for front ends
// that have no browser: the touch display.
//
// Same rules as the web UI: one line at a time, wait for the device's prompt (or
// two quiet seconds), answer "--More--" with a space, stop at an error message.
// Control lines: @pause <s>, @expect <text>, @break [ms], @timeout <s>.
// Not supported here: {{variables}} - nobody can type the values in.
namespace Player {
  enum State : uint8_t { IDLE = 0, RUNNING, DONE, FAILED };

  bool start(const String &name, uint8_t port);   // false: see result()
  void stop();
  void loop();

  State state();
  const char *name();
  uint8_t port();
  uint16_t line();                    // 1-based line being worked on
  uint16_t lines();
  const char *result();               // why it ended / what it is waiting for
}
