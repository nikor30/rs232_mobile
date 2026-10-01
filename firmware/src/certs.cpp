#include "certs.h"
#include "compat_mbedtls.h"   // mbedTLS 2.x (core 2) vs 3.x (core 3)
#include "legacy_ciphers.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

namespace Certs {

// ============================================================================ storage
static const char *DIR_TLS = "/tls";
static const char *CRT_FILE[SLOTS] = {"/tls/ca.pem", "/tls/client.crt", "/tls/https.crt"};
static const char *KEY_FILE[SLOTS] = {nullptr, "/tls/client.key", "/tls/https.key"};
static const char *CSR_KEY = "/tls/csr.key";
static const char *CSR_REQ = "/tls/csr.pem";
static const char *DEVCA_CRT = "/tls/devca.crt";
static const char *DEVCA_KEY = "/tls/devca.key";
static const char *AUTO_FLAG = "/tls/https.auto";
static const size_t MAX_UPLOAD = 24 * 1024;
static const long MAX_KDF_ITER = 100000;       // usual exports: 2048; more would block the loop for seconds
static const size_t MAX_CERTS = 12, MAX_KEYS = 4;

struct SlotData {
  String crt, key;
  bool match = false;           // key belongs to the (first) certificate
};
static SlotData slot[SLOTS];
static String csrKey, csrReq, caCrt, caKey;
static bool autoHttps = false;
static bool fsOk = false;
static std::mutex mtx;          // slot data: web handlers (loop task) vs. HTTPS task
static std::atomic<uint32_t> gen{1};

static bool fileExists(const char *path) {
  if (!fsOk) return false;
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  File dir = LittleFS.open(DIR_TLS);
  if (!dir) return false;
  bool found = false;
  for (File f = dir.openNextFile(); f && !found; f = dir.openNextFile()) {
    found = !strcmp(f.name(), name);
    f.close();
  }
  return found;
}

static String readFile(const char *path) {
  if (!fileExists(path)) return String();
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

static bool writeFile(const char *path, const String &s) {
  if (!fsOk) return false;
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t w = f.print(s);
  f.close();
  return w == s.length();
}

static bool fileExists(const char *path);

static void removeFile(const char *path) {
  if (fileExists(path)) LittleFS.remove(path);   // remove() logs an error for missing files
}

struct Burner;

static void wipeString(String &s) {
  if (s.length()) mbedtls_platform_zeroize((void *)s.c_str(), s.length());
  s = String();
}

// wipes a String holding key material when it goes out of scope
struct Burner {
  String &s;
  ~Burner() { wipeString(s); }
};

static void burn(std::string &s) {
  if (!s.empty()) mbedtls_platform_zeroize(&s[0], s.size());
  s.clear();
}

static int rng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);                  // hardware RNG (true random while the radio is on)
  return 0;
}

// ============================================================================ time
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int64_t toEpoch(const mbedtls_x509_time &t) {
  return daysFromCivil(t.year, t.mon, t.day) * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
}

bool timeKnown() { return time(nullptr) > 1700000000; }

void setTime(uint32_t epoch) {
  if (epoch < 1700000000 || timeKnown()) return;
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  Serial.printf("[TLS] Uhrzeit vom Browser übernommen\n");
}

static int64_t buildEpoch() {
  static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__;                   // "Sep 20 2026"
  char mon[4] = {d[0], d[1], d[2], 0};
  const char *p = strstr(M, mon);
  unsigned m = p ? (unsigned)(p - M) / 3 + 1 : 1;
  return daysFromCivil(atoi(d + 7), m, atoi(d + 4)) * 86400;
}

static String fmtDate(const mbedtls_x509_time &t) {
  char b[16];
  snprintf(b, sizeof(b), "%04d-%02d-%02d", t.year, t.mon, t.day);
  return String(b);
}

// epoch -> "YYYYMMDDhhmmss" without gmtime (time_t is 32 bit on ESP-IDF 4.4: dates after 2038)
static void x509Time(int64_t epoch, char out[24]) {
  int64_t days = epoch >= 0 ? epoch / 86400 : (epoch - 86399) / 86400;
  int64_t secs = epoch - days * 86400;
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int y = (int)(yoe + era * 400) + (m <= 2);
  const unsigned hh = (unsigned)(secs / 3600) % 24, mi = (unsigned)(secs / 60) % 60, ss = (unsigned)secs % 60;
  snprintf(out, 24, "%04u%02u%02u%02u%02u%02u", (unsigned)y % 10000, m % 100, d % 100, hh, mi, ss);
}

// ============================================================================ BER / DER reader
// Enough ASN.1 for PKCS#12, PKCS#7 and certificate extensions; also accepts the
// indefinite lengths and constructed OCTET STRINGs some Windows/Java exports use.
struct Tlv {
  uint8_t tag = 0;
  const uint8_t *s = nullptr;                 // element start (tag byte)
  const uint8_t *v = nullptr;                 // content
  size_t len = 0;                             // content length
  const uint8_t *end = nullptr;               // behind the element
};

static bool berRead(const uint8_t *p, const uint8_t *end, Tlv &t, int depth = 0) {
  t = Tlv();                                  // never leave fields of a previous element behind
  if (depth > 16 || p >= end || end - p < 2) return false;
  t.s = p;
  t.tag = *p++;
  if ((t.tag & 0x1f) == 0x1f) return false;   // high tag numbers: not used here
  uint8_t l = *p++;
  if (l == 0x80) {                            // indefinite length (BER), constructed only
    if (!(t.tag & 0x20)) return false;
    const uint8_t *q = p;
    while (true) {
      if (end - q < 2) return false;
      if (q[0] == 0 && q[1] == 0) {
        t.v = p;
        t.len = q - p;
        t.end = q + 2;
        return true;
      }
      Tlv c;
      if (!berRead(q, end, c, depth + 1)) return false;
      q = c.end;
    }
  }
  size_t len = 0;
  if (l & 0x80) {
    int n = l & 0x7f;
    if (n == 0 || n > 4 || end - p < n) return false;
    while (n--) len = (len << 8) | *p++;
  } else {
    len = l;
  }
  if ((size_t)(end - p) < len) return false;
  t.v = p;
  t.len = len;
  t.end = p + len;
  return true;
}

struct BerIter {
  const uint8_t *p, *end;
  explicit BerIter(const Tlv &t) : p(t.v), end(t.v + t.len) {}
  bool next(Tlv &c) {
    if (p >= end) return false;
    if (!berRead(p, end, c)) {
      p = end;
      return false;
    }
    p = c.end;
    return true;
  }
};

// content of an OCTET STRING: primitive, constructed (segments) or wrapped in [0]
static bool octets(const Tlv &t, std::string &out, int depth = 0) {
  if (!(t.tag & 0x20)) {
    out.append((const char *)t.v, t.len);
    return true;
  }
  if (depth > 8) return false;
  BerIter it(t);
  Tlv c;
  while (it.next(c))
    if (!octets(c, out, depth + 1)) return false;
  return true;
}

static long derInt(const Tlv &t) {
  if (t.tag != 0x02 || t.len == 0 || t.len > 4) return -1;
  long v = 0;
  for (size_t i = 0; i < t.len; i++) v = (v << 8) | t.v[i];
  return v;
}

#define OIDDEF(name, ...) static const uint8_t name[] = {__VA_ARGS__}
OIDDEF(OID_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01);
OIDDEF(OID_SIGNED_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02);
OIDDEF(OID_ENVELOPED_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x03);
OIDDEF(OID_ENCRYPTED_DATA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x06);
OIDDEF(OID_KEY_BAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01);
OIDDEF(OID_SHROUDED_KEY_BAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x02);
OIDDEF(OID_CERT_BAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03);
OIDDEF(OID_SAFE_CONTENTS_BAG, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x06);
OIDDEF(OID_X509_CERT, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x16, 0x01);
OIDDEF(OID_PBES2, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0D);
OIDDEF(OID_PBKDF2, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0C);
OIDDEF(OID_PBE_SHA_PREFIX, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x01);   // + 1..6
OIDDEF(OID_HMAC_PREFIX, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02);                  // + 7..11
OIDDEF(OID_AES_CBC_PREFIX, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01);         // + 2/22/42
OIDDEF(OID_SHA2_PREFIX, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02);            // + 1..4
OIDDEF(OID_SHA1, 0x2B, 0x0E, 0x03, 0x02, 0x1A);
OIDDEF(OID_DES_EDE3_CBC, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x03, 0x07);
OIDDEF(OID_DES_CBC, 0x2B, 0x0E, 0x03, 0x02, 0x07);
OIDDEF(OID_SAN, 0x55, 0x1D, 0x11);
OIDDEF(OID_UPN, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x14, 0x02, 0x03);

template <size_t N>
static bool oidIs(const Tlv &t, const uint8_t (&oid)[N]) {
  return t.tag == 0x06 && t.len == N && !memcmp(t.v, oid, N);
}
// OID = prefix + one more byte; returns that byte or -1
template <size_t N>
static int oidSuffix(const Tlv &t, const uint8_t (&prefix)[N]) {
  if (t.tag != 0x06 || t.len != N + 1 || memcmp(t.v, prefix, N)) return -1;
  return t.v[N];
}

// ============================================================================ password based decryption
static std::string bmpPassword(const String &pw) {    // UTF-8 -> UTF-16BE + 00 00 (PKCS#12)
  std::string o;
  const uint8_t *s = (const uint8_t *)pw.c_str();
  size_t n = pw.length(), i = 0;
  while (i < n) {
    uint32_t c = s[i++];
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= (0x3F >> extra);
    while (extra-- > 0 && i < n) c = (c << 6) | (s[i++] & 0x3F);
    if (c > 0xFFFF) {
      c -= 0x10000;
      uint16_t hi = 0xD800 | (c >> 10), lo = 0xDC00 | (c & 0x3FF);
      o += (char)(hi >> 8); o += (char)hi; o += (char)(lo >> 8); o += (char)lo;
    } else {
      o += (char)(c >> 8); o += (char)c;
    }
  }
  o += '\0';
  o += '\0';
  return o;
}

