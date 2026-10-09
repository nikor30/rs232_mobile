#pragma once
#include <Arduino.h>
#include "config.h"

struct SerialCfg {
  uint32_t baud = 9600;
  uint8_t  bits = 8;       // 5..8
  char     parity = 'N';   // N, E, O
  uint8_t  stop = 1;       // 1, 2
  bool     swap = false;   // swap RX/TX GPIOs (TTL side, e.g. MAX3232 labels)
};

struct PortCfg {
  bool      enabled = false;
  String    name;           // shown in the web UI, e.g. "Core-Switch"
  int8_t    rx = -1;        // GPIO (TTL side, before a possible swap)
  int8_t    tx = -1;
  SerialCfg serial;
};

struct Settings {
  PortCfg  port[MAX_PORTS];
  String   apSsid;
  String   apPass;          // generated randomly on first boot
  String   staSsid;         // empty = station mode off
  String   staPass;         // WPA2/3-PSK key or 802.1X password (PEAP/TTLS)
  uint8_t  staAuth = 0;     // 0 = PSK, 1 = 802.1X EAP-TLS (certificate), 2 = PEAP, 3 = EAP-TTLS
  String   staIdentity;     // 802.1X outer identity (empty = CN of the client certificate)
  String   staUser;         // PEAP/TTLS user name
  bool     staCaCheck = true;   // check the RADIUS server against the stored CA certificate
  uint8_t  staPhase2 = 0;   // TTLS inner method: 0 = MSCHAPv2, 1 = PAP
  String   hostname;
  String   webPass;         // optional: protects web UI (user "admin")
  bool     displayFlip = false;  // turn the picture by 180 degrees
  uint16_t displayTimeout = 60;  // seconds, 0 = always on
  uint8_t  displayBrightness = 255; // backlight 0..255
  bool     tcpEnabled = true;
  bool     tcpLan = false;       // raw TCP/telnet also from the LAN (station mode) - unauthenticated!
  uint8_t  apChannel = 0;        // 0 = auto, 1..13
  int8_t   txPower = 44;         // wifi_power_t (quarter dBm): 34/44/60/78
  int8_t   batPin = -1;          // ADC GPIO for the battery divider, -1 = no measurement
  int8_t   batLbo = -1;          // GPIO on a charger's low-battery output (open drain, low = empty)
  uint8_t  batType = 0;          // 0 = LiPo 1S, 1 = NiCd/NiMH 4 cells
  uint8_t  batDiv = 20;          // divider ratio x10 (20 = 2:1, 30 = 3:1)
  uint16_t batEmptyMv = 0;       // calibration: measured voltage that is 0 % (0 = the curve's own, bat_curve.h)
  uint16_t batFullMv = 0;        // calibration: measured voltage that is 100 %
  bool     httpsEnabled = false; // web UI additionally over TLS on port 443
  bool     httpsLanOnly = false; // in the LAN (station mode) redirect plain HTTP to HTTPS
  uint8_t  httpsCert = 0;        // 0 = own HTTPS certificate (device CA if none), 1 = 802.1X client certificate
  bool     bleEnabled = true;    // Bluetooth LE console, switched on the touch display
};

extern Settings settings;

namespace Store {
  void load();
  void ensureCredentials();      // first boot: AP SSID from MAC + random password
  void save();
  void saveSerial(uint8_t port);
  void saveBatCal();
  void factoryReset();          // wipe NVS namespace (new AP password next boot)
  uint32_t serialConfigValue(const SerialCfg &c);   // -> SERIAL_8N1 etc.
  String serialLabel(const SerialCfg &c);           // "9600 8N1"
  bool validSerial(const SerialCfg &c);
  String portName(uint8_t port);                    // name or "Port N"

  // GPIO lists of this board (config.h)
  bool rxPinOk(int pin);
  bool txPinOk(int pin);
  bool adcPinOk(int pin);
  // Checks a complete port/battery setup; returns nullptr or an error text.
  const char *checkPins(const Settings &s);
}
