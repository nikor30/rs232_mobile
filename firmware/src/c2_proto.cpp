#include "c2_proto.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ArduinoJson.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

extern "C" {
#define MLK_CONFIG_API_PARAMETER_SET 768
#define MLK_CONFIG_API_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM768
#include <mlkem_native.h>
}

namespace C2Proto {

static const size_t X_SIZE = 32;
static const size_t EK_SIZE = MLKEM768_PUBLICKEYBYTES;    // 1184
static const size_t DK_SIZE = MLKEM768_SECRETKEYBYTES;    // 2400
static const size_t CT_SIZE = MLKEM768_CIPHERTEXTBYTES;   // 1088
static const size_t REQ_BASE = 2 + ID_SIZE + X_SIZE + EK_SIZE + CT_SIZE;
static const size_t RESP_SIGNED = 1 + ID_SIZE + X_SIZE + 2 * CT_SIZE;
static const uint8_t VERSION = 1;

static_assert(REQ_BASE == REQ_SESSION, "request layout");
static_assert(REQ_BASE + PUB_SIZE + TOKEN_ID_SIZE == REQ_ENROLL, "request layout");
static_assert(RESP_SIGNED + 32 == RESP_SIZE, "response layout");

static void wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }

// Key material of a kilobyte and more. Internal RAM is what TLS and the radio
// compete for, so on the ESP32 these go to PSRAM when there is one.
#if defined(ESP32)
#include <esp_heap_caps.h>
static void *bigAlloc(size_t n) { return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT); }
#else
static void *bigAlloc(size_t n) { return malloc(n); }
#endif

// ---------------------------------------------------------------- primitives

static int rng(void *, unsigned char *out, size_t len) {
  c2_randombytes(out, len);
  return 0;
}

struct Part {
  const void *p;
  size_t n;
};

static void sha256(const Part *parts, size_t count, uint8_t out[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts_ret(&c, 0);
  for (size_t i = 0; i < count; i++) mbedtls_sha256_update_ret(&c, (const uint8_t *)parts[i].p, parts[i].n);
  mbedtls_sha256_finish_ret(&c, out);
  mbedtls_sha256_free(&c);
}

static bool hmac(const uint8_t *key, size_t keyLen, const Part *parts, size_t count, uint8_t out[32]) {
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  bool ok = mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
            mbedtls_md_hmac_starts(&c, key, keyLen) == 0;
  for (size_t i = 0; ok && i < count; i++) ok = mbedtls_md_hmac_update(&c, (const uint8_t *)parts[i].p, parts[i].n) == 0;
  ok = ok && mbedtls_md_hmac_finish(&c, out) == 0;
  mbedtls_md_free(&c);
  return ok;
}

// HKDF-Expand (RFC 5869) for up to 32 bytes: the first block is all we need.
static bool expand(const uint8_t prk[32], const char *label, const uint8_t th[32], uint8_t out[32]) {
  const uint8_t one = 1;
  Part parts[] = {{label, strlen(label)}, {th, 32}, {&one, 1}};
  return hmac(prk, 32, parts, 3, out);
}

// X25519 (RFC 7748): out = scalar * point; point == nullptr means the base point.
// mbedTLS keeps Montgomery coordinates as numbers, the wire format is little endian.
static bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point) {
  uint8_t k[32], u[32] = {9};
  memcpy(k, scalar, 32);
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
  if (point) {
    memcpy(u, point, 32);
    u[31] &= 127;            // the top bit is not part of the coordinate
  }
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q, r;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_ecp_point_init(&r);
  mbedtls_mpi_init(&d);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
            mbedtls_mpi_read_binary_le(&d, k, 32) == 0 &&
            mbedtls_mpi_read_binary_le(&q.X, u, 32) == 0 &&
            mbedtls_mpi_lset(&q.Z, 1) == 0 &&
            mbedtls_ecp_mul(&grp, &r, &d, &q, rng, nullptr) == 0 &&
            mbedtls_mpi_write_binary_le(&r.X, out, 32) == 0;
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  wipe(k, sizeof(k));
  if (!ok) return false;
  uint8_t acc = 0;                       // all zero = the peer sent a point of small order
  for (int i = 0; i < 32; i++) acc |= out[i];
  return acc != 0;
}

