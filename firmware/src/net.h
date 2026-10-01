#pragma once
#include <Arduino.h>

// WiFi (AP + optional STA), HTTP UI, WebSocket terminal, raw TCP, OTA.
namespace Net {
  void prepareRadio();          // call once before begin(): credentials + channel choice
  void begin();
  void loop();

  void onSerialData(uint8_t port, const uint8_t *data, size_t len);   // UART -> clients
  void onMessage(const String &msg);                    // info text -> clients + OLED
  void markDirty();                                     // push status soon

  uint8_t webClients();
  bool tcpConnected();             // any port
  bool tcpConnected(uint8_t port);
  uint8_t apStations();
  bool staConnected();
  String apIp();
  String staIp();
}
