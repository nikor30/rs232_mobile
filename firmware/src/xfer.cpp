#include "xfer.h"
#include "config.h"
#include "settings.h"
#include "serial_bridge.h"
#include "display.h"
#include <ArduinoJson.h>
#include <esp_random.h>

namespace Xfer {

static const uint8_t SOH = 0x01, STX = 0x02, EOT = 0x04, ACK = 0x06, NAK = 0x15, CAN = 0x18, SUB = 0x1A;
static const uint32_t START_TIMEOUT_MS = 120000;   // receiver must start within 2 min
static const uint32_t ACK_TIMEOUT_MS   = 10000;    // + transmit time of the block
static const uint8_t  MAX_RETRIES      = 10;
static const uint32_t OWNER_GRACE_MS   = 30000;    // browser may reconnect and continue
static const uint32_t STARVE_MS        = 60000;    // no file data from the browser
static const uint32_t NOTIFY_MS        = 200;

enum State : uint8_t { IDLE, WAIT_START, WAIT_HDR_ACK, WAIT_HDR_C, SEND_DATA, WAIT_ACK, WAIT_EOT_ACK, WAIT_FIN_C, WAIT_FIN_ACK };

static Notify notifyFn = nullptr;
static State st = IDLE;
static uint8_t port = 0;
static Proto proto = XMODEM;
static String fname;
static uint32_t fsize = 0;
static uint32_t xid = 0;
static int16_t owner = -1;           // WebSocket client, -1 = disconnected
static uint32_t ownerGoneAt = 0;

// file data: ring buffer filled by the browser
static uint8_t *buf = nullptr;
static uint32_t recv = 0;            // bytes received from the browser
static uint32_t taken = 0;           // bytes moved out of the ring into blocks
static uint8_t carry[1024];          // taken but not sent yet (after a 1K -> 128 fallback)
static uint16_t carryLen = 0, carryPos = 0;
static uint32_t sent = 0;            // file bytes confirmed by the receiver

// protocol
static bool crcMode = true, use1k = false, ever1k = false;
static uint8_t blk = 1;
static uint32_t blocks = 0;          // data blocks confirmed
static uint8_t frame[3 + 1024 + 2];
static uint16_t frameLen = 0, frameData = 0;
static uint8_t frameRaw[1024];       // file bytes of the frame in flight
static uint8_t retries = 0;
static uint16_t retriesTotal = 0;
static uint8_t cans = 0;
static uint32_t stateAt = 0, lastData = 0, timeoutMs = ACK_TIMEOUT_MS;
static uint8_t cand = 0;             // start candidate ('C' or NAK) while waiting
static uint32_t lastOther = 0;
static uint8_t prevByte = '\n';
static uint32_t t0 = 0, lastNotify = 0;
static String note;

// ---------------------------------------------------------------- helpers
static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static const char *protoName(Proto p) {
  return p == YMODEM ? "YMODEM" : (p == XMODEM_1K ? "XMODEM-1K" : "XMODEM");
}

static const char *stateName() {
  switch (st) {
    case IDLE: return "idle";
    case WAIT_START: return "wait";
    default: return "run";
  }
}

static const char *phaseText() {
  switch (st) {
    case WAIT_START: return "warte auf Empfänger";
    case WAIT_HDR_ACK:
    case WAIT_HDR_C: return "Kopfblock";
    case SEND_DATA:
    case WAIT_ACK: return "Daten";
    case WAIT_EOT_ACK:
    case WAIT_FIN_C:
    case WAIT_FIN_ACK: return "Abschluss";
    default: return "";
  }
}

static void fillJson(JsonDocument &d, const char *state, const String &msg) {
  d["type"] = "xfer";
  d["id"] = xid;
  d["state"] = state;
  d["phase"] = phaseText();
  d["port"] = port;
  d["proto"] = protoName(proto);
  d["name"] = fname;
  d["size"] = fsize;
  d["recv"] = recv;
  d["taken"] = taken - (carryLen - carryPos);
  d["sent"] = sent;
  d["blocks"] = blocks;
  d["bs"] = use1k ? 1024 : 128;
  d["crc"] = crcMode;
  d["retries"] = retriesTotal;
  d["ms"] = t0 ? millis() - t0 : 0;
  d["buf"] = XFER_BUF;
  d["owner"] = owner;
  if (msg.length()) d["msg"] = msg;
}

String statusJson() {
  JsonDocument d;
  if (st == IDLE) {
    d["type"] = "xfer";
    d["state"] = "idle";
  } else {
    fillJson(d, stateName(), note);
  }
  String s;
  serializeJson(d, s);
  return s;
}

static void notify() {
  lastNotify = millis();
  if (notifyFn) notifyFn(statusJson());
}

static void setState(State s) {
#ifdef XFER_TRACE
  Serial.printf("[XFER] state %d -> %d\n", st, s);
#endif
  st = s;
  stateAt = millis();
}

static uint32_t frameTimeMs(uint16_t len) {
  uint32_t baud = settings.port[port].serial.baud;
  if (!baud) baud = 9600;
  // 11 bits per byte, doubled for the software UART which drains in slices
  return (uint32_t)len * (Bridge::isHardware(port) ? 11000UL : 22000UL) / baud;
}

static void cleanup() {
  if (buf) free(buf);
  buf = nullptr;
  st = IDLE;
  owner = -1;
  note = "";
}

static void finishWith(const char *state, const String &msg, bool sendCan) {
  if (sendCan) {
    const uint8_t c[5] = {CAN, CAN, CAN, CAN, CAN};
    Bridge::writeRaw(port, c, sizeof(c));
  }
  JsonDocument d;
  fillJson(d, state, msg);
  String s;
  serializeJson(d, s);
  if (notifyFn) notifyFn(s);
  Serial.printf("[XFER] %s: %s\n", state, msg.c_str());
  Display::message(protoName(proto), msg.substring(0, 21).c_str(), 5000);
  cleanup();
}

static void fail(const String &why, bool sendCan = true) { finishWith("error", why, sendCan); }

static void done() {
  uint32_t ms = millis() - t0;
  char b[96];
  snprintf(b, sizeof(b), "%lu Bytes in %lu s übertragen", (unsigned long)fsize, (unsigned long)((ms + 500) / 1000));
  finishWith("done", b, false);
}

static uint32_t ready() { return (recv - taken) + (carryLen - carryPos); }

static size_t takeData(uint8_t *out, size_t n) {
  size_t i = 0;
  while (i < n && carryPos < carryLen) out[i++] = carry[carryPos++];
  while (i < n && taken < recv) out[i++] = buf[taken++ % XFER_BUF];
  return i;
}

static void buildBlock(uint8_t num, uint16_t size, const uint8_t *data, uint16_t n, uint8_t pad) {
  frame[0] = size == 1024 ? STX : SOH;
  frame[1] = num;
  frame[2] = 255 - num;
  memcpy(frame + 3, data, n);
  memset(frame + 3 + n, pad, size - n);
  if (crcMode) {
    uint16_t c = crc16(frame + 3, size);
    frame[3 + size] = c >> 8;
    frame[4 + size] = c & 0xFF;
    frameLen = 5 + size;
  } else {
    uint8_t s = 0;
    for (uint16_t i = 0; i < size; i++) s += frame[3 + i];
    frame[3 + size] = s;
    frameLen = 4 + size;
  }
}

static void sendFrame() {
  Bridge::writeRaw(port, frame, frameLen);
  timeoutMs = ACK_TIMEOUT_MS + frameTimeMs(frameLen);
  stateAt = millis();
}

static void sendEot() {
  const uint8_t e = EOT;
  Bridge::writeRaw(port, &e, 1);
  timeoutMs = ACK_TIMEOUT_MS;
  stateAt = millis();
}

// YMODEM block 0: "name\0size\0" (empty name = end of batch)
static void sendHeader(bool last) {
  uint8_t d[1024];
  memset(d, 0, sizeof(d));
  uint16_t n = 0;
  if (!last) {
    n = fname.length();
    memcpy(d, fname.c_str(), n);
    n++;                                   // NUL
    n += snprintf((char *)d + n, sizeof(d) - n, "%lu", (unsigned long)fsize);
    n++;
  }
  buildBlock(0, n > 128 ? 1024 : 128, d, n > 128 ? n : 128, 0);
  sendFrame();
}

// next data block (or EOT) - only called in SEND_DATA
static void sendNext() {
  uint32_t left = fsize - sent;
  if (!left) {
    retries = 0;
    setState(WAIT_EOT_ACK);
    sendEot();
    return;
  }
  uint16_t size = (use1k && left > 896) ? 1024 : 128;
  uint16_t n = left < size ? left : size;
  if (ready() < n) return;                           // wait for the browser
  if (Bridge::txFree(port) < (size_t)size + 5) return;
  takeData(frameRaw, n);
  frameData = n;
  buildBlock(blk, size, frameRaw, n, SUB);
  retries = 0;
  setState(WAIT_ACK);
  sendFrame();
}

static void blockAcked() {
  sent += frameData;
  blocks++;
  if (frame[0] == STX) ever1k = true;
  blk++;
  setState(SEND_DATA);
  sendNext();
}

static void retry() {
  retries++;
  retriesTotal++;
  if (retries > MAX_RETRIES) {
    fail("Abbruch nach " + String(MAX_RETRIES) + " Wiederholungen (Block " + String(blocks + 1) + ")");
    return;
  }
  if (st == WAIT_ACK && frame[0] == STX && !ever1k && retries >= 3) {
    // receiver does not take 1K blocks: send the same data again in 128 byte blocks
    use1k = false;
    memcpy(carry, frameRaw, frameData);
    carryLen = frameData;
    carryPos = 0;
    note = "Empfänger nimmt keine 1K-Blöcke, weiter mit 128 Byte";
    setState(SEND_DATA);
    sendNext();
    notify();
    return;
  }
  if (st == WAIT_EOT_ACK) sendEot();
  else sendFrame();
}

static void handshake(uint8_t c) {
  crcMode = c == 'C';
  if (proto == YMODEM && !crcMode) {
    proto = XMODEM;
    note = "Empfänger will XMODEM mit Prüfsumme";
  }
  use1k = crcMode && proto != XMODEM;
  t0 = millis();
  Serial.printf("[XFER] Start %s (%s, %u-Byte-Blöcke) Port %u\n", protoName(proto), crcMode ? "CRC" : "Prüfsumme",
                use1k ? 1024 : 128, port + 1);
  retries = 0;
  if (proto == YMODEM) {
    blk = 1;
    setState(WAIT_HDR_ACK);
    sendHeader(false);
  } else {
    blk = 1;
    setState(SEND_DATA);
    sendNext();
  }
  notify();
}

// returns false when the byte is not part of the protocol and belongs to the terminal
static bool handleByte(uint8_t b) {
#ifdef XFER_TRACE
  if (b < 0x20 || b == 'C') Serial.printf("[XFER] rx 0x%02x in state %d\n", b, st);
#endif
  if (b == CAN) {
    if (++cans >= 2) fail("vom Empfänger abgebrochen", false);
    return true;
  }
  cans = 0;
  // A reply can only refer to our frame once the frame has completely left the TX
  // pin. Earlier ones are stale (e.g. a NAK from the receiver's previous timeout):
  // answering them would put a second copy on the line and shift every later ACK.
  if ((b == ACK || b == NAK || b == 'C') && !Bridge::txIdle(port) &&
      (st == WAIT_ACK || st == WAIT_HDR_ACK || st == WAIT_EOT_ACK || st == WAIT_FIN_ACK))
    return true;
  // after the last block: text from the receiver ("transfer complete", prompt ...) means
  // it is finished (lrzsz "rb" does not ACK the closing YMODEM block, others skip it)
  if ((st == WAIT_FIN_C || st == WAIT_FIN_ACK) && b != ACK && b != NAK && b != 'C') {
    done();
    return false;
  }
  switch (st) {
    case WAIT_HDR_ACK:
      if (b == ACK) setState(WAIT_HDR_C);
      else if (b == NAK || b == 'C') retry();
      break;
    case WAIT_HDR_C:
      if (b == 'C' || b == NAK) {
        setState(SEND_DATA);
        sendNext();
      }
      break;
    case WAIT_ACK:
      if (b == ACK) blockAcked();
      else if (b == NAK || (b == 'C' && blocks == 0)) retry();
      break;
    case WAIT_EOT_ACK:
      if (b == ACK) {
        if (proto == YMODEM) setState(WAIT_FIN_C);
        else done();
      } else if (b == NAK) {
        retry();
      }
      break;
    case WAIT_FIN_C:
      if (b == 'C' || b == NAK) {
        retries = 0;
        setState(WAIT_FIN_ACK);
        sendHeader(true);
      }
      break;
    case WAIT_FIN_ACK:
      if (b == ACK) done();
      else if (b == NAK || b == 'C') retry();
      break;
    default:
      break;                                // SEND_DATA: stale NAKs while waiting for data
  }
  return true;
}

// ---------------------------------------------------------------- public
void begin(Notify n) { notifyFn = n; }

bool active() { return st != IDLE; }
bool on(uint8_t p) { return st != IDLE && p == port; }
bool blocksInput(uint8_t p) { return st != IDLE && st != WAIT_START && p == port; }

bool start(uint8_t p, Proto pr, const String &name, uint32_t size, uint8_t client, String &err) {
  if (st != IDLE) { err = "Es läuft schon eine Übertragung"; return false; }
  if (!Bridge::enabled(p)) { err = "Port nicht aktiv"; return false; }
  if (Bridge::autobaudRunning(p)) { err = "Auto-Baud läuft"; return false; }
  if (!size) { err = "Die Datei ist leer"; return false; }
  if (pr > YMODEM) pr = XMODEM;
  buf = (uint8_t *)malloc(XFER_BUF);
  if (!buf) { err = "Zu wenig Speicher"; return false; }

  // YMODEM header name: no path, printable ASCII, no spaces
  String n = name;
  int s = max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
  if (s >= 0) n = n.substring(s + 1);
  fname = "";
  for (size_t i = 0; i < n.length() && fname.length() < 64; i++) {
    char c = n[i];
    fname += (c > 0x20 && c < 0x7F) ? c : '_';
  }
  if (fname.isEmpty()) fname = "datei.bin";

  port = p;
  proto = pr;
  fsize = size;
  xid = esp_random() | 1;
  owner = client;
  recv = taken = sent = 0;
  carryLen = carryPos = 0;
  blocks = 0;
  retries = 0;
  retriesTotal = 0;
  cans = 0;
  ever1k = false;
  cand = 0;
  prevByte = '\n';
  t0 = 0;
  note = "";
  lastOther = millis() - 1000;
  lastData = millis();
  setState(WAIT_START);
  // receiver started before the upload was requested: its last 'C'/NAK is in the terminal
  // (the next one may take up to 10 s), so start with it
  uint8_t last[2] = {0, 0};
  uint32_t seq = Bridge::seqNow(p);
  if (seq >= 2 && millis() - Bridge::lastRxMs[p] < 10000 && Bridge::copyFrom(p, seq - 2, last, 2) == 2 &&
      (last[1] == 'C' || last[1] == NAK) &&
      (last[0] == '\r' || last[0] == '\n' || last[0] == 'C' || last[0] == NAK)) {
    cand = last[1];
    lastOther = millis() - 1000;
  }
  Serial.printf("[XFER] %s \"%s\" (%lu Bytes) auf Port %u: warte auf Empfänger\n", protoName(proto), fname.c_str(),
                (unsigned long)fsize, port + 1);
  Display::message(protoName(proto), "warte auf Empf.", 3000);
  notify();
  return true;
}

bool resume(uint8_t client, uint32_t id) {
  if (st == IDLE || id != xid) return false;
  owner = client;
  notify();
  return true;
}

void abort(const String &why) {
  if (st == IDLE) return;
  fail(why, st != WAIT_START);
}

void clientGone(uint8_t client) {
  if (st != IDLE && owner == client) {
    owner = -1;
    ownerGoneAt = millis();
  }
}

size_t feed(uint8_t client, const uint8_t *d, size_t n) {
  if (st == IDLE || !buf || owner != client) return 0;
  uint32_t fileLeft = fsize - recv;
  if (n > fileLeft) n = fileLeft;
  uint32_t space = XFER_BUF - (recv - taken);
  if (n > space) {
    fail("Pufferüberlauf (Browser zu schnell)");
    return 0;
  }
  for (size_t i = 0; i < n; i++) buf[(recv + i) % XFER_BUF] = d[i];
  recv += n;
  lastData = millis();
  if (st == SEND_DATA) sendNext();
  return n;
}

size_t onRx(uint8_t p, const uint8_t *d, size_t n) {
  if (st == IDLE || p != port) return 0;
  uint32_t now = millis();
  if (st == WAIT_START) {
    // A 'C' (CRC) or NAK (checksum) starts the transfer when it stands alone: at the
    // start of a line or after a pause, and not followed by more text (loop() waits
    // 25 ms). Ignored within 500 ms after the user typed a 'C' (its echo).
    for (size_t i = 0; i < n; i++) {
      uint8_t b = d[i];
      if (b == 'C' || b == NAK) {
        bool lineStart = prevByte == '\r' || prevByte == '\n' || prevByte == 'C' || prevByte == NAK;
        bool alone = lineStart || now - lastOther >= 20;
        cand = (alone && now - Bridge::lastUserStartMs[port] >= 500) ? b : 0;
      } else {
        cand = 0;
      }
      lastOther = now;
      prevByte = b;
    }
    return 0;                                // the user sees the receiver's messages
  }
  size_t i = 0;
  while (i < n && st != IDLE) {
    if (!handleByte(d[i])) break;            // transfer over, rest belongs to the terminal
    i++;
  }
  return i;
}

void loop() {
  if (st == IDLE) return;
  uint32_t now = millis();

  if (owner < 0 && now - ownerGoneAt > OWNER_GRACE_MS && recv < fsize) {
    fail("Browser getrennt", st != WAIT_START);
    return;
  }

  switch (st) {
    case WAIT_START:
      if (cand && now - lastOther >= 25) {
        uint8_t c = cand;
        cand = 0;
        handshake(c);
      } else if (now - stateAt > START_TIMEOUT_MS) {
        fail("Empfänger hat nicht gestartet (kein C/NAK)", false);
        return;
      }
      break;
    case SEND_DATA:
      sendNext();
      if (st == SEND_DATA && owner >= 0 && now - lastData > STARVE_MS) {
        fail("Keine Daten vom Browser");
        return;
      }
      break;
    case WAIT_HDR_ACK:
    case WAIT_ACK:
    case WAIT_EOT_ACK:
    case WAIT_FIN_ACK:
      if (now - stateAt > timeoutMs) {
        if (st == WAIT_FIN_ACK) { done(); return; }   // data is complete anyway
        retry();
      }
      break;
    case WAIT_HDR_C:
      if (now - stateAt > 60000) { fail("Empfänger fordert keine Daten an"); return; }
      break;
    case WAIT_FIN_C:
      if (now - stateAt > ACK_TIMEOUT_MS) { done(); return; }
      break;
    default:
      break;
  }
  if (st != IDLE && now - lastNotify >= NOTIFY_MS) notify();
}

}  // namespace Xfer
