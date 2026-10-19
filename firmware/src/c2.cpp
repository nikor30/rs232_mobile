#include "c2.h"
#include "c2_proto.h"
#include "c2_console.h"
#include "c2_e2e.h"
#include "c2_update.h"
#include "update_key.h"
#include "xfer.h"
#include "serial_bridge.h"
#include "config.h"
#include "settings.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "ble.h"
#include "configs.h"
#include "sdcard.h"
#include "player.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include "lang.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <soc/soc_memory_layout.h>
#include <mbedtls/base64.h>
#include "certs.h"
#include "sdbackup.h"
#include <mbedtls/sha256.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <functional>
#include <vector>

// The radio is on whenever this is called, so this is the hardware's true RNG.
extern "C" void c2_randombytes(uint8_t *out, size_t len) { esp_fill_random(out, len); }

namespace C2 {

// ML-KEM keeps whole polynomial matrices on the stack (about 20 kB measured),
// and a TLS connection takes about 45 kB of heap. Both at once do not fit next
// to WiFi, Bluetooth and the display, and they never have to: the key
// exchange is computed before the request goes out and after the answer is in.
// So the task itself stays small, and everything that touches ML-KEM runs in a
// short-lived task with a big stack (big()) while no connection is open.
static const uint32_t TASK_STACK = 12 * 1024;
static uint32_t kemStackFree = 22 * 1024;
static const char *NVS = "c2";
static const uint32_t RETRY_MIN_MS = 15000, RETRY_MAX_MS = 300000;
static const uint32_t REFUSED_RETRY_MS = 600000;

static SemaphoreHandle_t mux;
static Info shared;                      // under mux
static String reqToken;                  // under mux: token waiting for the task
static String reqUrl;                    // under mux: new address of the server
static bool reqUrlSet = false;           // under mux
static bool reqForget = false;           // under mux
static volatile bool reqRetry = false;   // somebody pressed "connect now"

// Most of what the server can ask for touches state that belongs to the main
// loop (ports, settings, the configuration player). The task hands such
// commands over; C2::loop() carries them out and hands the answers back.
struct MainJob {
  uint32_t id;
  String type, args;                     // args: the command's JSON object, or empty
};
static std::vector<MainJob> jobs;        // under mux: task -> main loop
static std::vector<String> jobResults;   // under mux: main loop -> task, one JSON object each
static const size_t MAX_JOBS = 8;
static volatile bool rebootWanted = false, rebootNow = false;
// The command whose answer has to reach the server before the restart. Not
// just "some answer": a profile sends certificates and then "set" in one go,
// and the device restarted as soon as the first of them was answered - the
// others stayed "sent" at the server for good.
static volatile uint32_t rebootAfter = 0;
static volatile uint32_t sdJob = 0;      // the "sd_put" command the task is to carry out (main loop -> task)
static volatile uint32_t updateJob = 0;  // the "update" command the task is to carry out (main loop -> task)
static char devName[33];

struct Lock {
  Lock() { xSemaphoreTake(mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mux); }
};

// ---- remote console (under mux). The main loop owns the serial port and the
// decision; the task carries the bytes. They meet in con.stream.
enum ConState : uint8_t {
  CON_NONE = 0,
  CON_ASK,       // the server has asked; waiting for a tap on the display
  CON_OPEN,
  CON_END,       // closed here; the task still has to tell the server
};
static struct {
  ConState state = CON_NONE;
  uint32_t id = 0;               // the command that asked
  uint8_t port = 0;
  uint16_t minutes = 0;
  uint32_t askedMs = 0, untilMs = 0;
  uint32_t seq = 0;              // next byte of the port's output to pass on (Bridge numbering)
  char reason[48] = "";
  C2Console::Stream *stream = nullptr;
  // End to end (c2_e2e.h): set when the browser sent its key with the request.
  C2E2E::Session *e2e = nullptr;
  char e2ePub[92] = "";          // this device's key, base64, in every console message
  uint8_t inPlain[C2E2E::MAX_PLAIN];
  size_t inPlainLen = 0;         // decrypted input the port has not taken yet
} con;

// Forgets the console's keys. Under mux.
static void conForgetKeys() {
  if (con.e2e) {
    memset(con.e2e, 0, sizeof(*con.e2e));
    free(con.e2e);
    con.e2e = nullptr;
  }
  con.e2ePub[0] = 0;
  memset(con.inPlain, 0, sizeof(con.inPlain));
  con.inPlainLen = 0;
}
static volatile bool conBusy = false;    // con.state != CON_NONE, readable without the lock
static bool conAnnounced = false;        // task: the server has the "yes" to the console that is open now
static bool conAtCollect = false;
static bool keepAlive = false;           // task: the connection stays up between requests (see requestOnce)
// Unattended access (main loop counts, everybody reads): seconds left.
static volatile uint32_t unattLeft = 0;
static uint32_t unattTickMs = 0, unattSavedS = 0;
// What is stored on the device, for the status: written by the main loop, read
// by the task, both under mux.
static String inventory = "[]", sdFiles = "[]";
static uint32_t inventoryAt = 0;
static uint32_t conRefuseUntil = 0;      // main loop: after a "no", further requests are turned down for a while
static const uint32_t CON_ASK_MS = 60000, CON_REFUSE_MS = 60000, CON_MAX_MIN = 60;
static const uint32_t CON_LOST_MS = 30000;   // without the server for this long, the console is closed

struct BigJob {
  const std::function<void()> *fn;
  TaskHandle_t caller;
  uint32_t free;                         // lowest free stack, set when the computation is done
};
static uint32_t bigUsed = 0;             // stack the last computation in big() took

// Runs fn on a stack large enough for ML-KEM and waits for it.
//
// The helper task parks itself when it is done and the caller removes it: a
// task that deletes itself is only cleaned up when the idle task next runs, and
// the next call needs the stack block again.
static void connClose();

// Key exchange, self-test and signature check need a lot of stack (measured:
// 19.3, 19.6 and 13.1 kB), more than any task here has. They run in a short
// task of their own on this one block, reserved at start-up. Taking it from the
// heap each time failed sooner or later: inside an 802.1X network the largest
// free piece shrank to 21 kB on the VIEWE board, and the device never got a
// session again. Reserved, it costs the same at the peak and cannot be missing.
static const uint32_t BIG_STACK = 22 * 1024;
static StackType_t bigStack[BIG_STACK / sizeof(StackType_t)] __attribute__((aligned(16)));
static StaticTask_t bigTcb;

static bool big(const std::function<void()> &fn) {
  connClose();                           // never together with a TLS connection
  BigJob job = {&fn, xTaskGetCurrentTaskHandle(), 0};
  auto entry = [](void *p) {
    BigJob *j = (BigJob *)p;
    (*j->fn)();
    j->free = uxTaskGetStackHighWaterMark(nullptr);
    kemStackFree = min(kemStackFree, j->free);
    xTaskNotifyGive(j->caller);
    for (;;) vTaskSuspend(nullptr);
  };
  TaskHandle_t h = xTaskCreateStaticPinnedToCore(entry, "c2kem", BIG_STACK, &job, 1, bigStack, &bigTcb, 0);
  if (h) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (eTaskGetState(h) != eSuspended) vTaskDelay(1);
    vTaskDelete(h);
    bigUsed = BIG_STACK - job.free;      // what this computation took: in the log after self-test and signature check
  }
  return h != nullptr;
}

// Memory for buffers that live across a call of big(): from the external RAM
// if there is any. Small blocks from the internal heap, taken before the TLS
// connection and still held after it, cut the free space into pieces - 74 kB
// free, yet no 28 kB in one piece for the key exchange. That left the device
// without a session for as long as it ran (seen after about every other start).
static uint8_t *ext(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return (uint8_t *)(p ? p : malloc(n));
}

// ---- task state (touched by the task only)
static uint8_t priv[C2Proto::PRIV_SIZE];
static bool haveKey = false;
static uint8_t *serverPub = nullptr;     // PUB_SIZE bytes once enrolled
// Change of the server key: the announced key waits in the NVS ("spub2") until
// a handshake needs it; the tag names the newest key held, for the polls.
static bool haveNext = false;
static char skTag[C2Proto::KEY_TAG_SIZE] = "";
static bool confirmFailed = false;       // the last handshake: the server did not prove the pinned key
static std::string serverUrl;
static State state = OFF;
static C2Proto::Session session;
static bool haveSession = false;
static JsonDocument results;             // answers to commands, handed in with the next poll
static uint32_t nextPollAt = 0, retryMs = RETRY_MIN_MS, pollS = 30;
// Not enrolled, but somebody has named a server: ask there without invitation.
static std::string wantUrl;
static uint32_t wantAt = 0, wantRetryMs = RETRY_MIN_MS;

static void publish(const char *error = nullptr, const char *code = nullptr) {
  Lock l;
  shared.state = state;
  if (error) strlcpy(shared.error, error, sizeof(shared.error));
  if (code) strlcpy(shared.code, code, sizeof(shared.code));
  if (state != PENDING) shared.code[0] = 0;
  strlcpy(shared.url, serverUrl.c_str(), sizeof(shared.url));
  strlcpy(shared.want, serverPub ? "" : wantUrl.c_str(), sizeof(shared.want));
  shared.stackFree = kemStackFree;
  shared.taskStackFree = uxTaskGetStackHighWaterMark(nullptr);
}

static void publishId() {
  uint8_t pub[C2Proto::PUB_SIZE], fp[32];
  char id[33] = "";
  bool ok = false;
  if (haveKey) big([&] { ok = C2Proto::publicKey(priv, pub); });
  if (ok) {
    C2Proto::fingerprint(pub, fp);
    for (size_t i = 0; i < C2Proto::ID_SIZE; i++) snprintf(id + 2 * i, 3, "%02x", fp[i]);
  }
  Lock l;
  strlcpy(shared.id, id, sizeof(shared.id));
}

// ---------------------------------------------------------------- storage

