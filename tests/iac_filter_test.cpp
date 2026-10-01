#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- verbatim copy of the class from src/net.cpp ----
class TelnetIacFilter {
 public:
  size_t apply(uint8_t *buf, size_t n) {
    static const uint8_t IAC = 255, SB = 250, SE = 240, WILL = 251, WONT = 252, DO = 253, DONT = 254;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
      uint8_t b = buf[i];
      switch (state_) {
        case 0:
          if (b == IAC) state_ = 1; else buf[out++] = b;
          break;
        case 1:
          if (b == IAC) { buf[out++] = 0xFF; state_ = 0; }
          else if (b == SB) state_ = 2;
          else if (b == WILL || b == WONT || b == DO || b == DONT) state_ = 3;
          else state_ = 0;
          break;
        case 2:
          if (b == IAC) state_ = 4;
          break;
        case 4:
          state_ = (b == SE) ? 0 : 2;
          break;
        default:
          state_ = 0;
          break;
      }
    }
    return out;
  }
  void reset() { state_ = 0; }
 private:
  uint8_t state_ = 0;
};

static int fails = 0;
// feed the input in chunks of `chunk` bytes to also exercise sequences split across TCP reads
static std::string run(std::vector<uint8_t> in, size_t chunk) {
  TelnetIacFilter f;
  std::string out;
  for (size_t i = 0; i < in.size(); i += chunk) {
    size_t n = std::min(chunk, in.size() - i);
    uint8_t buf[512];
    memcpy(buf, in.data() + i, n);
    size_t m = f.apply(buf, n);
    out.append((char *)buf, m);
  }
  return out;
}
static void check(const char *name, std::vector<uint8_t> in, std::string want) {
  for (size_t chunk = 1; chunk <= in.size(); chunk++) {
    std::string got = run(in, chunk);
    if (got != want) {
      printf("FAIL %-38s chunk=%zu  erwartet %zu B, bekommen %zu B\n", name, chunk, want.size(), got.size());
      fails++;
      return;
    }
  }
  printf("ok   %-38s (alle Chunk-Größen)\n", name);
}

int main() {
  const uint8_t IAC=255, SB=250, SE=240, WILL=251, DO=253, DONT=254, WONT=252, NOP=241, ECHOo=1, SGA=3, TTYPE=24, NAWS=31;

  check("reine Nutzdaten", {'s','h','o','w',' ','v','e','r','\r'}, "show ver\r");

  // was PuTTY "Telnet" / `telnet` beim Verbindungsaufbau schickt
  check("PuTTY-Telnet-Begrüßung + Befehl",
        {IAC,WILL,NAWS, IAC,WILL,TTYPE, IAC,DO,SGA, IAC,WILL,SGA, 'e','n','a','b','l','e','\r'},
        "enable\r");

  check("IAC IAC = echtes 0xFF", {'a', IAC,IAC, 'b'}, std::string("a\xff""b", 3));

  check("Subnegotiation (NAWS 80x24)",
        {IAC,SB,NAWS,0,80,0,24,IAC,SE, 'x'}, "x");

  check("Subnegotiation mit 0xFF-Daten drin",
        {IAC,SB,TTYPE,0,IAC,IAC,'A',IAC,SE, 'y'}, "y");

  check("1-Byte-Kommando (NOP)", {'a', IAC,NOP, 'b'}, "ab");

  check("DONT/WONT dazwischen", {'1', IAC,DONT,ECHOo, '2', IAC,WONT,SGA, '3'}, "123");

  // XMODEM-Blockdaten mit 0xFF müssen unverändert durch (nach IAC-Verdopplung durch den Client)
  std::vector<uint8_t> xm = {0x01,0x01,0xFE};
  std::string want(1, (char)0x01); want += (char)0x01; want += (char)0xFE;
  for (int i = 0; i < 4; i++) { xm.push_back(IAC); xm.push_back(IAC); want += (char)0xFF; }
  check("XMODEM-Block mit 0xFF (IAC-escaped)", xm, want);

  printf(fails ? "\n%d Test(s) fehlgeschlagen\n" : "\nalle Tests bestanden\n", fails);
  return fails != 0;
}
