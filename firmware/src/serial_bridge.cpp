#include "serial_bridge.h"
#include "lang.h"
#include "xfer.h"
#include <SoftwareSerial.h>
#include <driver/uart.h>
#include <hal/uart_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#include <soc/uart_periph.h>

namespace Bridge {

uint32_t rxBytes[MAX_PORTS] = {0}, txBytes[MAX_PORTS] = {0};
uint32_t lastRxMs[MAX_PORTS] = {0}, lastTxMs[MAX_PORTS] = {0};
uint32_t lastUserStartMs[MAX_PORTS] = {0};
uint32_t reopened[MAX_PORTS] = {0};
char reopenWhy[MAX_PORTS][40] = {};

static DataSink dataSink = nullptr;
static MsgSink msgSink = nullptr;

struct Port {
  bool on = false;
  bool hw = false;
  HardwareSerial *hs = nullptr;
  EspSoftwareSerial::UART *ss = nullptr;
  SerialCfg cfg;
  int rx = -1, tx = -1;                 // actual pins (after swap)
  // replay ring (size is a power of two)
  uint8_t *ring = nullptr;
  uint32_t ringSize = 0;
  uint32_t total = 0;                   // seq of the next byte
  // batching: collect bytes for up to 12 ms -> fewer, larger WebSocket frames
  uint8_t pend[512];
  size_t pendLen = 0;
  uint32_t pendSince = 0;
  // send queue (all ports): user input never blocks the loop. Hardware ports hand
  // it to the UART paced by the line speed, software ports bit-bang it in slices.
  uint8_t *txq = nullptr;
  uint16_t txHead = 0, txTail = 0;      // ring of TX_QUEUE bytes
  uint32_t busyUntilUs = 0;             // hardware: when the bytes given to the UART are out
  uint32_t charUs = 1042;               // time per character on the line
  int uartNum = -1;
  // self-check (check()): since when something has looked wrong
  uint32_t checkMs = 0, fifoSince = 0, txSince = 0;
  uint32_t unansweredSince = 0;         // sent to, nothing back since then
  uint32_t queueSince = 0;              // bytes waiting and none taken since then
  uint16_t queueTail = 0;
};
static Port ports[MAX_PORTS];

static HardwareSerial *const HW_UART[HW_PORTS] = {&Serial1, &Serial2};
static const int HW_UART_NUM[HW_PORTS] = {1, 2};

// ---------------------------------------------------------------- helpers
bool enabled(uint8_t p) { return p < MAX_PORTS && ports[p].on; }
bool isHardware(uint8_t p) { return p < MAX_PORTS && ports[p].hw; }
int rxPin(uint8_t p) { return p < MAX_PORTS ? ports[p].rx : -1; }
int txPin(uint8_t p) { return p < MAX_PORTS ? ports[p].tx : -1; }

uint8_t enabledCount() {
  uint8_t n = 0;
  for (auto &pt : ports) n += pt.on;
  return n;
}

String prefix(uint8_t p) { return enabledCount() > 1 ? Store::portName(p) + " · " : String(""); }

static void msg(const String &m) {
  if (msgSink) msgSink(m);
}

static size_t txqLen(const Port &pt) { return (uint16_t)(pt.txHead + TX_QUEUE - pt.txTail) % TX_QUEUE; }

// How long the bytes handed to the UART still take on the line, in us.
// busyUntilUs is a 32-bit micros() stamp, and micros() wraps every 71.6 min:
// once a port had sent nothing for more than half of that, the old stamp
// compared as lying in the future, drain() waited for a UART that had long
// been idle, and the port sent nothing for up to 36 minutes - typed bytes
// were queued (and counted), none went out (issue #12, 18/19 Oct 2026).
// The UART is never busy for longer than BUSY_MAX_US: a stamp further ahead
// is stale and is pulled to now.
static const uint32_t BUSY_MAX_US = 5000000;
static uint32_t busyAhead(Port &pt) {
  uint32_t now = micros(), ahead = pt.busyUntilUs - now;
  if ((int32_t)ahead <= 0 || ahead > BUSY_MAX_US) {
    pt.busyUntilUs = now;
    return 0;
  }
  return ahead;
}

bool swapOk(uint8_t p) { return p < MAX_PORTS && Store::txPinOk(settings.port[p].rx); }
uint32_t maxBaud(uint8_t p) { return isHardware(p) ? 1000000 : SW_MAX_BAUD; }

static EspSoftwareSerial::Config swConfig(const SerialCfg &c) {
  int v = (c.bits - 5) & 07;
  if (c.parity == 'E') v |= EspSoftwareSerial::PARITY_EVEN;
  else if (c.parity == 'O') v |= EspSoftwareSerial::PARITY_ODD;
  if (c.stop == 2) v |= 0200;
  return (EspSoftwareSerial::Config)v;
}

// ---------------------------------------------------------------- replay ring
static void ringPut(Port &pt, const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) pt.ring[(pt.total + i) & (pt.ringSize - 1)] = d[i];
  pt.total += n;
}

