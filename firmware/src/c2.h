#pragma once
#include <Arduino.h>

// Client of the command-and-control server (server/KONZEPT.md). The device
// calls the server over HTTP(S) whenever its WLAN client is connected; inside
// that runs the channel from c2_proto.h.
//
// Everything slow - TLS, ML-KEM, waiting for the server - happens in a task of
// its own, so the serial bridge never waits for it. The rest of the firmware
// talks to that task through the few functions below.
namespace C2 {
  enum State : uint8_t {
    OFF = 0,     // not enrolled
    ENROLLING,   // working on a token
    PENDING,     // enrolled, the administrator has to type in the code
    ACTIVE,
    REFUSED,     // the server does not accept this device (any more)
    FAILED,      // self-test failed: the cryptography is not usable on this build
  };

  struct Info {
    State    state;
    char     code[7];       // PENDING: the six digits to show
    char     id[33];        // device ID (hex), empty until a key exists
    char     url[96];       // server, empty when not enrolled
    char     error[64];     // last thing that went wrong, empty after a success
    uint32_t lastOkMs;      // millis() of the last answered poll, 0 = none yet
    uint32_t handshakeMs;   // duration of the last handshake, network included
    uint32_t cryptoMs;      // of that, time spent computing on the device
    uint32_t polls;
    uint32_t stackFree;     // lowest free stack of the ML-KEM computations so far, bytes
    uint32_t taskStackFree; // the same for the task that does the networking
  };

  void begin();
  void loop();                              // main loop: carries out what the server asked for
  Info info();
  const char *stateName(State s);
  String line();                            // one line for the display

  // From the main loop. Both return at once; the task does the work.
  const char *enroll(const String &token);  // nullptr, or why the token is not usable
  void forget();                            // drop server and device key

  // Remote console: the server asks, a person at the display decides - every
  // time. While it is open, what the administrator types goes to the port as
  // if typed in the web terminal, and the port's output goes to the server.
  struct Console {
    uint8_t  state;         // 0 none, 1 asked (waiting for the tap), 2 open
    uint8_t  port;          // 0-based
    uint16_t minutes;       // asked for
    uint32_t leftS;         // open: seconds until it closes by itself
  };
  Console console();
  void consoleAnswer(bool allow);           // main loop: the tap on the display
  void consoleEnd();                        // main loop: close it from here
}