static void loadStored() {
  Preferences p;
  if (!p.begin(NVS, true)) return;
  haveKey = p.getBytes("key", priv, sizeof(priv)) == sizeof(priv);
  wantUrl = p.getString("want", "").c_str();
  String url = p.getString("url", "");
  uint8_t st = p.getUChar("st", 0);
  String sas = p.getString("sas", "");
  if (url.length() && p.getBytesLength("spub") == C2Proto::PUB_SIZE && haveKey) {
    serverPub = (uint8_t *)malloc(C2Proto::PUB_SIZE);
    if (serverPub && p.getBytes("spub", serverPub, C2Proto::PUB_SIZE) == C2Proto::PUB_SIZE) {
      serverUrl = url.c_str();
      state = st == ACTIVE ? ACTIVE : PENDING;
      publish("", sas.c_str());
      C2Proto::keyTag(serverPub, skTag);
      uint8_t *next = ext(C2Proto::PUB_SIZE);
      if (next && p.getBytes("spub2", next, C2Proto::PUB_SIZE) == C2Proto::PUB_SIZE &&
          memcmp(next, serverPub, C2Proto::PUB_SIZE) != 0) {
        haveNext = true;
        C2Proto::keyTag(next, skTag);
      }
      free(next);
    }
  }
  p.end();
}

static void storeState(const char *sas) {
  Preferences p;
  if (!p.begin(NVS, false)) return;
  p.putUChar("st", state);
  if (sas) p.putString("sas", sas);
  else if (state != PENDING) p.remove("sas");
  p.end();
}

static void forgetAll() {
  Preferences p;
  if (p.begin(NVS, false)) {
    p.clear();
    p.end();
  }
  memset(priv, 0, sizeof(priv));
  haveKey = haveSession = haveNext = false;
  skTag[0] = 0;
  session.clear();
  free(serverPub);
  serverPub = nullptr;
  serverUrl.clear();
  wantUrl.clear();                       // or it would ask to be let in again at once
  results.clear();
  state = OFF;
  {
    Lock l;
    jobs.clear();
    jobResults.clear();
    if (con.state != CON_NONE) {
      delete con.stream;
      con.stream = nullptr;
      conForgetKeys();
      con.state = CON_NONE;
      conBusy = false;
    }
    conAnnounced = false;
    keepAlive = false;
    shared.lastOkMs = shared.polls = shared.handshakeMs = shared.cryptoMs = 0;
  }
  publish("");
  publishId();
  Serial.println("[C2]   abgemeldet, Geraeteschluessel geloescht");
}

// ---------------------------------------------------------------- HTTP

static int requestOnce(const std::string &base, const char *path, const uint8_t *body, size_t len,
                       uint8_t *out, size_t outMax, size_t &outLen);

// One request, tried up to three times if the connection fails: the device is
// often at the far end of someone's WLAN. Returns the HTTP status, or a
// negative HTTPClient error. Sending a record twice is harmless - if the first
// copy did arrive, the server takes the second for a replay and answers 401,
// and the caller shakes hands again.
static int request(const std::string &base, const char *path, const uint8_t *body, size_t len,
                   uint8_t *out, size_t outMax, size_t &outLen, int tries = 3) {
  int code = -1;
  for (int attempt = 0; attempt < tries; attempt++) {
    if (attempt) vTaskDelay(pdMS_TO_TICKS(1500));
    code = requestOnce(base, path, body, len, out, outMax, outLen);
    if (code >= 0) break;
  }
  return code;
}

// Why a request did not get an answer (negative results of requestOnce).
enum { NET_URL = -1, NET_CONNECT = -2, NET_SEND = -3, NET_TIMEOUT = -4, NET_ANSWER = -5 };
static const char *const NET_NAMES[] = {"", "Adresse unbrauchbar", "keine Verbindung", "Senden gescheitert",
                                        "keine Antwort", "Antwort unbrauchbar"};

// The connection. Normally it lives for one request. While a remote console is
// open (keepAlive) it stays up, and every request goes over it: the console
// asks several times a second, and a second TLS connection would not fit.
static WiFiClient *connPlain = nullptr;
static WiFiClientSecure *connTls = nullptr;
static Client *conn = nullptr;

static void connClose() {
  if (conn) conn->stop();
  delete connPlain;
  delete connTls;
  connPlain = nullptr;
  connTls = nullptr;
  conn = nullptr;
}

