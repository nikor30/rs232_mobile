#include "captive_dns.h"
#include "config.h"
#include <WiFiUdp.h>

namespace CaptiveDns {

#ifndef CAPTIVE_DNS_PORT
#define CAPTIVE_DNS_PORT 53
#endif

static WiFiUDP udp;
static IPAddress apIp;
static bool running = false;
static uint32_t count = 0;
static uint8_t buf[512];

uint32_t queries() { return count; }

void begin(IPAddress ip) {
  apIp = ip;
  running = udp.begin(CAPTIVE_DNS_PORT);
}

static void handle(int len) {
  if (len < 12) return;
  uint16_t flags = (buf[2] << 8) | buf[3];
  uint16_t qd = (buf[4] << 8) | buf[5];
  bool isQuery = !(flags & 0x8000);
  uint8_t opcode = (flags >> 11) & 0x0F;
  if (!isQuery || opcode != 0 || qd != 1) return;

  // walk the question name (no compression expected in queries)
  int p = 12;
  char name[96];
  int n = 0;
  while (p < len && buf[p] != 0) {
    uint8_t l = buf[p];
    if (l & 0xC0) return;                 // compressed name in a query: ignore
    if (p + 1 + l >= len) return;
    for (int i = 0; i < l && n < (int)sizeof(name) - 2; i++) name[n++] = (char)buf[p + 1 + i];
    if (n < (int)sizeof(name) - 1) name[n++] = '.';
    p += l + 1;
  }
  if (p + 5 > len) return;
  if (n > 0) n--;                         // drop trailing dot
  name[n] = 0;
  int qEnd = p + 5;                       // zero byte + QTYPE + QCLASS
  uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
  bool answerA = (qtype == 1 || qtype == 255);   // A or ANY

  count++;
#if DEBUG_NET_LOG
  Serial.printf("[dns] %s %s -> %s\n", name, qtype == 1 ? "A" : (qtype == 28 ? "AAAA" : "?"),
                answerA ? apIp.toString().c_str() : "(leer)");
#endif

  // response = header + question (copied) + optional answer
  uint8_t out[sizeof(buf) + 16];
  if (qEnd + 16 > (int)sizeof(out)) return;
  memcpy(out, buf, qEnd);
  out[2] = 0x84 | (buf[2] & 0x01);        // QR=1, AA=1, keep RD
  out[3] = 0x80;                          // RA=1, RCODE=0
  out[6] = 0; out[7] = answerA ? 1 : 0;   // ANCOUNT
  out[8] = 0; out[9] = 0;                 // NSCOUNT
  out[10] = 0; out[11] = 0;               // ARCOUNT (drop EDNS)
  int o = qEnd;
  if (answerA) {
    const uint8_t ans[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04};
    memcpy(out + o, ans, sizeof(ans));
    o += sizeof(ans);
    for (int i = 0; i < 4; i++) out[o++] = apIp[i];
  }
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(out, o);
  udp.endPacket();
}

void loop() {
  if (!running) return;
  for (int i = 0; i < 4; i++) {           // a few packets per loop pass
    int len = udp.parsePacket();
    if (len <= 0) return;
    if (len > (int)sizeof(buf)) {         // oversized query: drop it unread
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      udp.clear();                        // core 2 called this flush()
#else
      udp.flush();
#endif
      continue;
    }
    len = udp.read(buf, len);
    handle(len);
  }
}

}  // namespace CaptiveDns
