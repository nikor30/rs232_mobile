#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

// Device side of the channel to the command-and-control server: hybrid key
// exchange (X25519 + ML-KEM-768) and the record layer. The protocol is defined
// by server/internal/proto/proto.go (server/KONZEPT.md §4); this file has to
// produce and accept exactly the same bytes.
//
// Nothing here knows about Arduino, WiFi or HTTP - it needs mbedTLS 2.28,
// mlkem-native and ArduinoJson, and runs on a PC as well (tests/c2/).

// Cryptographic randomness, supplied by the platform (c2.cpp, or the host test).
extern "C" void c2_randombytes(uint8_t *out, size_t len);

namespace C2Proto {

constexpr size_t PUB_SIZE = 32 + 1184;      // static public key: X25519 || ML-KEM-768
constexpr size_t PRIV_SIZE = 32 + 64;       // X25519 scalar || ML-KEM seed (as the server stores its own)
constexpr size_t ID_SIZE = 16;
constexpr size_t TOKEN_ID_SIZE = 8;
constexpr size_t PSK_SIZE = 32;
constexpr size_t REQ_SESSION = 2322;
constexpr size_t REQ_ENROLL = 3546;
constexpr size_t RESP_SIZE = 2257;
constexpr size_t RECORD_OVERHEAD = 8 + 16;  // sequence number + GCM tag
constexpr size_t MAX_PLAINTEXT = 32 * 1024;

enum Kind : uint8_t { KIND_SESSION = 1, KIND_ENROLL = 2 };
enum Result { OK = 0, ERR_FORMAT, ERR_CONFIRM, ERR_CRYPTO };

bool generateKey(uint8_t priv[PRIV_SIZE]);
bool publicKey(const uint8_t priv[PRIV_SIZE], uint8_t pub[PUB_SIZE]);
void fingerprint(const uint8_t pub[PUB_SIZE], uint8_t out[32]);   // first ID_SIZE bytes = device ID

// What the administrator hands over: "c2e1:<base64url of JSON>".
struct Token {
  std::string url;
  uint8_t fingerprint[32];   // of the server's static public key
  uint8_t id[TOKEN_ID_SIZE];
  uint8_t secret[PSK_SIZE];  // never sent, mixed into the key derivation
};
bool parseToken(const char *text, Token &t);
bool tokenPinsServer(const Token &t, const uint8_t serverPub[PUB_SIZE]);

// One end of an established channel.
class Session {
 public:
  uint8_t id[ID_SIZE];
  char sas[7];               // six digits + NUL: the code a person compares when enrolling

  // out needs len + RECORD_OVERHEAD bytes
  bool seal(const uint8_t *plain, size_t len, uint8_t *out, size_t &outLen);
  // out needs len - RECORD_OVERHEAD bytes; false = not from the server, or replayed
  bool open(const uint8_t *record, size_t len, uint8_t *out, size_t &outLen);
  void clear();

 private:
  friend class Handshake;
  uint8_t kSend[32], kRecv[32];
  uint64_t sendSeq = 0, recvSeq = 0;
};

class Handshake {
 public:
  Handshake();
  ~Handshake();              // wipes the secrets
  Handshake(const Handshake &) = delete;
  Handshake &operator=(const Handshake &) = delete;

  // Builds the request (REQ_SESSION or REQ_ENROLL bytes) into req. serverPub
  // must already be trusted. tokenId and psk are used for KIND_ENROLL only.
  Result begin(Kind kind, const uint8_t priv[PRIV_SIZE], const uint8_t serverPub[PUB_SIZE],
               const uint8_t *tokenId, const uint8_t *psk, uint8_t *req, size_t &reqLen);
  // Checks the server's answer. OK only if the server proved the pinned key
  // (and, when enrolling, that it knows the token secret).
  Result finish(const uint8_t *resp, size_t len, Session &s);

 private:
  struct State;
  State *st;
};

// Known-answer tests of every primitive as built for this platform. Returns
// nullptr, or the name of the first one that failed.
const char *selfTest();

}  // namespace C2Proto
