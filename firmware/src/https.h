#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// TLS front end on port 443: terminates HTTPS/WSS and passes the plain traffic
// to the existing web server (port 80) and WebSocket server (port 81) through
// the loopback interface. Runs in its own task, one certificate for all sessions.
namespace Https {
  void begin();                     // starts the listener when enabled in the settings
  bool enabled();                   // switched on in the settings
  bool running();                   // listening (certificate available)
  uint8_t sessions();               // open TLS connections
  uint16_t port();
  String certName();                // certificate in use ("CN=... (Geräte-CA)")
  String lastError();
  // true when the connection came from the TLS front end (loopback)
  bool fromProxy(const IPAddress &ip);
}