static void putBE64(uint8_t *p, uint64_t v) {
  for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t getBE64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

// AES-256-GCM as the record layer uses it. Decrypting checks the tag first.
static bool gcm(bool encrypt, const uint8_t key[32], const uint8_t sid[ID_SIZE], char dir, uint64_t seq,
                const uint8_t *in, size_t len, uint8_t *out, uint8_t *tag) {
  uint8_t nonce[12] = {0}, aad[1 + ID_SIZE + 8];
  putBE64(nonce + 4, seq);
  aad[0] = (uint8_t)dir;
  memcpy(aad + 1, sid, ID_SIZE);
  putBE64(aad + 1 + ID_SIZE, seq);
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0;
  if (ok && encrypt)
    ok = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, aad, sizeof(aad), in, out, 16, tag) == 0;
  else if (ok)
    ok = mbedtls_gcm_auth_decrypt(&g, len, nonce, 12, aad, sizeof(aad), tag, 16, in, out) == 0;
  mbedtls_gcm_free(&g);
  return ok;
}

// ---------------------------------------------------------------- keys

bool generateKey(uint8_t priv[PRIV_SIZE]) {
  c2_randombytes(priv, PRIV_SIZE);
  return true;
}

// Expands the ML-KEM seed. ek goes to pub + 32 if pub is given; dk may be nullptr.
static bool expandKey(const uint8_t priv[PRIV_SIZE], uint8_t *pub, uint8_t *dk) {
  uint8_t *ek = (uint8_t *)bigAlloc(EK_SIZE), *tmp = dk ? dk : (uint8_t *)bigAlloc(DK_SIZE);
  bool ok = ek && tmp && crypto_kem_keypair_derand(ek, tmp, priv + X_SIZE) == 0;
  if (ok && pub) {
    ok = x25519(pub, priv, nullptr);
    memcpy(pub + X_SIZE, ek, EK_SIZE);
  }
  if (tmp && !dk) {
    wipe(tmp, DK_SIZE);
    free(tmp);
  }
  free(ek);
  return ok;
}

bool publicKey(const uint8_t priv[PRIV_SIZE], uint8_t pub[PUB_SIZE]) { return expandKey(priv, pub, nullptr); }

void fingerprint(const uint8_t pub[PUB_SIZE], uint8_t out[32]) {
  Part parts[] = {{"rs232-c2 key v1", 15}, {pub, PUB_SIZE}};
  sha256(parts, 2, out);
}

// ---------------------------------------------------------------- token

// Base64, standard or URL alphabet, padding optional. Returns the length or -1.
static int b64decode(const char *in, size_t inLen, uint8_t *out, size_t outMax) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < inLen; i++) {
    char c = in[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outMax) return -1;
      out[n++] = (uint8_t)(acc >> bits);
    }
  }
  return (int)n;
}

static bool field(JsonDocument &d, const char *key, uint8_t *out, size_t want) {
  const char *s = d[key] | "";
  return b64decode(s, strlen(s), out, want) == (int)want;
}

bool parseToken(const char *text, Token &t) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
  size_t len = strlen(text);
  while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n')) len--;
  if (len < 6 || len > 1024 || memcmp(text, "c2e1:", 5) != 0) return false;
  uint8_t buf[768];
  int n = b64decode(text + 5, len - 5, buf, sizeof(buf));
  if (n <= 0) return false;
  JsonDocument d;
  if (deserializeJson(d, (const char *)buf, (size_t)n)) return false;
  const char *url = d["u"] | "";
  t.url = url;
  return !t.url.empty() && t.url.size() <= 200 && field(d, "fp", t.fingerprint, 32) &&
         field(d, "id", t.id, TOKEN_ID_SIZE) && field(d, "s", t.secret, PSK_SIZE);
}

bool tokenPinsServer(const Token &t, const uint8_t serverPub[PUB_SIZE]) {
  uint8_t fp[32], diff = 0;
  fingerprint(serverPub, fp);
  for (int i = 0; i < 32; i++) diff |= fp[i] ^ t.fingerprint[i];
  return diff == 0;
}

// ---------------------------------------------------------------- handshake

struct Handshake::State {
  uint8_t priv[PRIV_SIZE];        // device static key
  uint8_t devPub[PUB_SIZE];
  uint8_t serverPub[PUB_SIZE];
  uint8_t ex[X_SIZE];             // ephemeral X25519 scalar
  uint8_t edk[DK_SIZE];           // ephemeral ML-KEM decapsulation key
  uint8_t ddk[DK_SIZE];           // the device's static one, expanded from the seed
  uint8_t ssS[32], dhES[32];
  uint8_t psk[PSK_SIZE];          // zeros unless enrolling
  uint8_t req[REQ_ENROLL];        // what was sent: part of the transcript
  size_t reqLen;
  bool begun;
};

Handshake::Handshake() : st(nullptr) {}

Handshake::~Handshake() {
  if (!st) return;
  wipe(st, sizeof(State));
  free(st);
}

