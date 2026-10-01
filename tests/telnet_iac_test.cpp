#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "extracted.inc"

static int fails = 0;
static void ok(const char *name, bool cond, const char *info = "") {
  printf("%-4s %-46s %s\n", cond ? "ok" : "FAIL", name, info);
  if (!cond) fails++;
}
static std::string hex(const uint8_t *d, size_t n) {
  std::string s; char b[8];
  for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X ", d[i]); s += b; }
  return s;
}

int main() {
  const uint8_t IAC=255, WILL=251, DO=253, SB=250, SE=240, ECHO=1, SGA=3, TTYPE=24, NAWS=31;

  // --- 1. Raw-Client (PuTTY "Raw"): kein Byte darf angefasst werden ---
  {
    TelnetSession s; s.reset();
    uint8_t d[] = {'s','h',0xFF,'o','w',0x0D};        // 0xFF mitten in Nutzdaten
    size_t n = s.filter(d, sizeof d);
    ok("raw: alle Bytes unveraendert", n == sizeof d && d[2] == 0xFF, hex(d, n).c_str());
    uint8_t out[32];
    ok("raw: keine IAC an den Client", s.pending(out, sizeof out) == 0);
  }

  // --- 2. Telnet-Client: Aushandlung raus, Begruessung rein ---
  {
    TelnetSession s; s.reset();
    uint8_t d[] = {IAC,WILL,NAWS, IAC,DO,ECHO, 'e','n','a',0x0D};
    size_t n = s.filter(d, sizeof d);
    ok("telnet: nur Nutzdaten bleiben", n == 4 && !memcmp(d, "ena\r", 4), hex(d, n).c_str());
    uint8_t out[32];
    size_t m = s.pending(out, sizeof out);
    // erwartet: WILL ECHO, WILL SGA (Begruessung) + DONT NAWS (Absage auf WILL NAWS)
    bool hasEcho = false, hasSga = false, hasDontNaws = false;
    for (size_t i = 0; i + 2 < m; i += 3) {
      if (out[i+1] == WILL && out[i+2] == ECHO) hasEcho = true;
      if (out[i+1] == WILL && out[i+2] == SGA) hasSga = true;
      if (out[i+1] == 254 && out[i+2] == NAWS) hasDontNaws = true;
    }
    ok("telnet: WILL ECHO gesendet", hasEcho, hex(out, m).c_str());
    ok("telnet: WILL SGA gesendet", hasSga);
    ok("telnet: WILL NAWS wird abgelehnt", hasDontNaws);
    ok("telnet: DO ECHO bekommt kein WONT", m == 9);
    ok("telnet: Begruessung nur einmal", s.pending(out, sizeof out) == 0);
  }

  // --- 3. Telnet: Subnegotiation und IAC IAC ---
  {
    TelnetSession s; s.reset();
    uint8_t d[] = {IAC,SB,TTYPE,0,'V','T','1','0','0',IAC,SE, 'x', IAC,IAC, 'y'};
    size_t n = s.filter(d, sizeof d);
    ok("telnet: SB verschluckt, IAC IAC = 0xFF",
       n == 3 && d[0]=='x' && d[1]==0xFF && d[2]=='y', hex(d, n).c_str());
  }

  // --- 4. Sequenz ueber Paketgrenzen hinweg ---
  {
    TelnetSession s; s.reset();
    uint8_t p1[] = {IAC,WILL};  uint8_t p2[] = {NAWS,'a'};
    size_t n1 = s.filter(p1, 2), n2 = s.filter(p2, 2);
    ok("telnet: Sequenz ueber zwei TCP-Reads", n1 == 0 && n2 == 1 && p2[0] == 'a');
  }

  // --- 5. reset(): zweite Sitzung wird wieder begruesst ---
  {
    TelnetSession s; s.reset();
    uint8_t d1[] = {IAC,WILL,NAWS}; s.filter(d1, 3);
    uint8_t o[32]; s.pending(o, sizeof o);
    s.reset();
    uint8_t d2[] = {IAC,WILL,NAWS}; s.filter(d2, 3);
    size_t m = s.pending(o, sizeof o);
    bool hasEcho = false;
    for (size_t i = 0; i + 2 < m; i += 3) if (o[i+1] == WILL && o[i+2] == ECHO) hasEcho = true;
    ok("reset: neue Sitzung wird neu begruesst", hasEcho, hex(o, m).c_str());
  }

  // --- 6. Raw bleibt raw, auch wenn spaeter ein 0xFF kommt ---
  {
    TelnetSession s; s.reset();
    uint8_t d1[] = {'a'}; s.filter(d1, 1);
    uint8_t d2[] = {IAC,WILL,ECHO};
    size_t n = s.filter(d2, 3);
    ok("raw bleibt raw trotz spaeterem 0xFF", n == 3, hex(d2, n).c_str());
  }

  printf(fails ? "\n%d Test(s) fehlgeschlagen\n" : "\nalle Tests bestanden\n", fails);
  return fails != 0;
}
