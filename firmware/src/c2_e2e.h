#pragma once
#include <stddef.h>
#include <stdint.h>

// End-to-end encryption of a remote console (server/KONZEPT.md §4.5): between
// the administrator's browser and this device, with the server only carrying
// what it cannot read. Portable (mbedTLS only), checked on the PC against
// WebCrypto by tests/c2/e2e_test.sh.
//
//   browser -> device: its one-time P-256 key, in the console request
//   device -> browser: this device's one-time key, in its console messages
//   both: Z = ECDH; 68 bytes = HKDF-SHA256(salt = keyB || keyD, Z, "rs232-console-e2e-v1")
//         = key browser->device (32) | key device->browser (32) | code (4: big-endian % 10^6)
//   frames in both byte streams: 2 bytes length L (big-endian) | L bytes AES-256-GCM
//         (ciphertext and 16-byte tag), nonce = 4 zero bytes | 8-byte frame counter (big-endian)
//
// The six digits are shown on the display and in the browser: if they are the
// same, nobody sits in between - not even the server.
namespace C2E2E {

constexpr size_t PUB = 65;           // uncompressed P-256 point
constexpr size_t TAG = 16;
constexpr size_t MAX_PLAIN = 512;    // per frame

struct Session {
  uint8_t kIn[32], kOut[32];         // in: from the browser; out: to it
  uint64_t ctrIn = 0, ctrOut = 0;
  char code[7] = "";
};

// The device's half: takes the browser's key, makes its own (devicePub) and
// the session. rng as for mbedTLS. False: the browser's key is unusable.
bool start(Session &s, const uint8_t browserPub[PUB], uint8_t devicePub[PUB],
           int (*rng)(void *, unsigned char *, size_t), void *rngCtx);

// One frame out of n <= MAX_PLAIN bytes into out (room: 2 + n + TAG); returns its size.
size_t seal(Session &s, const uint8_t *plain, size_t n, uint8_t *out);

// The first frame of buf: 0 = not complete yet; otherwise how many bytes of buf
// it took, plain/plainLen what it said (plain: room for MAX_PLAIN). bad = it was
// not genuine - the stream is useless from there on.
size_t open(Session &s, const uint8_t *buf, size_t len, uint8_t *plain, size_t &plainLen, bool &bad);

}  // namespace C2E2E