static mbedtls_md_type_t mdFromDigestOid(const Tlv &oid) {
  if (oidIs(oid, OID_SHA1)) return MBEDTLS_MD_SHA1;
  switch (oidSuffix(oid, OID_SHA2_PREFIX)) {
    case 1: return MBEDTLS_MD_SHA256;
    case 2: return MBEDTLS_MD_SHA384;
    case 3: return MBEDTLS_MD_SHA512;
    case 4: return MBEDTLS_MD_SHA224;
  }
  return MBEDTLS_MD_NONE;
}

// PKCS#12 key derivation (RFC 7292 appendix B.2); id 1 = key, 2 = IV, 3 = MAC key.
// Own implementation: mbedtls 2.28 limits the password to 31 characters.
static bool p12Kdf(mbedtls_md_type_t mdt, uint8_t id, const std::string &pw, const uint8_t *salt, size_t slen,
                   long iter, uint8_t *out, size_t n) {
  const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(mdt);
  if (!mi || iter < 1 || iter > MAX_KDF_ITER) return false;
  const size_t u = mbedtls_md_get_size(mi);
  const size_t v = (mdt == MBEDTLS_MD_SHA384 || mdt == MBEDTLS_MD_SHA512) ? 128 : 64;
  std::string D(v, (char)id);
  size_t sl = slen ? v * ((slen + v - 1) / v) : 0, pl = pw.size() ? v * ((pw.size() + v - 1) / v) : 0;
  std::string I(sl + pl, '\0');
  for (size_t i = 0; i < sl; i++) I[i] = (char)salt[i % slen];
  for (size_t i = 0; i < pl; i++) I[sl + i] = pw[i % pw.size()];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, mi, 0)) { mbedtls_md_free(&ctx); return false; }
  uint8_t A[64];
  size_t done = 0;
  while (done < n) {
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const uint8_t *)D.data(), v);
    mbedtls_md_update(&ctx, (const uint8_t *)I.data(), I.size());
    mbedtls_md_finish(&ctx, A);
    for (long k = 1; k < iter; k++) {
      mbedtls_md_starts(&ctx);
      mbedtls_md_update(&ctx, A, u);
      mbedtls_md_finish(&ctx, A);
    }
    size_t take = n - done < u ? n - done : u;
    memcpy(out + done, A, take);
    done += take;
    if (done >= n) break;
    for (size_t j = 0; j < I.size(); j += v) {    // I_j = (I_j + B + 1) mod 2^(8v), B = A repeated
      unsigned c = 1;
      for (int i = (int)v - 1; i >= 0; i--) {
        c += (uint8_t)I[j + i] + A[i % u];
        I[j + i] = (char)c;
        c >>= 8;
      }
    }
  }
  mbedtls_md_free(&ctx);
  mbedtls_platform_zeroize(A, sizeof(A));
  burn(I);
  return true;
}

static bool unpad(std::string &s, size_t bs) {
  if (s.empty() || s.size() % bs) return false;
  uint8_t n = (uint8_t)s.back();
  if (n == 0 || n > bs) return false;
  for (size_t i = s.size() - n; i < s.size(); i++)
    if ((uint8_t)s[i] != n) return false;
  s.resize(s.size() - n);
  return true;
}

static bool aesCbcDecrypt(const uint8_t *key, size_t keyLen, const uint8_t *iv16, const std::string &ct, std::string &pt) {
  if (ct.empty() || ct.size() % 16) return false;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  uint8_t iv[16];
  memcpy(iv, iv16, 16);
  pt.assign(ct.size(), '\0');
  bool ok = !mbedtls_aes_setkey_dec(&aes, key, keyLen * 8) &&
            !mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ct.size(), iv, (const uint8_t *)ct.data(), (uint8_t *)&pt[0]);
  mbedtls_aes_free(&aes);
  return ok;
}

enum DecErr { DEC_OK = 0, DEC_FORMAT, DEC_PASSWORD, DEC_UNSUPPORTED };

// algId = AlgorithmIdentifier of PKCS#12 PBE or PBES2 (PKCS#5)
static DecErr pbeDecrypt(const Tlv &algId, const String &pw, const std::string &bmpPw, const std::string &ct,
                         std::string &pt, String &algName) {
  BerIter it(algId);
  Tlv oid, params;
  if (!it.next(oid) || !it.next(params) || params.tag != 0x30) return DEC_FORMAT;
  uint8_t key[32], iv[16];
  DecErr res = DEC_FORMAT;

  int pbe = oidSuffix(oid, OID_PBE_SHA_PREFIX);
  if (pbe > 0) {                               // PKCS#12 PBE: SHA-1 + 3DES / RC2 / RC4
    BerIter pi(params);
    Tlv salt, iter;
    if (!pi.next(salt) || salt.tag != 0x04 || !pi.next(iter)) return DEC_FORMAT;
    Legacy::Cipher c;
    size_t kl;
    unsigned bits = 0;
    switch (pbe) {
      case 3: c = Legacy::DES_EDE3_CBC; kl = 24; algName = "3DES"; break;
      case 4: c = Legacy::DES_EDE3_CBC; kl = 16; algName = "2-Key-3DES"; break;
      case 5: c = Legacy::RC2_CBC; kl = 16; bits = 128; algName = "RC2-128"; break;
      case 6: c = Legacy::RC2_CBC; kl = 5; bits = 40; algName = "RC2-40"; break;
      default: algName = "RC4"; return DEC_UNSUPPORTED;
    }
    if (!p12Kdf(MBEDTLS_MD_SHA1, 1, bmpPw, salt.v, salt.len, derInt(iter), key, kl) ||
        !p12Kdf(MBEDTLS_MD_SHA1, 2, bmpPw, salt.v, salt.len, derInt(iter), iv, 8))
      return DEC_FORMAT;
    if (ct.empty() || ct.size() % 8) return DEC_FORMAT;
    pt.assign(ct.size(), '\0');
    Legacy::cbcDecrypt(c, key, kl, iv, (const uint8_t *)ct.data(), ct.size(), (uint8_t *)&pt[0], bits);
    res = unpad(pt, 8) ? DEC_OK : DEC_PASSWORD;
  } else if (oidIs(oid, OID_PBES2)) {          // PKCS#5 v2: PBKDF2 + AES / 3DES
    BerIter pi(params);
    Tlv kdf, enc;
    if (!pi.next(kdf) || !pi.next(enc)) return DEC_FORMAT;
    BerIter ki(kdf);
    Tlv kdfOid, kdfParams;
    if (!ki.next(kdfOid) || !oidIs(kdfOid, OID_PBKDF2) || !ki.next(kdfParams)) {
      algName = "PBES2 (scrypt?)";
      return DEC_UNSUPPORTED;
    }
    BerIter kp(kdfParams);
    Tlv salt, iter, x;
    if (!kp.next(salt) || salt.tag != 0x04 || !kp.next(iter)) return DEC_FORMAT;
    mbedtls_md_type_t prf = MBEDTLS_MD_SHA1;
    while (kp.next(x)) {
      if (x.tag != 0x30) continue;               // keyLength INTEGER: implied by the cipher
      BerIter pr(x);
      Tlv prfOid;
      if (!pr.next(prfOid)) continue;
      switch (oidSuffix(prfOid, OID_HMAC_PREFIX)) {
        case 7: prf = MBEDTLS_MD_SHA1; break;
        case 8: prf = MBEDTLS_MD_SHA224; break;
        case 9: prf = MBEDTLS_MD_SHA256; break;
        case 10: prf = MBEDTLS_MD_SHA384; break;
        case 11: prf = MBEDTLS_MD_SHA512; break;
        default: algName = "PBES2 (PRF)"; return DEC_UNSUPPORTED;
      }
    }
    BerIter ei(enc);
    Tlv encOid, ivT;
    if (!ei.next(encOid) || !ei.next(ivT)) return DEC_FORMAT;
    int aes = oidSuffix(encOid, OID_AES_CBC_PREFIX);
    size_t kl = 0, ivl = 8;
    if (aes == 2 || aes == 22 || aes == 42) {
      kl = aes == 2 ? 16 : aes == 22 ? 24 : 32;
      ivl = 16;
      algName = String("AES-") + (kl * 8);
    } else if (oidIs(encOid, OID_DES_EDE3_CBC)) {
      kl = 24;
      algName = "3DES";
    } else if (oidIs(encOid, OID_DES_CBC)) {
      kl = 8;
      algName = "DES";
    } else {
      algName = "PBES2 (Verfahren)";
      return DEC_UNSUPPORTED;
    }
    if (ivT.tag != 0x04 || ivT.len != ivl) return DEC_FORMAT;
    long iterations = derInt(iter);
    if (iterations < 1 || iterations > MAX_KDF_ITER) return DEC_FORMAT;
    mbedtls_md_context_t md;
    (void)md;
    bool ok = !rsPbkdf2Hmac(prf, (const uint8_t *)pw.c_str(), pw.length(), salt.v, salt.len,
                            (unsigned)iterations, kl, key);
    if (!ok) return DEC_FORMAT;
    if (ivl == 16) {
      if (!aesCbcDecrypt(key, kl, ivT.v, ct, pt)) res = DEC_FORMAT;
      else res = unpad(pt, 16) ? DEC_OK : DEC_PASSWORD;
    } else {
      if (ct.empty() || ct.size() % 8) return DEC_FORMAT;
      pt.assign(ct.size(), '\0');
      Legacy::cbcDecrypt(kl == 8 ? Legacy::DES_CBC : Legacy::DES_EDE3_CBC, key, kl, ivT.v, (const uint8_t *)ct.data(),
                         ct.size(), (uint8_t *)&pt[0]);
      res = unpad(pt, 8) ? DEC_OK : DEC_PASSWORD;
    }
  } else {
    algName = "unbekannt";
    return DEC_UNSUPPORTED;
  }
  mbedtls_platform_zeroize(key, sizeof(key));
  if (res != DEC_OK) burn(pt);
  return res;
}

// ============================================================================ parsing uploads
struct Parsed {
  std::vector<std::string> certs;             // DER (bounded: hostile files must not eat the heap)
  std::vector<std::string> keys;              // unencrypted PKCS#8 / PKCS#1 / SEC1 DER
  void addCert(const std::string &d) { if (certs.size() < MAX_CERTS) certs.push_back(d); }
  void addKey(const std::string &d) { if (keys.size() < MAX_KEYS) keys.push_back(d); }
  bool csr = false;
  bool needPassword = false;
  ~Parsed() {
    for (auto &k : keys) burn(k);
  }
};

