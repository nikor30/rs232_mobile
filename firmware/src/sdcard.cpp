#include "sdcard.h"

#if HAS_SDCARD

#include <FS.h>
#include <SD.h>
#include <SPI.h>

namespace Sd {

static const uint32_t FLUSH_MS = 2000;     // SD writes are slow: collect, then flush
static const size_t   BUF_SIZE = 1024;

static bool ok = false;
#if SD_SHARES_LCD_BUS
static SPIClass &spi = SPI;                // LovyanGFX has opened this bus for the LCD
#else
static SPIClass spi(HSPI);
#endif

struct Log {
  File f;
  String name;
  uint32_t bytes = 0;
  uint8_t buf[BUF_SIZE];
  size_t len = 0;
  uint32_t lastFlush = 0;
};
static Log logs[MAX_PORTS];

bool mounted() { return ok; }
uint64_t totalMb() { return ok ? SD.totalBytes() / (1024ULL * 1024ULL) : 0; }
uint64_t usedMb() { return ok ? SD.usedBytes() / (1024ULL * 1024ULL) : 0; }

const char *typeName() {
  if (!ok) return "keine Karte";
  switch (SD.cardType()) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    default:        return "unbekannt";
  }
}

bool begin() {
#if !SD_SHARES_LCD_BUS
  spi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
#endif
  // 20 MHz is conservative: the card shares its pins with nothing else here, but
  // long ribbon wiring on these panel boards does not like the full 40 MHz.
  // A card that kept its power through a reset sometimes misses the first
  // attempt, so try again, slower.
  for (uint32_t hz : {20000000u, 10000000u, 4000000u}) {
    ok = SD.begin(PIN_SD_CS, spi, hz);
    if (ok) break;
    SD.end();
    delay(100);
  }
  if (!ok) {
    Serial.println("[SD]   keine Karte gefunden");
    return false;
  }
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists("/configs")) SD.mkdir("/configs");
  if (!SD.exists("/xfer")) SD.mkdir("/xfer");
  Serial.printf("[SD]   %s, %llu von %llu MB belegt\n", typeName(), usedMb(), totalMb());
  return true;
}

// There is no clock in this device, so files are numbered instead of dated.
static String nextName(uint8_t port) {
  for (uint16_t i = 1; i < 1000; i++) {
    char p[32];
    snprintf(p, sizeof(p), "/logs/port%u-%03u.log", (unsigned)(port + 1), (unsigned)i);
    if (!SD.exists(p)) return String(p);
  }
  return String();
}

bool logStart(uint8_t port) {
  if (!ok || port >= MAX_PORTS || logs[port].f) return false;
  String n = nextName(port);
  if (!n.length()) return false;
  logs[port].f = SD.open(n, FILE_WRITE);
  if (!logs[port].f) return false;
  logs[port].name = n;
  logs[port].bytes = 0;
  logs[port].len = 0;
  logs[port].lastFlush = millis();
  Serial.printf("[SD]   Mitschnitt Port %u -> %s\n", port + 1, n.c_str());
  return true;
}

static void flush(uint8_t port) {
  Log &l = logs[port];
  if (!l.f || !l.len) return;
  l.f.write(l.buf, l.len);
  l.f.flush();
  l.len = 0;
  l.lastFlush = millis();
}

void logStop(uint8_t port) {
  if (port >= MAX_PORTS || !logs[port].f) return;
  flush(port);
  logs[port].f.close();
  logs[port].name = "";
}

bool logging(uint8_t port) { return port < MAX_PORTS && logs[port].f; }
String logName(uint8_t port) { return port < MAX_PORTS ? logs[port].name : String(); }
uint32_t logBytes(uint8_t port) { return port < MAX_PORTS ? logs[port].bytes : 0; }

void write(uint8_t port, const uint8_t *data, size_t len) {
  if (port >= MAX_PORTS || !logs[port].f) return;
  Log &l = logs[port];
  for (size_t i = 0; i < len; i++) {
    l.buf[l.len++] = data[i];
    if (l.len == BUF_SIZE) flush(port);
  }
  l.bytes += len;
}

void loop() {
  if (!ok) return;
  uint32_t now = millis();
  for (uint8_t p = 0; p < MAX_PORTS; p++)
    if (logs[p].f && logs[p].len && now - logs[p].lastFlush > FLUSH_MS) flush(p);
}

}  // namespace Sd

#else   // ---------------------------------------------------------- no SD slot

namespace Sd {
bool begin() { return false; }
bool mounted() { return false; }
uint64_t totalMb() { return 0; }
uint64_t usedMb() { return 0; }
const char *typeName() { return "kein Steckplatz"; }
bool logStart(uint8_t) { return false; }
void logStop(uint8_t) {}
bool logging(uint8_t) { return false; }
String logName(uint8_t) { return String(); }
uint32_t logBytes(uint8_t) { return 0; }
void write(uint8_t, const uint8_t *, size_t) {}
void loop() {}
}  // namespace Sd

#endif