uint32_t seqNow(uint8_t p) { return enabled(p) ? ports[p].total : 0; }
uint32_t ringStartSeq(uint8_t p) {
  if (!enabled(p)) return 0;
  return ports[p].total > ports[p].ringSize ? ports[p].total - ports[p].ringSize : 0;
}

size_t copyFrom(uint8_t p, uint32_t fromSeq, uint8_t *out, size_t max) {
  if (!enabled(p)) return 0;
  Port &pt = ports[p];
  if (fromSeq < ringStartSeq(p)) fromSeq = ringStartSeq(p);
  if (fromSeq >= pt.total) return 0;
  size_t n = min((size_t)(pt.total - fromSeq), max);
  for (size_t i = 0; i < n; i++) out[i] = pt.ring[(fromSeq + i) & (pt.ringSize - 1)];
  return n;
}

static void flushPending(uint8_t p) {
  Port &pt = ports[p];
  if (!pt.pendLen) return;
  ringPut(pt, pt.pend, pt.pendLen);
  rxBytes[p] += pt.pendLen;
  lastRxMs[p] = millis();
  pt.unansweredSince = 0;
  if (dataSink) dataSink(p, pt.pend, pt.pendLen);
  pt.pendLen = 0;
}

// ---------------------------------------------------------------- low level I/O
static void openPort(uint8_t p, const SerialCfg &c) {
  Port &pt = ports[p];
  pt.cfg = c;
  bool swap = c.swap && swapOk(p);                // never make an input-only GPIO the TX pin
  pt.rx = swap ? settings.port[p].tx : settings.port[p].rx;
  pt.tx = swap ? settings.port[p].rx : settings.port[p].tx;
  pt.charUs = (1 + c.bits + (c.parity != 'N' ? 1 : 0) + c.stop) * 1000000UL / (c.baud ? c.baud : 9600);
  pt.txHead = pt.txTail = 0;
  pt.busyUntilUs = micros();
  pt.fifoSince = pt.txSince = pt.unansweredSince = pt.queueSince = 0;
  if (pt.hw) {
    pt.hs->end();
    pt.hs->setRxBufferSize(UART_RX_BUF);
    pt.hs->setTxBufferSize(UART_TX_BUF);
    pt.hs->begin(c.baud, Store::serialConfigValue(c), pt.rx, pt.tx);
  } else {
    pt.ss->end();
    pt.ss->begin(c.baud, swConfig(c), pt.rx, pt.tx, false, SW_RX_BUF, SW_ISR_BUF);
  }
}

static int portAvailable(Port &pt) { return pt.hw ? pt.hs->available() : pt.ss->available(); }

static size_t portRead(Port &pt, uint8_t *buf, size_t n) {
  if (pt.hw) return pt.hs->read(buf, n);
  int r = pt.ss->read(buf, n);
  return r > 0 ? (size_t)r : 0;
}

