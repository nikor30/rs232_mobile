#pragma once
// ============================================================================
//  mbedTLS 2.x (Arduino core 2.x / IDF 4.4) vs 3.x (core 3.x / IDF 5.x)
//
//  Three kinds of break hit this project's certificate code:
//    1. struct fields became private      -> accessors / MBEDTLS_PRIVATE()
//    2. parsing and key checks want RNG   -> extra f_rng/p_rng arguments
//    3. renamed or extended functions     -> pbkdf2_hmac_ext, csr_set_extension
//
//  The wrappers below keep one source tree building for both. They add no
//  behaviour of their own; on mbedTLS 3 the RNG is the ESP32 hardware entropy
//  source, which is what the platform's own TLS stack uses as well.
// ============================================================================
#include <mbedtls/version.h>
#include <mbedtls/pk.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ssl.h>
#include <esp_random.h>

#if MBEDTLS_VERSION_MAJOR >= 3

static inline int rsTlsRng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static inline int rsPkParseKey(mbedtls_pk_context *pk, const unsigned char *key, size_t keylen,
                               const unsigned char *pwd, size_t pwdlen) {
  return mbedtls_pk_parse_key(pk, key, keylen, pwd, pwdlen, rsTlsRng, nullptr);
}

static inline int rsPkCheckPair(const mbedtls_pk_context *pub, const mbedtls_pk_context *prv) {
  return mbedtls_pk_check_pair(pub, prv, rsTlsRng, nullptr);
}

static inline int rsPbkdf2Hmac(mbedtls_md_type_t md, const unsigned char *pw, size_t pwlen,
                               const unsigned char *salt, size_t saltlen,
                               unsigned int iterations, uint32_t keylen, unsigned char *out) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(md, pw, pwlen, salt, saltlen, iterations, keylen, out);
}

static inline int rsCsrSetExtension(mbedtls_x509write_csr *req, const char *oid, size_t oidlen,
                                    const unsigned char *val, size_t vallen) {
  return mbedtls_x509write_csr_set_extension(req, oid, oidlen, 0 /* not critical */, val, vallen);
}

static inline mbedtls_ecp_group_id rsEcGroupId(const mbedtls_pk_context *pk) {
  return mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(*pk));
}

static inline bool rsCrtIsCa(const mbedtls_x509_crt *c) {
  return c->MBEDTLS_PRIVATE(ca_istrue) != 0;
}

static inline bool rsCrtHasExt(const mbedtls_x509_crt *c, int extType) {
  return (c->MBEDTLS_PRIVATE(ext_types) & extType) != 0;
}

// 3.x names TLS versions as one enum instead of major/minor
static inline void rsSslMinTls12(mbedtls_ssl_config *c) {
  mbedtls_ssl_conf_min_tls_version(c, MBEDTLS_SSL_VERSION_TLS1_2);
}

// 3.x dropped the MPI variant: the serial number is handed over as raw bytes
static inline int rsCrtSetSerial(mbedtls_x509write_cert *crt, const mbedtls_mpi *serial) {
  unsigned char buf[24];
  size_t n = mbedtls_mpi_size(serial);
  if (n > sizeof(buf)) return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
  int r = mbedtls_mpi_write_binary(serial, buf, n);
  return r ? r : mbedtls_x509write_crt_set_serial_raw(crt, buf, n);
}

#else   // ------------------------------------------------------ mbedTLS 2.x

static inline int rsPkParseKey(mbedtls_pk_context *pk, const unsigned char *key, size_t keylen,
                               const unsigned char *pwd, size_t pwdlen) {
  return mbedtls_pk_parse_key(pk, key, keylen, pwd, pwdlen);
}

static inline int rsPkCheckPair(const mbedtls_pk_context *pub, const mbedtls_pk_context *prv) {
  return mbedtls_pk_check_pair(pub, prv);
}

static inline int rsPbkdf2Hmac(mbedtls_md_type_t md, const unsigned char *pw, size_t pwlen,
                               const unsigned char *salt, size_t saltlen,
                               unsigned int iterations, uint32_t keylen, unsigned char *out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  int r = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md), 1);
  if (!r) r = mbedtls_pkcs5_pbkdf2_hmac(&ctx, pw, pwlen, salt, saltlen, iterations, keylen, out);
  mbedtls_md_free(&ctx);
  return r;
}

static inline int rsCsrSetExtension(mbedtls_x509write_csr *req, const char *oid, size_t oidlen,
                                    const unsigned char *val, size_t vallen) {
  return mbedtls_x509write_csr_set_extension(req, oid, oidlen, val, vallen);
}

static inline mbedtls_ecp_group_id rsEcGroupId(const mbedtls_pk_context *pk) {
  return mbedtls_pk_ec(*pk)->grp.id;
}

static inline bool rsCrtIsCa(const mbedtls_x509_crt *c) { return c->ca_istrue != 0; }

static inline bool rsCrtHasExt(const mbedtls_x509_crt *c, int extType) {
  return (c->ext_types & extType) != 0;
}

static inline int rsCrtSetSerial(mbedtls_x509write_cert *crt, const mbedtls_mpi *serial) {
  return mbedtls_x509write_crt_set_serial(crt, serial);
}

static inline void rsSslMinTls12(mbedtls_ssl_config *c) {
  mbedtls_ssl_conf_min_version(c, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
}

#endif
