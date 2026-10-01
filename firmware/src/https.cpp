#include "https.h"
#include "certs.h"
#include "compat_mbedtls.h"
#include "config.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <esp_random.h>

#include <atomic>
#include <mutex>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

namespace Https {

// Local targets of the front end. The host simulation uses a separate loopback
// address so that "came through TLS" can be told apart from a normal client.
#ifndef TLS_LOOPBACK_IP
#define TLS_LOOPBACK_IP 0x7F000001UL           // 127.0.0.1
#endif
static const uint16_t TLS_PORT = 443;
static const int MAX_CONN = 3;                 // RAM: ~35 kB heap per TLS session
static const size_t BUFSZ = 1024;
static const uint32_t HANDSHAKE_TIMEOUT = 20000, IDLE_TIMEOUT = 120000, WS_TIMEOUT = 900000;

struct Conn {
  int cs = -1, ls = -1;                        // client (TLS) and local (plain) socket
  mbedtls_ssl_context ssl;
  bool used = false, ws = false, clientEof = false, localEof = false, halfClosed = false;
  uint8_t st = 0;                              // 0 handshake, 1 read request line, 2 forwarding
  bool wantRd = true, wantWr = false;          // what the TLS layer is waiting for
  uint8_t up[BUFSZ], down[BUFSZ];              // browser -> device, device -> browser
  size_t upLen = 0, upOff = 0, downLen = 0, downOff = 0;
  uint32_t startMs = 0, lastMs = 0;
};

static Conn conn[MAX_CONN];
static int listenFd = -1;
static mbedtls_ssl_config conf;
static mbedtls_x509_crt srvCrt;
static mbedtls_pk_context srvKey;
static mbedtls_ssl_cache_context cache;
static bool tlsReady = false, ctxInit = false;
static uint32_t certGen = 0;
static std::atomic<int> active{0};
static std::atomic<bool> listening{false};
static String certText, errText;
static std::mutex txtLock;

bool enabled() { return settings.httpsEnabled; }
bool running() { return listening.load(); }
uint8_t sessions() { return (uint8_t)active.load(); }
uint16_t port() { return TLS_PORT; }

static void setText(String &dst, const String &v) {
  std::lock_guard<std::mutex> l(txtLock);
  dst = v;
}
String certName() {
  std::lock_guard<std::mutex> l(txtLock);
  return certText;
}
String lastError() {
  std::lock_guard<std::mutex> l(txtLock);
  return errText;
}

bool fromProxy(const IPAddress &ip) {
  uint32_t v = TLS_LOOPBACK_IP;
  return ip[0] == (uint8_t)(v >> 24) && ip[1] == (uint8_t)(v >> 16) && ip[2] == (uint8_t)(v >> 8) &&
         ip[3] == (uint8_t)v;
}

static int rng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

// ---------------------------------------------------------------- sockets
static void setNonBlocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int bioSend(void *ctx, const unsigned char *buf, size_t len) {
  int fd = (int)(intptr_t)ctx;
  int n = send(fd, buf, len, 0);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
  return n;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len) {
  int fd = (int)(intptr_t)ctx;
  int n = recv(fd, buf, len, 0);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
  return n;                                    // 0 = closed by the browser
}

// plain connection to the web server (80) or WebSocket server (81) of this device
static int localConnect(uint16_t p) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in src;
  memset(&src, 0, sizeof(src));
  src.sin_family = AF_INET;
  src.sin_addr.s_addr = htonl(TLS_LOOPBACK_IP);
  bind(fd, (struct sockaddr *)&src, sizeof(src));      // source address marks "came through TLS"
  struct sockaddr_in dst = src;
  dst.sin_port = htons(p);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setNonBlocking(fd);                                  // never block the TLS task
  if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0 && errno != EINPROGRESS && errno != EALREADY) {
    close(fd);
    return -1;
  }
  return fd;
}

static void closeConn(Conn &c) {
  if (c.used) {
    mbedtls_ssl_free(&c.ssl);
    active--;
  }
  if (c.cs >= 0) close(c.cs);
  if (c.ls >= 0) close(c.ls);
  c = Conn();
}

// ---------------------------------------------------------------- TLS setup
static Certs::Slot pickSlot() {
  if (settings.httpsCert == 1 && Certs::serverUsable(Certs::CLIENT)) return Certs::CLIENT;
  return Certs::HTTPS;
}