static void discardInput(Port &pt) {
  uint8_t tmp[64];
  while (portAvailable(pt) > 0 && portRead(pt, tmp, sizeof(tmp))) {}
}

// queue the bytes; the loop sends them (drain)
static size_t portWrite(uint8_t p, const uint8_t *d, size_t n) {
  Port &pt = ports[p];
  size_t i = 0;
  while (i < n && txqLen(pt) < TX_QUEUE - 1) {
    pt.txq[pt.txHead] = d[i++];
    pt.txHead = (pt.txHead + 1) % TX_QUEUE;
  }
  return i;
}

static size_t txqPeek(const Port &pt, uint8_t *buf, size_t n) {
  size_t len = txqLen(pt);
  if (n > len) n = len;
  for (size_t i = 0; i < n; i++) buf[i] = pt.txq[(pt.txTail + i) % TX_QUEUE];
  return n;
}

static void drain(Port &pt) {
  if (!txqLen(pt)) return;
  uint8_t buf[256];
  if (pt.hw) {
    // keep at most ~20 ms of data inside the UART driver: its write never blocks
    uint32_t ahead = busyAhead(pt);
    if (ahead >= 20000) return;
    size_t n = (20000 - ahead) / pt.charUs + 16;
    if (n > sizeof(buf)) n = sizeof(buf);
    n = txqPeek(pt, buf, n);
    size_t w = pt.hs->write(buf, n);
    pt.txTail = (pt.txTail + w) % TX_QUEUE;
    pt.busyUntilUs += w * pt.charUs;
  } else {
    // bit-banging blocks: one slice of ~2 ms per loop pass
    size_t n = max((uint32_t)1, pt.cfg.baud / 4800);
    n = txqPeek(pt, buf, min(n, (size_t)16));
    pt.ss->write(buf, n);
    pt.txTail = (pt.txTail + n) % TX_QUEUE;
  }
}

static void drainAll(Port &pt) {
  while (txqLen(pt)) {
    drain(pt);
    if (pt.hw) delayMicroseconds(200);
  }
}

size_t txFree(uint8_t p) {
  if (!enabled(p)) return 0;
  return TX_QUEUE - 1 - txqLen(ports[p]);
}

bool txIdle(uint8_t p) {
  if (!enabled(p)) return true;
  Port &pt = ports[p];
  if (txqLen(pt)) return false;
  return !pt.hw || busyAhead(pt) <= 1000;
}

bool busy() {
  for (auto &pt : ports)
    if (pt.on && txqLen(pt)) return true;
  return false;
}

// ---------------------------------------------------------------- auto baud
static const uint32_t AB_RATES[] = {9600, 115200, 19200, 38400, 57600};
static const size_t AB_COUNT = sizeof(AB_RATES) / sizeof(AB_RATES[0]);
static struct {
  bool active = false;
  uint8_t port = 0;
  size_t idx = 0;
  uint32_t t0 = 0;
  size_t n = 0, printable = 0;
  size_t bestN = 0;            // most bytes heard in any attempt ...
  uint32_t bestRate = 0;       // ... and at which rate
  SerialCfg orig;
} ab;

static void abTry() {
  Port &pt = ports[ab.port];
  SerialCfg c;
  c.baud = AB_RATES[ab.idx];
  c.swap = ab.orig.swap;
  openPort(ab.port, c);
  delay(5);
  discardInput(pt);
  uint8_t cr = '\r';
  portWrite(ab.port, &cr, 1);
  drainAll(pt);
  ab.t0 = millis();
  ab.n = ab.printable = 0;
}

