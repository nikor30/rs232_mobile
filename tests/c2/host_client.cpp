// The firmware's protocol code (firmware/src/c2_proto.cpp, c2_console.cpp) on a
// PC, talking to the real server over plain HTTP. Driven by run.sh.
//
//   c2host selftest
//   c2host enroll STATEFILE TOKEN      -> "ID <hex>", "CODE <six digits>"
//   c2host poll STATEFILE              -> "STATE <state>", "CMD <id> <type>" ...; with a change of
//                                      the server key also "KEY announced <tag>" (the new key is
//                                      kept) and "KEY changed <tag>" (it is the pinned one now)
//   c2host console STATEFILE           says yes to a request for the remote console and plays the
//                                      serial port -> "CONSOLE <id> open", "CLOSED <reason>",
//                                      "TYPED <hex>", "BRK <count>"
//   c2host update STATEFILE KEYFILE SEQ  takes an "update" command the way the firmware does:
//                                      signature and manifest (firmware/src/c2_update.cpp, KEYFILE
//                                      is the raw public key, SEQ what is "installed"), then the
//                                      image -> "UPDATE ok <fw> <lang> <bytes>" or "UPDATE refused: <why>"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include "c2_console.h"
#include "c2_proto.h"
#include "c2_update.h"
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

extern "C" void c2_randombytes(uint8_t *out, size_t len) {
  while (len) {
    ssize_t n = getrandom(out, len, 0);
    if (n <= 0) abort();
    out += n;
    len -= (size_t)n;
  }
}

using Bytes = std::vector<uint8_t>;

[[noreturn]] static void die(const std::string &msg) {
  fprintf(stderr, "c2host: %s\n", msg.c_str());
  exit(1);
}

// One HTTP/1.0 exchange; returns the status code.
static int http(const std::string &url, const char *method, const std::string &path, const Bytes &body, Bytes &resp) {
  if (url.rfind("http://", 0) != 0) die("only http:// here: " + url);
  std::string host = url.substr(7), port = "80";
  if (size_t c = host.find(':'); c != std::string::npos) { port = host.substr(c + 1); host.resize(c); }
  addrinfo hints{}, *ai;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai)) die("cannot resolve " + host);
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) die("cannot connect to " + url);
  freeaddrinfo(ai);
  std::string head = std::string(method) + " " + path + " HTTP/1.0\r\nHost: " + host +
                     "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  Bytes out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  for (size_t off = 0; off < out.size();) {
    ssize_t n = write(fd, out.data() + off, out.size() - off);
    if (n <= 0) die("write failed");
    off += (size_t)n;
  }
  std::string all;
  char buf[4096];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) all.append(buf, (size_t)n);
  close(fd);
  size_t sep = all.find("\r\n\r\n");
  if (all.size() < 12 || sep == std::string::npos) die("bad HTTP answer");
  resp.assign(all.begin() + sep + 4, all.end());
  return atoi(all.c_str() + 9);
}

struct Device {
  uint8_t priv[C2Proto::PRIV_SIZE];
  uint8_t serverPub[C2Proto::PUB_SIZE];
  uint8_t nextPub[C2Proto::PUB_SIZE];   // announced by the server (key change), if haveNext
  bool haveNext = false;
  std::string url;
};

static void save(const char *file, const Device &d) {
  std::ofstream f(file, std::ios::binary);
  f.write((const char *)d.priv, sizeof(d.priv)).write((const char *)d.serverPub, sizeof(d.serverPub));
  f.put(d.haveNext ? 1 : 0).write((const char *)d.nextPub, sizeof(d.nextPub)) << d.url;
}

static Device load(const char *file) {
  std::ifstream f(file, std::ios::binary);
  Device d;
  if (!f.read((char *)d.priv, sizeof(d.priv)).read((char *)d.serverPub, sizeof(d.serverPub))) die(std::string("cannot read ") + file);
  d.haveNext = f.get() == 1;
  if (!f.read((char *)d.nextPub, sizeof(d.nextPub))) die(std::string("cannot read ") + file);
  std::getline(f, d.url, '\0');
  return d;
}

// False: the server did not prove the pinned key.
static bool tryHandshake(const Device &d, C2Proto::Kind kind, const C2Proto::Token *tok, C2Proto::Session &s);

