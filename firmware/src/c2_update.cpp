#include "c2_update.h"

#include <ArduinoJson.h>
#include <string.h>

extern "C" {
#include <mldsa_native.h>
}

namespace C2Update {

// Binds the signature to this purpose: the same key signing something else
// produces nothing that passes here. The same string is in tools/fw_sign.py.
static const char CONTEXT[] = "rs232-console firmware";

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

const char *check(const char *manifest, size_t manifestLen, const uint8_t *sig, size_t sigLen,
                  const uint8_t key[KEY_SIZE], const char *board, uint32_t installedSeq, uint32_t maxSize,
                  Manifest &out) {
  if (!manifestLen || manifestLen > MAX_MANIFEST || sigLen != SIG_SIZE) return "the package is not readable";
  // The signature first: nothing of the manifest is believed before it holds.
  if (PQCP_MLDSA_NATIVE_MLDSA65_verify(sig, (const uint8_t *)manifest, manifestLen, (const uint8_t *)CONTEXT,
                                       sizeof(CONTEXT) - 1, key) != 0)
    return "the signature does not match the key of this device";
  JsonDocument d;
  if (deserializeJson(d, manifest, manifestLen)) return "the package is not readable";
  if (strcmp(d["product"] | "", "rs232-console")) return "the package is not for this product";
  if (strcmp(d["board"] | "", board)) return "the package is for another board";
  const char *fw = d["fw"] | "", *lang = d["lang"] | "", *hash = d["sha256"] | "";
  uint32_t seq = d["seq"] | 0UL, size = d["size"] | 0UL;
  if (!fw[0] || strlen(fw) >= sizeof(out.fw) || strlen(lang) >= sizeof(out.lang) || strlen(hash) != 64 || !seq)
    return "the package is not readable";
  if (!size || size > maxSize) return "the image does not fit this device";
  if (seq <= installedSeq) return "the package is not newer than what this device has installed";
  for (int i = 0; i < 32; i++) {
    int hi = hexval(hash[2 * i]), lo = hexval(hash[2 * i + 1]);
    if (hi < 0 || lo < 0) return "the package is not readable";
    out.sha256[i] = (uint8_t)(hi << 4 | lo);
  }
  strcpy(out.fw, fw);
  strcpy(out.lang, lang);
  out.seq = seq;
  out.size = size;
  return nullptr;
}

}  // namespace C2Update
