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
    char     want[96];      // not enrolled: the server this device asks to be let in at, by itself
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

  // The device reports in by itself as soon as its WLAN client is connected and
  // tries again when the server does not answer, with growing pauses. For
  // somebody standing in front of it there is a way to not wait for that.
  // The address of the server. For an enrolled device: where it is reached
  // now (it has moved, or has a name); its key stays the pinned one. For one
  // that is not enrolled: the server it asks to be let in at, without an
  // invitation, as soon as the WLAN is there and again until the server takes
  // it (which it does only while an administrator has opened registration).
  // Empty ends that asking. nullptr, or why not.
  const char *setServer(const String &url);
  bool down();                              // enrolled, but no contact with the server right now
  void reconnect();                         // try now (and ask the WLAN client to join again)

  // Remote console: the server asks, a person at the display decides - every
  // time. While it is open, what the administrator types goes to the port as
  // if typed in the web terminal, and the port's output goes to the server.
  struct Console {
    uint8_t  state;         // 0 none, 1 asked (waiting for the tap), 2 open
    uint8_t  port;          // 0-based
    uint16_t minutes;       // asked for
    uint32_t leftS;         // open: seconds until it closes by itself
    char     code[7];       // end to end: the six digits to compare with the browser, empty without
  };
  Console console();
  void consoleAnswer(bool allow);           // main loop: the tap on the display
  void consoleEnd();                        // main loop: close it from here

  // Unattended access: for this long a remote console opens without the tap.
  // Granted at the device only, counted in the device's own running time (it
  // has no clock a server could not move), kept across restarts.
  static const uint32_t UNATTENDED_MAX_S = 7UL * 24 * 3600;
  void unattendedSet(uint32_t seconds);     // 0 ends it
  uint32_t unattendedLeft();                // seconds, 0 = a tap is needed
}