static void abFinish(bool found) {
  ab.active = false;
  uint8_t p = ab.port;
  if (found) {
    SerialCfg c;
    c.baud = AB_RATES[ab.idx];
    c.swap = ab.orig.swap;
    settings.port[p].serial = c;
    Store::saveSerial(p);
    openPort(p, c);
    msg(prefix(p) + "Auto-Baud: " + Store::serialLabel(c) + TR(" detected", " erkannt", " detectado"));
    uint8_t cr = '\r';                // fresh prompt
    portWrite(p, &cr, 1);
  } else {
    openPort(p, ab.orig);
    if (!ab.bestN) {
      // not a single edge on the line: nothing ever answered our CR
      msg(prefix(p) + TR("Auto-baud: line silent - not a single byte. Check crossover (pin 2/3), ", "Auto-Baud: Leitung stumm - kein einziges Byte. Kreuzung (Pin 2/3), ", "Auto-baud: linha muda - nenhum byte. Verifique o cruzamento (pinos 2/3), ")
                      TR("GND (pin 5) and the other end", "GND (Pin 5) und Gegenstelle pruefen", "GND (pino 5) e o equipamento remoto"));
    } else {
      // bytes did arrive, they just never looked like text -> framing, not wiring
      msg(prefix(p) + TR("Auto-baud: unreadable characters only (max. ", "Auto-Baud: nur unlesbare Zeichen (max. ", "Auto-baud: só caracteres ilegíveis (máx. ") + String((unsigned)ab.bestN) + TR(" bytes at ", " Byte bei ", " bytes a ") +
          String(ab.bestRate) + TR(" baud) - the wiring is right, check data bits/parity or interference", " Baud) - Verdrahtung stimmt, Datenbits/Paritaet oder Stoerung pruefen", " baud) - a fiação está certa, verifique bits de dados/paridade ou interferência"));
    }
  }
}

static void abStep() {
  Port &pt = ports[ab.port];
  uint8_t buf[64];
  while (portAvailable(pt) > 0) {
    size_t n = portRead(pt, buf, sizeof(buf));
    if (!n) break;
    for (size_t i = 0; i < n; i++) {
      uint8_t b = buf[i];
      ab.n++;
      if ((b >= 0x20 && b < 0x7F) || b == '\r' || b == '\n' || b == '\t' || b == 0x08 || b == 0x07) ab.printable++;
    }
  }
  if (millis() - ab.t0 < 400) return;
  if (ab.n >= 2 && ab.printable * 10 >= ab.n * 9) {    // >= 90 % printable
    abFinish(true);
    return;
  }
  if (ab.n > ab.bestN) {                               // remember the liveliest attempt
    ab.bestN = ab.n;
    ab.bestRate = AB_RATES[ab.idx];
  }
  do { ab.idx++; } while (ab.idx < AB_COUNT && AB_RATES[ab.idx] > maxBaud(ab.port));
  if (ab.idx < AB_COUNT) { abTry(); return; }
  abFinish(false);
}

void startAutobaud(uint8_t p) {
  if (ab.active || !enabled(p) || Xfer::on(p)) return;
  flushPending(p);
  ab.active = true;
  ab.port = p;
  ab.idx = 0;
  ab.bestN = 0;
  ab.bestRate = 0;
  ab.orig = ports[p].cfg;
  while (ab.idx < AB_COUNT && AB_RATES[ab.idx] > maxBaud(p)) ab.idx++;
  msg(prefix(p) + TR("Auto-baud: running ...", "Auto-Baud: läuft ...", "Auto-baud: em andamento ..."));
  abTry();
}

bool autobaudRunning(uint8_t p) { return ab.active && ab.port == p; }
bool autobaudRunning() { return ab.active; }

// ---------------------------------------------------------------- public
void begin(DataSink onData, MsgSink onMsg) {
  dataSink = onData;
  msgSink = onMsg;
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    Port &pt = ports[p];
    if (!settings.port[p].enabled) continue;
    pt.hw = p < HW_PORTS;
    if (pt.hw) {
      pt.hs = HW_UART[p];
      pt.uartNum = HW_UART_NUM[p];
    } else {
      pt.ss = new EspSoftwareSerial::UART();
    }
    pt.txq = (uint8_t *)malloc(TX_QUEUE);
    pt.ringSize = p == 0 ? RING_SIZE : RING_SIZE_EXTRA;
    pt.ring = (uint8_t *)malloc(pt.ringSize);
    if (!pt.ring || !pt.txq) {
      Serial.printf("[PORT] %u: kein Speicher\n", p + 1);
      continue;
    }
    pt.on = true;
    openPort(p, settings.port[p].serial);
  }
}