static bool setupTls() {
  Certs::Slot s = pickSlot();
  String crtPem, keyPem;
  Certs::copy(s, crtPem, keyPem);
  if (crtPem.isEmpty() || keyPem.isEmpty()) {
    if (settings.httpsCert == 1 && !Certs::hasCert(Certs::CLIENT))
      setText(errText, "Client-Zertifikat fehlt");
    return false;
  }
  if (ctxInit) {                                 // free what a previous certificate allocated
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&srvCrt);
    mbedtls_pk_free(&srvKey);
    mbedtls_ssl_cache_free(&cache);
    ctxInit = false;
  }
  mbedtls_ssl_config_init(&conf);
  mbedtls_x509_crt_init(&srvCrt);
  mbedtls_pk_init(&srvKey);
  mbedtls_ssl_cache_init(&cache);
  int r = mbedtls_x509_crt_parse(&srvCrt, (const uint8_t *)crtPem.c_str(), crtPem.length() + 1);
  if (!r) r = rsPkParseKey(&srvKey, (const uint8_t *)keyPem.c_str(), keyPem.length() + 1, nullptr, 0);
  if (!r) r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (!r) {
    rsSslMinTls12(&conf);                                  // no TLS below 1.2
    mbedtls_ssl_conf_rng(&conf, rng, nullptr);
    mbedtls_ssl_cache_set_max_entries(&cache, 4);
    mbedtls_ssl_conf_session_cache(&conf, &cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
    r = mbedtls_ssl_conf_own_cert(&conf, &srvCrt, &srvKey);
  }
  ctxInit = true;
  if (r) {
    char e[100];
    mbedtls_strerror(r, e, sizeof(e));
    setText(errText, String("TLS-Setup: ") + e);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&srvCrt);
    mbedtls_pk_free(&srvKey);
    mbedtls_ssl_cache_free(&cache);
    ctxInit = false;
    return false;
  }
  String name = Certs::summary(s);
  setText(certText, name + (s == Certs::CLIENT ? " (802.1X-Zertifikat)" : Certs::httpsIsAuto() ? " (Geräte-CA)" : ""));
  setText(errText, "");
  tlsReady = true;
  certGen = Certs::generation();
  Serial.printf("[TLS] HTTPS-Zertifikat: %s\n", certName().c_str());
  return true;
}

// a TLS session needs two ~16.5 kB buffers plus context
static bool memOk() {
  return ESP.getFreeHeap() > 70000 && ESP.getMaxAllocHeap() > 20000;
}

// ---------------------------------------------------------------- per connection
static void startConn(int fd) {
  for (auto &c : conn) {
    if (c.cs >= 0) continue;
    c = Conn();
    c.cs = fd;
    mbedtls_ssl_init(&c.ssl);
    if (mbedtls_ssl_setup(&c.ssl, &conf)) {    // out of memory
      mbedtls_ssl_free(&c.ssl);
      close(fd);
      c = Conn();
      setText(errText, "Zu wenig Speicher für eine weitere TLS-Verbindung");
      return;
    }
    c.used = true;
    active++;
    mbedtls_ssl_set_bio(&c.ssl, (void *)(intptr_t)fd, bioSend, bioRecv, nullptr);
    c.startMs = c.lastMs = millis();
    return;
  }
  close(fd);                                   // no free slot
}

// request line of the first request decides the target: "GET /ws..." -> WebSocket server
static bool route(Conn &c) {
  int n = mbedtls_ssl_read(&c.ssl, c.up + c.upLen, sizeof(c.up) - c.upLen);
  if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
  if (n <= 0) return false;
  c.upLen += n;
  c.lastMs = millis();
  const char *nl = (const char *)memchr(c.up, '\n', c.upLen);
  if (!nl) return c.upLen < sizeof(c.up);
  String line((const char *)c.up, nl - (const char *)c.up);
  int sp = line.indexOf(' ');
  String path = sp > 0 ? line.substring(sp + 1) : String();
  path.trim();
  c.ws = path.startsWith("/ws");
  c.ls = localConnect(c.ws ? WS_PORT : HTTP_PORT);
  if (c.ls < 0) return false;
  c.st = 2;
  return true;
}

