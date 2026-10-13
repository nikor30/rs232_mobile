#pragma once
#include <stddef.h>
#include <stdint.h>

// Firmware update through the control center: the part that decides whether a
// package may be installed. The owner signs a manifest on their own computer
// (tools/fw_sign.py, ML-DSA-65); the manifest names the image by size and
// SHA-256. The server only passes both on - it cannot make this code say yes.
//
// Nothing here knows about Arduino: it needs mldsa-native and ArduinoJson and
// runs on a PC as well (tests/c2/).
namespace C2Update {

constexpr size_t KEY_SIZE = 1952;        // ML-DSA-65 public key
constexpr size_t SIG_SIZE = 3309;
constexpr size_t MAX_MANIFEST = 1024;

struct Manifest {
  char fw[25];
  char lang[9];
  uint32_t seq;                          // when it was signed (Unix time); never go back
  uint32_t size;
  uint8_t sha256[32];
};

// Checks the signature over the manifest text (as received, byte for byte) and
// only then reads it. Returns nullptr and fills out, or says in English why not.
// Needs about 20 kB of stack.
const char *check(const char *manifest, size_t manifestLen, const uint8_t *sig, size_t sigLen,
                  const uint8_t key[KEY_SIZE], const char *board, uint32_t installedSeq, uint32_t maxSize,
                  Manifest &out);

}  // namespace C2Update