void apply(uint8_t p, const SerialCfg &cfg) {
  if (!enabled(p) || Xfer::on(p)) return;
  if (cfg.baud > maxBaud(p)) {
    msg(prefix(p) + TR("Software UART: max. ", "Software-UART: max. ", "UART por software: máx. ") + String(SW_MAX_BAUD) + " Baud");
    return;
  }
  if (cfg.swap && !swapOk(p)) {
    msg(prefix(p) + TR("Cannot swap RX/TX: GPIO", "RX/TX tauschen geht nicht: GPIO", "Não é possível trocar RX/TX: GPIO") + String(settings.port[p].rx) + TR(" cannot send", " kann nicht senden", " não pode transmitir"));
    return;
  }
  flushPending(p);
  settings.port[p].serial = cfg;
  Store::saveSerial(p);
  openPort(p, cfg);
  msg(prefix(p) + TR("Serial: ", "Seriell: ", "Serial: ") + Store::serialLabel(cfg));
}

size_t write(uint8_t p, const uint8_t *data, size_t len) {
  if (!enabled(p) || autobaudRunning(p) || !len) return 0;
  if (Xfer::blocksInput(p)) return 0;
  if (memchr(data, 'C', len) || memchr(data, 0x15, len)) lastUserStartMs[p] = millis();
  return writeRaw(p, data, len);
}

size_t writeRaw(uint8_t p, const uint8_t *data, size_t len) {
  if (!enabled(p) || !len) return 0;
  size_t n = portWrite(p, data, len);
  txBytes[p] += n;
  lastTxMs[p] = millis();
  if (n && !ports[p].unansweredSince) ports[p].unansweredSince = millis();
  return n;
}

void sendBreak(uint8_t p, uint16_t ms) {
  if (!enabled(p) || autobaudRunning(p) || Xfer::blocksInput(p)) return;
  Port &pt = ports[p];
  pt.txTail = pt.txHead;             // queued input is dropped (BREAK is for ROMmon & co)
  if (pt.hw) {
    while (busyAhead(pt)) delay(1);    // at most ~20 ms
    pt.hs->flush();
    // TTL low = RS232 space = BREAK: invert the idle TX line (UART driver keeps running)
    uart_set_line_inverse((uart_port_t)pt.uartNum, UART_SIGNAL_TXD_INV);
    delay(ms);
    uart_set_line_inverse((uart_port_t)pt.uartNum, UART_SIGNAL_INV_DISABLE);
  } else {
    digitalWrite(pt.tx, LOW);
    delay(ms);
    digitalWrite(pt.tx, HIGH);
  }
  msg(prefix(p) + "BREAK: " + String(ms) + TR(" ms sent", " ms gesendet", " ms enviado"));
}

// Debug console "pins": what the wiring of a hardware port does, without the
// UART. The port is closed for about a tenth of a second and opened again.
//  - each pin with pull-down and with pull-up: a driven pin reads the same both
//    times (an RS232 receiver idles high), an open one follows the pull
//  - each pin driven low and high while the other one is read: through a
//    level shifter and a loopback on the RS232 side the input follows the
//    output (two inversions) - the pair that does is the right TX -> RX
static volatile int8_t probeReq = -1;
void requestPinProbe(uint8_t port) { probeReq = port; }

