#include "c2.h"
#include "c2_proto.h"
#include "config.h"
#include "settings.h"
#include "power.h"
#include "display.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <functional>

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
static char devName[33];

struct Lock {
  Lock() { xSemaphoreTake(mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mux); }
};

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
static bool big(const std::function<void()> &fn) {
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
  haveKey = haveSession = false;
  session.clear();
  free(serverPub);
  serverPub = nullptr;
  serverUrl.clear();
  results.clear();
  state = OFF;
  {
    Lock l;
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
                   uint8_t *out, size_t outMax, size_t &outLen) {
  int code = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
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

// HTTP/1.0 by hand: one request, one answer, connection closed. Small enough to
// see exactly where a weak link gives up.
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

  WiFiClient plain;
  WiFiClientSecure tls;
  Client *c = &plain;
  int code = NET_CONNECT;
  const char *step = "Verbinden";
  bool up;
  if (https) {
    // The certificate is not checked: the server is authenticated inside, by
    // the key the enrollment token pinned (KONZEPT.md §8, open question).
    tls.setInsecure();
    tls.setHandshakeTimeout(TIMEOUT_MS / 1000);
    up = tls.connect(host.c_str(), port, (int32_t)TIMEOUT_MS);   // also the limit for one write
    c = &tls;
  } else {
    up = plain.connect(host.c_str(), port, (int32_t)TIMEOUT_MS);
  }
  if (up) {
    step = "Senden";
    code = NET_SEND;
    String head = String(body ? "POST " : "GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n";
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
  c->stop();
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
  haveSession = false;
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
        publish(r == C2Proto::ERR_CONFIRM ? "Server hat seinen Schluessel nicht bewiesen"
                : r == C2Proto::ERR_CRYPTO ? "zu wenig Speicher fuer den Handshake" : "Handshake: Antwort unbrauchbar");
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

// Sends one message and reads the answer on the current session.
static Ex exchange(JsonDocument &msg, JsonDocument &reply) {
  String plain;
  serializeJson(msg, plain);
  const size_t MAX_REPLY = 8192;       // commands are small; nothing here needs the protocol's 32 kB
  size_t bodyLen = C2Proto::ID_SIZE + plain.length() + C2Proto::RECORD_OVERHEAD, n = 0, got = 0;
  uint8_t *body = (uint8_t *)malloc(bodyLen), *resp = (uint8_t *)malloc(MAX_REPLY);
  Ex ex = EX_BAD;
  if (body && resp) {
    memcpy(body, session.id, C2Proto::ID_SIZE);
    if (session.seal((const uint8_t *)plain.c_str(), plain.length(), body + C2Proto::ID_SIZE, n)) {
      ex = fromHttp(request(serverUrl, "/v1/msg", body, bodyLen, resp, MAX_REPLY, got));
      if (ex == EX_OK) {
        // decrypts in place: the plaintext is shorter than the record
        if (!session.open(resp, got, resp, n) || deserializeJson(reply, (const char *)resp, n)) ex = EX_BAD;
      }
    }
  }
  free(body);
  free(resp);
  if (ex != EX_OK) haveSession = false;
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

// Only what this list knows is ever carried out, whatever the server sends.
static void run(JsonObject cmd) {
  const char *type = cmd["type"] | "";
  JsonObject res = results.as<JsonArray>().add<JsonObject>();
  res["id"] = cmd["id"];
  res["ok"] = true;
  if (!strcmp(type, "ping")) res["out"] = "pong";
  else if (!strcmp(type, "status")) {
    JsonObject o = res["out"].to<JsonObject>();
    fillStatus(o);
    o["ip"] = WiFi.localIP().toString();
    o["board"] = BOARD_NAME;
  } else {
    res["ok"] = false;
    res["out"] = "unbekannter Auftrag";
  }
  Serial.printf("[C2]   Auftrag %lu \"%s\" %s\n", (unsigned long)(cmd["id"] | 0UL), type, res["ok"] ? "ausgefuehrt" : "abgelehnt");
}

// ---------------------------------------------------------------- enroll / poll

static void doEnroll(const String &text) {
  C2Proto::Token tok;
  if (!C2Proto::parseToken(text.c_str(), tok)) return publish("Token unbrauchbar");
  if (WiFi.status() != WL_CONNECTED) return publish("kein WLAN-Client verbunden");
  State before = state;
  state = ENROLLING;
  publish("");
  Serial.printf("[C2]   Anmeldung bei %s\n", tok.url.c_str());

  uint8_t *key = (uint8_t *)malloc(C2Proto::PUB_SIZE);
  size_t got = 0;
  const char *err = nullptr;
  int code = key ? request(tok.url, "/v1/server-key", nullptr, 0, key, C2Proto::PUB_SIZE, got) : -1;
  if (code != 200 || got != C2Proto::PUB_SIZE) err = "Server nicht erreichbar";
  else if (!C2Proto::tokenPinsServer(tok, key)) err = "Serverschluessel passt nicht zum Token";
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
    state = OFF;
    err = ex == EX_REFUSED ? "Token abgelehnt (verbraucht oder abgelaufen)" : ex == EX_NET ? "Server nicht erreichbar" : "Anmeldung gescheitert";
    Serial.printf("[C2]   Anmeldung gescheitert: %s\n", err);
    Preferences p;                       // the previous server (if any) is no longer valid either
    if (p.begin(NVS, false)) {
      p.remove("url");
      p.remove("spub");
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
    p.putUChar("st", state);
    p.putString("sas", session.sas);
    p.end();
  }
  publish("", session.sas);
  Serial.printf("[C2]   angemeldet, wartet auf Bestaetigung - Code %s\n", session.sas);
  Display::wake();
  Display::message("Leitstelle: Code", session.sas, 60000);
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
      ex = handshake(C2Proto::KIND_SESSION, nullptr);
      if (ex != EX_OK) break;
    }
    msg.clear();
    msg["t"] = "poll";
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
    return publish("vom Server abgewiesen");
  }
  if (ex != EX_OK) return retry(ex == EX_NET ? "Server nicht erreichbar" : "Antwort des Servers unbrauchbar");

  results.clear();
  retryMs = RETRY_MIN_MS;
  const char *st = reply["state"] | "";
  State was = state;
  state = !strcmp(st, "active") ? ACTIVE : PENDING;
  if (state != was) {
    storeState(nullptr);
    Serial.printf("[C2]   Zustand: %s\n", stateName(state));
    if (state == ACTIVE) Display::message("Leitstelle", "verbunden", 4000);
  }
  for (JsonObject cmd : reply["cmds"].as<JsonArray>()) {
    if (!results.is<JsonArray>()) results.to<JsonArray>();
    run(cmd);
  }
  pollS = constrain((int)(reply["poll_s"] | 30), 5, 3600);
  // answers go back at once, not a poll interval later
  nextPollAt = millis() + (results.size() ? 500 : pollS * 1000UL);
  {
    Lock l;
    shared.lastOkMs = millis();
    shared.polls++;
  }
  publish("");
}

static void task(void *) {
  const char *failed = "zu wenig Speicher";
  big([&] { failed = C2Proto::selfTest(); });
  if (failed) {
    state = FAILED;
    char buf[64];
    snprintf(buf, sizeof(buf), "Selbsttest: %s", failed);
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
    if (serverPub && WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextPollAt) >= 0) doPoll();
    vTaskDelay(pdMS_TO_TICKS(200));
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

Info info() {
  if (!mux) return Info{};               // the display asks before begin() has run
  Lock l;
  return shared;
}

const char *stateName(State s) {
  switch (s) {
    case ENROLLING: return "meldet an";
    case PENDING:   return "wartet auf Bestaetigung";
    case ACTIVE:    return "verbunden";
    case REFUSED:   return "abgewiesen";
    case FAILED:    return "Fehler";
    default:        return "nicht angemeldet";
  }
}

String line() {
  Info i = info();
  if (i.state == PENDING) return String("Code ") + i.code;
  if (i.state == ACTIVE && i.error[0]) return "nicht erreichbar";
  return stateName(i.state);
}

const char *enroll(const String &token) {
  C2Proto::Token tok;
  if (!C2Proto::parseToken(token.c_str(), tok)) return "Das ist kein Einladungs-Token";
  if (WiFi.status() != WL_CONNECTED) return "Erst den WLAN-Client verbinden (Menü → Netz)";
  if (!mux) return "Gerät startet noch";
  Lock l;
  if (shared.state == FAILED) return "Leitstelle auf diesem Gerät nicht verfügbar";
  reqToken = token;
  return nullptr;
}

void forget() {
  if (!mux) return;
  Lock l;
  reqForget = true;
}

}  // namespace C2
