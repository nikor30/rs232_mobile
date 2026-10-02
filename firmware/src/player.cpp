#include "player.h"
#include "configs.h"
#include "serial_bridge.h"
#include "net.h"

namespace Player {

static State st = IDLE;
static String text;                  // the whole configuration, lines separated by '\n'
static char cfgName[Configs::MAX_NAME + 1];
static char res[48];
static uint8_t prt = 0;
static uint16_t lineNo = 0, lineCount = 0;
static size_t pos = 0;               // start of the next line in text

// what the device has sent since the last line went out
static char tail[256];
static size_t tailLen = 0;
static uint32_t seq = 0;             // read position in the port's replay ring
static uint32_t got = 0;             // bytes received since the line was sent

enum Phase : uint8_t { NEXT, WAIT_PROMPT, WAIT_EXPECT, WAIT_TIME, SEND_WAIT };
static Phase phase = NEXT;
static uint32_t phaseStart = 0, quietSince = 0, waitUntil = 0, timeoutMs = 30000;
static String expect, pending;       // text waited for / line that did not fit the send queue yet
static bool moreAnswered = false;
static uint16_t errors = 0;

State state() { return st; }
const char *name() { return cfgName; }
uint8_t port() { return prt; }
uint16_t line() { return lineNo; }
uint16_t lines() { return lineCount; }
const char *result() { return res; }

static void setResult(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(res, sizeof(res), fmt, ap);
  va_end(ap);
}

static void finish(State s) {
  st = s;
  text = String();
  pending = String();
  Net::onMessage(String("Konfiguration ") + cfgName + ": " + res);
  Net::markDirty();
}

static void drain() {
  uint8_t buf[128];
  uint32_t start = Bridge::ringStartSeq(prt);
  if ((int32_t)(start - seq) > 0) seq = start;
  for (;;) {
    size_t n = Bridge::copyFrom(prt, seq, buf, sizeof(buf));
    if (!n) break;
    seq += n;
    got += n;
    quietSince = millis();
    for (size_t i = 0; i < n; i++) {
      if (tailLen == sizeof(tail) - 1) {
        memmove(tail, tail + 64, tailLen - 64);
        tailLen -= 64;
      }
      tail[tailLen++] = buf[i] ? buf[i] : ' ';
    }
    tail[tailLen] = 0;
  }
}

static bool containsNoCase(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++)
    if (!strncasecmp(hay, needle, n)) return true;
  return false;
}

// the web UI's ERROR_RE, spelled out
static bool looksLikeError() {
  static const char *const WORDS[] = {"% Invalid", "%Invalid", "% Incomplete", "%Incomplete", "% Ambiguous",
                                      "% Unknown", "% Unrecognized", "% Bad", "% Error", "%Error",
                                      "Command fail", "\nerror:", "unknown command", "Unknown action"};
  for (const char *w : WORDS)
    if (containsNoCase(tail, w)) return true;
  return false;
}

// the web UI's PROMPT_RE: the output ends in one of > # $ % ] : ) and maybe a blank
static bool atPrompt() {
  size_t n = tailLen;
  if (n && tail[n - 1] == ' ') n--;
  return n && strchr(">#$%]:)", tail[n - 1]);
}

static bool endsWithMore() {
  size_t n = tailLen;
  while (n && (tail[n - 1] == ' ' || tail[n - 1] == '\r' || tail[n - 1] == '\n')) n--;
  return n >= 8 && !strncmp(tail + n - 8, "--More--", 8);
}

static void resetTail() {
  tailLen = 0;
  tail[0] = 0;
  got = 0;
  quietSince = millis();
  moreAnswered = false;
}

bool start(const String &cfg, uint8_t port) {
  if (st == RUNNING) { setResult("laeuft schon"); return false; }
  strlcpy(cfgName, cfg.c_str(), sizeof(cfgName));
  prt = port;
  lineNo = lineCount = 0;
  if (port >= MAX_PORTS || !Bridge::enabled(port)) { setResult("Port ist aus"); st = FAILED; return false; }
  if (!Configs::read(cfg, text)) { setResult("nicht gefunden"); st = FAILED; return false; }
  if (text.indexOf("{{") >= 0) {
    text = String();
    setResult("hat {{Variablen}}: nur im Web");
    st = FAILED;
    return false;
  }
  text.replace("\r\n", "\n");
  text.replace('\r', '\n');
  while (text.endsWith("\n")) text.remove(text.length() - 1);
  if (!text.length()) { setResult("ist leer"); st = FAILED; return false; }
  lineCount = 1;
  for (size_t i = 0; i < text.length(); i++)
    if (text[i] == '\n') lineCount++;
  pos = 0;
  errors = 0;
  timeoutMs = 30000;
  seq = Bridge::seqNow(port);
  resetTail();
  phase = NEXT;
  st = RUNNING;
  setResult("laeuft");
  Serial.printf("[CFG]  spiele \"%s\" auf Port %u ab, %u Zeilen\n", cfgName, port + 1, lineCount);
  Net::markDirty();
  return true;
}