Result Handshake::begin(Kind kind, const uint8_t priv[PRIV_SIZE], const uint8_t serverPub[PUB_SIZE],
                        const uint8_t *tokenId, const uint8_t *psk, uint8_t *req, size_t &reqLen) {
  if (kind != KIND_SESSION && kind != KIND_ENROLL) return ERR_FORMAT;
  if (kind == KIND_ENROLL && (!tokenId || !psk)) return ERR_FORMAT;
  if (!st) {
    st = (State *)bigAlloc(sizeof(State));
    if (!st) return ERR_CRYPTO;
  }
  memset(st, 0, sizeof(State));
  memcpy(st->priv, priv, PRIV_SIZE);
  memcpy(st->serverPub, serverPub, PUB_SIZE);
  if (kind == KIND_ENROLL) memcpy(st->psk, psk, PSK_SIZE);
  if (!expandKey(priv, st->devPub, st->ddk)) return ERR_CRYPTO;

  uint8_t *p = req;
  *p++ = VERSION;
  *p++ = kind;
  uint8_t fp[32];
  fingerprint(st->devPub, fp);
  memcpy(p, fp, ID_SIZE);
  p += ID_SIZE;

  uint8_t coins[64];
  c2_randombytes(st->ex, X_SIZE);
  if (!x25519(p, st->ex, nullptr)) return ERR_CRYPTO;                          // ephemeral X25519 public key
  p += X_SIZE;
  c2_randombytes(coins, 64);
  if (crypto_kem_keypair_derand(p, st->edk, coins) != 0) return ERR_CRYPTO;    // ephemeral ML-KEM key
  p += EK_SIZE;
  c2_randombytes(coins, 32);
  // an encapsulation key that fails the FIPS 203 input check is a bad server key
  if (crypto_kem_enc_derand(p, st->ssS, serverPub + X_SIZE, coins) != 0) return ERR_FORMAT;
  p += CT_SIZE;
  wipe(coins, sizeof(coins));
  if (!x25519(st->dhES, st->ex, serverPub)) return ERR_FORMAT;
  if (kind == KIND_ENROLL) {
    memcpy(p, st->devPub, PUB_SIZE);
    p += PUB_SIZE;
    memcpy(p, tokenId, TOKEN_ID_SIZE);
    p += TOKEN_ID_SIZE;
  }
  reqLen = (size_t)(p - req);

  // No hash is left open while the request is under way: the TLS connection
  // in between needs the SHA hardware for itself.
  memcpy(st->req, req, reqLen);
  st->reqLen = reqLen;
  st->begun = true;
  return OK;
}

Result Handshake::finish(const uint8_t *resp, size_t len, Session &s) {
  if (!st || !st->begun) return ERR_FORMAT;
  st->begun = false;                       // one answer per request
  if (len != RESP_SIZE || resp[0] != VERSION) return ERR_FORMAT;
  const uint8_t *sid = resp + 1;
  const uint8_t *esx = sid + ID_SIZE;
  const uint8_t *ctE = esx + X_SIZE;
  const uint8_t *ctD = ctE + CT_SIZE;
  const uint8_t *confirm = resp + RESP_SIGNED;

  // Order as in the server's schedule(): ssS, ssE, ssD, dhES, dhEE, dhSE.
  uint8_t ikm[6 * 32], th[32], prk[32], k[32], want[32];
  memcpy(ikm, st->ssS, 32);
  memcpy(ikm + 96, st->dhES, 32);
  Result res = ERR_FORMAT;
  bool ok = crypto_kem_dec(ikm + 32, ctE, st->edk) == 0 && crypto_kem_dec(ikm + 64, ctD, st->ddk) == 0 &&
            x25519(ikm + 128, st->ex, esx) && x25519(ikm + 160, st->priv, esx);
  if (ok) {
    res = ERR_CRYPTO;
    Part transcript[] = {{"rs232-c2 hs v1", 14}, {st->serverPub, PUB_SIZE}, {st->devPub, PUB_SIZE},
                         {st->req, st->reqLen}, {resp, RESP_SIGNED}};
    sha256(transcript, 5, th);
    Part ikmPart[] = {{ikm, sizeof(ikm)}};
    Part confPart[] = {{"server", 6}, {th, 32}};
    ok = hmac(st->psk, PSK_SIZE, ikmPart, 1, prk) && expand(prk, "confirm", th, k) && hmac(k, 32, confPart, 2, want);
  }
  if (ok) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= want[i] ^ confirm[i];
    ok = diff == 0;
    if (!ok) res = ERR_CONFIRM;
  }
  if (ok) {
    ok = expand(prk, "c2s", th, s.kSend) && expand(prk, "s2c", th, s.kRecv) && expand(prk, "sas", th, k);
    if (ok) {
      uint32_t v = ((uint32_t)k[0] << 24) | ((uint32_t)k[1] << 16) | ((uint32_t)k[2] << 8) | k[3];
      snprintf(s.sas, sizeof(s.sas), "%06lu", (unsigned long)(v % 1000000));
      memcpy(s.id, sid, ID_SIZE);
      s.sendSeq = s.recvSeq = 0;
      res = OK;
    }
  }
  wipe(ikm, sizeof(ikm));
  wipe(prk, sizeof(prk));
  wipe(k, sizeof(k));
  wipe(st, offsetof(State, req));          // the handshake secrets are used up
  return res;
}