static bool decErr(DecErr e, const String &alg, bool emptyPw, String &err) {
  if (e == DEC_OK) return true;
  if (e == DEC_PASSWORD) err = emptyPw ? "Datei ist verschlüsselt – bitte Passwort angeben" : "Passwort falsch";
  else if (e == DEC_UNSUPPORTED) err = "Verschlüsselung " + alg + " wird nicht unterstützt – bitte mit AES-256 exportieren";
  else err = "Datei beschädigt oder unbekanntes Format";
  return false;
}

// EncryptedPrivateKeyInfo ::= SEQUENCE { AlgorithmIdentifier, OCTET STRING }
static bool decryptPkcs8(const Tlv &epki, const String &pw, const std::string &bmpPw, Parsed &P, String &err) {
  BerIter it(epki);
  Tlv alg, data;
  if (!it.next(alg) || alg.tag != 0x30 || !it.next(data)) return decErr(DEC_FORMAT, "", false, err);
  std::string ct, pt;
  if (!octets(data, ct)) return decErr(DEC_FORMAT, "", false, err);
  String algName;
  DecErr e = pbeDecrypt(alg, pw, bmpPw, ct, pt, algName);
  if (!decErr(e, algName, pw.isEmpty(), err)) return false;
  P.addKey(pt);
  burn(pt);
  return true;
}

static bool p7Certs(const Tlv &contentInfo, Parsed &P) {
  // ContentInfo { signedData, [0] { SignedData { version, digestAlgs, encapContent, [0] certificates ... } } }
  BerIter it(contentInfo);
  Tlv oid, wrap, sd;
  if (!it.next(oid) || !oidIs(oid, OID_SIGNED_DATA) || !it.next(wrap) || wrap.tag != 0xA0) return false;
  BerIter wi(wrap);
  if (!wi.next(sd) || sd.tag != 0x30) return false;
  BerIter si(sd);
  Tlv x;
  size_t before = P.certs.size();
  while (si.next(x)) {
    if (x.tag != 0xA0) continue;               // [0] IMPLICIT SET OF Certificate
    BerIter ci(x);
    Tlv c;
    while (ci.next(c))
      if (c.tag == 0x30) P.addCert(std::string((const char *)c.s, c.end - c.s));
  }
  return P.certs.size() > before;
}

static void parseSafeContents(const std::string &sc, const String &pw, const std::string &bmpPw, Parsed &P,
                              String &err, int depth);

static void parseBag(const Tlv &bag, const String &pw, const std::string &bmpPw, Parsed &P, String &err, int depth) {
  BerIter it(bag);
  Tlv id, val, inner;
  if (!it.next(id) || !it.next(val) || val.tag != 0xA0) return;
  BerIter vi(val);
  if (!vi.next(inner)) return;
  if (oidIs(id, OID_KEY_BAG)) {
    P.addKey(std::string((const char *)inner.s, inner.end - inner.s));
  } else if (oidIs(id, OID_SHROUDED_KEY_BAG)) {
    String e;
    if (!decryptPkcs8(inner, pw, bmpPw, P, e) && err.isEmpty()) err = e;
  } else if (oidIs(id, OID_CERT_BAG)) {
    BerIter ci(inner);
    Tlv type, cv;
    if (!ci.next(type) || !oidIs(type, OID_X509_CERT) || !ci.next(cv)) return;
    std::string der;
    if (octets(cv, der)) P.addCert(der);
  } else if (oidIs(id, OID_SAFE_CONTENTS_BAG) && depth < 3) {
    parseSafeContents(std::string((const char *)inner.s, inner.end - inner.s), pw, bmpPw, P, err, depth + 1);
  }
}

static void parseSafeContents(const std::string &sc, const String &pw, const std::string &bmpPw, Parsed &P,
                              String &err, int depth) {
  Tlv seq;
  const uint8_t *d = (const uint8_t *)sc.data();
  if (!berRead(d, d + sc.size(), seq) || seq.tag != 0x30) return;
  BerIter it(seq);
  Tlv bag;
  while (it.next(bag))
    if (bag.tag == 0x30) parseBag(bag, pw, bmpPw, P, err, depth);
}

// MAC check: 1 = ok, 0 = wrong password, -1 = cannot check (e.g. PBMAC1)
static int p12Mac(const Tlv &mac, const std::string &bmpPw, const std::string &data) {
  BerIter it(mac);
  Tlv di, salt, iter;
  if (!it.next(di) || di.tag != 0x30 || !it.next(salt) || salt.tag != 0x04) return -1;
  long iterations = 1;
  if (it.next(iter)) iterations = derInt(iter);
  BerIter dit(di);
  Tlv alg, dig, aoid;
  if (!dit.next(alg) || !dit.next(dig) || dig.tag != 0x04) return -1;
  BerIter ai(alg);
  if (!ai.next(aoid)) return -1;
  mbedtls_md_type_t md = mdFromDigestOid(aoid);
  const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(md);
  if (!mi) return -1;
  size_t hl = mbedtls_md_get_size(mi);
  uint8_t key[64], out[64];
  if (!p12Kdf(md, 3, bmpPw, salt.v, salt.len, iterations, key, hl)) return -1;
  mbedtls_md_hmac(mi, key, hl, (const uint8_t *)data.data(), data.size(), out);
  mbedtls_platform_zeroize(key, sizeof(key));
  return dig.len == hl && !memcmp(out, dig.v, hl) ? 1 : 0;
}

static bool parseP12(const Tlv &pfx, const String &pw, Parsed &P, String &err) {
  BerIter it(pfx);
  Tlv ver, auth, mac;
  if (!it.next(ver) || !it.next(auth) || auth.tag != 0x30) return decErr(DEC_FORMAT, "", false, err);
  bool hasMac = it.next(mac) && mac.tag == 0x30;
  BerIter ai(auth);
  Tlv oid, cont;
  if (!ai.next(oid)) return decErr(DEC_FORMAT, "", false, err);
  if (oidIs(oid, OID_SIGNED_DATA)) {
    err = "PKCS#12 mit Public-Key-Schutz wird nicht unterstützt – bitte mit Passwort exportieren";
    return false;
  }
  if (!oidIs(oid, OID_DATA) || !ai.next(cont)) return decErr(DEC_FORMAT, "", false, err);
  std::string safe;
  if (!octets(cont, safe)) return decErr(DEC_FORMAT, "", false, err);

  std::string bmpPw = bmpPassword(pw);
  if (hasMac) {
    int r = p12Mac(mac, bmpPw, safe);
    if (r == 0 && pw.isEmpty() && p12Mac(mac, std::string(), safe) == 1) {
      bmpPw.clear();                           // "no password" variant (zero length)
      r = 1;
    }
    if (r == 0) {
      err = pw.isEmpty() ? "Datei ist verschlüsselt – bitte Passwort angeben" : "Passwort falsch";
      burn(bmpPw);
      return false;
    }
  }

  Tlv seq;
  const uint8_t *d = (const uint8_t *)safe.data();
  if (!berRead(d, d + safe.size(), seq) || seq.tag != 0x30) {
    burn(bmpPw);
    return decErr(DEC_FORMAT, "", false, err);
  }
  BerIter si(seq);
  Tlv ci;
  String firstErr;
  while (si.next(ci)) {
    BerIter cit(ci);
    Tlv ctype, cval;
    if (!cit.next(ctype) || !cit.next(cval) || cval.tag != 0xA0) continue;
    if (oidIs(ctype, OID_DATA)) {
      std::string sc;
      if (octets(cval, sc)) parseSafeContents(sc, pw, bmpPw, P, firstErr, 0);
    } else if (oidIs(ctype, OID_ENCRYPTED_DATA)) {
      // EncryptedData { version, EncryptedContentInfo { contentType, algorithm, [0] IMPLICIT content } }
      BerIter vi(cval);
      Tlv ed;
      if (!vi.next(ed)) continue;
      BerIter ei(ed);
      Tlv v, eci;
      if (!ei.next(v) || !ei.next(eci)) continue;
      BerIter ec(eci);
      Tlv type, alg, content;
      if (!ec.next(type) || !ec.next(alg) || !ec.next(content)) continue;
      std::string ct, pt;
      if (!octets(content, ct)) continue;
      String algName;
      DecErr e = pbeDecrypt(alg, pw, bmpPw, ct, pt, algName);
      if (e != DEC_OK) {
        String msg;
        decErr(e, algName, pw.isEmpty(), msg);
        if (firstErr.isEmpty()) firstErr = msg;
        continue;
      }
      parseSafeContents(pt, pw, bmpPw, P, firstErr, 0);
    } else if (oidIs(ctype, OID_ENVELOPED_DATA)) {
      if (firstErr.isEmpty()) firstErr = "Zertifikats-verschlüsselter Teil wird nicht unterstützt";
    }
  }
  burn(bmpPw);
  if (P.certs.empty() && P.keys.empty() && firstErr.length()) {
    err = firstErr;
    return false;
  }
  if (firstErr.length() && P.keys.empty()) {   // certificates readable, key not
    err = firstErr;
    return false;
  }
  return true;
}

static bool b64decode(const char *s, size_t n, std::string &out) {
  std::string clean;
  clean.reserve(n);
  for (size_t i = 0; i < n; i++)
    if (!isspace((unsigned char)s[i])) clean += s[i];
  size_t olen = 0;
  mbedtls_base64_decode(nullptr, 0, &olen, (const uint8_t *)clean.data(), clean.size());
  out.assign(olen, '\0');
  if (!olen) return false;
  if (mbedtls_base64_decode((uint8_t *)&out[0], olen, &olen, (const uint8_t *)clean.data(), clean.size())) return false;
  out.resize(olen);
  return true;
}

OIDDEF(OID_RSA, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01);
OIDDEF(OID_EC_PUBKEY, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01);