static C2Proto::Session handshake(const Device &d, C2Proto::Kind kind, const C2Proto::Token *tok) {
  C2Proto::Session s;
  if (!tryHandshake(d, kind, tok, s)) die("handshake: the server did not prove its key");
  return s;
}

// The handshake of a device that is enrolled, as c2.cpp does it: with the
// pinned key, and if the server cannot prove that one, with the key it has
// announced - which is the pinned one from then on.
static C2Proto::Session sessionHandshake(Device &d, const char *file) {
  C2Proto::Session s;
  if (tryHandshake(d, C2Proto::KIND_SESSION, nullptr, s)) return s;
  if (!d.haveNext) die("handshake: the server did not prove its key");
  Device with = d;
  memcpy(with.serverPub, d.nextPub, C2Proto::PUB_SIZE);
  with.haveNext = false;
  if (!tryHandshake(with, C2Proto::KIND_SESSION, nullptr, s)) die("handshake: the server proved neither the pinned key nor the announced one");
  d = with;
  save(file, d);
  char tag[C2Proto::KEY_TAG_SIZE];
  C2Proto::keyTag(d.serverPub, tag);
  printf("KEY changed %s\n", tag);
  return s;
}

static bool tryHandshake(const Device &d, C2Proto::Kind kind, const C2Proto::Token *tok, C2Proto::Session &s) {
  C2Proto::Handshake hs;
  Bytes req(C2Proto::REQ_ENROLL), resp;
  size_t n = 0;
  if (hs.begin(kind, d.priv, d.serverPub, tok ? tok->id : nullptr, tok ? tok->secret : nullptr, req.data(), n) != C2Proto::OK)
    die("handshake: cannot build the request");
  req.resize(n);
  int code = http(d.url, "POST", "/v1/handshake", req, resp);
  if (code != 200) die("handshake: HTTP " + std::to_string(code));
  C2Proto::Result r = hs.finish(resp.data(), resp.size(), s);
  if (r == C2Proto::ERR_CONFIRM) return false;
  if (r != C2Proto::OK) die("handshake: bad answer");
  return true;
}

static JsonDocument exchange(const Device &d, C2Proto::Session &s, JsonDocument &msg) {
  std::string plain;
  serializeJson(msg, plain);
  Bytes body(C2Proto::ID_SIZE + plain.size() + C2Proto::RECORD_OVERHEAD), resp;
  memcpy(body.data(), s.id, C2Proto::ID_SIZE);
  size_t n = 0;
  if (!s.seal((const uint8_t *)plain.data(), plain.size(), body.data() + C2Proto::ID_SIZE, n)) die("seal failed");
  int code = http(d.url, "POST", "/v1/msg", body, resp);
  if (code != 200) die("msg: HTTP " + std::to_string(code));
  Bytes out(resp.size());
  if (!s.open(resp.data(), resp.size(), out.data(), n)) die("the server's answer does not open");
  JsonDocument reply;
  if (deserializeJson(reply, (const char *)out.data(), n)) die("the server's answer is not JSON");
  return reply;
}

