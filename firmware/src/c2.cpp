#include "c2.h"
#include "c2_proto.h"
#include "c2_console.h"
#include "serial_bridge.h"
#include "config.h"
#include "settings.h"
#include "power.h"
#include "display.h"
#include "net.h"
#include "configs.h"
#include "player.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include "lang.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
static const uint32_t KEM_STACK = 28 * 1024;
static uint32_t kemStackFree = KEM_STACK;
static const char *NVS = "c2";
static const uint32_t RETRY_MIN_MS = 15000, RETRY_MAX_MS = 300000;
static const uint32_t REFUSED_RETRY_MS = 600000;

static SemaphoreHandle_t mux;
static Info shared;                      // under mux
static String reqToken;                  // under mux: token waiting for the task
static bool reqForget = false;           // under mux

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
} con;
static volatile bool conBusy = false;    // con.state != CON_NONE, readable without the lock
static bool conAnnounced = false;        // task: the server has the "yes" to the console that is open now
static bool conAtCollect = false;
static bool keepAlive = false;           // task: the connection stays up between requests (see requestOnce)
static uint32_t conRefuseUntil = 0;      // main loop: after a "no", further requests are turned down for a while
static const uint32_t CON_ASK_MS = 60000, CON_REFUSE_MS = 60000, CON_MAX_MIN = 60;
static const uint32_t CON_LOST_MS = 30000;   // without the server for this long, the console is closed

struct BigJob {
  const std::function<void()> *fn;
  TaskHandle_t caller;
};

// Runs fn on a stack large enough for ML-KEM and waits for it. False = no
// memory for that stack right now.
//
// The stack is ours, not FreeRTOS's: a task that deletes itself is only cleaned
// up when the idle task next runs, and the TLS connection that follows needs
// those 28 kB back at once. So the helper parks itself when it is done and the
// caller removes it and frees the stack.
static void connClose();