static void pinProbe(uint8_t p) {
  Port &pt = ports[p];
  if (!pt.on || !pt.hw) { Serial.printf("[PORT] Pins: Port %u ist kein offener Hardware-Port\n", p + 1); return; }
  const int a = settings.port[p].rx, b = settings.port[p].tx;
  pt.hs->end();
  auto level = [](int pin, uint8_t mode) { pinMode(pin, mode); delay(5); return digitalRead(pin); };
  auto idle = [&](int pin) {
    int lo = level(pin, INPUT_PULLDOWN), hi = level(pin, INPUT_PULLUP);
    pinMode(pin, INPUT);
    return lo && hi ? "getrieben high" : !lo && !hi ? "getrieben low" : "offen (folgt dem Pull)";
  };
  Serial.printf("[PORT] Pins Port %u (eingestellt RX GPIO%d, TX GPIO%d): GPIO%d %s, GPIO%d %s\n", p + 1, a, b, a, idle(a), b, idle(b));
  const int pairs[2][2] = {{b, a}, {a, b}};
  for (auto &pr : pairs) {
    int out = pr[0], in = pr[1];
    pinMode(in, INPUT_PULLDOWN);
    pinMode(out, OUTPUT);
    digitalWrite(out, LOW);  delay(5); int l = digitalRead(in);
    digitalWrite(out, HIGH); delay(5); int h = digitalRead(in);
    digitalWrite(out, LOW);  delay(5); int l2 = digitalRead(in);
    pinMode(out, INPUT);
    pinMode(in, INPUT);
    Serial.printf("[PORT] Pins: GPIO%d sendet -> GPIO%d liest %d/%d/%d bei low/high/low: %s\n", out, in, l, h, l2,
                  (!l && h && !l2) ? "folgt (Schleife geschlossen)" : (l && !h && l2) ? "folgt invertiert" : "folgt nicht");
  }
  openPort(p, pt.cfg);
}

// ---------------------------------------------------------------- self-check
// On 18 Oct 2026 port 1 went deaf twice for hours (issue #12): bytes were
// handed to the UART, the switch answered nothing, and opening the port again
// with the same settings brought the answer at once. Every half second the
// hardware port is looked at where the UART driver can get stuck - data in
// the receive FIFO that the driver does not take, receive interrupts switched
// off, a transmitter that does not finish, pins no longer routed to the UART,
// a wrong clock divider, input that waits in the queue and is not handed on
// (the cause of issue #12 was such a stall, see busyAhead()). As the last
// resort: the line was sent to and nothing
// at all has come back for 5 s (a console echoes within milliseconds). Then
// the port is opened again - it costs nothing - and why is logged and counted
// (rxReopen/rxWhy in /api/status).
static const uint32_t CHECK_MS = 500, STUCK_MS = 1500, UNANSWERED_MS = 5000;

static void reopen(uint8_t p, const char *why) {
  Port &pt = ports[p];
  flushPending(p);
  reopened[p]++;
  strlcpy(reopenWhy[p], why, sizeof(reopenWhy[p]));
  Serial.printf("[PORT] Port %u: %s - neu geoeffnet (%lu. Mal)\n", p + 1, why, (unsigned long)reopened[p]);
  openPort(p, pt.cfg);
}