// HTTP by hand: one request, one answer, and (unless keepAlive) the connection
// closed. Small enough to see exactly where a weak link gives up.
static int requestOnce(const std::string &base, const char *path, const uint8_t *body, size_t len,
                       uint8_t *out, size_t outMax, size_t &outLen) {
  const uint32_t TIMEOUT_MS = 20000;     // generous: a few kilobytes can take seconds at the edge of a WLAN
  uint32_t started = millis();
  outLen = 0;
  String url = base.c_str();
  bool https = url.startsWith("https://");
  if (!https && !url.startsWith("http://")) return NET_URL;
  String host = url.substring(https ? 8 : 7);
  int slash = host.indexOf('/');
  if (slash >= 0) host.remove(slash);
  uint16_t port = https ? 443 : 80;
  int colon = host.indexOf(':');
  if (colon >= 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host.remove(colon);
  }
  if (host.isEmpty() || !port) return NET_URL;

  int code = NET_CONNECT;
  const char *step = "Verbinden";
  bool up = conn && conn->connected();
  if (!up) {
    connClose();
    if (https) {
      // The certificate is not checked: the server is authenticated inside, by
      // the key the enrollment token pinned (KONZEPT.md §8, open question).
      connTls = new WiFiClientSecure();
      connTls->setInsecure();
      connTls->setHandshakeTimeout(TIMEOUT_MS / 1000);
      up = connTls->connect(host.c_str(), port, (int32_t)TIMEOUT_MS);   // also the limit for one write
      conn = connTls;
    } else {
      connPlain = new WiFiClient();
      up = connPlain->connect(host.c_str(), port, (int32_t)TIMEOUT_MS);
      conn = connPlain;
    }
  }
  Client *c = conn;
  if (up) {
    step = "Senden";
    code = NET_SEND;
    String head = String(body ? "POST " : "GET ") + path +
                  (keepAlive ? " HTTP/1.1\r\nHost: " : " HTTP/1.0\r\nHost: ") + host +
                  (keepAlive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n");
    if (body) head += "Content-Type: application/octet-stream\r\nContent-Length: " + String((unsigned)len) + "\r\n";
    head += "\r\n";
    bool sent = c->write((const uint8_t *)head.c_str(), head.length()) == head.length();
    for (size_t off = 0; sent && off < len;) {
      size_t n = c->write(body + off, min((size_t)1024, len - off));
      if (!n) sent = false;
      off += n;
    }
    if (sent) {
      // status line and headers, then exactly Content-Length bytes
      step = "Antwort";
      code = NET_TIMEOUT;
      String line;
      int status = 0, length = -1;
      bool headers = true;
      while (headers && millis() - started < TIMEOUT_MS) {
        int ch = c->read();
        if (ch < 0) {
          if (!c->connected() && !c->available()) break;
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        if (ch != '\n') {
          if (ch != '\r' && line.length() < 200) line += (char)ch;
          continue;
        }
        if (line.startsWith("HTTP/")) status = line.substring(line.indexOf(' ') + 1).toInt();
        else if (line.isEmpty()) headers = false;
        else {
          String lower = line;
          lower.toLowerCase();
          if (lower.startsWith("content-length:")) length = lower.substring(15).toInt();
        }
        line = "";
      }
      if (!headers && status) {
        code = status;
        // The server states the length. Chunked answers are not understood, on purpose.
        if (status == 200 && (length < 0 || (size_t)length > outMax)) code = NET_ANSWER;
        while (code == 200 && outLen < (size_t)length) {
          int n = c->read(out + outLen, length - outLen);
          if (n > 0) outLen += n;
          else if (millis() - started > TIMEOUT_MS || (!c->connected() && !c->available())) code = NET_TIMEOUT;
          else vTaskDelay(pdMS_TO_TICKS(5));
        }
      }
    }
  }
  if (!keepAlive || code != 200) connClose();   // after anything unexpected the stream is not trusted any more
  if (code != 200)
    Serial.printf("[C2]   %s: %s%s nach %lu ms bei \"%s\" (Heap %u kB)\n", path,
                  code < 0 ? NET_NAMES[-code] : "HTTP ", code < 0 ? "" : String(code).c_str(),
                  (unsigned long)(millis() - started), step, (unsigned)(ESP.getFreeHeap() / 1024));
  return code;
}

enum Ex { EX_OK = 0, EX_NET, EX_REHANDSHAKE, EX_REFUSED, EX_BAD };

static Ex fromHttp(int code) {
  return code == 200 ? EX_OK : code == 401 ? EX_REHANDSHAKE : code == 403 ? EX_REFUSED : code < 0 ? EX_NET : EX_BAD;
}

static Ex handshake(C2Proto::Kind kind, const C2Proto::Token *tok) {
  haveSession = confirmFailed = false;
  uint8_t *req = ext(C2Proto::REQ_ENROLL), *resp = ext(C2Proto::RESP_SIZE);
  C2Proto::Handshake hs;
  size_t n = 0, got = 0;
  Ex ex = EX_BAD;
  uint32_t t0 = millis(), crypto = 0;
  C2Proto::Result r = C2Proto::ERR_CRYPTO;
  if (req && resp)
    big([&] { r = hs.begin(kind, priv, serverPub, tok ? tok->id : nullptr, tok ? tok->secret : nullptr, req, n); });
  if (r != C2Proto::OK)
    Serial.printf("[C2]   Handshake nicht begonnen: Heap %u B, groesster Block %u B\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (r == C2Proto::OK) {
    crypto = millis() - t0;
    int code = request(serverUrl, "/v1/handshake", req, n, resp, C2Proto::RESP_SIZE, got);
    ex = fromHttp(code);
    if (ex == EX_BAD) Serial.printf("[C2]   Handshake: HTTP %d\n", code);
    if (ex == EX_OK) {
      uint32_t t1 = millis();
      r = C2Proto::ERR_CRYPTO;
      big([&] { r = hs.finish(resp, got, session); });
      crypto += millis() - t1;
      if (r == C2Proto::OK) haveSession = true;
      else {
        ex = EX_BAD;
        Serial.printf("[C2]   Handshake nicht abgeschlossen (%d, Antwort %u B): Heap %u B, groesster Block %u B\n", (int)r, (unsigned)got,
                      (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        confirmFailed = r == C2Proto::ERR_CONFIRM;
        publish(r == C2Proto::ERR_CONFIRM ? TR("the server did not prove its key", "Server hat seinen Schluessel nicht bewiesen", "o servidor nao provou sua chave")
                : r == C2Proto::ERR_CRYPTO ? TR("not enough memory for the handshake", "zu wenig Speicher fuer den Handshake", "memoria insuficiente para o handshake") : TR("handshake: unusable answer", "Handshake: Antwort unbrauchbar", "handshake: resposta invalida"));
      }
    }
  }
  free(req);
  free(resp);
  if (haveSession) {
    Lock l;
    shared.handshakeMs = millis() - t0;
    shared.cryptoMs = crypto;
  }
  return ex;
}

// A handshake with the pinned server key. If the server cannot prove that one
// and has announced another, that one is tried - and pinned if it works: the
// server has changed its key.
static Ex sessionHandshake() {
  Ex ex = handshake(C2Proto::KIND_SESSION, nullptr);
  if (ex != EX_BAD || !confirmFailed || !haveNext) return ex;
  uint8_t *next = ext(C2Proto::PUB_SIZE);
  bool ok = false;
  {
    Preferences p;
    if (next && p.begin(NVS, true)) {
      ok = p.getBytes("spub2", next, C2Proto::PUB_SIZE) == C2Proto::PUB_SIZE;
      p.end();
    }
  }
  if (!ok) {
    free(next);
    return ex;
  }
  uint8_t *pinned = serverPub;
  serverPub = next;
  Ex ex2 = handshake(C2Proto::KIND_SESSION, nullptr);
  if (ex2 != EX_OK) {                    // not that one either: nothing changes
    serverPub = pinned;
    free(next);
    return ex2 == EX_BAD ? ex : ex2;
  }
  free(pinned);
  haveNext = false;
  C2Proto::keyTag(serverPub, skTag);
  Preferences p;
  if (p.begin(NVS, false)) {
    p.putBytes("spub", serverPub, C2Proto::PUB_SIZE);
    p.remove("spub2");
    p.end();
  }
  Serial.printf("[C2]   Serverschluessel gewechselt (%s)\n", skTag);
  return EX_OK;
}

// What a poll's answer says about a new server key: kept for the handshake
// that will need it. True if this is news.
static bool noteNewKey(const char *b64) {
  uint8_t *next = ext(C2Proto::PUB_SIZE);
  char tag[C2Proto::KEY_TAG_SIZE];
  bool news = false;
  if (next && serverPub && C2Proto::announcedKey(b64, serverPub, next)) {
    C2Proto::keyTag(next, tag);
    if (!haveNext || strcmp(tag, skTag)) {
      Preferences p;
      if (p.begin(NVS, false)) {
        news = p.putBytes("spub2", next, C2Proto::PUB_SIZE) == C2Proto::PUB_SIZE;
        p.end();
      }
      if (news) {
        haveNext = true;
        strlcpy(skTag, tag, sizeof(skTag));
        Serial.printf("[C2]   neuer Serverschluessel angekuendigt (%s)\n", skTag);
      }
    }
  }
  free(next);
  return news;
}

// Sends one message and reads the answer on the current session. With
// once = true a request that gets no answer is not repeated and the session is
// kept: the caller seals a fresh record and tries again.
static Ex exchange(JsonDocument &msg, JsonDocument &reply, bool once = false) {
  String plain;
  serializeJson(msg, plain);
  const size_t MAX_REPLY = 8192;       // commands are small; nothing here needs the protocol's 32 kB
  size_t bodyLen = C2Proto::ID_SIZE + plain.length() + C2Proto::RECORD_OVERHEAD, n = 0, got = 0;
  uint8_t *body = (uint8_t *)malloc(bodyLen), *resp = (uint8_t *)malloc(MAX_REPLY);
  Ex ex = EX_BAD;
  if (body && resp) {
    memcpy(body, session.id, C2Proto::ID_SIZE);
    if (session.seal((const uint8_t *)plain.c_str(), plain.length(), body + C2Proto::ID_SIZE, n)) {
      ex = fromHttp(request(serverUrl, "/v1/msg", body, bodyLen, resp, MAX_REPLY, got, once ? 1 : 3));
      if (ex == EX_OK) {
        // decrypts in place: the plaintext is shorter than the record
        if (!session.open(resp, got, resp, n) || deserializeJson(reply, (const char *)resp, n)) ex = EX_BAD;
      }
    }
  }
  free(body);
  free(resp);
  if (ex != EX_OK && !(once && ex == EX_NET)) haveSession = false;
  return ex;
}

// ---------------------------------------------------------------- commands

static uint32_t fwSeq = 0;               // "fwseq" in the NVS: when the installed package was signed

static void fillStatus(JsonObject o) {
  o["fw"] = FW_VERSION;
  o["lang"] = LANG_CODE;
  o["hw"] = FW_BOARD_ID;                 // which packages fit: the server offers and sends only those
  o["seq"] = fwSeq;                      // and only a package signed later than this one is taken
  // who is on the serial ports here, at the device: the control center shows it
  o["web"] = Net::webClients();
  o["tcp"] = Net::tcpConnected();
  o["bt"] = Ble::state() == Ble::CONNECTED;
  o["ctl"] = settings.c2Control;         // may the server change things here?
  if (unattLeft) o["unatt"] = (uint32_t)unattLeft;
  {
    Lock l;
    o["cfgs"] = serialized(inventory);   // the stored configurations: name and size
  }
  JsonObject sd = o["sd"].to<JsonObject>();
  sd["on"] = Sd::mounted();
  if (Sd::stick().on) sd["stick"] = true;
  if (Sd::mounted()) {
    sd["mb"] = (uint32_t)Sd::totalMb();
    sd["used"] = (uint32_t)Sd::usedMb();
    Lock l;
    sd["files"] = serialized(sdFiles);   // what lies in the card's top folder
  }
  o["uptime_s"] = millis() / 1000;
  o["heap"] = ESP.getFreeHeap();
  o["rssi"] = WiFi.RSSI();
  if (Power::present()) {
    o["bat_pct"] = Power::pct();
    o["bat_mv"] = Power::mv();
    o["charge"] = Power::chargeName();
  }
}

// Takes a command from the server. "ping" is answered here; everything else is
// queued for the main loop, which knows what it will and will not do.
static void run(JsonObject cmd) {
  const char *type = cmd["type"] | "";
  uint32_t id = cmd["id"] | 0UL;
  const char *refused = nullptr;
  if (!strcmp(type, "ping")) {
    JsonObject res = results.as<JsonArray>().add<JsonObject>();
    res["id"] = id;
    res["ok"] = true;
    res["out"] = "pong";
  } else {
    MainJob job;
    job.id = id;
    job.type = type;
    if (cmd["args"].is<JsonObject>()) serializeJson(cmd["args"], job.args);
    Lock l;
    if (jobs.size() >= MAX_JOBS) refused = "too many commands at once";
    else jobs.push_back(job);
  }
  if (refused) {
    JsonObject res = results.as<JsonArray>().add<JsonObject>();
    res["id"] = id;
    res["ok"] = false;
    res["out"] = refused;
  }
  Serial.printf("[C2]   Auftrag %lu \"%s\" %s\n", (unsigned long)id, type, refused ? refused : "angenommen");
}

// Moves the main loop's answers into the next poll. Returns how many there were.
static size_t collectJobResults() {
  std::vector<String> done;
  {
    Lock l;
    done.swap(jobResults);
    // The "yes" and the change to CON_OPEN happen under one lock, so a
    // console that is open here has its answer in this poll or an earlier one.
    conAtCollect = con.state == CON_OPEN || con.state == CON_END;
  }
  for (const String &r : done) {
    JsonDocument d;
    if (deserializeJson(d, r)) continue;
    if (!results.is<JsonArray>()) results.to<JsonArray>();
    results.as<JsonArray>().add(d);
  }
  return done.size();
}

// ---------------------------------------------------------------- enroll / poll

// open: without an invitation, at the server whose address somebody typed in
// (tok.url). There is no secret then and nothing to check the server's key
// against - it is taken as shown, and the code on the display, typed in at the
// server, is what ties the two together.
static void doEnroll(const String &text, bool open = false) {
  C2Proto::Token tok;
  if (open) {
    tok.url = text.c_str();
    memset(tok.id, 0, sizeof(tok.id));
    memset(tok.secret, 0, sizeof(tok.secret));
  } else if (!C2Proto::parseToken(text.c_str(), tok)) return publish(TR("unusable token", "Token unbrauchbar", "token invalido"));
  if (WiFi.status() != WL_CONNECTED) return publish(TR("Wi-Fi client not connected", "kein WLAN-Client verbunden", "cliente Wi-Fi nao conectado"));
  State before = state;
  state = ENROLLING;
  publish("");
  Serial.printf("[C2]   Anmeldung bei %s\n", tok.url.c_str());

  uint8_t *key = (uint8_t *)malloc(C2Proto::PUB_SIZE);
  size_t got = 0;
  const char *err = nullptr;
  int code = key ? request(tok.url, "/v1/server-key", nullptr, 0, key, C2Proto::PUB_SIZE, got) : -1;
  if (code != 200 || got != C2Proto::PUB_SIZE) err = TR("server not reachable", "Server nicht erreichbar", "servidor inacessivel");
  else if (!open && !C2Proto::tokenPinsServer(tok, key)) err = TR("the server key does not match the token", "Serverschluessel passt nicht zum Token", "a chave do servidor nao confere com o token");
  if (err) {
    free(key);
    state = before;
    Serial.printf("[C2]   Anmeldung gescheitert: %s (HTTP %d)\n", err, code);
    return publish(err);
  }
  // From here on the old registration is gone: one server at a time.
  if (!haveKey) {
    C2Proto::generateKey(priv);
    haveKey = true;
  }
  free(serverPub);
  serverPub = key;
  serverUrl = tok.url;
  haveNext = false;
  C2Proto::keyTag(serverPub, skTag);
  results.clear();
  publishId();

  Ex ex = handshake(C2Proto::KIND_ENROLL, &tok);
  JsonDocument msg, reply;
  if (ex == EX_OK) {
    msg["t"] = "enroll";
    msg["name"] = devName;
    msg["info"]["fw"] = FW_VERSION;
    msg["info"]["board"] = BOARD_NAME;
    msg["info"]["hw"] = FW_BOARD_ID;
    ex = exchange(msg, reply);
  }
  if (ex != EX_OK) {
    free(serverPub);
    serverPub = nullptr;
    serverUrl.clear();
    skTag[0] = 0;
    state = OFF;
    err = ex == EX_REFUSED ? (open ? TR("the server takes no new devices now", "Server nimmt gerade keine neuen Geraete", "o servidor nao aceita novos agora")
                                   : TR("token refused (used or expired)", "Token abgelehnt (verbraucht oder abgelaufen)", "token recusado (usado ou expirado)"))
          : ex == EX_NET ? TR("server not reachable", "Server nicht erreichbar", "servidor inacessivel") : TR("enrollment failed", "Anmeldung gescheitert", "falha no registro");
    Serial.printf("[C2]   Anmeldung gescheitert: %s\n", err);
    Preferences p;                       // the previous server (if any) is no longer valid either
    if (p.begin(NVS, false)) {
      p.remove("url");
      p.remove("spub");
      p.remove("spub2");
      p.remove("st");
      p.remove("sas");
      p.end();
    }
    return publish(err);
  }
  state = PENDING;
  Preferences p;
  if (p.begin(NVS, false)) {
    p.putBytes("key", priv, sizeof(priv));
    p.putString("url", serverUrl.c_str());
    p.putBytes("spub", serverPub, C2Proto::PUB_SIZE);
    p.remove("spub2");
    p.putUChar("st", state);
    p.putString("sas", session.sas);
    p.remove("want");                    // it is in; from here on the pinned server counts
    p.end();
  }
  wantUrl.clear();
  publish("", session.sas);
  Serial.printf("[C2]   angemeldet, wartet auf Bestaetigung - Code %s\n", session.sas);
  Display::wake();
  Display::message(TR("Control center: code", "Leitstelle: Code", "Central: codigo"), session.sas, 60000);
  nextPollAt = millis() + 5000;
  retryMs = RETRY_MIN_MS;
}

static void doPoll() {
  uint32_t now = millis();
  auto retry = [&](const char *err) {
    nextPollAt = now + retryMs;
    retryMs = min(retryMs * 2, RETRY_MAX_MS);
    publish(err);
  };
  Ex ex = EX_OK;
  JsonDocument msg, reply;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!haveSession) {
      ex = sessionHandshake();
      if (ex != EX_OK) break;
    }
    collectJobResults();
    msg.clear();
    msg["t"] = "poll";
    if (skTag[0]) msg["sk"] = skTag;     // the newest server key this device holds
    fillStatus(msg["status"].to<JsonObject>());
    if (results.is<JsonArray>() && results.size()) msg["results"] = results;
    ex = exchange(msg, reply);
    if (ex != EX_REHANDSHAKE) break;     // the server forgot the session (restart, one hour passed)
  }
  if (ex == EX_REFUSED) {
    if (state != REFUSED) Serial.println("[C2]   vom Server abgewiesen (gesperrt oder unbekannt)");
    state = REFUSED;
    storeState(nullptr);
    nextPollAt = now + REFUSED_RETRY_MS;
    return publish(TR("refused by the server", "vom Server abgewiesen", "recusado pelo servidor"));
  }
  if (ex != EX_OK) return retry(ex == EX_NET ? TR("server not reachable", "Server nicht erreichbar", "servidor inacessivel") : TR("unusable answer from the server", "Antwort des Servers unbrauchbar", "resposta invalida do servidor"));

  if (rebootWanted)                                         // has the answer to "reboot", "set" or "update" arrived?
    for (JsonObject r : results.as<JsonArray>())
      if ((r["id"] | 0UL) == rebootAfter) rebootNow = true;
  if (conAtCollect) conAnnounced = true;
  results.clear();
  retryMs = RETRY_MIN_MS;
  const char *st = reply["state"] | "";
  State was = state;
  state = !strcmp(st, "active") ? ACTIVE : PENDING;
  if (state != was) {
    storeState(nullptr);
    Serial.printf("[C2]   Zustand: %s\n", stateName(state));
    if (state == ACTIVE) Display::message(TR("Control center", "Leitstelle", "Central"), TR("connected", "verbunden", "conectado"), 4000);
  }
  for (JsonObject cmd : reply["cmds"].as<JsonArray>()) {
    if (!results.is<JsonArray>()) results.to<JsonArray>();
    run(cmd);
  }
  pollS = constrain((int)(reply["poll_s"] | 30), 5, 3600);
  // answers go back at once, not a poll interval later
  nextPollAt = millis() + (results.size() ? 500 : pollS * 1000UL);
  const char *rekey = reply["rekey"] | "";
  if (rekey[0] && noteNewKey(rekey)) nextPollAt = millis() + 500;   // tell the server that it is here
  {
    Lock l;
    shared.lastOkMs = millis();
    shared.polls++;
  }
  publish("");
}

// ---------------------------------------------------------------- remote console, task side

static uint32_t conOkMs = 0;             // last console message the server answered

// Why a remote console ended travels over the wire in English - the server's
// interface translates it for whoever reads it there. What this device shows
// is in its own language.
static const char *here(const char *reason) {
  static const struct { const char *wire, *text; } KNOWN[] = {
    {"closed at the device", TR("closed at the device", "am Geraet beendet", "encerrado no dispositivo")},
    {"time is up", TR("time is up", "Zeit abgelaufen", "tempo esgotado")},
    {"port switched off", TR("port switched off", "Port abgeschaltet", "porta desligada")},
    {"Wi-Fi lost", TR("Wi-Fi lost", "WLAN getrennt", "Wi-Fi perdido")},
    {"refused by the server", TR("refused by the server", "vom Server abgewiesen", "recusado pelo servidor")},
    {"server not reachable", TR("server not reachable", "Server nicht erreichbar", "servidor inacessivel")},
    {"signed off", TR("signed off", "abgemeldet", "desconectado")},
    {"closed by the administrator", TR("closed by the administrator", "vom Administrator beendet", "encerrado pelo administrador")},
    {"device revoked", TR("device revoked", "Geraet gesperrt", "dispositivo bloqueado")},
    {"the device no longer answers", TR("no contact with the server", "keine Verbindung zum Server", "sem contato com o servidor")},
    {"server restarted", TR("server restarted", "Server neu gestartet", "servidor reiniciado")},
  };
  for (const auto &k : KNOWN)
    if (!strcmp(reason, k.wire)) return k.text;
  return reason;
}

static void conFinish(const char *reason) {
  {
    Lock l;
    delete con.stream;
    con.stream = nullptr;
    conForgetKeys();
    con.state = CON_NONE;
    conBusy = false;
  }
  conAnnounced = false;
  conOkMs = 0;
  keepAlive = false;
  connClose();
  nextPollAt = millis() + 1000;
  Serial.printf("[C2]   Fernkonsole beendet: %s\n", reason);
  char text[40];
  snprintf(text, sizeof(text), "%s", here(reason));
  Display::message(TR("Remote console ended", "Fernkonsole beendet", "Console remoto encerrado"), text, 5000);
}

// One question to the server: here is what the port has sent, what has been typed?
static void conStep() {
  JsonDocument msg, reply;
  size_t n;
  char endReason[48] = "";
  {
    Lock l;
    if (con.state != CON_OPEN && con.state != CON_END) return;
    if (con.state == CON_END) strlcpy(endReason, con.reason, sizeof(endReason));
    uint32_t left = con.state == CON_OPEN && (int32_t)(con.untilMs - millis()) > 0 ? (con.untilMs - millis()) / 1000 : 0;
    n = C2Console::buildMessage(msg, con.id, *con.stream, left, endReason);
    if (con.e2ePub[0]) msg["e2e"] = (const char *)con.e2ePub;   // the browser's other half
  }
  if (!conOkMs) conOkMs = millis();
  keepAlive = true;
  Ex ex = haveSession ? exchange(msg, reply, true) : EX_REHANDSHAKE;
  if (ex == EX_REHANDSHAKE) {            // an hour has passed, or the server was restarted
    ex = sessionHandshake();
    if (ex == EX_OK) return;             // next round, on the new session
  }
  if (ex == EX_REFUSED) {
    conFinish("refused by the server");
    nextPollAt = millis();               // the poll finds out what that means
    return;
  }
  if (ex != EX_OK) {
    if (endReason[0] || millis() - conOkMs > CON_LOST_MS) conFinish(endReason[0] ? endReason : "server not reachable");
    else vTaskDelay(pdMS_TO_TICKS(500));
    return;
  }
  conOkMs = millis();
  const char *end = "signed off";
  {
    Lock l;
    if (con.stream) {
      C2Console::accepted(*con.stream, n);
      end = C2Console::applyReply(reply, *con.stream);
    }
  }
  if (endReason[0]) conFinish(endReason);
  else if (end) conFinish(end);
}

// ---------------------------------------------------------------- firmware update, task side

static void answer(uint32_t id, bool ok, const String &outJson);
static String quoted(const char *text);

// One question to the server about the package, with patience: a weak WLAN
// loses requests, and the session may run out in the middle of a download.
static bool updAsk(uint32_t id, int32_t off, JsonDocument &reply, const char *what = "fw") {
  uint32_t t0 = millis();
  for (;;) {
    JsonDocument msg;
    msg["t"] = what;
    msg["id"] = id;
    if (off < 0) msg["head"] = true;
    else msg["off"] = off;
    keepAlive = true;
    Ex ex = haveSession ? exchange(msg, reply, true) : EX_REHANDSHAKE;
    if (ex == EX_OK) return true;
    if (ex == EX_REHANDSHAKE) ex = sessionHandshake();
    if (ex == EX_REFUSED || ex == EX_BAD || millis() - t0 > 90000) return false;
    if (ex != EX_OK) vTaskDelay(pdMS_TO_TICKS(1500));
  }
}

// Fetches the package behind an "update" command, checks it and installs it.
// Nothing is made bootable before the signature over the manifest has held
// and the image has the hash the manifest names.
static void doUpdate(uint32_t id) {
  auto refuse = [&](const char *why) {
    Serial.printf("[C2]   Firmware-Update abgelehnt: %s\n", why);
    Display::message("Firmware", TR("update refused", "Update abgelehnt", "atualizacao recusada"), 4000);
    answer(id, false, quoted(why));
    nextPollAt = millis();
  };
#ifndef UPDATE_KEY_PRESENT
  return refuse("this firmware was built without an update key");
#else
  JsonDocument reply;
  if (!updAsk(id, -1, reply)) return refuse("the package could not be fetched");
  // Everything kept across the signature check lives in the external RAM: the
  // internal heap is the scarce one.
  const char *m64 = reply["m"] | "", *sig64 = reply["sig"] | "";
  size_t manifestLen = strlen(m64), sigLen = 0;
  uint8_t *work = ext(C2Update::MAX_MANIFEST + C2Update::SIG_SIZE + 4096);
  char *manifest = (char *)work;
  uint8_t *sig = work + C2Update::MAX_MANIFEST, *buf = sig + C2Update::SIG_SIZE;
  const char *err = work ? nullptr : "not enough memory";
  if (!err && (!manifestLen || manifestLen >= C2Update::MAX_MANIFEST ||
               mbedtls_base64_decode(sig, C2Update::SIG_SIZE, &sigLen, (const uint8_t *)sig64, strlen(sig64))))
    err = "the package is not readable";
  if (!err) memcpy(manifest, m64, manifestLen);
  reply.clear();
  C2Update::Manifest m;
  if (!err) {
    Preferences p;
    p.begin(NVS, true);
    uint32_t installed = p.getUInt("fwseq", 0);
    p.end();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    uint32_t room = next ? next->size : 0;
    const char *verdict = "not enough memory for the signature check";
    connClose();
    Serial.printf("[C2]   Firmware-Update: Signatur wird geprueft (Heap %u B, groesster Block %u B, Puffer %s)\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), esp_ptr_external_ram(work) ? "extern" : "intern");
    big([&] { verdict = C2Update::check(manifest, manifestLen, sig, sigLen, UPDATE_KEY, FW_BOARD_ID, installed, room, m); });
    Serial.printf("[C2]   Signaturpruefung: %lu B Stack benutzt\n", (unsigned long)bigUsed);
    err = verdict;
  }
  if (err) {
    free(work);
    return refuse(err);
  }
  Serial.printf("[C2]   Firmware-Update: Signatur gueltig, %s (%s), %lu Bytes\n", m.fw, m.lang, (unsigned long)m.size);
  if (!Update.begin(m.size)) {
    free(work);
    return refuse("the device cannot take an update right now");
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  uint32_t off = 0;
  int shown = -1;
  err = nullptr;
  while (off < m.size && !err) {
    int pct = (int)((uint64_t)off * 100 / m.size);
    if (pct / 5 != shown) {
      shown = pct / 5;
      char line[24];
      snprintf(line, sizeof(line), "%s  %d %%", m.fw, pct);
      Display::message(TR("Firmware update", "Firmware-Update", "Atualizacao"), line, 120000);
    }
    if (!updAsk(id, (int32_t)off, reply)) { err = "the download broke off"; break; }
    const char *d = reply["d"] | "";
    size_t n = 0;
    if (mbedtls_base64_decode(buf, 4096, &n, (const uint8_t *)d, strlen(d)) || !n || off + n > m.size) err = "the download is damaged";
    else if (Update.write(buf, n) != n) err = "writing to the flash failed";
    else {
      mbedtls_sha256_update_ret(&sha, buf, n);
      off += n;
    }
    reply.clear();
  }
  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&sha, hash);
  mbedtls_sha256_free(&sha);
  free(work);
  if (!err && memcmp(hash, m.sha256, 32)) err = "the image is not the one that was signed";
  if (err || !Update.end(true)) {
    Update.abort();
    return refuse(err ? err : "the image was not accepted by the flash");
  }
  Preferences p;
  if (p.begin(NVS, false)) {
    p.putUInt("fwseq", m.seq);
    p.end();
  }
  Serial.printf("[C2]   Firmware-Update: %s (%s) installiert, Neustart\n", m.fw, m.lang);
  Display::message(TR("Firmware update", "Firmware-Update", "Atualizacao"), TR("installed, restarting", "installiert, Neustart", "instalada, reiniciando"), 10000);
  char out[80];
  snprintf(out, sizeof(out), "updated to %s (%s), restarting", m.fw, m.lang);
  answer(id, true, quoted(out));
  rebootAfter = id;
  rebootWanted = true;                   // carried out once the answer has reached the server
  nextPollAt = millis();
#endif
}

// Fetches the file behind an "sd_put" command and writes it to the SD card.
// The server names size and MD5 sum first. What has arrived stays on the card
// when the connection breaks or the device restarts, and is continued from
// there; at the end the file is read back from the card and has to have the
// sum, only then does it get its name.
static void sdJobKeep(uint32_t id) {     // across a restart: the command the device is working on
  Preferences p;
  if (!p.begin(NVS, false)) return;
  if (id) p.putUInt("sdjob", id);
  else p.remove("sdjob");
  p.end();
}

static void sdShow(int pct) {
  char line[24];
  snprintf(line, sizeof(line), TR("checking  %d %%", "pruefe  %d %%", "verificando  %d %%"), pct);
  Display::message(TR("To the SD card", "Auf die SD-Karte", "Para o cartao SD"), line, 120000);
}

static void doSdPut(uint32_t id) {
  auto refuse = [&](const char *why) {
    Serial.printf("[C2]   Datei fuer die SD-Karte: %s\n", why);
    Display::message(TR("SD card", "SD-Karte", "Cartao SD"), TR("transfer failed", "Uebertragung gescheitert", "transferencia falhou"), 4000);
    sdJobKeep(0);
    answer(id, false, quoted(why));
    nextPollAt = millis();
  };
  JsonDocument reply;
  if (!updAsk(id, -1, reply, "img")) return refuse("the file could not be fetched");
  String name = reply["n"] | "";
  uint32_t size = reply["size"] | 0UL;
  uint8_t want[16];
  String md5 = reply["md5"] | "";
  bool sumOk = md5.length() == 32;
  for (int i = 0; sumOk && i < 16; i++) {
    char b[3] = {md5[2 * i], md5[2 * i + 1], 0};
    char *end;
    want[i] = (uint8_t)strtoul(b, &end, 16);
    sumOk = *end == 0;
  }
  reply.clear();
  if (!size || !sumOk) return refuse("the server's answer is not usable");
  uint8_t *buf = ext(4096);
  if (!buf) return refuse("not enough memory");
  uint32_t off = 0;
  const char *err = Sd::putBegin(name.c_str(), size, md5.substring(0, 8).c_str(), off);
  if (err) {
    free(buf);
    return refuse(err);
  }
  sdJobKeep(id);
  Serial.printf("[C2]   SD-Karte: lade \"%s\", %lu Bytes%s", name.c_str(), (unsigned long)size, off ? "" : "\n");
  if (off) Serial.printf(" - setzt bei %lu fort\n", (unsigned long)off);
  int shown = -1, misses = 0;
  bool paused = false;
  while (off < size && !err) {
    int pct = (int)((uint64_t)off * 100 / size);
    if (pct != shown) {
      shown = pct;
      char line[24];
      snprintf(line, sizeof(line), "%d %%", pct);
      Display::message(TR("To the SD card", "Auf die SD-Karte", "Para o cartao SD"), line, 120000);
    }
    if (!settings.c2Control) { err = "stopped at the device - send it again to continue"; paused = true; break; }
    bool got = updAsk(id, (int32_t)off, reply, "img");
    size_t n = 0;
    const char *d = reply["d"] | "";
    if (got && (mbedtls_base64_decode(buf, 4096, &n, (const uint8_t *)d, strlen(d)) || !n || off + n > size)) { err = "the download is damaged"; break; }
    bool stored = got && Sd::putWrite(buf, n);
    if (!stored) {
      // The link is down, or the server is, or it has forgotten the session:
      // wait and ask again, for half an hour. Nothing is lost - the file is
      // closed and continued at the place the card says it has. Closing it
      // also matters for the heap: an open file and the connection cut it up.
      Sd::putPause();
      reply.clear();
      connClose();
      paused = true;
      if (++misses > 360) { err = got ? "writing to the SD card failed - send it again to continue" : "the download broke off - send it again to continue"; break; }
      if (misses % 6 == 1) Serial.printf("[C2]   SD-Karte: %s bei %lu, neuer Versuch %d\n", got ? "Schreiben gescheitert" : "keine Antwort", (unsigned long)off, misses);
      vTaskDelay(pdMS_TO_TICKS(got ? 300 : 5000));
      if (!haveSession && WiFi.status() == WL_CONNECTED) sessionHandshake();
      if ((err = Sd::putBegin(name.c_str(), size, md5.substring(0, 8).c_str(), off))) break;
      paused = false;
      continue;
    }
    misses = 0;
    off += n;
    reply.clear();
  }
  free(buf);
  if (paused) Sd::putPause();            // what is there stays for the next attempt
  else err = Sd::putFinish(size, want, sdShow);   // also after "damaged": that removes the part
  inventoryAt = 0;                       // the next status names it
  if (err) return refuse(err);
  sdJobKeep(0);
  Serial.printf("[C2]   SD-Karte: \"%s\" gespeichert, MD5 stimmt\n", name.c_str());
  Display::message(TR("SD card", "SD-Karte", "Cartao SD"), TR("file stored", "Datei gespeichert", "arquivo salvo"), 4000);
  answer(id, true, quoted((String("stored ") + name + " (" + size + " bytes, MD5 " + md5 + " checked on the card)").c_str()));
  nextPollAt = millis();
}

static void task(void *) {
  const char *failed = TR("not enough memory", "zu wenig Speicher", "memoria insuficiente");
  big([&] { failed = C2Proto::selfTest(); });
  Serial.printf("[C2]   Selbsttest: %lu B Stack benutzt\n", (unsigned long)bigUsed);
  if (failed) {
    state = FAILED;
    char buf[64];
    snprintf(buf, sizeof(buf), TR("self-test: %s", "Selbsttest: %s", "autoteste: %s"), failed);
    publish(buf);
    Serial.printf("[C2]   %s - Leitstelle abgeschaltet\n", buf);
    vTaskDelete(nullptr);
  }
  loadStored();
  publishId();
  publish();
  // tools/fw_sign.py looks for this marker in an image, to not sign one language as another
  Serial.printf("[C2]   %s\n", "UI_LANG=" LANG_CODE ";");
  // the same for the board: the package is signed for the hardware this image was built for
  Serial.printf("[C2]   %s\n", "FW_BOARD=" FW_BOARD_ID ";");
  Serial.printf("[C2]   Selbsttest ok, %s%s%s\n", stateName(state), serverUrl.empty() ? "" : " bei ", serverUrl.c_str());
  nextPollAt = millis() + 8000;          // give the WLAN client time to join
  bool wifiWas = false;
  for (;;) {
    String token;
    bool forget, changed = false;
    {
      Lock l;
      token = reqToken;
      reqToken = "";
      if (reqUrl.length() && serverPub) {
        serverUrl = reqUrl.c_str();
        Preferences p;
        if (p.begin(NVS, false)) {
          p.putString("url", serverUrl.c_str());
          p.end();
        }
        Serial.printf("[C2]   Server jetzt unter %s\n", serverUrl.c_str());
        connClose();
        haveSession = false;
        reqRetry = true;
      } else if (reqUrlSet && !serverPub) {
        wantUrl = reqUrl.c_str();
        Preferences p;
        if (p.begin(NVS, false)) {
          if (wantUrl.empty()) p.remove("want");
          else p.putString("want", wantUrl.c_str());
          p.end();
        }
        wantAt = millis();
        wantRetryMs = RETRY_MIN_MS;
        changed = true;
      }
      reqUrl = "";
      reqUrlSet = false;
      forget = reqForget;
      reqForget = false;
    }
    if (forget) forgetAll();
    if (reqRetry) publish();             // a changed address shows at once
    if (changed) publish("");
    if (token.length()) doEnroll(token);
    bool answers;
    {
      Lock l;
      answers = !jobResults.empty();
    }
    if (answers && state == ACTIVE) nextPollAt = millis();   // answers go back at once
    bool wifi = WiFi.status() == WL_CONNECTED;
    bool online = serverPub && wifi;
    // The moment the WLAN is there the device reports in - it does not sit out
    // what is left of a wait that began while the server could not be reached.
    // The same when somebody asks for it at the device.
    if (serverPub && ((wifi && !wifiWas) || reqRetry)) {
      if (wifi) {
        Serial.printf("[C2]   %s - meldet sich sofort\n", reqRetry ? "Verbinden verlangt" : "WLAN verbunden");
        connClose();
        retryMs = RETRY_MIN_MS;
        nextPollAt = millis() + (reqRetry ? 0 : 1500);   // DHCP has just finished: let the stack settle
      }
    } else if (!serverPub && wifi && (!wifiWas || reqRetry)) {
      wantAt = millis() + (reqRetry ? 0 : 1500);
      wantRetryMs = RETRY_MIN_MS;
      reqRetry = false;
    } else if (serverPub && !wifi && wifiWas) publish(TR("Wi-Fi client not connected", "kein WLAN-Client verbunden", "cliente Wi-Fi nao conectado"));
    if (wifi || !serverPub) reqRetry = false;            // without WLAN the wish waits for it
    wifiWas = wifi;
    if (!serverPub && !wantUrl.empty() && wifi && (int32_t)(millis() - wantAt) >= 0) {
      doEnroll(wantUrl.c_str(), true);
      wantAt = millis() + wantRetryMs;   // not taken: ask again, with growing pauses
      wantRetryMs = min(wantRetryMs * 2, RETRY_MAX_MS);
      continue;
    }
    if (online && (int32_t)(millis() - nextPollAt) >= 0) doPoll();
    if (sdJob && online && state == ACTIVE) {
      uint32_t id = sdJob;
      doSdPut(id);
      sdJob = 0;
    }
    if (updateJob && online && state == ACTIVE) {
      uint32_t id = updateJob;
      doUpdate(id);
      updateJob = 0;
    }
    // While a console is open the task asks without pause; the server holds
    // each question for a moment when there is nothing to say.
    bool conLive = false, conClosed = false;
    char why[48] = "";
    if (conBusy) {
      Lock l;
      conLive = con.state == CON_OPEN || con.state == CON_END;
      conClosed = con.state == CON_END;
      strlcpy(why, con.reason, sizeof(why));
    }
    if (!conLive) conOkMs = 0;
    else if (!conAnnounced || state != ACTIVE) {
      // the server does not know of it yet; if it has ended already, there is nothing to tell
      if (conClosed) conFinish(why);
      conLive = false;
    } else if (online) conStep();
    else {
      if (!conOkMs) conOkMs = millis();
      if (millis() - conOkMs > CON_LOST_MS) conFinish("Wi-Fi lost");
    }
    vTaskDelay(pdMS_TO_TICKS(conLive && online ? 5 : 200));
  }
}

// ---------------------------------------------------------------- interface

void begin() {
  mux = xSemaphoreCreateMutex();
  memset(&shared, 0, sizeof(shared));
  strlcpy(devName, settings.apSsid.c_str(), sizeof(devName));
  unattLeft = unattSavedS = settings.c2UnattS > UNATTENDED_MAX_S ? UNATTENDED_MAX_S : settings.c2UnattS;
  unattTickMs = millis();
  {                                      // a file for the SD card that a restart interrupted is continued
    Preferences p;
    if (p.begin(NVS, true)) {
      sdJob = p.getUInt("sdjob", 0);
      fwSeq = p.getUInt("fwseq", 0);
      p.end();
    }
  }
  // core 0, next to WiFi and the display: the serial bridge has core 1 to itself
  xTaskCreatePinnedToCore(task, "c2", TASK_STACK, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------- main loop side

static uint32_t playJob = 0;             // command that started the configuration now playing

static void answer(uint32_t id, bool ok, const String &outJson) {
  String r = String("{\"id\":") + id + ",\"ok\":" + (ok ? "true" : "false") + ",\"out\":" + outJson + "}";
  Lock l;
  jobResults.push_back(r);
}

static String quoted(const char *text) {
  JsonDocument d;
  d.set(text);
  String out;
  serializeJson(d, out);
  return out;
}

static String playReport() {
  JsonDocument d;
  d["name"] = Player::name();
  d["port"] = Player::port() + 1;
  d["line"] = Player::line();
  d["lines"] = Player::lines();
  d["result"] = Player::result();
  String out;
  serializeJson(d, out);
  return out;
}

// The whole list of what this device does for the server. Reading is always
// allowed; anything that changes something needs settings.c2Control, which can
// only be switched at the device itself.
static void execute(const MainJob &job) {
  JsonDocument args;
  if (job.args.length()) deserializeJson(args, job.args);
  const char *denied = "not allowed at the device (Menu -> Network -> Control center)";
  if (job.type == "status") answer(job.id, true, Net::deviceStatus());
  else if (job.type == "settings") answer(job.id, true, Net::deviceSettings());
  else if (job.type == "configs") answer(job.id, true, Configs::listJson());
  else if (job.type == "play") {
    const char *name = args["name"] | "";
    int port = args["port"] | 1;
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else if (Player::state() == Player::RUNNING) answer(job.id, false, quoted("a configuration is being played already"));
    else if (port < 1 || port > MAX_PORTS || !Player::start(name, port - 1)) answer(job.id, false, quoted(port < 1 || port > MAX_PORTS ? "unknown port" : Player::result()));
    else {
      playJob = job.id;                  // answered when the player has finished
      Serial.printf("[C2]   spielt \"%s\" auf Port %d ab\n", name, port);
    }
  } else if (job.type == "reboot") {
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else {
      answer(job.id, true, quoted("restarting"));
      rebootAfter = job.id;
      rebootWanted = true;               // carried out once the answer has reached the server
    }
  } else if (job.type == "update") {
    // A signed firmware package. Whether it is the owner's is decided by the
    // signature (doUpdate), whether now is a good moment, here.
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else if (updateJob || sdJob) answer(job.id, false, quoted("an update is under way already"));
    else if (conBusy || Player::state() == Player::RUNNING || Xfer::active()) answer(job.id, false, quoted("the device is busy (console, playback or file transfer)"));
    else if (Power::low()) answer(job.id, false, quoted("the battery is too low for an update"));
    else {
      Serial.println("[C2]   Firmware-Update angefragt");
      updateJob = job.id;
    }
  } else if (job.type == "sd_delete") {
    const char *name = args["name"] | "";
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else if (sdJob) answer(job.id, false, quoted("a transfer is under way already"));
    else {
      const char *err = Sd::removeRoot(name);
      inventoryAt = 0;                   // the next status no longer names it
      Serial.printf("[C2]   SD-Karte: \"%s\" loeschen: %s\n", name, err ? err : "geloescht");
      answer(job.id, !err, quoted(err ? err : "deleted"));
    }
  } else if (job.type == "sd_put") {
    // A file from the server's library to the SD card. It takes the device's
    // channel for as long as it lasts - minutes for megabytes.
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else if (sdJob || updateJob) answer(job.id, false, quoted("a transfer is under way already"));
    else if (!Sd::mounted()) answer(job.id, false, quoted(Sd::stick().on ? "the SD card is lent to the USB port" : "no SD card in the device"));
    else if (conBusy || Xfer::active()) answer(job.id, false, quoted("the device is busy (console, playback or file transfer)"));
    else {
      Serial.println("[C2]   Datei fuer die SD-Karte angefragt");
      sdJobKeep(job.id);                 // from here on a restart does not lose it
      sdJob = job.id;
    }
  } else if (job.type == "set") {
    // The settings of the web interface, from afar. Whether the server may
    // change things at all (c2Control) is not among them: that stays here.
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else {
      const char *err = Net::applySettings(job.args, true);
      if (err) answer(job.id, false, quoted(err));
      else {
        Serial.println("[C2]   Einstellungen von der Leitstelle gespeichert, Neustart");
        answer(job.id, true, quoted("saved, restarting"));
        rebootAfter = job.id;
        rebootWanted = true;
      }
    }
  } else if (job.type == "config_get" || job.type == "config_put" || job.type == "config_delete") {
    // Stored configurations may hold passwords: reading one out needs the same
    // permission as changing something.
    const char *name = args["name"] | "";
    String text;
    if (!settings.c2Control) answer(job.id, false, quoted(denied));
    else if (job.type == "config_get") {
      if (!Configs::read(name, text)) answer(job.id, false, quoted("configuration not found"));
      else if (text.length() > 12000) answer(job.id, false, quoted("configuration too large for the control center"));
      else {
        JsonDocument d;
        d["name"] = name;
        d["text"] = text;
        String out;
        serializeJson(d, out);
        answer(job.id, true, out);
      }
    } else if (job.type == "config_put") {
      const char *err = Configs::save(name, args["text"] | "", name);
      answer(job.id, !err, quoted(err ? err : "stored"));
      Serial.printf("[C2]   Konfiguration \"%s\" von der Leitstelle: %s\n", name, err ? err : "gespeichert");
    } else {
      bool gone = Configs::remove(name);
      answer(job.id, gone, quoted(gone ? "deleted" : "configuration not found"));
    }
  } else if (job.type == "console") {
    // Never without a tap on the display - unless somebody at the device has
    // granted unattended access for a while (unattendedSet).
    int port = args["port"] | 1, minutes = constrain((int)(args["minutes"] | 15), 1, (int)CON_MAX_MIN);
    bool unattended = unattLeft > 0;
    {
      const char *no = nullptr;
      Lock l;
      if (con.state != CON_NONE) no = "a remote console is already open or asked for";
      else if (port < 1 || port > MAX_PORTS || !Bridge::enabled(port - 1)) no = "Port unbekannt";
      else if (!unattended && !Display::usable()) no = "the device has no display on which somebody could agree";
      else if (!unattended && (int32_t)(conRefuseUntil - millis()) > 0) no = "just refused at the device";
      if (no) {
        String r = String("{\"id\":") + job.id + ",\"ok\":false,\"out\":" + quoted(no) + "}";
        jobResults.push_back(r);
        return;
      }
      con.state = CON_ASK;
      conBusy = true;
      con.id = job.id;
      con.port = port - 1;
      con.minutes = minutes;
      con.askedMs = millis();
      conForgetKeys();
      // The browser's one-time key: the console runs encrypted between it and
      // this device, and the display shows the code to compare.
      const char *bk = args["e2e"] | "";
      uint8_t bpub[C2E2E::PUB], dpub[C2E2E::PUB];
      size_t bn = 0;
      if (*bk && !mbedtls_base64_decode(bpub, sizeof(bpub), &bn, (const uint8_t *)bk, strlen(bk)) && bn == C2E2E::PUB) {
        con.e2e = (C2E2E::Session *)heap_caps_calloc(1, sizeof(C2E2E::Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        auto rng = [](void *, unsigned char *out, size_t len) { esp_fill_random(out, len); return 0; };
        size_t pl = 0;
        if (con.e2e && C2E2E::start(*con.e2e, bpub, dpub, rng, nullptr) &&
            !mbedtls_base64_encode((uint8_t *)con.e2ePub, sizeof(con.e2ePub), &pl, dpub, sizeof(dpub)))
          con.e2ePub[pl] = 0;
        else conForgetKeys();
      }
    }
    Display::wake();
    if (unattended) {
      Serial.printf("[C2]   Fernkonsole angefragt: Port %d, %d min - unbeaufsichtigter Zugriff ist erlaubt (noch %lu s)\n", port, minutes, (unsigned long)unattLeft);
      consoleAnswer(true);
    } else Serial.printf("[C2]   Fernkonsole angefragt: Port %d, %d min - wartet auf Zustimmung am Display\n", port, minutes);
  } else if (job.type == "sd_backup" || job.type == "sd_restore") {
    // The encrypted backup on the SD card (sdbackup.cpp). It holds passwords
    // and keys, and a restore changes everything: both need the permission.
    const char *k64 = args["key"] | "";
    uint8_t key[32];
    size_t n = 0;
    String info;
    const char *err = nullptr;
    if (!settings.c2Control) err = denied;
    else if (mbedtls_base64_decode(key, sizeof(key), &n, (const uint8_t *)k64, strlen(k64)) || n != sizeof(key)) err = "the backup key is not usable";
    else if (job.type == "sd_backup") err = SdBackup::write(key, info);
    else err = SdBackup::restore(key, info);
    memset(key, 0, sizeof(key));
    Serial.printf("[C2]   %s: %s\n", job.type == "sd_backup" ? "Sicherung auf die SD-Karte" : "Wiederherstellung von der SD-Karte", err ? err : info.c_str());
    answer(job.id, !err, quoted(err ? err : info.c_str()));
    if (!err && job.type == "sd_restore") {
      rebootAfter = job.id;
      rebootWanted = true;               // the restored settings take effect with the start
    }
  } else if (job.type == "cert_put") {
    // A certificate for the WLAN client, sent by a profile (server/internal/
    // server/profiles.go): the same checks as an upload in the web interface.
    // It takes effect with the next start of the client - the "set" that
    // follows restarts the device.
    const char *slot = args["slot"] | "";
    const char *b64 = args["data"] | "";
    size_t len = strlen(b64), n = 0;
    uint8_t *buf = len ? ext(len * 3 / 4 + 4) : nullptr;
    String msg;
    bool ok = false;
    if (!settings.c2Control) msg = denied;
    else if (strcmp(slot, "ca") && strcmp(slot, "client")) msg = "unknown slot";
    else if (!buf || mbedtls_base64_decode(buf, len * 3 / 4 + 4, &n, (const uint8_t *)b64, len) || !n) msg = "the file is not base64";
    else ok = Certs::upload(!strcmp(slot, "ca") ? Certs::CA : Certs::CLIENT, buf, n, args["pass"] | "", msg);
    free(buf);
    Serial.printf("[C2]   Zertifikat \"%s\" von der Leitstelle: %s\n", slot, ok ? "gespeichert" : msg.c_str());
    answer(job.id, ok, quoted(msg.c_str()));
  } else answer(job.id, false, quoted("unknown command"));
}

// Closes the console from this side; the task tells the server. Under mux.
static void conEndLocked(const char *reason) {
  if (con.state != CON_OPEN) return;
  con.state = CON_END;
  strlcpy(con.reason, reason, sizeof(con.reason));
}

void consoleAnswer(bool allow) {
  if (!mux) return;
  String r;
  {
    Lock l;
    if (con.state != CON_ASK) return;
    C2Console::Stream *st = allow ? new (std::nothrow) C2Console::Stream() : nullptr;
    const char *no = !allow ? "refused at the device" : !st ? "not enough memory" : nullptr;
    if (no) {
      r = String("{\"id\":") + con.id + ",\"ok\":false,\"out\":" + quoted(no) + "}";
      con.state = CON_NONE;
      conBusy = false;
      conRefuseUntil = millis() + CON_REFUSE_MS;
    } else {
      r = String("{\"id\":") + con.id + ",\"ok\":true,\"out\":{\"port\":" + (con.port + 1) + ",\"minutes\":" + con.minutes + "}}";
      con.stream = st;
      con.seq = Bridge::seqNow(con.port);    // from now on; what was on the screen before stays here
      con.untilMs = millis() + con.minutes * 60000UL;
      con.state = CON_OPEN;
    }
    jobResults.push_back(r);
  }
  Serial.printf("[C2]   Fernkonsole %s\n", allow ? "am Display erlaubt" : "am Display abgelehnt");
  if (allow) Net::onMessage(String(TR("Control center: remote console on ", "Leitstelle: Fernkonsole auf ", "Central: console remoto em ")) + Store::portName(con.port) + TR(" allowed for ", " für ", " permitido por ") + con.minutes + TR(" min", " min erlaubt", " min"));
}

void unattendedSet(uint32_t seconds) {
  unattLeft = seconds > UNATTENDED_MAX_S ? UNATTENDED_MAX_S : seconds;
  unattTickMs = millis();
  unattSavedS = settings.c2UnattS = unattLeft;
  Store::save();
  Serial.printf("[C2]   unbeaufsichtigter Zugriff: %s (%lu s)\n", unattLeft ? "erlaubt" : "aus", (unsigned long)unattLeft);
}

uint32_t unattendedLeft() { return unattLeft; }

// Counts the unattended access down in the device's own running time and
// writes down what is left every ten minutes: a restart costs at most that.
static void unattendedTick() {
  if (!unattLeft) return;
  uint32_t gone = (millis() - unattTickMs) / 1000;
  if (!gone) return;
  unattTickMs += gone * 1000;
  unattLeft = gone >= unattLeft ? 0 : unattLeft - gone;
  if (!unattLeft || unattSavedS - unattLeft >= 600) {
    unattSavedS = settings.c2UnattS = unattLeft;
    Store::save();
    if (!unattLeft) Serial.println("[C2]   unbeaufsichtigter Zugriff abgelaufen");
  }
}

// The names and sizes of the stored configurations, at most as many as fit
// comfortably into every status report.
static void inventoryTick() {
  if (inventoryAt && millis() - inventoryAt < 60000) return;
  inventoryAt = millis() | 1;
  JsonDocument src, dst;
  deserializeJson(src, Configs::listJson());
  JsonArray out = dst.to<JsonArray>();
  size_t room = 1200;
  for (JsonObject c : src["configs"].as<JsonArray>()) {
    const char *n = c["name"] | "";
    if (strlen(n) + 24 > room) break;
    room -= strlen(n) + 24;
    JsonObject o = out.add<JsonObject>();
    o["n"] = n;
    o["s"] = c["size"] | 0;
  }
  String s, files = sdJob ? String() : Sd::rootJson(700);   // not while a file is being written
  serializeJson(dst, s);
  Lock l;
  inventory = s;
  if (files.length()) sdFiles = files;
}

void consoleEnd() {
  if (!mux) return;
  Lock l;
  conEndLocked("closed at the device");
}

Console console() {
  Console c = {};
  if (!mux || !conBusy) return c;
  Lock l;
  c.state = con.state == CON_ASK ? 1 : con.state == CON_OPEN ? 2 : 0;
  c.port = con.port;
  c.minutes = con.minutes;
  if (con.state == CON_OPEN && (int32_t)(con.untilMs - millis()) > 0) c.leftS = (con.untilMs - millis()) / 1000;
  if (con.e2e) strlcpy(c.code, con.e2e->code, sizeof(c.code));
  return c;
}

// Main loop: moves the bytes between the serial port and con.stream.
static void conPump() {
  uint32_t now = millis();
  bool brk = false;
  uint8_t port;
  {
    Lock l;
    port = con.port;
    if (con.state == CON_ASK && now - con.askedMs > CON_ASK_MS) {
      String r = String("{\"id\":") + con.id + ",\"ok\":false,\"out\":" + quoted("nobody answered at the device") + "}";
      jobResults.push_back(r);
      con.state = CON_NONE;
      conBusy = false;
      conRefuseUntil = now + CON_REFUSE_MS;
      Serial.println("[C2]   Fernkonsole: niemand hat am Display geantwortet");
    }
    if (con.state != CON_OPEN) return;
    C2Console::Stream &st = *con.stream;
    if ((int32_t)(con.untilMs - now) <= 0) return conEndLocked("time is up");
    if (!Bridge::enabled(port)) return conEndLocked("port switched off");
    // port -> server. What does not fit waits in the port's ring; if the ring
    // has moved on, the gap is counted so the offsets stay true.
    uint32_t first = Bridge::ringStartSeq(port);
    if (con.e2e) {
      // Encrypted: frames go into the stream, never a gap in it (the browser
      // reads frame after frame). What the ring lost before is just lost.
      if ((int32_t)(first - con.seq) > 0) con.seq = first;
      size_t room = st.outRoom();
      if (room > 2 + C2E2E::TAG + 16) {
        uint8_t plain[C2E2E::MAX_PLAIN];
        size_t want = min(room - 2 - C2E2E::TAG, C2E2E::MAX_PLAIN);
        size_t n = Bridge::copyFrom(port, con.seq, plain, want);
        if (n) {
          st.outLen += C2E2E::seal(*con.e2e, plain, n, st.out + st.outLen);
          con.seq += n;
        }
      }
    } else {
      if ((int32_t)(first - con.seq) > 0 && !st.outLen) {
        st.outSkip(first - con.seq);
        con.seq = first;
      }
      if (st.outRoom() && (int32_t)(first - con.seq) <= 0) {
        size_t n = Bridge::copyFrom(port, con.seq, st.out + st.outLen, st.outRoom());
        st.outLen += n;
        con.seq += n;
      }
    }
    // server -> port: like typing in the web terminal. Input the port will
    // not take now (file transfer, auto-baud) is dropped, as it is there.
    if (con.e2e) {
      // frames from the browser: opened one at a time, as the port takes them
      while (!con.inPlainLen && st.inLen) {
        bool bad = false;
        size_t used = C2E2E::open(*con.e2e, st.in, st.inLen, con.inPlain, con.inPlainLen, bad);
        if (bad) return conEndLocked("console data was not genuine");
        if (!used) break;                  // the rest of the frame is still on its way
        st.inDrop(used);
      }
      if (con.inPlainLen) {
        size_t room = Bridge::txFree(port), n = Bridge::write(port, con.inPlain, min(con.inPlainLen, room));
        if (!n && room) n = con.inPlainLen;   // the port does not take input now: dropped, as in the web terminal
        memmove(con.inPlain, con.inPlain + n, con.inPlainLen - n);
        con.inPlainLen -= n;
      }
    } else if (st.inLen) {
      size_t room = Bridge::txFree(port), n = Bridge::write(port, st.in, min(st.inLen, room));
      st.inDrop(n ? n : room ? st.inLen : 0);
    }
    if (st.brkDue) {
      st.brkDue--;
      brk = true;
    }
  }
  if (brk) Bridge::sendBreak(port);      // takes 300 ms: not under the lock
}

// Main loop: the recording of a remote console (the switch at the server).
// It is the SD card's session log of that port - the same file REC at the
// display writes; one that was running already is used and left running.
// The card is not touched under the lock.
static void conRecord() {
  static bool on = false, own = false;
  static uint8_t port = 0;
  if (!conBusy && !on) return;
  bool want;
  {
    Lock l;
    want = con.state == CON_OPEN && con.stream && con.stream->recWant;
    if (want && !on) port = con.port;
  }
  if (want == on) return;
  char name[40] = "", err[48] = "";
  if (want) {
    if (!Sd::mounted()) strlcpy(err, "no SD card in the device", sizeof(err));
    else if (Sd::logging(port)) own = false;
    else if (Sd::logStart(port)) own = true;
    else strlcpy(err, "the recording could not be started on the SD card", sizeof(err));
    if (!err[0]) strlcpy(name, Sd::logName(port).c_str(), sizeof(name));
    Serial.printf("[C2]   Fernkonsole: Mitschnitt %s\n", err[0] ? err : name);
  } else {
    if (own) Sd::logStop(port);
    own = false;
    Serial.println("[C2]   Fernkonsole: Mitschnitt beendet");
  }
  on = want;
  Lock l;
  if (con.stream) {
    strlcpy(con.stream->rec, name, sizeof(con.stream->rec));
    strlcpy(con.stream->recErr, err, sizeof(con.stream->recErr));
  }
}

void loop() {
  if (!mux) return;
  if (rebootNow) {
    Serial.println("[C2]   Neustart auf Auftrag der Leitstelle");
    Display::message(TR("Control center", "Leitstelle", "Central"), TR("Restarting ...", "Neustart ...", "Reiniciando ..."), 3000);
    Display::loop();
    delay(800);
    ESP.restart();
  }
  if (playJob && Player::state() != Player::RUNNING) {
    answer(playJob, Player::state() == Player::DONE, playReport());
    playJob = 0;
  }
  if (conBusy) conPump();
  conRecord();
  unattendedTick();
  inventoryTick();
  MainJob job;
  {
    Lock l;
    if (jobs.empty()) return;
    job = jobs.front();
    jobs.erase(jobs.begin());
  }
  execute(job);
}

Info info() {
  if (!mux) return Info{};               // the display asks before begin() has run
  Lock l;
  return shared;
}

const char *stateName(State s) {
  switch (s) {
    case ENROLLING: return TR("enrolling", "meldet an", "registrando");
    case PENDING:   return TR("waiting for confirmation", "wartet auf Bestaetigung", "aguardando confirmacao");
    case ACTIVE:    return TR("connected", "verbunden", "conectado");
    case REFUSED:   return TR("refused", "abgewiesen", "recusado");
    case FAILED:    return TR("error", "Fehler", "erro");
    default:        return TR("not enrolled", "nicht angemeldet", "nao registrado");
  }
}

String line() {
  Info i = info();
  if (i.state == PENDING) return String(TR("Code ", "Code ", "Codigo ")) + i.code;
  if (i.state == ACTIVE && i.error[0]) return TR("not reachable", "nicht erreichbar", "inacessivel");
  if (i.state == OFF && i.want[0]) return TR("asking the server", "fragt beim Server an", "pedindo ao servidor");
  if (i.state == ACTIVE && conBusy) {
    Console c = console();
    if (c.state == 2) return String(TR("Remote console port ", "Fernkonsole Port ", "Console remoto porta ")) + (c.port + 1);
  }
  if (i.state == ACTIVE && unattLeft) {   // somebody has to be able to see that the door is open
    uint32_t h = (unattLeft + 3599) / 3600;
    return String(TR("unattended, ", "unbeaufsichtigt, ", "sem supervisao, ")) + h + " h";
  }
  return stateName(i.state);
}

const char *enroll(const String &token) {
  C2Proto::Token tok;
  if (!C2Proto::parseToken(token.c_str(), tok)) return TR("This is not an invitation token", "Das ist kein Einladungs-Token", "Isto não é um token de convite");
  if (WiFi.status() != WL_CONNECTED) return TR("Connect the Wi-Fi client first (Menu → Network)", "Erst den WLAN-Client verbinden (Menü → Netz)", "Conecte primeiro o cliente Wi-Fi (Menu → Rede)");
  if (!mux) return TR("The device is still starting", "Gerät startet noch", "O dispositivo ainda está iniciando");
  Lock l;
  if (shared.state == FAILED) return TR("Control center not available on this device", "Leitstelle auf diesem Gerät nicht verfügbar", "Central não disponível neste dispositivo");
  reqToken = token;
  return nullptr;
}

void forget() {
  if (!mux) return;
  Lock l;
  reqForget = true;
}

const char *setServer(const String &text) {
  String url = text;
  url.trim();
  if (url.isEmpty()) {                   // stop asking; an enrolled device keeps its server
    if (!mux) return nullptr;
    Lock l;
    if (shared.state != OFF) return TR("Sign off to leave the server", "Zum Verlassen des Servers abmelden", "Para sair do servidor, use Sair da central");
    reqUrl = "";
    reqUrlSet = true;
    return nullptr;
  }
  while (url.endsWith("/")) url.remove(url.length() - 1);
  bool ok = (url.startsWith("https://") && url.length() > 8) || (url.startsWith("http://") && url.length() > 7);
  for (size_t i = 0; ok && i < url.length(); i++) ok = url[i] > ' ' && url[i] < 127 && url[i] != '"' && url[i] != '\\';
  if (!ok || url.length() >= sizeof(shared.url)) return TR("The address has to look like https://name-or-address (at most 95 characters)", "Die Adresse muss wie https://name-oder-adresse aussehen (höchstens 95 Zeichen)", "O endereço deve ser como https://nome-ou-endereco (no máximo 95 caracteres)");
  if (!mux) return TR("The device is still starting", "Gerät startet noch", "O dispositivo ainda está iniciando");
  Lock l;
  if (shared.state == FAILED) return TR("Control center not available on this device", "Leitstelle auf diesem Gerät nicht verfügbar", "Central não disponível neste dispositivo");
  if (shared.state == ENROLLING) return TR("The device is enrolling right now", "Das Gerät meldet sich gerade an", "O dispositivo está se registrando agora");
  reqUrl = url;
  reqUrlSet = true;
  return nullptr;
}

bool down() {
  if (!mux) return false;
  Lock l;
  if (shared.state == OFF && shared.want[0]) return true;   // asking to be let in, not taken yet
  if (shared.state != ACTIVE && shared.state != PENDING && shared.state != REFUSED) return false;
  return shared.state == REFUSED || shared.error[0] || WiFi.status() != WL_CONNECTED;
}

void reconnect() {
  if (!mux) return;
  // "whatever was lost": a WLAN client that has given up is asked to join again as well
  if (settings.staSsid.length() && WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  reqRetry = true;
}

}  // namespace C2