static bool big(const std::function<void()> &fn) {
  connClose();                           // never together with a TLS connection
  StackType_t *stack = (StackType_t *)heap_caps_malloc(KEM_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  BigJob job = {&fn, xTaskGetCurrentTaskHandle()};
  auto entry = [](void *p) {
    BigJob *j = (BigJob *)p;
    (*j->fn)();
    kemStackFree = min(kemStackFree, (uint32_t)uxTaskGetStackHighWaterMark(nullptr));
    xTaskNotifyGive(j->caller);
    for (;;) vTaskSuspend(nullptr);
  };
  TaskHandle_t h = stack && tcb ? xTaskCreateStaticPinnedToCore(entry, "c2kem", KEM_STACK, &job, 1, stack, tcb, 0) : nullptr;
  if (h) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (eTaskGetState(h) != eSuspended) vTaskDelay(1);
    vTaskDelete(h);
  }
  free(stack);
  free(tcb);
  return h != nullptr;
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

static void publish(const char *error = nullptr, const char *code = nullptr) {
  Lock l;
  shared.state = state;
  if (error) strlcpy(shared.error, error, sizeof(shared.error));
  if (code) strlcpy(shared.code, code, sizeof(shared.code));
  if (state != PENDING) shared.code[0] = 0;
  strlcpy(shared.url, serverUrl.c_str(), sizeof(shared.url));
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
      uint8_t *next = (uint8_t *)malloc(C2Proto::PUB_SIZE);
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
  results.clear();
  state = OFF;
  {
    Lock l;
    jobs.clear();
    jobResults.clear();
    if (con.state != CON_NONE) {
      delete con.stream;
      con.stream = nullptr;
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
  uint8_t *req = (uint8_t *)malloc(C2Proto::REQ_ENROLL), *resp = (uint8_t *)malloc(C2Proto::RESP_SIZE);
  C2Proto::Handshake hs;
  size_t n = 0, got = 0;
  Ex ex = EX_BAD;
  uint32_t t0 = millis(), crypto = 0;
  C2Proto::Result r = C2Proto::ERR_CRYPTO;
  if (req && resp)
    big([&] { r = hs.begin(kind, priv, serverPub, tok ? tok->id : nullptr, tok ? tok->secret : nullptr, req, n); });
  if (r == C2Proto::OK) {
    crypto = millis() - t0;
    ex = fromHttp(request(serverUrl, "/v1/handshake", req, n, resp, C2Proto::RESP_SIZE, got));
    if (ex == EX_OK) {
      uint32_t t1 = millis();
      r = C2Proto::ERR_CRYPTO;
      big([&] { r = hs.finish(resp, got, session); });
      crypto += millis() - t1;
      if (r == C2Proto::OK) haveSession = true;
      else {
        ex = EX_BAD;
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
  uint8_t *next = (uint8_t *)malloc(C2Proto::PUB_SIZE);
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
  uint8_t *next = (uint8_t *)malloc(C2Proto::PUB_SIZE);
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

static void fillStatus(JsonObject o) {
  o["fw"] = FW_VERSION;
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

static void doEnroll(const String &text) {
  C2Proto::Token tok;
  if (!C2Proto::parseToken(text.c_str(), tok)) return publish(TR("unusable token", "Token unbrauchbar", "token invalido"));
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
  else if (!C2Proto::tokenPinsServer(tok, key)) err = TR("the server key does not match the token", "Serverschluessel passt nicht zum Token", "a chave do servidor nao confere com o token");
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
    ex = exchange(msg, reply);
  }
  if (ex != EX_OK) {
    free(serverPub);
    serverPub = nullptr;
    serverUrl.clear();
    skTag[0] = 0;
    state = OFF;
    err = ex == EX_REFUSED ? TR("token refused (used or expired)", "Token abgelehnt (verbraucht oder abgelaufen)", "token recusado (usado ou expirado)")
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
    p.end();
  }
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

  if (rebootWanted && results.size()) rebootNow = true;    // the answer to "reboot" has arrived at the server
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

static void task(void *) {
  const char *failed = TR("not enough memory", "zu wenig Speicher", "memoria insuficiente");
  big([&] { failed = C2Proto::selfTest(); });
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
  Serial.printf("[C2]   Selbsttest ok, %s%s%s\n", stateName(state), serverUrl.empty() ? "" : " bei ", serverUrl.c_str());
  nextPollAt = millis() + 8000;          // give the WLAN client time to join
  for (;;) {
    String token;
    bool forget;
    {
      Lock l;
      token = reqToken;
      reqToken = "";
      forget = reqForget;
      reqForget = false;
    }
    if (forget) forgetAll();
    if (token.length()) doEnroll(token);
    bool answers;
    {
      Lock l;
      answers = !jobResults.empty();
    }
    if (answers && state == ACTIVE) nextPollAt = millis();   // answers go back at once
    bool online = serverPub && WiFi.status() == WL_CONNECTED;
    if (online && (int32_t)(millis() - nextPollAt) >= 0) doPoll();
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
      rebootWanted = true;               // carried out once the answer has reached the server
    }
  } else if (job.type == "console") {
    // Never without a tap on the display, whatever the settings say.
    int port = args["port"] | 1, minutes = constrain((int)(args["minutes"] | 15), 1, (int)CON_MAX_MIN);
    const char *no = nullptr;
    Lock l;
    if (con.state != CON_NONE) no = "a remote console is already open or asked for";
    else if (port < 1 || port > MAX_PORTS || !Bridge::enabled(port - 1)) no = "Port unbekannt";
    else if (!Display::usable()) no = "the device has no display on which somebody could agree";
    else if ((int32_t)(conRefuseUntil - millis()) > 0) no = "just refused at the device";
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
    Display::wake();
    Serial.printf("[C2]   Fernkonsole angefragt: Port %d, %d min - wartet auf Zustimmung am Display\n", port, minutes);
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

void consoleEnd() {
  if (!mux) return;
  Lock l;
  conEndLocked("closed at the device");
}

Console console() {
  Console c = {0, 0, 0, 0};
  if (!mux || !conBusy) return c;
  Lock l;
  c.state = con.state == CON_ASK ? 1 : con.state == CON_OPEN ? 2 : 0;
  c.port = con.port;
  c.minutes = con.minutes;
  if (con.state == CON_OPEN && (int32_t)(con.untilMs - millis()) > 0) c.leftS = (con.untilMs - millis()) / 1000;
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
    if ((int32_t)(first - con.seq) > 0 && !st.outLen) {
      st.outSkip(first - con.seq);
      con.seq = first;
    }
    if (st.outRoom() && (int32_t)(first - con.seq) <= 0) {
      size_t n = Bridge::copyFrom(port, con.seq, st.out + st.outLen, st.outRoom());
      st.outLen += n;
      con.seq += n;
    }
    // server -> port: like typing in the web terminal. Input the port will
    // not take now (file transfer, auto-baud) is dropped, as it is there.
    if (st.inLen) {
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
  if (i.state == ACTIVE && conBusy) {
    Console c = console();
    if (c.state == 2) return String(TR("Remote console port ", "Fernkonsole Port ", "Console remoto porta ")) + (c.port + 1);
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

}  // namespace C2
