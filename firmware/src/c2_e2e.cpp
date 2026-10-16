#include "c2_e2e.h"

#include <string.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace C2E2E {

static const char INFO[] = "rs232-console-e2e-v1";

// HKDF-SHA256 (RFC 5869) from HMAC: the Arduino core's mbedTLS declares
// mbedtls_hkdf() but does not ship it.
static bool hkdf(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen,
                 const uint8_t *info, size_t infoLen, uint8_t *out, size_t outLen) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t prk[32], t[32];
  if (!md || outLen > 255 * 32 || mbedtls_md_hmac(md, salt, saltLen, ikm, ikmLen, prk)) return false;
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  bool ok = !mbedtls_md_setup(&c, md, 1);
  size_t done = 0, tLen = 0;
  for (uint8_t i = 1; ok && done < outLen; i++) {
    ok = !mbedtls_md_hmac_starts(&c, prk, sizeof(prk)) && !mbedtls_md_hmac_update(&c, t, tLen) &&
         !mbedtls_md_hmac_update(&c, info, infoLen) && !mbedtls_md_hmac_update(&c, &i, 1) && !mbedtls_md_hmac_finish(&c, t);
    tLen = sizeof(t);
    size_t n = outLen - done < tLen ? outLen - done : tLen;
    memcpy(out + done, t, n);
    done += n;
  }
  mbedtls_md_free(&c);
  memset(prk, 0, sizeof(prk));
  memset(t, 0, sizeof(t));
  return ok;
}

bool start(Session &s, const uint8_t browserPub[PUB], uint8_t devicePub[PUB],
           int (*rng)(void *, unsigned char *, size_t), void *rngCtx) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point qB, qD;
  mbedtls_mpi d, z;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&qB);
  mbedtls_ecp_point_init(&qD);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  uint8_t zb[32], salt[2 * PUB], okm[68];
  size_t olen = 0;
  bool ok = !mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) &&
            !mbedtls_ecp_point_read_binary(&grp, &qB, browserPub, PUB) &&
            !mbedtls_ecp_check_pubkey(&grp, &qB) &&
            !mbedtls_ecdh_gen_public(&grp, &d, &qD, rng, rngCtx) &&
            !mbedtls_ecp_point_write_binary(&grp, &qD, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, devicePub, PUB) && olen == PUB &&
            !mbedtls_ecdh_compute_shared(&grp, &z, &qB, &d, rng, rngCtx) &&
            !mbedtls_mpi_write_binary(&z, zb, sizeof(zb));
  if (ok) {
    memcpy(salt, browserPub, PUB);
    memcpy(salt + PUB, devicePub, PUB);
    ok = hkdf(salt, sizeof(salt), zb, sizeof(zb), (const uint8_t *)INFO, sizeof(INFO) - 1, okm, sizeof(okm));
  }
  if (ok) {
    memcpy(s.kIn, okm, 32);
    memcpy(s.kOut, okm + 32, 32);
    uint32_t c = ((uint32_t)okm[64] << 24) | ((uint32_t)okm[65] << 16) | ((uint32_t)okm[66] << 8) | okm[67];
    c %= 1000000;
    for (int i = 5; i >= 0; i--, c /= 10) s.code[i] = '0' + c % 10;
    s.code[6] = 0;
    s.ctrIn = s.ctrOut = 0;
  }
  memset(zb, 0, sizeof(zb));
  memset(okm, 0, sizeof(okm));
  mbedtls_mpi_free(&d);
  mbedtls_mpi_free(&z);
  mbedtls_ecp_point_free(&qB);
  mbedtls_ecp_point_free(&qD);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static void nonce(uint8_t iv[12], uint64_t ctr) {
  memset(iv, 0, 4);
  for (int i = 0; i < 8; i++) iv[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
}

size_t seal(Session &s, const uint8_t *plain, size_t n, uint8_t *out) {
  if (n > MAX_PLAIN) n = MAX_PLAIN;
  uint8_t iv[12];
  nonce(iv, s.ctrOut++);
  size_t l = n + TAG;
  out[0] = (uint8_t)(l >> 8);
  out[1] = (uint8_t)l;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = !mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, s.kOut, 256) &&
            !mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12, nullptr, 0, plain, out + 2, TAG, out + 2 + n);
  mbedtls_gcm_free(&g);
  return ok ? 2 + l : 0;
}

size_t open(Session &s, const uint8_t *buf, size_t len, uint8_t *plain, size_t &plainLen, bool &bad) {
  bad = false;
  plainLen = 0;
  if (len < 2) return 0;
  size_t l = ((size_t)buf[0] << 8) | buf[1];
  if (l < TAG || l > MAX_PLAIN + TAG) { bad = true; return 0; }
  if (len < 2 + l) return 0;
  uint8_t iv[12];
  nonce(iv, s.ctrIn);
  size_t n = l - TAG;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = !mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, s.kIn, 256) &&
            !mbedtls_gcm_auth_decrypt(&g, n, iv, 12, nullptr, 0, buf + 2 + n, TAG, buf + 2, plain);
  mbedtls_gcm_free(&g);
  if (!ok) { bad = true; return 0; }
  s.ctrIn++;
  plainLen = n;
  return 2 + l;
}

}  // namespace C2E2E
