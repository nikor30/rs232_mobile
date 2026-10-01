#pragma once
#include <stdint.h>
#include <stddef.h>

// CBC decryption for DES, 3DES (EDE, 2 or 3 keys) and RC2 - only what is needed
// to read legacy PKCS#12 files and DES-encrypted PEM keys.
namespace Legacy {
  enum Cipher { DES_CBC, DES_EDE3_CBC, RC2_CBC };
  // len must be a multiple of 8; out may equal in. rc2Bits = effective key bits (RC2 only).
  bool cbcDecrypt(Cipher c, const uint8_t *key, size_t keyLen, const uint8_t iv[8], const uint8_t *in, size_t len,
                  uint8_t *out, unsigned rc2Bits = 0);
}
