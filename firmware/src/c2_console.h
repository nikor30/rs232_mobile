#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

// The two byte streams of a remote console (server/KONZEPT.md §4.5): what the
// serial port sends goes to the server, what the administrator types comes
// back. Each message names the offset of its bytes in the stream, so one that
// is repeated after a lost answer brings nothing twice.
//
// Portable like c2_proto.h (ArduinoJson and mbedTLS's base64 only); the host
// test in tests/c2/ runs this code against the real server.
namespace C2Console {

constexpr size_t OUT_MAX = 2048;   // port output waiting for the server; what does not fit stays in the port's ring
constexpr size_t IN_MAX = 1024;    // input waiting for the port
constexpr size_t CHUNK = 700;      // output bytes per message: with base64 and the record still about 1 kB

struct Stream {
  uint8_t out[OUT_MAX];
  size_t outLen = 0;
  uint64_t outOff = 0;             // offset of out[0] since the session began
  uint8_t in[IN_MAX];
  size_t inLen = 0;
  uint64_t inAck = 0;              // input bytes accepted so far
  uint32_t brkSeen = 0;            // BREAKs the server has asked for in total
  uint32_t brkDue = 0;             // of those, not yet put on the line

  size_t outRoom() const { return OUT_MAX - outLen; }
  void outPut(const uint8_t *data, size_t n);        // n <= outRoom()
  void outSkip(uint64_t n) { outOff += n; }          // bytes that were lost before they got here; only while out is empty
  void inDrop(size_t n);                             // the port has taken the first n bytes of in
};

// Writes the next message into msg and returns how many bytes of s.out it
// carries - hand that number to accepted() once the server has answered.
size_t buildMessage(JsonDocument &msg, uint32_t id, const Stream &s, uint32_t leftS, const char *end);

// The server has the first n bytes of s.out.
void accepted(Stream &s, size_t n);

// Takes the server's answer. Returns nullptr, or the reason if the server has
// closed the session (the pointer lives as long as reply does).
const char *applyReply(JsonDocument &reply, Stream &s);

}  // namespace C2Console
