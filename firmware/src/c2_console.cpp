#include "c2_console.h"

#include <string.h>
#include <mbedtls/base64.h>

namespace C2Console {

void Stream::outPut(const uint8_t *data, size_t n) {
  if (n > outRoom()) n = outRoom();
  memcpy(out + outLen, data, n);
  outLen += n;
}

void Stream::inDrop(size_t n) {
  if (n > inLen) n = inLen;
  memmove(in, in + n, inLen - n);
  inLen -= n;
}

size_t buildMessage(JsonDocument &msg, uint32_t id, const Stream &s, uint32_t leftS, const char *end) {
  size_t n = s.outLen < CHUNK ? s.outLen : CHUNK;
  msg.clear();
  msg["t"] = "con";
  msg["id"] = id;
  msg["off"] = s.outOff;
  if (n) {
    char b64[(CHUNK + 2) / 3 * 4 + 1];
    size_t len = 0;
    mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &len, s.out, n);
    b64[len] = 0;
    msg["d"] = b64;                      // char array: ArduinoJson copies it
  }
  msg["iack"] = s.inAck;
  msg["left_s"] = leftS;
  if (end && end[0]) msg["end"] = end;
  return n;
}

void accepted(Stream &s, size_t n) {
  if (n > s.outLen) n = s.outLen;
  memmove(s.out, s.out + n, s.outLen - n);
  s.outLen -= n;
  s.outOff += n;
}

const char *applyReply(JsonDocument &reply, Stream &s) {
  const char *end = reply["end"] | "";
  if (end[0]) return end;
  const char *b64 = reply["d"] | "";
  size_t b64Len = strlen(b64);
  if (b64Len) {
    uint8_t data[IN_MAX];
    size_t n = 0;
    uint64_t off = reply["ioff"] | (uint64_t)0;
    // Bytes before inAck are known already. What does not fit now is left
    // unconfirmed, and the server sends it again.
    if (b64Len <= (IN_MAX / 3) * 4 && !mbedtls_base64_decode(data, sizeof(data), &n, (const unsigned char *)b64, b64Len) &&
        off <= s.inAck && off + n > s.inAck) {
      size_t skip = (size_t)(s.inAck - off), take = n - skip;
      if (take > IN_MAX - s.inLen) take = IN_MAX - s.inLen;
      memcpy(s.in + s.inLen, data + skip, take);
      s.inLen += take;
      s.inAck += take;
    }
  }
  uint32_t brk = reply["brk"] | 0U;
  if (brk > s.brkSeen) {                 // a counter, not an event: an answer that comes twice breaks once
    s.brkDue += brk - s.brkSeen;
    s.brkSeen = brk;
  }
  return nullptr;
}

}  // namespace C2Console
