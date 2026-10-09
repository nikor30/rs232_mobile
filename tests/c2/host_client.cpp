// The firmware's protocol code (firmware/src/c2_proto.cpp) on a PC, talking to
// the real server over plain HTTP. Driven by run.sh.
//
//   c2host selftest
//   c2host enroll STATEFILE TOKEN      -> "ID <hex>", "CODE <six digits>"
//   c2host poll STATEFILE              -> "STATE <state>", "CMD <id> <type>" ...
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include "c2_proto.h"

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
  std::string url;
};

static void save(const char *file, const Device &d) {
  std::ofstream f(file, std::ios::binary);
  f.write((const char *)d.priv, sizeof(d.priv)).write((const char *)d.serverPub, sizeof(d.serverPub)) << d.url;
}

static Device load(const char *file) {
  std::ifstream f(file, std::ios::binary);
  Device d;
  if (!f.read((char *)d.priv, sizeof(d.priv)).read((char *)d.serverPub, sizeof(d.serverPub))) die(std::string("cannot read ") + file);
  std::getline(f, d.url, '\0');
  return d;
}

static C2Proto::Session handshake(const Device &d, C2Proto::Kind kind, const C2Proto::Token *tok) {
  C2Proto::Handshake hs;
  Bytes req(C2Proto::REQ_ENROLL), resp;
  size_t n = 0;
  if (hs.begin(kind, d.priv, d.serverPub, tok ? tok->id : nullptr, tok ? tok->secret : nullptr, req.data(), n) != C2Proto::OK)
    die("handshake: cannot build the request");
  req.resize(n);
  int code = http(d.url, "POST", "/v1/handshake", req, resp);
  if (code != 200) die("handshake: HTTP " + std::to_string(code));
  C2Proto::Session s;
  C2Proto::Result r = hs.finish(resp.data(), resp.size(), s);
  if (r == C2Proto::ERR_CONFIRM) die("handshake: the server did not prove its key");
  if (r != C2Proto::OK) die("handshake: bad answer");
  return s;
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
    C2Proto::Token tok;
    if (!C2Proto::parseToken(argv[3], tok)) die("not a valid enrollment token");
    Device d;
    d.url = tok.url;
    C2Proto::generateKey(d.priv);
    Bytes key;
    if (http(d.url, "GET", "/v1/server-key", {}, key) != 200 || key.size() != C2Proto::PUB_SIZE) die("cannot fetch the server key");
    if (!C2Proto::tokenPinsServer(tok, key.data())) die("server key does not match the token");
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
    C2Proto::Session s = handshake(d, C2Proto::KIND_SESSION, nullptr);
    JsonDocument msg;
    msg["t"] = "poll";
    msg["status"]["uptime_s"] = 1;
    JsonDocument reply = exchange(d, s, msg);
    printf("STATE %s\n", reply["state"] | "?");
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
  die("usage: c2host selftest | enroll STATEFILE TOKEN | poll STATEFILE");
}