// PrivateKeyInfo ::= SEQUENCE { version, AlgorithmIdentifier { OID, ... }, ... }
static bool keyAlgSupported(const std::string &der) {
  Tlv seq, ver, alg, oid;
  const uint8_t *d = (const uint8_t *)der.data();
  if (!berRead(d, d + der.size(), seq) || seq.tag != 0x30) return true;    // PKCS#1 / SEC1: no OID
  BerIter it(seq);
  if (!it.next(ver) || !it.next(alg) || alg.tag != 0x30) return true;
  BerIter ai(alg);
  if (!ai.next(oid) || oid.tag != 0x06) return true;
  return oidIs(oid, OID_RSA) || oidIs(oid, OID_EC_PUBKEY);
}

static bool keyDerOk(const std::string &der, String &err) {
  if (!keyAlgSupported(der)) {
    err = "Schlüsseltyp nicht unterstützt (nur RSA und EC P-256/384/521)";
    return false;
  }
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int r = rsPkParseKey(&pk, (const uint8_t *)der.data(), der.size(), nullptr, 0);
  bool ok = r == 0;
  if (ok) {
    mbedtls_pk_type_t t = mbedtls_pk_get_type(&pk);
    if (t != MBEDTLS_PK_RSA && t != MBEDTLS_PK_ECKEY) {
      ok = false;
      r = MBEDTLS_ERR_PK_UNKNOWN_PK_ALG;
    }
  }
  mbedtls_pk_free(&pk);
  if (!ok) err = r == MBEDTLS_ERR_PK_UNKNOWN_PK_ALG ? "Schlüsseltyp nicht unterstützt (nur RSA und EC P-256/384/521)"
                                                     : "Privater Schlüssel nicht lesbar (Passwort falsch?)";
  return ok;
}

// "-----BEGIN RSA PRIVATE KEY-----" with "Proc-Type: 4,ENCRYPTED" / "DEK-Info: AES-256-CBC,<iv>"
static bool decryptLegacyPem(const String &dek, const std::string &ct, const String &pw, std::string &pt, String &err) {
  if (pw.isEmpty()) {
    err = "Schlüssel ist verschlüsselt – bitte Passwort angeben";
    return false;
  }
  int comma = dek.indexOf(',');
  String alg = dek.substring(0, comma), ivHex = dek.substring(comma + 1);
  alg.trim();
  ivHex.trim();
  size_t kl = 0, ivl = 16;
  Legacy::Cipher lc = Legacy::DES_EDE3_CBC;
  if (alg == "AES-128-CBC") kl = 16;
  else if (alg == "AES-192-CBC") kl = 24;
  else if (alg == "AES-256-CBC") kl = 32;
  else if (alg == "DES-EDE3-CBC") { kl = 24; ivl = 8; }
  else if (alg == "DES-CBC") { kl = 8; ivl = 8; lc = Legacy::DES_CBC; }
  else {
    err = "PEM-Verschlüsselung " + alg + " wird nicht unterstützt";
    return false;
  }
  if (comma < 0 || ivHex.length() != ivl * 2) {
    err = "DEK-Info ungültig";
    return false;
  }
  uint8_t iv[16], key[32];
  for (size_t i = 0; i < ivl; i++) iv[i] = (uint8_t)strtol(ivHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  // OpenSSL EVP_BytesToKey(MD5, salt = IV[0..7], 1 round)
  const mbedtls_md_info_t *md5 = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  if (!md5) {
    err = "MD5 fehlt";
    return false;
  }
  uint8_t dig[16];
  size_t have = 0;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, md5, 0);
  while (have < kl) {
    mbedtls_md_starts(&ctx);
    if (have) mbedtls_md_update(&ctx, dig, 16);
    mbedtls_md_update(&ctx, (const uint8_t *)pw.c_str(), pw.length());
    mbedtls_md_update(&ctx, iv, 8);
    mbedtls_md_finish(&ctx, dig);
    size_t take = kl - have < 16 ? kl - have : 16;
    memcpy(key + have, dig, take);
    have += take;
  }
  mbedtls_md_free(&ctx);
  bool ok;
  if (ivl == 16) {
    ok = aesCbcDecrypt(key, kl, iv, ct, pt);
    ok = ok && unpad(pt, 16);
  } else {
    ok = !ct.empty() && ct.size() % 8 == 0;
    if (ok) {
      pt.assign(ct.size(), '\0');
      Legacy::cbcDecrypt(lc, key, kl, iv, (const uint8_t *)ct.data(), ct.size(), (uint8_t *)&pt[0]);
      ok = unpad(pt, 8);
    }
  }
  mbedtls_platform_zeroize(key, sizeof(key));
  if (!ok) {
    burn(pt);
    err = "Passwort falsch";
  }
  return ok;
}

static bool parseDer(const uint8_t *d, size_t n, const String &pw, Parsed &P, String &err);

static bool parsePem(const char *s, size_t n, const String &pw, Parsed &P, String &err) {
  std::string text(s, n);
  size_t pos = 0;
  bool any = false;
  while ((pos = text.find("-----BEGIN ", pos)) != std::string::npos) {
    size_t le = text.find("-----", pos + 11);
    if (le == std::string::npos) break;
    std::string label = text.substr(pos + 11, le - pos - 11);
    size_t bodyStart = le + 5;
    std::string endMark = "-----END " + label + "-----";
    size_t be = text.find(endMark, bodyStart);
    if (be == std::string::npos) {
      err = "PEM-Block „" + String(label.c_str()) + "“ unvollständig";
      return false;
    }
    std::string body = text.substr(bodyStart, be - bodyStart);
    pos = be + endMark.size();
    // RFC 1421 headers (legacy encrypted keys): lines with ':' before an empty line
    String dek;
    if (body.find(':') != std::string::npos) {
      size_t blank = body.find("\n\n");
      if (blank == std::string::npos) blank = body.find("\r\n\r\n");
      if (blank != std::string::npos) {
        std::string hdr = body.substr(0, blank);
        size_t di = hdr.find("DEK-Info:");
        if (di != std::string::npos) {
          size_t eol = hdr.find_first_of("\r\n", di);
          dek = String(hdr.substr(di + 9, eol == std::string::npos ? std::string::npos : eol - di - 9).c_str());
          dek.trim();
        }
        body = body.substr(blank);
      }
    }
    std::string der;
    if (!b64decode(body.data(), body.size(), der)) {
      err = "PEM-Block „" + String(label.c_str()) + "“ ist kein gültiges Base64";
      return false;
    }
    any = true;
    if (label == "CERTIFICATE" || label == "X509 CERTIFICATE" || label == "TRUSTED CERTIFICATE") {
      Tlv t;
      const uint8_t *p = (const uint8_t *)der.data();
      if (berRead(p, p + der.size(), t)) P.addCert(std::string((const char *)t.s, t.end - t.s));
    } else if (label == "PRIVATE KEY" || label == "RSA PRIVATE KEY" || label == "EC PRIVATE KEY") {
      if (dek.length()) {
        std::string pt;
        if (!decryptLegacyPem(dek, der, pw, pt, err)) return false;
        P.addKey(pt);
        burn(pt);
      } else {
        P.addKey(der);
      }
    } else if (label == "ENCRYPTED PRIVATE KEY") {
      if (pw.isEmpty()) {
        err = "Schlüssel ist verschlüsselt – bitte Passwort angeben";
        return false;
      }
      Tlv t;
      const uint8_t *p = (const uint8_t *)der.data();
      std::string bmpPw = bmpPassword(pw);
      bool ok = berRead(p, p + der.size(), t) && decryptPkcs8(t, pw, bmpPw, P, err);
      burn(bmpPw);
      if (!ok) {
        if (err.isEmpty()) err = "Verschlüsselter Schlüssel nicht lesbar";
        return false;
      }
    } else if (label == "PKCS7" || label == "CMS") {
      if (!parseDer((const uint8_t *)der.data(), der.size(), pw, P, err)) return false;
    } else if (label == "PKCS12") {
      if (!parseDer((const uint8_t *)der.data(), der.size(), pw, P, err)) return false;
    } else if (label == "CERTIFICATE REQUEST" || label == "NEW CERTIFICATE REQUEST") {
      P.csr = true;
    }
    burn(der);
  }
  if (!any) err = "Keine PEM-Daten gefunden";
  return any;
}

static bool parseDer(const uint8_t *d, size_t n, const String &pw, Parsed &P, String &err) {
  Tlv t;
  if (!berRead(d, d + n, t) || t.tag != 0x30) {
    err = "Unbekanntes Dateiformat (erwartet PEM, DER, .p7b oder .p12/.pfx)";
    return false;
  }
  BerIter it(t);
  Tlv a, b;
  it.next(a);
  it.next(b);
  if (a.tag == 0x02 && a.len == 1 && a.v[0] == 3 && b.tag == 0x30) return parseP12(t, pw, P, err);
  if (a.tag == 0x06) {
    if (p7Certs(t, P)) return true;
    err = "PKCS#7-Datei ohne Zertifikate";
    return false;
  }
  if (a.tag == 0x30 && b.tag == 0x04) {        // EncryptedPrivateKeyInfo
    std::string bmpPw = bmpPassword(pw);
    bool ok = decryptPkcs8(t, pw, bmpPw, P, err);
    burn(bmpPw);
    return ok;
  }
  size_t len = t.end - d;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  bool isCrt = mbedtls_x509_crt_parse_der(&crt, d, len) == 0;
  mbedtls_x509_crt_free(&crt);
  if (isCrt) {
    P.addCert(std::string((const char *)d, len));
    return true;
  }
  mbedtls_x509_csr csr;
  mbedtls_x509_csr_init(&csr);
  bool isCsr = mbedtls_x509_csr_parse_der(&csr, d, len) == 0;
  mbedtls_x509_csr_free(&csr);
  if (isCsr) {
    P.csr = true;
    return true;
  }
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool isKey = rsPkParseKey(&pk, d, len, nullptr, 0) == 0;
  mbedtls_pk_free(&pk);
  if (isKey) {
    P.addKey(std::string((const char *)d, len));
    return true;
  }
  err = "Datei nicht erkannt (Zertifikat, Schlüssel, .p7b oder .p12/.pfx erwartet)";
  return false;
}