void stop() {
  if (st != RUNNING) return;
  setResult("abgebrochen in Zeile %u", lineNo);
  finish(FAILED);
}

static void sendLine(const String &l) {
  pending = l + "\r";
  phase = SEND_WAIT;
}

static void nextLine() {
  if (pos > text.length()) {
    if (errors) setResult("fertig, %u Fehlermeldungen", errors);
    else setResult("fertig, %u Zeilen", lineCount);
    finish(DONE);
    return;
  }
  int end = text.indexOf('\n', pos);
  if (end < 0) end = text.length();
  String l = text.substring(pos, end);
  pos = end + 1;
  lineNo++;

  if (l.startsWith("@")) {
    int sp = l.indexOf(' ');
    String cmd = sp < 0 ? l.substring(1) : l.substring(1, sp);
    String arg = sp < 0 ? String() : l.substring(sp + 1);
    cmd.toLowerCase();
    arg.trim();
    if (cmd == "pause") {
      float s = arg.toFloat();
      waitUntil = millis() + (uint32_t)((s > 0 ? s : 1) * 1000);
      phase = WAIT_TIME;
    } else if (cmd == "expect") {
      expect = arg;
      phaseStart = millis();
      phase = WAIT_EXPECT;
    } else if (cmd == "break") {
      int ms = arg.toInt();
      if (ms <= 0) ms = 500;
      Bridge::sendBreak(prt, ms);
      waitUntil = millis() + 300;
      phase = WAIT_TIME;
    } else if (cmd == "timeout") {
      float s = arg.toFloat();
      timeoutMs = (uint32_t)((s >= 1 ? s : 30) * 1000);
    } else {
      setResult("unbekannte Zeile @%s (%u)", cmd.c_str(), lineNo);
      finish(FAILED);
    }
    return;
  }
  resetTail();
  sendLine(l);
}

void loop() {
  if (st != RUNNING) return;
  if (!Bridge::enabled(prt)) { setResult("Port wurde abgeschaltet"); finish(FAILED); return; }
  drain();
  uint32_t now = millis();
  switch (phase) {
    case NEXT:
      nextLine();
      break;
    case SEND_WAIT:                    // the port's send queue takes the line when it has room
      if (Bridge::txFree(prt) < pending.length()) break;
      Bridge::write(prt, (const uint8_t *)pending.c_str(), pending.length());
      pending = String();
      phaseStart = now;
      quietSince = now;
      phase = WAIT_PROMPT;
      break;
    case WAIT_TIME:
      if ((int32_t)(now - waitUntil) >= 0) phase = NEXT;
      break;
    case WAIT_EXPECT:
      if (containsNoCase(tail, expect.c_str())) { resetTail(); phase = NEXT; }
      else if (now - phaseStart > timeoutMs) {
        setResult("\"%.20s\" kam nicht (Zeile %u)", expect.c_str(), lineNo);
        finish(FAILED);
      }
      break;
    case WAIT_PROMPT: {
      uint32_t quiet = now - quietSince;
      if (got && endsWithMore()) {
        if (!moreAnswered) { Bridge::write(prt, (const uint8_t *)" ", 1); moreAnswered = true; }
        else if (quiet > 300) moreAnswered = false;      // the next page has its own --More--
        if (now - phaseStart > timeoutMs) { setResult("haengt bei --More-- (Zeile %u)", lineNo); finish(FAILED); }
        break;
      }
      bool done = (got && quiet >= 120 && atPrompt()) || (quiet >= 2000 && now - phaseStart >= 2000) ||
                  now - phaseStart > timeoutMs;
      if (!done) break;
      if (looksLikeError()) {
        errors++;
        setResult("Fehlermeldung bei Zeile %u", lineNo);
        finish(FAILED);
        break;
      }
      phase = NEXT;
      break;
    }
  }
}

}  // namespace Player