int main(int argc, char **argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "selftest") {
    const char *failed = C2Proto::selfTest();
    if (failed) die(std::string("self-test failed: ") + failed);
    puts("self-test ok");
    return 0;
  }
  if (cmd == "enroll" && argc == 4) {
    // An address instead of a token: without invitation, as c2.cpp does it -
    // no secret, and the server's key is taken as shown.
    bool open = !strncmp(argv[3], "http://", 7) || !strncmp(argv[3], "https://", 8);
    C2Proto::Token tok;
    if (open) {
      tok.url = argv[3];
      memset(tok.id, 0, sizeof(tok.id));
      memset(tok.secret, 0, sizeof(tok.secret));
    } else if (!C2Proto::parseToken(argv[3], tok)) die("not a valid enrollment token");
    Device d;
    d.url = tok.url;
    C2Proto::generateKey(d.priv);
    Bytes key;
    if (http(d.url, "GET", "/v1/server-key", {}, key) != 200 || key.size() != C2Proto::PUB_SIZE) die("cannot fetch the server key");
    if (!open && !C2Proto::tokenPinsServer(tok, key.data())) die("server key does not match the token");
    memcpy(d.serverPub, key.data(), C2Proto::PUB_SIZE);
    C2Proto::Session s = handshake(d, C2Proto::KIND_ENROLL, &tok);
    JsonDocument msg;
    msg["t"] = "enroll";
    msg["name"] = "firmware-code-on-host";
    msg["info"]["fw"] = "host-test";
    JsonDocument reply = exchange(d, s, msg);
    save(argv[2], d);
    uint8_t pub[C2Proto::PUB_SIZE], fp[32];
    C2Proto::publicKey(d.priv, pub);
    C2Proto::fingerprint(pub, fp);
    printf("ID ");
    for (size_t i = 0; i < C2Proto::ID_SIZE; i++) printf("%02x", fp[i]);
    printf("\nCODE %s\nSTATE %s\n", s.sas, reply["state"] | "?");
    return 0;
  }
  if (cmd == "poll" && argc == 3) {
    Device d = load(argv[2]);
    C2Proto::Session s = sessionHandshake(d, argv[2]);
    char tag[C2Proto::KEY_TAG_SIZE];
    C2Proto::keyTag(d.haveNext ? d.nextPub : d.serverPub, tag);
    JsonDocument msg;
    msg["t"] = "poll";
    msg["sk"] = tag;
    msg["status"]["uptime_s"] = 1;
    JsonDocument reply = exchange(d, s, msg);
    printf("STATE %s\n", reply["state"] | "?");
    uint8_t announced[C2Proto::PUB_SIZE];
    if (C2Proto::announcedKey(reply["rekey"] | "", d.serverPub, announced) &&
        !(d.haveNext && !memcmp(announced, d.nextPub, C2Proto::PUB_SIZE))) {
      memcpy(d.nextPub, announced, C2Proto::PUB_SIZE);
      d.haveNext = true;
      save(argv[2], d);
      C2Proto::keyTag(d.nextPub, tag);
      printf("KEY announced %s\n", tag);
    }
    JsonDocument next;
    next["t"] = "poll";
    JsonArray results = next["results"].to<JsonArray>();
    for (JsonObject c : reply["cmds"].as<JsonArray>()) {
      printf("CMD %lu %s\n", (unsigned long)(c["id"] | 0UL), c["type"] | "?");
      JsonObject r = results.add<JsonObject>();
      r["id"] = c["id"];
      r["ok"] = true;
      r["out"] = "pong from firmware code";
    }
    if (results.size()) exchange(d, s, next);     // second record on the same session
    return 0;
  }
  if (cmd == "update" && argc == 5) {
    Device d = load(argv[2]);
    std::ifstream kf(argv[3], std::ios::binary);
    Bytes key((std::istreambuf_iterator<char>(kf)), std::istreambuf_iterator<char>());
    if (key.size() != C2Update::KEY_SIZE) die("the key file is not a raw ML-DSA-65 public key");
    C2Proto::Session s = sessionHandshake(d, argv[2]);
    JsonDocument msg;
    msg["t"] = "poll";
    JsonDocument reply = exchange(d, s, msg);
    unsigned long id = 0;
    for (JsonObject c : reply["cmds"].as<JsonArray>())
      if (!strcmp(c["type"] | "", "update")) id = c["id"] | 0UL;
    if (!id) die("no update command waiting");
    auto ask = [&](long off) {
      JsonDocument m;
      m["t"] = "fw";
      m["id"] = id;
      if (off < 0) m["head"] = true;
      else m["off"] = off;
      return exchange(d, s, m);
    };
    std::string why;
    C2Update::Manifest m;
    Bytes image;
    {
      JsonDocument head = ask(-1);
      std::string manifest = head["m"] | "";
      const char *sig64 = head["sig"] | "";
      Bytes sig(C2Update::SIG_SIZE + 16);
      size_t sigLen = 0;
      if (mbedtls_base64_decode(sig.data(), sig.size(), &sigLen, (const uint8_t *)sig64, strlen(sig64))) why = "the package is not readable";
      else if (const char *err = C2Update::check(manifest.data(), manifest.size(), sig.data(), sigLen, key.data(), "waveshare-s3-lcd2",
                                                 (uint32_t)strtoul(argv[4], nullptr, 10), 6 << 20, m))
        why = err;
    }
    while (why.empty() && image.size() < m.size) {
      JsonDocument part = ask((long)image.size());
      const char *b64 = part["d"] | "";
      uint8_t buf[4096];
      size_t n = 0;
      if (mbedtls_base64_decode(buf, sizeof(buf), &n, (const uint8_t *)b64, strlen(b64)) || !n || image.size() + n > m.size) why = "the download is damaged";
      else image.insert(image.end(), buf, buf + n);
    }
    if (why.empty()) {
      uint8_t hash[32];
      mbedtls_sha256_ret(image.data(), image.size(), hash, 0);
      if (memcmp(hash, m.sha256, 32)) why = "the image is not the one that was signed";
    }
    JsonDocument next;
    next["t"] = "poll";
    JsonObject r = next["results"].to<JsonArray>().add<JsonObject>();
    r["id"] = id;
    r["ok"] = why.empty();
    r["out"] = why.empty() ? std::string("updated to ") + m.fw + " (" + m.lang + "), restarting" : why;
    exchange(d, s, next);
    if (why.empty()) printf("UPDATE ok %s %s %zu\n", m.fw, m.lang, image.size());
    else printf("UPDATE refused: %s\n", why.c_str());
    return 0;
  }
  if (cmd == "console" && argc == 3) {
    Device d = load(argv[2]);
    C2Proto::Session s = sessionHandshake(d, argv[2]);
    time_t start = time(nullptr);
    // the request, and the yes that a tap on the display gives on the device
    uint32_t id = 0;
    JsonDocument yes;
    yes["t"] = "poll";
    while (!id) {
      JsonDocument msg;
      msg["t"] = "poll";
      JsonDocument reply = exchange(d, s, msg);
      for (JsonObject c : reply["cmds"].as<JsonArray>()) {
        if (strcmp(c["type"] | "", "console")) continue;
        id = c["id"] | 0U;
        JsonObject r = yes["results"].to<JsonArray>().add<JsonObject>();
        r["id"] = id;
        r["ok"] = true;
        r["out"]["port"] = c["args"]["port"];
        r["out"]["minutes"] = c["args"]["minutes"];
      }
      if (!id) {
        if (time(nullptr) - start > 20) die("console: nobody asked");
        usleep(200000);
      }
    }
    exchange(d, s, yes);
    printf("CONSOLE %lu open\n", (unsigned long)id);
    fflush(stdout);

    // The "port": greets with more than one message holds, echoes what is
    // typed and answers a BREAK.
    static C2Console::Stream st;
    for (int i = 0; i < 200; i++) st.outPut((const uint8_t *)"0123456789", 10);
    std::string typed;
    unsigned breaks = 0;
    for (unsigned round = 1;; round++) {
      if (time(nullptr) - start > 90) die("console: still open after 90 s");
      JsonDocument msg;
      size_t n = C2Console::buildMessage(msg, id, st, 60, "");
      JsonDocument reply = exchange(d, s, msg);
      // Every third answer is "lost": the same message goes out again, as the
      // device does it. Nothing may arrive twice, in either direction.
      if (round % 3 == 0) reply = exchange(d, s, msg);
      C2Console::accepted(st, n);
      if (const char *end = C2Console::applyReply(reply, st)) {
        printf("CLOSED %s\n", end);
        break;
      }
      // the "SD card" records when the server asks, as the device does it
      if (st.recWant && !st.rec[0]) strcpy(st.rec, "/logs/port1-001.log");
      if (!st.recWant) st.rec[0] = 0;
      size_t take = st.inLen < st.outRoom() ? st.inLen : st.outRoom();
      typed.append((const char *)st.in, take);
      st.outPut(st.in, take);
      st.inDrop(take);
      for (; st.brkDue && st.outRoom() >= 7; st.brkDue--, breaks++) st.outPut((const uint8_t *)"[BREAK]", 7);
    }
    printf("TYPED ");
    for (unsigned char c : typed) printf("%02x", c);
    printf("\nBRK %u\n", breaks);
    return 0;
  }
  die("usage: c2host selftest | enroll STATEFILE TOKEN | poll STATEFILE | console STATEFILE");
}