static bool parseInput(const uint8_t *d, size_t n, const String &pw, Parsed &P, String &err) {
  if (!n) {
    err = "Datei ist leer";
    return false;
  }
  static const char MARK[] = "-----BEGIN ";
  bool isPem = false;
  for (size_t i = 0; i + sizeof(MARK) - 1 <= n && !isPem; i++) isPem = !memcmp(d + i, MARK, sizeof(MARK) - 1);
  if (isPem) return parsePem((const char *)d, n, pw, P, err);
  if (d[0] == 0x30) return parseDer(d, n, pw, P, err);
  // plain Base64 without PEM lines (some Windows ".cer" files)
  bool b64 = true;
  for (size_t i = 0; i < n && b64; i++)
    b64 = isalnum(d[i]) || d[i] == '+' || d[i] == '/' || d[i] == '=' || isspace(d[i]);
  std::string der;
  if (b64 && b64decode((const char *)d, n, der) && !der.empty() && (uint8_t)der[0] == 0x30) {
    bool ok = parseDer((const uint8_t *)der.data(), der.size(), pw, P, err);
    burn(der);
    return ok;
  }
  err = "Unbekanntes Dateiformat (erwartet PEM, DER, .p7b oder .p12/.pfx)";
  return false;
}

// ============================================================================ certificate details
static String pemEncode(const char *label, const uint8_t *der, size_t n) {
  String head = String("-----BEGIN ") + label + "-----\n";
  String foot = String("-----END ") + label + "-----\n";
  size_t cap = head.length() + foot.length() + (n + 2) / 3 * 4 + n / 48 + 8;
  std::string buf(cap, '\0');
  size_t olen = 0;
  if (mbedtls_pem_write_buffer(head.c_str(), foot.c_str(), der, n, (uint8_t *)&buf[0], cap, &olen)) return String();
  String out(buf.c_str());
  burn(buf);
  return out;
}

static String keyPemFromDer(const std::string &der) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  String out;
  if (!rsPkParseKey(&pk, (const uint8_t *)der.data(), der.size(), nullptr, 0)) {
    const size_t cap = 6200;                   // RSA 4096 fits
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (buf) {
      int n = mbedtls_pk_write_key_der(&pk, buf, cap);   // written at the END of buf
      if (n > 0)
        out = pemEncode(mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA ? "RSA PRIVATE KEY" : "EC PRIVATE KEY",
                        buf + cap - n, n);
      mbedtls_platform_zeroize(buf, cap);
      free(buf);
    }
  }
  mbedtls_pk_free(&pk);
  return out;
}

static String keyPemFromPk(mbedtls_pk_context &pk) {
  const size_t cap = 6200;
  uint8_t *buf = (uint8_t *)malloc(cap);
  String out;
  if (!buf) return out;
  int n = mbedtls_pk_write_key_der(&pk, buf, cap);
  if (n > 0)
    out = pemEncode(mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA ? "RSA PRIVATE KEY" : "EC PRIVATE KEY", buf + cap - n, n);
  mbedtls_platform_zeroize(buf, cap);
  free(buf);
  return out;
}

static String pkDesc(const mbedtls_pk_context *pk) {
  mbedtls_pk_type_t t = mbedtls_pk_get_type(pk);
  if (t == MBEDTLS_PK_RSA) return String("RSA ") + (unsigned)mbedtls_pk_get_bitlen(pk);
  if (t == MBEDTLS_PK_ECKEY || t == MBEDTLS_PK_ECDSA) {
    mbedtls_ecp_group_id id = rsEcGroupId(pk);
    if (id == MBEDTLS_ECP_DP_SECP256R1) return "EC P-256";
    if (id == MBEDTLS_ECP_DP_SECP384R1) return "EC P-384";
    if (id == MBEDTLS_ECP_DP_SECP521R1) return "EC P-521";
    const mbedtls_ecp_curve_info *ci = mbedtls_ecp_curve_info_from_grp_id(id);
    return String("EC ") + (ci ? ci->name : "?");
  }
  return "unbekannt";
}

static std::vector<String> sanList(const mbedtls_x509_crt *c) {
  std::vector<String> out;
  Tlv cert, tbs, x;
  if (!berRead(c->raw.p, c->raw.p + c->raw.len, cert)) return out;
  BerIter ci(cert);
  if (!ci.next(tbs)) return out;
  BerIter ti(tbs);
  while (ti.next(x)) {
    if (x.tag != 0xA3) continue;
    BerIter xi(x);
    Tlv exts, ext;
    if (!xi.next(exts)) break;
    BerIter ei(exts);
    while (ei.next(ext)) {
      BerIter f(ext);
      Tlv oid, v;
      if (!f.next(oid) || !oidIs(oid, OID_SAN)) continue;
      while (f.next(v) && v.tag != 0x04) {}
      if (v.tag != 0x04) break;
      Tlv names, gn;
      if (!berRead(v.v, v.v + v.len, names)) break;
      BerIter ni(names);
      while (ni.next(gn)) {
        String s;
        if (gn.tag == 0x82) {
          s = String(std::string((const char *)gn.v, gn.len).c_str());
        } else if (gn.tag == 0x87 && gn.len == 4) {
          s = String(gn.v[0]) + "." + gn.v[1] + "." + gn.v[2] + "." + gn.v[3];
        } else if (gn.tag == 0x87 && gn.len == 16) {
          char b[48];
          int o = 0;
          for (int i = 0; i < 16; i += 2) o += snprintf(b + o, sizeof(b) - o, "%s%x", i ? ":" : "", (gn.v[i] << 8) | gn.v[i + 1]);
          s = b;
        } else if (gn.tag == 0x81) {
          s = "E-Mail:" + String(std::string((const char *)gn.v, gn.len).c_str());
        } else if (gn.tag == 0x86) {
          s = "URI:" + String(std::string((const char *)gn.v, gn.len).c_str());
        } else if (gn.tag == 0xA0) {             // otherName { type-id, [0] value }
          BerIter oi(gn);
          Tlv tid, val, str;
          if (oi.next(tid) && oidIs(tid, OID_UPN) && oi.next(val)) {
            BerIter vi(val);
            if (vi.next(str)) s = "UPN:" + String(std::string((const char *)str.v, str.len).c_str());
          }
        }
        if (s.length()) out.push_back(s);
      }
    }
  }
  return out;
}

static bool ekuHas(const mbedtls_x509_crt *c, const char *oid, size_t len) {
  return mbedtls_x509_crt_check_extended_key_usage(c, oid, len) == 0;
}

static void crtJson(JsonObject o, const mbedtls_x509_crt *c, bool full) {
  char buf[256];
  mbedtls_x509_dn_gets(buf, sizeof(buf), &c->subject);
  o["subject"] = buf;
  mbedtls_x509_dn_gets(buf, sizeof(buf), &c->issuer);
  o["issuer"] = buf;
  o["from"] = fmtDate(c->valid_from);
  o["to"] = fmtDate(c->valid_to);
  if (timeKnown()) {
    time_t now = time(nullptr);
    o["expired"] = toEpoch(c->valid_to) < now;
    o["notYet"] = toEpoch(c->valid_from) > now + 86400;
  }
  o["ca"] = rsCrtIsCa(c);
  o["self"] = c->issuer_raw.len == c->subject_raw.len && !memcmp(c->issuer_raw.p, c->subject_raw.p, c->issuer_raw.len);
  if (!full) return;
  o["key"] = pkDesc(&c->pk);
  JsonArray eku = o["eku"].to<JsonArray>();
  if (rsCrtHasExt(c, MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE)) {
    for (const mbedtls_x509_sequence *s = &c->ext_key_usage; s && s->buf.p; s = s->next) {
      if (!MBEDTLS_OID_CMP(MBEDTLS_OID_SERVER_AUTH, &s->buf)) eku.add("serverAuth");
      else if (!MBEDTLS_OID_CMP(MBEDTLS_OID_CLIENT_AUTH, &s->buf)) eku.add("clientAuth");
      else if (!MBEDTLS_OID_CMP(MBEDTLS_OID_ANY_EXTENDED_KEY_USAGE, &s->buf)) eku.add("any");
      else eku.add("andere");
    }
  }
  JsonArray san = o["san"].to<JsonArray>();
  for (auto &s : sanList(c)) san.add(s);
  uint8_t h[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), c->raw.p, c->raw.len, h);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02X", h[i]);
  o["sha256"] = hex;
}

static bool parseChain(const String &pem, mbedtls_x509_crt &chain) {
  mbedtls_x509_crt_init(&chain);
  if (pem.isEmpty()) return false;
  return mbedtls_x509_crt_parse(&chain, (const uint8_t *)pem.c_str(), pem.length() + 1) == 0;
}

static bool parseKey(const String &pem, mbedtls_pk_context &pk) {
  mbedtls_pk_init(&pk);
  if (pem.isEmpty()) return false;
  return rsPkParseKey(&pk, (const uint8_t *)pem.c_str(), pem.length() + 1, nullptr, 0) == 0;
}

static bool pemPairMatches(const String &crtPem, const String &keyPem) {
  mbedtls_x509_crt c;
  mbedtls_pk_context k;
  bool ok = parseChain(crtPem, c) & parseKey(keyPem, k);
  ok = ok && rsPkCheckPair(&c.pk, &k) == 0;
  mbedtls_x509_crt_free(&c);
  mbedtls_pk_free(&k);
  return ok;
}

// ============================================================================ state
void begin() {
  fsOk = LittleFS.begin(true);
  if (fsOk) LittleFS.mkdir(DIR_TLS);
  bool have[SLOTS], match[SLOTS];
  {
    std::lock_guard<std::mutex> l(mtx);
    for (uint8_t s = 0; s < SLOTS; s++) {
      slot[s].crt = readFile(CRT_FILE[s]);
      slot[s].key = KEY_FILE[s] ? readFile(KEY_FILE[s]) : String();
      slot[s].match = s != CA && slot[s].crt.length() && slot[s].key.length() && pemPairMatches(slot[s].crt, slot[s].key);
      have[s] = slot[s].crt.length() > 0;
      match[s] = slot[s].match;
    }
    csrKey = readFile(CSR_KEY);
    csrReq = readFile(CSR_REQ);
    caCrt = readFile(DEVCA_CRT);
    caKey = readFile(DEVCA_KEY);
    autoHttps = fileExists(AUTO_FLAG);
  }
  static const char *N[SLOTS] = {"CA", "Client", "HTTPS"};
  for (uint8_t s = 0; s < SLOTS; s++)            // summary() takes the lock itself
    if (have[s])
      Serial.printf("[TLS] %-6s: %s%s\n", N[s], summary((Slot)s).c_str(),
                    s == CA ? "" : (match[s] ? " (mit Schlüssel)" : " (Schlüssel fehlt/passt nicht)"));
}

