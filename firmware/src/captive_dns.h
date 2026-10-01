#pragma once
#include <Arduino.h>

// Minimal DNS server for the captive portal: answers every A query with the
// access point IP, AAAA/other types with an empty NOERROR answer (so clients
// fall back to IPv4). Tolerates EDNS (additional records) unlike DNSServer.h.
namespace CaptiveDns {
  void begin(IPAddress ip);
  void loop();
  uint32_t queries();
}
