#pragma once
#include <Arduino.h>

// WiFi (AP + optional STA), HTTP UI, WebSocket terminal, raw TCP, OTA.
namespace Net {
  void prepareRadio();          // call once before begin(): credentials + channel choice
  void begin();
  void loop();

  void onSerialData(uint8_t port, const uint8_t *data, size_t len);   // UART -> clients
  void onMessage(const String &msg);                    // info text -> clients + display
  void markDirty();                                     // push status soon

  uint8_t webClients();
  bool tcpConnected();             // any port
  bool tcpConnected(uint8_t port);
  uint8_t apStations();
  bool staConnected();
  String apIp();
  String staIp();
  String eapDiag();                          // debug console: would the WLAN supplicant accept certificate and key?

  // What the web UI shows, as JSON - also handed to the command-and-control server.
  String deviceStatus();        // ports, counters, network, battery
  String deviceSettings();      // settings without any password or key
  const char *applySettings(const String &json, bool wire);   // check and store; nullptr or the error (wire: in English)
}