void wipe() {
  fsOk = LittleFS.begin(true);
  if (fsOk) {
    File dir = LittleFS.open(DIR_TLS);
    std::vector<String> names;
    if (dir) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        names.push_back(String(DIR_TLS) + "/" + f.name());
        f.close();
      }
      dir.close();
    }
    for (auto &n : names) LittleFS.remove(n);
  }
  std::lock_guard<std::mutex> l(mtx);
  for (auto &s : slot) {
    wipeString(s.key);
    s.crt = String();
    s.match = false;
  }
  wipeString(csrKey);
  wipeString(caKey);
  csrReq = caCrt = String();
  autoHttps = false;
  gen++;
}

// "U" = caller already holds mtx
static bool hasCertU(Slot s) { return s < SLOTS && slot[s].crt.length(); }
static bool readyU(Slot s) { return hasCertU(s) && (s == CA || slot[s].match); }

bool hasCert(Slot s) {
  std::lock_guard<std::mutex> l(mtx);
  return hasCertU(s);
}
bool hasKey(Slot s) {
  std::lock_guard<std::mutex> l(mtx);
  return s < SLOTS && slot[s].key.length();
}
bool ready(Slot s) {
  std::lock_guard<std::mutex> l(mtx);
  return readyU(s);
}
bool httpsIsAuto() {
  std::lock_guard<std::mutex> l(mtx);
  return autoHttps && hasCertU(HTTPS);
}
uint32_t generation() { return gen.load(); }
String csrPem() {
  std::lock_guard<std::mutex> l(mtx);
  return csrReq;
}
String deviceCaPem() {
  std::lock_guard<std::mutex> l(mtx);
  return caCrt;
}

void copy(Slot s, String &c, String &k) {
  std::lock_guard<std::mutex> l(mtx);
  c = s < SLOTS ? slot[s].crt : String();
  k = s < SLOTS ? slot[s].key : String();
}

// always work on a copy: parsing takes long and the web handler may replace the PEM meanwhile
static String certCopy(Slot s) {
  std::lock_guard<std::mutex> l(mtx);
  return s < SLOTS ? slot[s].crt : String();
}

bool serverUsable(Slot s) {
  String pem = certCopy(s);
  if (pem.isEmpty() || !ready(s)) return false;
  mbedtls_x509_crt c;
  bool ok = parseChain(pem, c) &&
            ekuHas(&c, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) &&
            mbedtls_x509_crt_check_key_usage(&c, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0;
  mbedtls_x509_crt_free(&c);
  return ok;
}

static String cnOf(const mbedtls_x509_crt *c) {
  for (const mbedtls_x509_name *n = &c->subject; n; n = n->next)
    if (n->oid.p && !MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid))
      return String(std::string((const char *)n->val.p, n->val.len).c_str());
  return String();
}

String subjectCN(Slot s) {
  String pem = certCopy(s);
  if (pem.isEmpty()) return String();
  mbedtls_x509_crt c;
  String cn = parseChain(pem, c) ? cnOf(&c) : String();
  mbedtls_x509_crt_free(&c);
  return cn;
}

String summary(Slot s) {
  String pem = certCopy(s);
  if (pem.isEmpty()) return String();
  mbedtls_x509_crt c;
  String out;
  if (parseChain(pem, c)) {
    String cn = cnOf(&c);
    if (cn.isEmpty()) {
      char buf[128];
      mbedtls_x509_dn_gets(buf, sizeof(buf), &c.subject);
      cn = buf;
    }
    out = cn + " · bis " + fmtDate(c.valid_to);
  }
  mbedtls_x509_crt_free(&c);
  return out;
}

bool remove(Slot s) {
  if (s >= SLOTS) return false;
  {
    std::lock_guard<std::mutex> l(mtx);
    removeFile(CRT_FILE[s]);
    if (KEY_FILE[s]) removeFile(KEY_FILE[s]);
    slot[s].crt = String();
    wipeString(slot[s].key);
    slot[s].match = false;
    if (s == HTTPS) {
      removeFile(AUTO_FLAG);
      autoHttps = false;
    }
  }
  gen++;
  return true;
}

// ============================================================================ upload
static bool crtFromDer(const std::string &der, mbedtls_x509_crt &c) {
  mbedtls_x509_crt_init(&c);
  return mbedtls_x509_crt_parse_der(&c, (const uint8_t *)der.data(), der.size()) == 0;
}

static String derChainToPem(const std::vector<std::string> &ders) {
  String out;
  for (auto &d : ders) out += pemEncode("CERTIFICATE", (const uint8_t *)d.data(), d.size());
  return out;
}

bool upload(Slot s, const uint8_t *data, size_t len, const String &password, String &msg) {
  if (s >= SLOTS) { msg = "Unbekannter Speicherplatz"; return false; }
  if (!fsOk) { msg = "Dateisystem nicht verfügbar"; return false; }
  if (len > MAX_UPLOAD) { msg = "Datei zu groß (max. 24 kB)"; return false; }
  Parsed P;
  String err;
  bool parsed = false;
  try {
    parsed = parseInput(data, len, password, P, err);
  } catch (const std::bad_alloc &) {           // crafted file with huge nesting
    msg = "Zu wenig Speicher für diese Datei";
    return false;
  }
  if (!parsed) { msg = err; return false; }
  if (P.certs.empty() && P.keys.empty()) {
    msg = P.csr ? "Das ist ein Zertifikatsantrag (CSR) – hochgeladen wird das von der CA ausgestellte Zertifikat"
                : "Keine Zertifikate oder Schlüssel in der Datei";
    return false;
  }
  // every certificate must be readable
  for (auto &d : P.certs) {
    mbedtls_x509_crt c;
    bool ok = crtFromDer(d, c);
    mbedtls_x509_crt_free(&c);
    if (!ok) { msg = "Zertifikat in der Datei nicht lesbar"; return false; }
  }

  if (s == CA) {
    if (P.certs.empty()) { msg = "Keine Zertifikate in der Datei (für „CA“ wird das Zertifikat der ausstellenden CA gebraucht)"; return false; }
    std::vector<std::string> cas, all;
    for (auto &d : P.certs) {
      mbedtls_x509_crt c;
      crtFromDer(d, c);
      bool isCa = rsCrtIsCa(&c) || (c.issuer_raw.len == c.subject_raw.len && !memcmp(c.issuer_raw.p, c.subject_raw.p, c.issuer_raw.len));
      mbedtls_x509_crt_free(&c);
      (isCa ? cas : all).push_back(d);
    }
    bool onlyLeaf = cas.empty();
    const std::vector<std::string> &use = onlyLeaf ? all : cas;
    String pem = derChainToPem(use);
    {
      std::lock_guard<std::mutex> l(mtx);
      if (!writeFile(CRT_FILE[CA], pem)) { msg = "Schreibfehler"; return false; }
      slot[CA].crt = pem;
    }
    gen++;
    msg = String(use.size()) + " Zertifikat(e) gespeichert: " + summary(CA);
    if (!P.keys.empty()) msg += " (privater Schlüssel ignoriert)";
    if (onlyLeaf) msg += ". Achtung: kein CA-Zertifikat – der RADIUS-Server muss genau dieses Zertifikat verwenden";
    return true;
  }

  // ---- client / HTTPS: certificate + private key
  std::string keyDer;
  String keySource;
  if (!P.keys.empty()) {
    if (!keyDerOk(P.keys[0], err)) { msg = err; return false; }
    keyDer = P.keys[0];
    keySource = "aus der Datei";
  }
  String curKeyPem, curCrtPem;
  Burner burnCur{curKeyPem};                   // key material never stays in freed heap
  {
    std::lock_guard<std::mutex> l(mtx);
    curKeyPem = slot[s].key;
    curCrtPem = slot[s].crt;
  }
  String keyPem;                               // PEM of the key that goes with the result
  Burner burnNew{keyPem};
  int leaf = -1;

  auto matchKey = [&](mbedtls_pk_context &pk) -> int {
    for (size_t i = 0; i < P.certs.size(); i++) {
      mbedtls_x509_crt c;
      bool m = crtFromDer(P.certs[i], c) && rsPkCheckPair(&c.pk, &pk) == 0;
      mbedtls_x509_crt_free(&c);
      if (m) return (int)i;
    }
    return -1;
  };

  if (!keyDer.empty()) {
    keyPem = keyPemFromDer(keyDer);
    if (keyPem.isEmpty()) { msg = "Schlüssel konnte nicht umgewandelt werden"; return false; }
    if (!P.certs.empty()) {
      mbedtls_pk_context pk;
      parseKey(keyPem, pk);
      leaf = matchKey(pk);
      mbedtls_pk_free(&pk);
      if (leaf < 0) { msg = "Der Schlüssel passt zu keinem Zertifikat in der Datei"; wipeString(keyPem); return false; }
    } else if (curCrtPem.length()) {
      if (!pemPairMatches(curCrtPem, keyPem)) { msg = "Schlüssel passt nicht zum gespeicherten Zertifikat"; wipeString(keyPem); return false; }
    }
  } else {
    // certificate(s) only: take the stored key or the key of the CSR generated on the device
    const String *cands[2] = {&curKeyPem, &csrKey};
    const char *src[2] = {"bereits gespeichert", "aus dem Antrag (CSR)"};
    for (int k = 0; k < 2 && leaf < 0; k++) {
      if (cands[k]->isEmpty()) continue;
      mbedtls_pk_context pk;
      if (parseKey(*cands[k], pk)) {
        leaf = matchKey(pk);
        if (leaf >= 0) {
          keyPem = *cands[k];
          keySource = src[k];
        }
      }
      mbedtls_pk_free(&pk);
    }
    if (leaf < 0) {                            // no key yet: first certificate that is not a CA
      for (size_t i = 0; i < P.certs.size() && leaf < 0; i++) {
        mbedtls_x509_crt c;
        if (crtFromDer(P.certs[i], c) && !rsCrtIsCa(&c)) leaf = (int)i;
        mbedtls_x509_crt_free(&c);
      }
      if (leaf < 0) leaf = 0;
    }
  }

  String crtPem = curCrtPem;
  if (!P.certs.empty()) {
    std::vector<std::string> chain;
    chain.push_back(P.certs[leaf]);
    for (size_t i = 0; i < P.certs.size(); i++)
      if ((int)i != leaf && P.certs[i] != P.certs[leaf]) chain.push_back(P.certs[i]);
    crtPem = derChainToPem(chain);
  }
  bool match = crtPem.length() && keyPem.length();
  {
    std::lock_guard<std::mutex> l(mtx);
    bool ok = true;
    if (!P.certs.empty()) ok = writeFile(CRT_FILE[s], crtPem);
    if (keyPem.length()) ok = ok && writeFile(KEY_FILE[s], keyPem);
    else if (!P.certs.empty()) removeFile(KEY_FILE[s]);     // old key belongs to another certificate
    if (!ok) { msg = "Schreibfehler"; return false; }
    slot[s].crt = crtPem;
    if (keyPem.length()) slot[s].key = keyPem;
    else if (!P.certs.empty()) wipeString(slot[s].key);
    slot[s].match = match;
    if (s == HTTPS) {
      removeFile(AUTO_FLAG);
      autoHttps = false;
    }
  }
  gen++;

  if (P.certs.empty()) {
    msg = crtPem.length() ? "Schlüssel gespeichert (passt zum Zertifikat)" : "Schlüssel gespeichert – Zertifikat fehlt noch";
    return true;
  }
  msg = "Gespeichert: " + summary(s);
  if (P.certs.size() > 1) msg += String(" + ") + (P.certs.size() - 1) + " Zwischen-/CA-Zertifikat(e)";
  if (match) msg += String(", Schlüssel ") + keySource;
  else msg += ". Passender privater Schlüssel fehlt noch";
  mbedtls_x509_crt c;
  if (parseChain(crtPem, c)) {
    if (s == CLIENT && !ekuHas(&c, MBEDTLS_OID_CLIENT_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)))
      msg += ". Warnung: Zertifikat ist nicht für Client-Authentifizierung (EKU)";
    if (s == HTTPS && !ekuHas(&c, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)))
      msg += ". Warnung: Zertifikat ist nicht für Server-Authentifizierung (EKU) – Browser lehnen es ab";
    if (s == HTTPS && sanList(&c).empty())
      msg += ". Warnung: keine alternativen Namen (SAN) – Chrome/iOS lehnen es ab";
  }
  mbedtls_x509_crt_free(&c);
  return true;
}