static void service(Conn &c) {
  uint32_t now = millis();
  if (c.st == 0) {
    int r = mbedtls_ssl_handshake(&c.ssl);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
      c.wantRd = r == MBEDTLS_ERR_SSL_WANT_READ;
      c.wantWr = !c.wantRd;
      if (now - c.startMs > HANDSHAKE_TIMEOUT) closeConn(c);
      return;
    }
    if (r) {
      if (r != MBEDTLS_ERR_SSL_CONN_EOF && r != MBEDTLS_ERR_NET_RECV_FAILED) {
        char e[100];
        mbedtls_strerror(r, e, sizeof(e));
        setText(errText, String("Handshake: ") + e);
      }
      closeConn(c);
      return;
    }
    c.st = 1;
    c.wantRd = true;
    c.wantWr = false;
    c.lastMs = now;
    return;
  }
  if (c.st == 1) {
    if (!route(c)) closeConn(c);
    else if (now - c.startMs > HANDSHAKE_TIMEOUT) closeConn(c);
    return;
  }

  // ---- forwarding
  bool tlsWantRd = false, tlsWantWr = false;
  if (c.upOff < c.upLen) {                     // browser -> device
    int n = send(c.ls, c.up + c.upOff, c.upLen - c.upOff, 0);
    if (n > 0) {
      c.upOff += n;
      c.lastMs = now;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      closeConn(c);
      return;
    }
  }
  if (c.upOff == c.upLen && !c.clientEof) {
    c.upLen = c.upOff = 0;
    int n = mbedtls_ssl_read(&c.ssl, c.up, sizeof(c.up));
    if (n > 0) {
      c.upLen = n;
      c.lastMs = now;
    } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
      tlsWantRd = tlsWantRd || n == MBEDTLS_ERR_SSL_WANT_READ;
      tlsWantWr = tlsWantWr || n == MBEDTLS_ERR_SSL_WANT_WRITE;
    } else {
      c.clientEof = true;                      // browser closed: let the web server finish and close
    }
  }
  if (c.downOff < c.downLen) {                 // device -> browser
    int n = mbedtls_ssl_write(&c.ssl, c.down + c.downOff, c.downLen - c.downOff);
    if (n > 0) {
      c.downOff += n;
      c.lastMs = now;
    } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
      tlsWantRd = tlsWantRd || n == MBEDTLS_ERR_SSL_WANT_READ;
      tlsWantWr = true;
    } else {
      closeConn(c);
      return;
    }
  }
  if (c.downOff == c.downLen && !c.localEof) {
    c.downLen = c.downOff = 0;
    int n = recv(c.ls, c.down, sizeof(c.down), 0);
    if (n > 0) {
      c.downLen = n;
      c.lastMs = now;
    } else if (n == 0) {
      c.localEof = true;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      c.localEof = true;
    }
  }
  // the browser is gone and everything it sent is delivered: tell the web server
  if (c.clientEof && c.upOff == c.upLen && !c.halfClosed) {
    c.halfClosed = true;
    shutdown(c.ls, SHUT_WR);
  }
  // wait only for what is really pending, otherwise select() returns at once every time
  c.wantRd = tlsWantRd || (!c.clientEof && c.upOff == c.upLen);
  c.wantWr = tlsWantWr || c.downOff < c.downLen;
  if (c.localEof && c.downOff == c.downLen) {  // answer complete (the web server closes after it)
    mbedtls_ssl_close_notify(&c.ssl);
    closeConn(c);
    return;
  }
  if (c.clientEof && c.downOff == c.downLen && c.localEof) {
    closeConn(c);
    return;
  }
  uint32_t limit = c.ws ? WS_TIMEOUT : IDLE_TIMEOUT;
  if (now - c.lastMs > limit) closeConn(c);
}

// ---------------------------------------------------------------- task
static void tlsTask(void *) {
  while (true) {
    if (!tlsReady) {
      if (listenFd >= 0) {                     // no usable certificate: do not leave clients hanging
        close(listenFd);
        listenFd = -1;
        listening.store(false);
      }
      if (!setupTls()) {                       // e.g. certificate still being generated
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        continue;
      }
    }
    if (listenFd < 0) {
      listenFd = socket(AF_INET, SOCK_STREAM, 0);
      int one = 1;
      setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      a.sin_port = htons(TLS_PORT);
      if (bind(listenFd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(listenFd, 4) < 0) {
        close(listenFd);
        listenFd = -1;
        setText(errText, "Port 443 nicht verfügbar");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        continue;
      }
      setNonBlocking(listenFd);
      listening.store(true);
      Serial.printf("[TLS] HTTPS auf Port %u bereit\n", TLS_PORT);
    }

    // new certificate: close the sessions (they still reference the old context), then reload
    if (certGen != Certs::generation()) {
      for (auto &c : conn)
        if (c.cs >= 0) closeConn(c);
      tlsReady = false;
      continue;
    }

    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int maxFd = listenFd;
    bool room = active.load() < MAX_CONN && memOk();
    if (room) FD_SET(listenFd, &rd);
    bool immediate = false;
    for (auto &c : conn) {
      if (c.cs < 0) continue;
      if (c.wantRd) FD_SET(c.cs, &rd);
      if (c.wantWr) FD_SET(c.cs, &wr);
      // already decrypted data waiting in the TLS buffer: do not wait for the socket
      if (c.st == 2 && c.upOff == c.upLen && mbedtls_ssl_get_bytes_avail(&c.ssl) > 0) immediate = true;
      if (c.cs > maxFd) maxFd = c.cs;
      if (c.ls >= 0) {
        if (c.downOff == c.downLen && !c.localEof) FD_SET(c.ls, &rd);
        if (c.upOff < c.upLen) FD_SET(c.ls, &wr);
        if (c.ls > maxFd) maxFd = c.ls;
      }
    }
    struct timeval tv = {0, immediate ? 0 : 50000};
    if (select(maxFd + 1, &rd, &wr, nullptr, &tv) < 0) {
      vTaskDelay(20 / portTICK_PERIOD_MS);     // the fd sets are undefined now
      continue;
    }

    if (room && FD_ISSET(listenFd, &rd)) {
      struct sockaddr_in peer;
      socklen_t pl = sizeof(peer);
      int fd = accept(listenFd, (struct sockaddr *)&peer, &pl);
      if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setNonBlocking(fd);
        startConn(fd);
      }
    }
    for (auto &c : conn)
      if (c.cs >= 0) service(c);
  }
}

void begin() {
  if (!settings.httpsEnabled) return;
  if (xTaskCreatePinnedToCore(tlsTask, "https", 10240, nullptr, 1, nullptr, 0) != pdPASS)
    setText(errText, "Task konnte nicht gestartet werden");
}

}  // namespace Https
