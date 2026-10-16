// The device's half of the console's end-to-end encryption (firmware/src/
// c2_e2e.cpp) on the PC, driven by e2e_test.mjs, which plays the browser with
// WebCrypto - the same calls the admin page makes.
//
//   e2e_host <browser key, hex>    prints: DEV <hex> / CODE <six digits> / FRAME <hex of a sealed greeting>,
//                                  then reads frames (hex, one per line) from stdin and prints PLAIN <text> or BAD
#include "c2_e2e.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/random.h>

static int rng(void *, unsigned char *out, size_t n) { return getrandom(out, n, 0) == (ssize_t)n ? 0 : -1; }

static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
  return v;
}
static std::string hex(const uint8_t *b, size_t n) {
  std::string s;
  char t[3];
  for (size_t i = 0; i < n; i++) { snprintf(t, 3, "%02x", b[i]); s += t; }
  return s;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  auto bpub = unhex(argv[1]);
  if (bpub.size() != C2E2E::PUB) { puts("BADKEY"); return 1; }
  C2E2E::Session s;
  uint8_t dpub[C2E2E::PUB];
  if (!C2E2E::start(s, bpub.data(), dpub, rng, nullptr)) { puts("BADKEY"); return 1; }
  printf("DEV %s\nCODE %s\n", hex(dpub, sizeof(dpub)).c_str(), s.code);
  const char *greet = "Switch>show version\r\nCisco IOS Software";
  uint8_t frame[2 + C2E2E::MAX_PLAIN + C2E2E::TAG];
  for (int i = 0; i < 2; i++) {          // two frames: the counter has to move on both sides
    size_t n = C2E2E::seal(s, (const uint8_t *)greet, strlen(greet), frame);
    printf("FRAME %s\n", hex(frame, n).c_str());
  }
  fflush(stdout);
  std::string line;
  while (std::getline(std::cin, line)) {
    auto f = unhex(line);
    uint8_t plain[C2E2E::MAX_PLAIN];
    size_t pl = 0;
    bool bad = false;
    size_t used = C2E2E::open(s, f.data(), f.size(), plain, pl, bad);
    if (bad || !used) puts("BAD");
    else printf("PLAIN %s\n", std::string((char *)plain, pl).c_str());
    fflush(stdout);
  }
  return 0;
}