// ============================================================================ info
String infoJson() {
  JsonDocument d;
  static const char *ID[SLOTS] = {"ca", "client", "https"};
  JsonArray arr = d["slots"].to<JsonArray>();
  String crtOf[SLOTS], keyOf[SLOTS], csrCopy, caCopy;
  bool matchOf[SLOTS], autoFlag;
  {
    std::lock_guard<std::mutex> l(mtx);
    for (uint8_t s = 0; s < SLOTS; s++) {
      crtOf[s] = slot[s].crt;
      keyOf[s] = slot[s].key;
      matchOf[s] = slot[s].match;
    }
    csrCopy = csrReq;
    caCopy = caCrt;
    autoFlag = autoHttps;
  }
  {
    for (uint8_t s = 0; s < SLOTS; s++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = ID[s];
      o["cert"] = crtOf[s].length() > 0;
      o["key"] = keyOf[s].length() > 0;
      o["match"] = matchOf[s];
      o["ready"] = s == CA ? crtOf[s].length() > 0 : matchOf[s];
      if (s == HTTPS) o["auto"] = autoFlag && crtOf[s].length();
      if (keyOf[s].length()) {
        mbedtls_pk_context k;
        if (parseKey(keyOf[s], k)) o["keyType"] = pkDesc(&k);
        mbedtls_pk_free(&k);
      }
      mbedtls_x509_crt chain;
      if (parseChain(crtOf[s], chain)) {
        JsonArray cs = o["certs"].to<JsonArray>();
        int i = 0;
        for (const mbedtls_x509_crt *c = &chain; c && c->raw.p && i < 8; c = c->next, i++)
          crtJson(cs.add<JsonObject>(), c, s == CA || i == 0);
      }
      mbedtls_x509_crt_free(&chain);
    }
    JsonObject csr = d["csr"].to<JsonObject>();
    csr["busy"] = busy();
    csr["have"] = csrCopy.length() > 0;
    if (csrCopy.length()) {
      mbedtls_x509_csr r;
      mbedtls_x509_csr_init(&r);
      if (!mbedtls_x509_csr_parse(&r, (const uint8_t *)csrCopy.c_str(), csrCopy.length() + 1)) {
        char buf[256];
        mbedtls_x509_dn_gets(buf, sizeof(buf), &r.subject);
        csr["subject"] = buf;
        csr["key"] = pkDesc(&r.pk);
      }
      mbedtls_x509_csr_free(&r);
    }
    JsonObject ca = d["devca"].to<JsonObject>();
    mbedtls_x509_crt c;
    if (parseChain(caCopy, c)) crtJson(ca, &c, true);
    mbedtls_x509_crt_free(&c);
  }
  d["time"] = timeKnown() ? (uint32_t)time(nullptr) : 0;
  String out;
  serializeJson(d, out);
  return out;
}

// ============================================================================ key generation (background task)
enum JobKind : uint8_t { JOB_CSR = 1, JOB_AUTO = 2 };
static std::atomic<int> jobState{0};           // 0 idle, 1 running, 2 finished
static struct {
  JobKind kind;
  // input
  bool rsa;
  String subject;                              // CSR / leaf subject
  std::string san;                             // DER GeneralNames
  String caSubject, caKeyIn;                   // AUTO: signing CA (key empty -> create)
  int64_t notBefore;
  // output
  bool ok;
  String err, keyPem, reqPem, caKeyOut, caCrtOut;
} job;
static bool autoPending = false;
static String pendHost, pendCa, pendIp;

bool busy() { return jobState.load() != 0; }

static void derLen(std::string &o, size_t n) {
  if (n < 128) { o += (char)n; return; }
  if (n < 256) { o += (char)0x81; o += (char)n; return; }
  o += (char)0x82; o += (char)(n >> 8); o += (char)n;
}
static std::string derTlv(uint8_t tag, const std::string &content) {
  std::string o(1, (char)tag);
  derLen(o, content.size());
  return o + content;
}

static bool parseIp4(const String &s, uint8_t ip[4]) {
  int a, b, c, d;
  char tail;
  if (sscanf(s.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
  return true;
}

// "rs232, rs232.local 192.168.4.1" -> DER GeneralNames; err on invalid entries
static bool buildSan(const String &list, std::string &der, String &err) {
  std::string names;
  String cur;
  auto flush = [&]() -> bool {
    cur.trim();
    if (cur.isEmpty()) return true;
    uint8_t ip[4];
    if (parseIp4(cur, ip)) {
      names += derTlv(0x87, std::string((const char *)ip, 4));
    } else {
      for (size_t i = 0; i < cur.length(); i++) {
        char c = cur[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == '*' || c == '_')) {
          err = "Ungültiger Name: " + cur;
          return false;
        }
      }
      names += derTlv(0x82, std::string(cur.c_str()));
    }
    cur = String();
    return true;
  };
  for (size_t i = 0; i < list.length(); i++) {
    char c = list[i];
    if (c == ',' || c == ';' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!flush()) return false;
    } else {
      cur += c;
    }
  }
  if (!flush()) return false;
  der = names.empty() ? std::string() : derTlv(0x30, names);
  return true;
}

static bool genKey(mbedtls_pk_context &pk, bool rsa) {
  mbedtls_pk_init(&pk);
  if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(rsa ? MBEDTLS_PK_RSA : MBEDTLS_PK_ECKEY))) return false;
  if (rsa) return mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), rng, nullptr, 2048, 65537) == 0;
  return mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, nullptr) == 0;
}

static const unsigned char EKU_BOTH[] = {0x30, 0x14, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01,
                                         0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02};
static const unsigned char EKU_SERVER[] = {0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};

static String derToPemOwned(uint8_t *buf, size_t cap, int n, const char *label) {
  String out = n > 0 ? pemEncode(label, buf + cap - n, n) : String();
  return out;
}

static void runCsr() {
  mbedtls_pk_context pk;
  if (!genKey(pk, job.rsa)) {
    mbedtls_pk_free(&pk);
    job.err = "Schlüsselerzeugung fehlgeschlagen";
    return;
  }
  mbedtls_x509write_csr req;
  mbedtls_x509write_csr_init(&req);
  mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
  mbedtls_x509write_csr_set_key(&req, &pk);
  int r = mbedtls_x509write_csr_set_subject_name(&req, job.subject.c_str());
  mbedtls_x509write_csr_set_key_usage(&req, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | (job.rsa ? MBEDTLS_X509_KU_KEY_ENCIPHERMENT : 0));
  if (!job.san.empty())
    r = r ? r : rsCsrSetExtension(&req, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
                                                    (const uint8_t *)job.san.data(), job.san.size());
  r = r ? r : rsCsrSetExtension(&req, MBEDTLS_OID_EXTENDED_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE),
                                                  EKU_BOTH, sizeof(EKU_BOTH));
  const size_t cap = 4096;
  uint8_t *buf = (uint8_t *)malloc(cap);
  int n = (!r && buf) ? mbedtls_x509write_csr_der(&req, buf, cap, rng, nullptr) : -1;
  if (n > 0) {
    job.reqPem = derToPemOwned(buf, cap, n, "CERTIFICATE REQUEST");
    job.keyPem = keyPemFromPk(pk);
    job.ok = job.reqPem.length() && job.keyPem.length();
  }
  if (!job.ok) job.err = r ? "Ungültiger Name (Beispiel: CN=rs232-01,O=Firma,C=DE)" : "CSR konnte nicht erzeugt werden";
  free(buf);
  mbedtls_x509write_csr_free(&req);
  mbedtls_pk_free(&pk);
}