// ---------------------------------------------------------------- records

bool Session::seal(const uint8_t *plain, size_t len, uint8_t *out, size_t &outLen) {
  if (len > MAX_PLAINTEXT || sendSeq == UINT64_MAX) return false;
  uint64_t seq = sendSeq++;
  putBE64(out, seq);
  if (!gcm(true, kSend, id, 'c', seq, plain, len, out + 8, out + 8 + len)) return false;
  outLen = len + RECORD_OVERHEAD;
  return true;
}

bool Session::open(const uint8_t *record, size_t len, uint8_t *out, size_t &outLen) {
  if (len < RECORD_OVERHEAD || len > MAX_PLAINTEXT + RECORD_OVERHEAD) return false;
  uint64_t seq = getBE64(record);
  if (seq < recvSeq) return false;
  size_t n = len - RECORD_OVERHEAD;
  if (!gcm(false, kRecv, id, 's', seq, record + 8, n, out, (uint8_t *)record + 8 + n)) return false;
  recvSeq = seq + 1;
  outLen = n;
  return true;
}

void Session::clear() {
  wipe(kSend, sizeof(kSend));
  wipe(kRecv, sizeof(kRecv));
  memset(sas, 0, sizeof(sas));
  sendSeq = recvSeq = 0;
}

// ---------------------------------------------------------------- self-test

static bool hex(const char *h, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(h + 2 * i, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static bool is(const uint8_t *got, const char *wantHex, size_t n) {
  uint8_t want[64];
  return n <= sizeof(want) && hex(wantHex, want, n) && memcmp(got, want, n) == 0;
}

const char *selfTest() {
  uint8_t a[64], b[64], out[64];

  // RFC 7748 section 6.1
  hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
  hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", b, 32);
  if (!x25519(out, a, nullptr) || !is(out, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32)) return "X25519 base";
  if (!x25519(out, a, b) || !is(out, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32)) return "X25519";
  memset(b, 0, 32);
  if (x25519(out, a, b)) return "X25519 zero point";

  // RFC 5869 test case 1 (first 32 bytes of the output)
  {
    uint8_t ikm[22], salt[13], prk[32], info[11];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    info[10] = 1;
    Part p1[] = {{ikm, sizeof(ikm)}}, p2[] = {{info, sizeof(info)}};
    if (!hmac(salt, sizeof(salt), p1, 1, prk) || !is(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32)) return "HKDF extract";
    if (!hmac(prk, 32, p2, 1, out) || !is(out, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf", 32)) return "HKDF expand";
  }

  // AES-256-GCM, zero key, zero nonce, one zero block (GCM specification, test case 14)
  {
    uint8_t key[32] = {0}, nonce[12] = {0}, pt[16] = {0}, ct[16], tag[16];
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 16, nonce, 12, nullptr, 0, pt, ct, 16, tag) == 0;
    mbedtls_gcm_free(&g);
    if (!ok || !is(ct, "cea7403d4d606b6e074ec5d3baf39d18", 16) || !is(tag, "d0d1c8a799996bf0265b98b5d48ab919", 16)) return "AES-256-GCM";
  }

  // ML-KEM-768: the key for the seed 00 01 .. 3f, as Go's crypto/mlkem derives it
  {
    uint8_t *ek = (uint8_t *)bigAlloc(EK_SIZE), *dk = (uint8_t *)bigAlloc(DK_SIZE), *ct = (uint8_t *)bigAlloc(CT_SIZE);
    const char *failed = nullptr;
    for (int i = 0; i < 64; i++) a[i] = (uint8_t)i;
    if (!ek || !dk || !ct) failed = "ML-KEM (no memory)";
    else if (crypto_kem_keypair_derand(ek, dk, a) != 0) failed = "ML-KEM keygen";
    else {
      Part p[] = {{ek, EK_SIZE}};
      sha256(p, 1, out);
      if (!is(out, "0b7934c83125c788995e2ba6bd761e33046b3e40571be53e023309a29f398cc9", 32)) failed = "ML-KEM key";
      else if (crypto_kem_enc_derand(ct, out, ek, a) != 0 || crypto_kem_dec(out + 32, ct, dk) != 0 ||
               memcmp(out, out + 32, 32) != 0) failed = "ML-KEM encaps/decaps";
    }
    free(ek);
    free(dk);
    free(ct);
    if (failed) return failed;
  }
  return nullptr;
}

}  // namespace C2Proto