static void check(uint8_t p) {
  Port &pt = ports[p];
  uint32_t now = millis();
  if (now - pt.checkMs < CHECK_MS) return;
  pt.checkMs = now;
  if (!pt.hw || pt.uartNum < 0 || Xfer::on(p) || autobaudRunning(p)) {
    pt.unansweredSince = pt.queueSince = 0;
    return;
  }
  uart_dev_t *hw = UART_LL_GET_HW(pt.uartNum);
  const char *why = nullptr;
  bool fifo = uart_ll_get_rxfifo_len(hw) > 0 && portAvailable(pt) <= 0;
  bool rxOff = !(uart_ll_get_intr_ena_status(hw) & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT));
  if (!fifo && !rxOff) pt.fifoSince = 0;
  else if (!pt.fifoSince) pt.fifoSince = now;
  else if (now - pt.fifoSince >= STUCK_MS) why = rxOff ? "Empfangs-Interrupt aus" : "Daten im FIFO, Treiber nimmt sie nicht";
  bool txBusy = !uart_ll_is_tx_idle(hw) && !busyAhead(pt);
  if (!txBusy) pt.txSince = 0;
  else if (!pt.txSince) pt.txSince = now;
  else if (!why && now - pt.txSince >= STUCK_MS) why = "Sender wird nicht fertig";
  const uint32_t rxSig = pt.uartNum == 1 ? U1RXD_IN_IDX : U2RXD_IN_IDX, txSig = pt.uartNum == 1 ? U1TXD_OUT_IDX : U2TXD_OUT_IDX;
  // (the UART's own IO_MUX pins bypass the matrix: nothing to compare there)
  const auto &pins = uart_periph_signal[pt.uartNum].pins;
  if (!why && pt.rx >= 0 && pt.rx != pins[SOC_UART_RX_PIN_IDX].default_gpio &&
      (!GPIO.func_in_sel_cfg[rxSig].sig_in_sel || (int)GPIO.func_in_sel_cfg[rxSig].func_sel != pt.rx)) why = "RX-Pin nicht mehr am UART";
  if (!why && pt.tx >= 0 && pt.tx != pins[SOC_UART_TX_PIN_IDX].default_gpio && GPIO.func_out_sel_cfg[pt.tx].func_sel != txSig)
    why = "TX-Pin nicht mehr am UART";
  if (!why && pt.cfg.baud) {
    uint32_t b = uart_ll_get_baudrate(hw);
    if (b < pt.cfg.baud * 95 / 100 || b > pt.cfg.baud * 105 / 100) why = "Baudrate im UART verstellt";
  }
  // input waits in the queue and the loop has not handed any of it to the UART
  if (!txqLen(pt) || pt.txTail != pt.queueTail) pt.queueSince = 0;
  else if (!pt.queueSince) pt.queueSince = now;
  else if (!why && now - pt.queueSince >= STUCK_MS) why = "Sendewarteschlange steht";
  pt.queueTail = pt.txTail;
  if (!why && pt.unansweredSince && !txqLen(pt) && now - pt.unansweredSince >= UNANSWERED_MS) why = "gesendet, 5 s keine Antwort";
  if (why) reopen(p, why);
}

void loop() {
  if (probeReq >= 0) {
    int8_t p = probeReq;
    probeReq = -1;
    if (p < MAX_PORTS) pinProbe(p);
  }
  for (uint8_t p = 0; p < MAX_PORTS; p++) {
    Port &pt = ports[p];
    if (!pt.on) continue;
    drain(pt);
    check(p);
    if (autobaudRunning(p)) {
      abStep();
      continue;
    }
    int avail = portAvailable(pt);
    if (avail > 0) {
      if (Xfer::on(p)) {
        // file transfer: hand every byte to the engine right away (timing), it
        // passes back what belongs to the terminal (text before the handshake)
        flushPending(p);
        uint8_t buf[128];
        size_t n = portRead(pt, buf, min((size_t)avail, sizeof(buf)));
        size_t used = n ? Xfer::onRx(p, buf, n) : 0;
        rxBytes[p] += used;
        if (used) lastRxMs[p] = millis();
        if (n > used) {
          memcpy(pt.pend, buf + used, n - used);
          pt.pendLen = n - used;
          flushPending(p);
        }
        continue;
      }
      size_t room = sizeof(pt.pend) - pt.pendLen;
      size_t n = portRead(pt, pt.pend + pt.pendLen, min((size_t)avail, room));
      if (n) {
        if (!pt.pendLen) pt.pendSince = millis();
        pt.pendLen += n;
      }
    }
    if (pt.pendLen && (pt.pendLen >= sizeof(pt.pend) - 32 || millis() - pt.pendSince >= 12)) flushPending(p);
  }
}

}  // namespace Bridge