static bool writeCrt(mbedtls_pk_context &subjectKey, mbedtls_pk_context &issuerKey, const String &subject,
                     const String &issuer, int64_t from, int64_t to, bool ca, const std::string &san, String &pemOut) {
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  mbedtls_mpi serial;
  mbedtls_mpi_init(&serial);
  char nb[24], na[24];
  x509Time(from, nb);
  x509Time(to, na);
  int r = mbedtls_mpi_fill_random(&serial, 15, rng, nullptr);
  if (!r && mbedtls_mpi_get_bit(&serial, 0) == 0) r = mbedtls_mpi_set_bit(&serial, 0, 1);   // never zero
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &subjectKey);
  mbedtls_x509write_crt_set_issuer_key(&crt, &issuerKey);
  r = r ? r : rsCrtSetSerial(&crt, &serial);
  r = r ? r : mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str());
  r = r ? r : mbedtls_x509write_crt_set_issuer_name(&crt, issuer.c_str());
  r = r ? r : mbedtls_x509write_crt_set_validity(&crt, nb, na);
  r = r ? r : mbedtls_x509write_crt_set_basic_constraints(&crt, ca ? 1 : 0, ca ? 0 : -1);
  r = r ? r : mbedtls_x509write_crt_set_subject_key_identifier(&crt);
  r = r ? r : mbedtls_x509write_crt_set_authority_key_identifier(&crt);
  r = r ? r : mbedtls_x509write_crt_set_key_usage(&crt, ca ? (MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN)
                                                           : MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
  if (!ca) {
    r = r ? r : mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_EXTENDED_KEY_USAGE,
                                                    MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, EKU_SERVER,
                                                    sizeof(EKU_SERVER));
    if (!san.empty())
      r = r ? r : mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                      MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0,
                                                      (const uint8_t *)san.data(), san.size());
  }
  const size_t cap = 4096;
  uint8_t *buf = (uint8_t *)malloc(cap);
  int n = (!r && buf) ? mbedtls_x509write_crt_der(&crt, buf, cap, rng, nullptr) : -1;
  pemOut = derToPemOwned(buf, cap, n, "CERTIFICATE");
  free(buf);
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  return pemOut.length() > 0;
}

static void runAuto() {
  mbedtls_pk_context caPk, leafPk;
  mbedtls_pk_init(&caPk);
  mbedtls_pk_init(&leafPk);
  bool ok = true;
  if (job.caKeyIn.isEmpty()) {                 // first HTTPS use: create the device CA (valid ~25 years)
    ok = genKey(caPk, false) &&
         writeCrt(caPk, caPk, job.caSubject, job.caSubject, daysFromCivil(2024, 1, 1) * 86400,
                  daysFromCivil(2049, 12, 31) * 86400, true, std::string(), job.caCrtOut);
    if (ok) job.caKeyOut = keyPemFromPk(caPk);
    ok = ok && job.caKeyOut.length();
  } else {
    mbedtls_pk_free(&caPk);
    ok = parseKey(job.caKeyIn, caPk);
  }
  String leafCrt;
  // Apple: server certificates from private CAs may be valid for at most 825 days
  ok = ok && genKey(leafPk, false) &&
       writeCrt(leafPk, caPk, job.subject, job.caSubject, job.notBefore, job.notBefore + 800LL * 86400, false, job.san,
                leafCrt);
  if (ok) {
    job.keyPem = keyPemFromPk(leafPk);
    job.reqPem = leafCrt;                      // reused: leaf certificate
    job.ok = job.keyPem.length() > 0;
  }
  if (!job.ok) job.err = "HTTPS-Zertifikat konnte nicht erzeugt werden";
  mbedtls_pk_free(&caPk);
  mbedtls_pk_free(&leafPk);
}

static void jobTask(void *) {
  // RSA key generation runs for seconds without yielding: the idle watchdog of this
  // core would reboot the device, so it is switched off while the job runs.
  disableCore0WDT();
  if (job.kind == JOB_CSR) runCsr();
  else runAuto();
  enableCore0WDT();
  jobState.store(2);
  vTaskDelete(nullptr);
}

static bool startJob() {
  job.ok = false;
  job.err = job.keyPem = job.reqPem = job.caKeyOut = job.caCrtOut = String();
  jobState.store(1);
  // RSA key generation takes seconds: own task on core 0, next to WiFi, below the serial bridge
  if (xTaskCreatePinnedToCore(jobTask, "certgen", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    jobState.store(0);
    return false;
  }
  return true;
}

bool csrStart(const String &subjectIn, const String &sans, bool rsa, String &err) {
  if (busy()) { err = "Schlüsselerzeugung läuft bereits"; return false; }
  String subject = subjectIn;
  subject.trim();
  if (subject.isEmpty()) { err = "Name (CN) fehlt"; return false; }
  if (subject.indexOf('=') < 0) subject = "CN=" + subject;
  mbedtls_x509write_csr probe;
  mbedtls_x509write_csr_init(&probe);
  int r = mbedtls_x509write_csr_set_subject_name(&probe, subject.c_str());
  mbedtls_x509write_csr_free(&probe);
  if (r) { err = "Ungültiger Name (Beispiel: CN=rs232-01,O=Firma,C=DE)"; return false; }
  std::string san;
  if (!buildSan(sans, san, err)) return false;
  job.kind = JOB_CSR;
  job.rsa = rsa;
  job.subject = subject;
  job.san = san;
  if (!startJob()) { err = "Zu wenig Speicher"; return false; }
  Serial.printf("[TLS] erzeuge %s-Schlüssel + CSR für %s ...\n", rsa ? "RSA-2048" : "EC-P-256", subject.c_str());
  return true;
}

static String sanitizeName(const String &s) {
  String o;
  for (size_t i = 0; i < s.length() && o.length() < 40; i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == ' ') o += c;
  }
  o.trim();
  return o.length() ? o : String("RS232");
}

void ensureAutoHttps(const String &host, const String &caName, const String &staIp) {
  if (busy()) {                                // try again when the running job is finished
    autoPending = true;
    pendHost = host;
    pendCa = caName;
    pendIp = staIp;
    return;
  }
  std::vector<String> want = {host, host + ".local", "192.168.4.1"};
  if (staIp.length()) want.push_back(staIp);
  String crtPem, keyPem, caKeyPem, caCrtPem;
  bool isAuto;
  {
    std::lock_guard<std::mutex> l(mtx);
    if (slot[HTTPS].crt.length() && !autoHttps) return;      // uploaded certificate: never touched
    crtPem = slot[HTTPS].crt;
    isAuto = autoHttps && slot[HTTPS].match;
    caKeyPem = caKey;
    caCrtPem = caCrt;
  }
  if (isAuto && caKeyPem.length()) {
    mbedtls_x509_crt c;
    bool fine = parseChain(crtPem, c);
    if (fine) {
      std::vector<String> have = sanList(&c);
      for (auto &w : want) {
        bool found = false;
        for (auto &h : have) found = found || h.equalsIgnoreCase(w);
        fine = fine && found;
      }
      // the same base as when issuing: a browser clock behind the firmware build
      // date must not trigger a new certificate on every connect
      int64_t base = timeKnown() ? (int64_t)time(nullptr) : 0;
      if (base < buildEpoch()) base = buildEpoch();
      fine = fine && toEpoch(c.valid_to) > base + 30 * 86400 && toEpoch(c.valid_from) <= base;
    }
    mbedtls_x509_crt_free(&c);
    if (fine) return;
  }
  String list;
  for (auto &w : want) list += w + ",";
  std::string san;
  String err;
  if (!buildSan(list, san, err)) return;
  String hostCn = sanitizeName(host);
  job.kind = JOB_AUTO;
  job.subject = "CN=" + hostCn + ",O=RS232 Web Console";
  job.san = san;
  job.caKeyIn = caKeyPem;
  if (caKeyPem.length()) {                     // issuer must match the existing CA exactly
    mbedtls_x509_crt c;
    String cn = parseChain(caCrtPem, c) ? cnOf(&c) : String();
    mbedtls_x509_crt_free(&c);
    job.caSubject = "CN=" + cn + ",O=RS232 Web Console";
  } else {
    job.caSubject = "CN=" + sanitizeName(caName) + " CA,O=RS232 Web Console";
  }
  int64_t now = timeKnown() ? (int64_t)time(nullptr) : 0;
  int64_t built = buildEpoch();
  job.notBefore = (now > built ? now : built) - 2 * 86400;
  if (startJob()) Serial.printf("[TLS] erzeuge HTTPS-Zertifikat (%s) ...\n", list.c_str());
}

void loop() {
  if (jobState.load() != 2) return;
  if (job.kind == JOB_CSR) {
    if (job.ok) {
      std::lock_guard<std::mutex> l(mtx);
      writeFile(CSR_KEY, job.keyPem);
      writeFile(CSR_REQ, job.reqPem);
      wipeString(csrKey);
      csrKey = job.keyPem;
      csrReq = job.reqPem;
      Serial.println("[TLS] CSR fertig");
    } else {
      Serial.printf("[TLS] CSR: %s\n", job.err.c_str());
    }
  } else if (job.ok) {
    std::lock_guard<std::mutex> l(mtx);
    if (job.caKeyOut.length()) {
      writeFile(DEVCA_KEY, job.caKeyOut);
      writeFile(DEVCA_CRT, job.caCrtOut);
      caKey = job.caKeyOut;
      caCrt = job.caCrtOut;
    }
    writeFile(CRT_FILE[HTTPS], job.reqPem);
    writeFile(KEY_FILE[HTTPS], job.keyPem);
    writeFile(AUTO_FLAG, "1");
    slot[HTTPS].crt = job.reqPem;
    wipeString(slot[HTTPS].key);
    slot[HTTPS].key = job.keyPem;
    slot[HTTPS].match = true;
    autoHttps = true;
    Serial.println("[TLS] HTTPS-Zertifikat erzeugt");
  } else {
    Serial.printf("[TLS] %s\n", job.err.c_str());
  }
  wipeString(job.keyPem);
  wipeString(job.caKeyOut);
  wipeString(job.caKeyIn);
  gen++;
  jobState.store(0);
  if (autoPending) {
    autoPending = false;
    ensureAutoHttps(pendHost, pendCa, pendIp);
  }
}

}  // namespace Certs
