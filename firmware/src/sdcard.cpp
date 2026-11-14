#include "sdcard.h"
#include "config.h"
#include "lang.h"

#include <errno.h>
#include "mbedtls/md5.h"
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <USB.h>
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/rtc_cntl_struct.h"
#include "ff.h"
#include "diskio.h"
#include "sd_diskio.h"
#include "display.h"
#include "power.h"

namespace Sd {

static const uint32_t FLUSH_MS = 2000;     // SD writes are slow: collect, then flush
static const size_t   BUF_SIZE = 1024;

static bool ok = false;
static bool rawCard = false;             // a card answers, but holds no file system the board can mount
static bool lent = false;                  // the card belongs to the USB host (stick mode)
static SPIClass &spi = SPI;                // LovyanGFX has opened this bus for the LCD

// The bus is shared with the LCD: the display task and the card must take turns.
struct BusGuard {
  BusGuard() { Display::busLock(); }
  ~BusGuard() { Display::busUnlock(); }
};

// Diagnostic trail, kept in flash so it survives the resets it is about: how
// each start went ("K" after power-on, "R" after a reset, "+" mounted, "-xx" not
// mounted with the card's answer) and "!<s>" when a mounted card stopped
// answering after that many seconds.
static String hist;
static bool histLoaded = false;
static void note(const String &event) {
  Preferences p;
  p.begin("sddiag", false);
  if (!histLoaded) { hist = p.getString("hist", ""); histLoaded = true; }
  hist += (hist.length() ? " " : "") + event;
  while (hist.length() > 160) {
    int sp = hist.indexOf(' ');
    hist = sp < 0 ? String() : hist.substring(sp + 1);
  }
  p.putString("hist", hist);
  p.end();
}
String history() {
  if (!histLoaded) {
    Preferences p;
    p.begin("sddiag", true);
    hist = p.getString("hist", "");
    p.end();
    histLoaded = true;
  }
  return "Verlauf: " + hist;
}

struct Log {
  File f;
  String name;
  uint32_t bytes = 0;
  uint8_t buf[BUF_SIZE];
  size_t len = 0;
  uint32_t lastFlush = 0;
};
static Log logs[MAX_PORTS];

// usedBytes() walks the whole allocation table - seconds on a large card. The
// front ends ask on every redraw, so the figures are taken once and refreshed
// when a recording ends.
static uint64_t cachedTotalMb = 0, cachedUsedMb = 0;
static void refreshSizes() {
  BusGuard guard;
  cachedTotalMb = SD.totalBytes() / (1024ULL * 1024ULL);
  cachedUsedMb = SD.usedBytes() / (1024ULL * 1024ULL);
}

bool mounted() { return ok; }
uint64_t totalMb() { return ok ? cachedTotalMb : 0; }
uint64_t usedMb() { return ok ? cachedUsedMb : 0; }

const char *typeName() {
  if (lent) return TR("USB stick", "USB-Stick", "pendrive USB");
  if (!ok) return TR("no card", "keine Karte", "sem cartão");
  switch (SD.cardType()) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    default:        return "unbekannt";
  }
}

bool begin() {
  if (ok) return true;
  if (lent) return false;
  static uint32_t lastFailure = 0;       // setup() asks twice on boards that start the card early
  if (lastFailure && millis() - lastFailure < 5000) return false;
  BusGuard guard;
  static bool firstAttempt = true;
  const char *boot = esp_reset_reason() == ESP_RST_POWERON ? "K" : "R";
  // Called before the LCD is started (see lcd_ui.cpp): a card fresh from power-up
  // still listens in SD mode, where it ignores chip select and would take the
  // display traffic for commands. It has to be switched to SPI mode first.
  spi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, -1);
  // 20 MHz is conservative.
  // A card that kept its power through a reset sometimes misses the first
  // attempt, so try again, slower.
  for (uint32_t hz : {20000000u, 10000000u, 4000000u}) {
    ok = SD.begin(PIN_SD_CS, spi, hz);
    if (ok) break;
    SD.end();
    delay(100);
  }
  if (!ok) {
    // Say what the slot answers to a bare CMD0, so "no card" can be told from
    // "card does not start": 01 = card present and idle, FF = nothing answers.
    pinMode(PIN_SD_MISO, INPUT_PULLUP);
    spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_SD_CS, HIGH);
    for (int i = 0; i < 10; i++) spi.transfer(0xFF);
    digitalWrite(PIN_SD_CS, LOW);
    const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
    for (uint8_t b : cmd0) spi.transfer(b);
    uint8_t r[8];
    for (uint8_t &b : r) b = spi.transfer(0xFF);
    digitalWrite(PIN_SD_CS, HIGH);
    spi.transfer(0xFF);
    spi.endTransaction();
    rawCard = r[0] == 0x01;              // idle: a card is there, only its file system is not
    Serial.printf("[SD]   keine Karte gefunden (Antwort auf CMD0: %02X %02X %02X %02X %02X %02X %02X %02X)\n",
                  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    if (firstAttempt) {
      char ev[8];
      snprintf(ev, sizeof(ev), "%s-%02X", boot, r[0]);
      note(ev);
    }
    firstAttempt = false;
    lastFailure = millis() | 1;
    return false;
  }
  rawCard = false;
  if (firstAttempt) note(String(boot) + "+");
  firstAttempt = false;
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists("/configs")) SD.mkdir("/configs");
  if (!SD.exists("/xfer")) SD.mkdir("/xfer");
  refreshSizes();
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
  BusGuard guard;
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
  BusGuard guard;
  l.f.write(l.buf, l.len);
  l.f.flush();
  l.len = 0;
  l.lastFlush = millis();
}

void logStop(uint8_t port) {
  if (port >= MAX_PORTS || !logs[port].f) return;
  BusGuard guard;
  flush(port);
  logs[port].f.close();
  logs[port].name = "";
  refreshSizes();
}

// ---- a file from the control center
// It is written as "<name>.<tag>.part" (the tag is the beginning of its MD5
// sum, so a part never continues another file) and gets its name only when
// what is on the card has been read back and has the right sum. A part that
// is left behind by a broken connection or a restart is continued.
static File putFile;
static String putName, putPart;
static uint32_t putBytes = 0;

static bool nameOk(const char *name) {
  size_t n = strlen(name);
  return n && n <= 96 && name[0] != '.' && !strpbrk(name, "/\\:*?\"<>|");
}

const char *putBegin(const char *name, uint32_t size, const char *tag, uint32_t &have) {
  have = 0;
  if (!ok) return "no SD card in the device";
  if (putName.length()) return "a file is being written already";
  if (!nameOk(name) || strlen(tag) != 8 || strpbrk(tag, "/\\.")) return "the name cannot be used on the card";
  String part = String("/") + name + "." + tag + ".part";
  {
    BusGuard guard;
    File f = SD.open(part);
    if (f && !f.isDirectory() && f.size() <= size) have = f.size();
    if (f) f.close();
    if (!have) SD.remove(part);
  }
  refreshSizes();
  if ((uint64_t)(size - have) / (1024 * 1024) + 1 >= cachedTotalMb - cachedUsedMb) return "not enough room on the SD card";
  BusGuard guard;
  putFile = SD.open(part, have ? "r+" : "w");
  if (putFile && have && !putFile.seek(have)) putFile.close();
  if (!putFile) return "the file could not be created on the card";
  putName = String("/") + name;
  putPart = part;
  putBytes = have;
  return nullptr;
}

// The card now and then refuses a write (seen on the board at hand). What
// had been handed over before may then not be on the card either - the file
// system collects before it writes. So nothing is repaired in place: the
// caller closes the file (putPause), opens it again (putBegin) and goes on
// from what the card says it has, which is what the last flush made sure of.
bool putWrite(const uint8_t *data, size_t len) {
  if (!putName.length() || !putFile) return false;
  BusGuard guard;
  if (putFile.write(data, len) != len) {
    Serial.printf("[SD]   Schreiben bei %lu gescheitert (errno %d)\n", (unsigned long)putBytes, errno);
    return false;
  }
  putBytes += len;
  // handed to the card every quarter megabyte: that much at most is fetched
  // again after a failed write or a power cut
  if (putBytes / (256 * 1024) != (putBytes - len) / (256 * 1024)) {
    putFile.flush();
    if (putFile.getWriteError()) {
      Serial.printf("[SD]   Sichern bei %lu gescheitert\n", (unsigned long)putBytes);
      return false;
    }
  }
  return true;
}

// Stops for now: what has arrived stays on the card and is continued later.
void putPause() {
  if (!putName.length()) return;
  BusGuard guard;
  if (putFile) putFile.close();
  putName = putPart = "";
}

// The end: reads back what the card holds and compares its MD5 sum with the
// one the server named. Only then does the file get its name.
const char *putFinish(uint32_t size, const uint8_t md5[16], void (*progress)(int pct)) {
  if (!putName.length()) return "no file is being written";
  String name = putName, part = putPart;
  uint8_t *buf = (uint8_t *)malloc(4096);
  const char *err = buf ? nullptr : "not enough memory";
  {
    BusGuard guard;
    if (putFile) putFile.close();
    putFile = SD.open(part);
    if (!putFile || putFile.size() != size) err = err ? err : "the file on the card is incomplete";
  }
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  for (uint32_t off = 0; !err && off < size;) {
    size_t n;
    {
      BusGuard guard;                    // piece by piece: the display shares the bus
      n = putFile.read(buf, 4096);
    }
    if (!n) { err = "the file on the card cannot be read back"; break; }
    mbedtls_md5_update_ret(&ctx, buf, n);
    off += n;
    if (progress && (off / 4096) % 64 == 0) progress((int)((uint64_t)off * 100 / size));
    // Let go of the bus and of the processor in between: reading megabytes in
    // one go starves the display and trips the watchdog (it did - the device
    // then restarted, found the job again and restarted again).
    if ((off / 4096) % 4 == 0) vTaskDelay(pdMS_TO_TICKS(2));
  }
  uint8_t sum[16];
  mbedtls_md5_finish_ret(&ctx, sum);
  mbedtls_md5_free(&ctx);
  free(buf);
  if (!err && memcmp(sum, md5, 16)) err = "the file on the SD card does not have the right MD5 sum";
  BusGuard guard;
  if (putFile) putFile.close();
  if (!err) {
    SD.remove(name);                     // a newer one of the same name takes its place
    if (!SD.rename(part, name)) err = "the file could not be stored on the SD card";
  }
  if (err) SD.remove(part);              // damaged: the next attempt starts from nothing
  putName = putPart = "";
  cachedUsedMb = SD.usedBytes() / (1024ULL * 1024ULL);
  return err;
}

const char *removeRoot(const char *name) {
  if (!ok) return "no SD card in the device";
  size_t n = strlen(name);
  if (!n || n > 96 || name[0] == '.' || strpbrk(name, "/\\")) return "the name cannot be used on the card";
  if (putName.length()) return "a file is being written already";
  BusGuard guard;
  String path = String("/") + name;
  File f = SD.open(path);
  bool file = f && !f.isDirectory();
  if (f) f.close();
  if (!file) return "file not found on the SD card";
  if (!SD.remove(path)) return "the file could not be deleted";
  cachedUsedMb = SD.usedBytes() / (1024ULL * 1024ULL);
  return nullptr;
}

const char *writeRoot(const char *name, const uint8_t *data, size_t len) {
  if (!ok) return "no SD card in the device";
  if (putName.length()) return "a file is being written already";
  BusGuard guard;
  String path = String("/") + name, tmp = path + ".tmp";
  if (SD.exists(tmp)) SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return "the SD card does not take the file";
  size_t done = 0;
  while (done < len) {
    size_t n = f.write(data + done, min((size_t)4096, len - done));
    if (!n) break;
    done += n;
    vTaskDelay(1);                       // the display shares the bus
  }
  f.close();
  if (done != len) { SD.remove(tmp); return "writing to the SD card failed"; }
  // read back: the card refuses writes now and then without saying so (WAVESHARE.md)
  f = SD.open(tmp);
  bool same = f && f.size() == len;
  uint8_t buf[512];
  for (size_t off = 0; same && off < len;) {
    size_t n = f.read(buf, min(sizeof(buf), len - off));
    same = n > 0 && !memcmp(buf, data + off, n);
    off += n;
  }
  if (f) f.close();
  if (!same) { SD.remove(tmp); return "the file on the SD card does not read back the same"; }
  if (SD.exists(path)) SD.remove(path);
  if (!SD.rename(tmp, path)) return "the file could not be named on the SD card";
  cachedUsedMb = SD.usedBytes() / (1024ULL * 1024ULL);
  return nullptr;
}

const char *readRoot(const char *name, uint8_t *&data, size_t &len, size_t max) {
  data = nullptr;
  len = 0;
  if (!ok) return "no SD card in the device";
  if (putName.length()) return "a file is being written already";
  BusGuard guard;
  File f = SD.open(String("/") + name);
  if (!f || f.isDirectory()) { if (f) f.close(); return "no backup on the SD card"; }
  size_t size = f.size();
  if (!size || size > max) { f.close(); return "the backup on the SD card has an impossible size"; }
  data = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) { f.close(); return "not enough memory"; }
  size_t got = 0;
  while (got < size) {
    int n = f.read(data + got, min((size_t)4096, size - got));
    if (n <= 0) break;
    got += n;
  }
  f.close();
  if (got != size) { free(data); data = nullptr; return "reading the SD card failed"; }
  len = size;
  return nullptr;
}

String rootJson(size_t room) {
  String out = "[";
  if (ok && !putName.length()) {
    BusGuard guard;
    File dir = SD.open("/");
    for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
      String n = f.name();
      if (!f.isDirectory() && n.length() && n[0] != '.' && !n.endsWith(".part") && n.indexOf('"') < 0 && n.indexOf('\\') < 0) {
        String one = String(out.length() > 1 ? "," : "") + "{\"n\":\"" + n + "\",\"s\":" + String((uint32_t)f.size()) + "}";
        if (out.length() + one.length() + 1 > room) { f.close(); break; }
        out += one;
      }
      f.close();
    }
    if (dir) dir.close();
  }
  return out + "]";
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

void end() {
  if (lent) { lent = false; ok = true; }   // power-off: give the card a clean end
  if (!ok) return;
  for (uint8_t p = 0; p < MAX_PORTS; p++) logStop(p);
  BusGuard guard;
  SD.end();
  ok = false;
}

void loop() {
  if (!ok) return;
  uint32_t now = millis();
  // Is the card still there? Reads one raw sector every 20 s. On the shared bus
  // a card has been seen to stop answering; this pins down when.
  static uint32_t lastCheck = 0;
  static uint8_t sector[512];
  if (now - lastCheck > 20000) {
    lastCheck = now;
    BusGuard guard;
    if (!SD.readRAW(sector, 0)) {
      Serial.printf("[SD]   Karte antwortet nicht mehr (nach %lu s Betrieb)\n", (unsigned long)(now / 1000));
      note("!" + String(now / 1000) + "s");
      for (uint8_t p = 0; p < MAX_PORTS; p++)
        if (logs[p].f) { logs[p].f.close(); logs[p].name = ""; }
      SD.end();
      ok = false;
      return;
    }
  }
  for (uint8_t p = 0; p < MAX_PORTS; p++)
    if (logs[p].f && logs[p].len && now - logs[p].lastFlush > FLUSH_MS) flush(p);
}

// ---- USB memory stick ----
// The card stays initialised (SD keeps its drive), but the file system on it is
// not touched from here any more: two masters over one FAT would wreck it. The
// host's sector requests arrive in the USB task and go straight to the card.
//
// The SCSI side is ours, not the core's USBMSC: its handler for commands that
// TinyUSB does not know itself keeps the length in a uint16_t, so its "-1 =
// unsupported" came out as success. Linux uses only commands TinyUSB knows; a
// Cisco C2960CX also sends others, got "done" for writes that never happened,
// and could not format the card ("Didn't find 55AA signature in System Sector").
static uint8_t drive = 0xFF;
static volatile bool stickMedia = false, stickEjected = false;
static bool ownUsb = false;                                  // the USB port is taken from the serial/JTAG unit: dead, or our stick
static void stickDevice();
static void usbRestore();
static volatile uint32_t stickSeen[8];                       // opcodes that reached stickScsi(), one bit each
static volatile uint32_t stickRead = 0, stickWritten = 0;    // sectors
static volatile uint32_t stickRetries = 0, stickFails = 0;   // requests the card refused at first / for good

// SD keeps the number of its drive to itself.
struct DriveOf : fs::SDFS {
  static uint8_t get(fs::SDFS &s) { return s.*(&DriveOf::_pdrv); }
};

// The card refuses a transfer now and then (errno 5 on the shared bus, see
// putWrite). A refused write handed to the host as an error is lost for good:
// Linux drops the page ("lost async page write"), and a 25 MB file never came
// across intact. So every request gets a few more tries, with the bus let go
// in between; the same sectors written twice do no harm.
static const int STICK_TRIES = 5;

// A medium in RAM instead of the card (POST /api/stick {"ram":true}): what a
// host does with the stick can be watched without a card, and the card's own
// refusals are out of the picture. Empty at the start, gone with the restart.
static const uint32_t RAM_SECTORS = 8192;  // 4 MB
static uint8_t *ramDisk = nullptr;

// Fills the RAM medium before stick mode starts, so the host finds a file
// system on it that was made on a PC (POST /api/stick/ram, then /api/stick).
static bool ramAlloc() {
  if (!ramDisk) ramDisk = (uint8_t *)heap_caps_calloc(RAM_SECTORS, 512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return ramDisk != nullptr;
}

bool stickRamLoad(uint32_t off, const uint8_t *data, size_t n) {
  if (lent || off + n > RAM_SECTORS * 512UL || !ramAlloc()) return false;
  memcpy(ramDisk + off, data, n);
  return true;
}

static bool stickIoRaw(bool write, uint8_t *buffer, uint32_t lba, uint32_t count) {
  if (ramDisk) {
    // Only the first RAM_SECTORS are backed; a larger reported size reads as zeros beyond them.
    uint32_t in = lba >= RAM_SECTORS ? 0 : min(count, RAM_SECTORS - lba);
    if (write) {
      if (in < count) return false;
      memcpy(ramDisk + lba * 512, buffer, count * 512);
    } else {
      memcpy(buffer, ramDisk + lba * 512, in * 512);
      memset(buffer + in * 512, 0, (count - in) * 512);
    }
    return true;
  }
  for (int i = 0; i < STICK_TRIES; i++) {
    if (i) {
      stickRetries = stickRetries + 1;
      vTaskDelay(pdMS_TO_TICKS(5 * i));
    }
    BusGuard guard;
    DRESULT r = write ? disk_write(drive, buffer, lba, count) : disk_read(drive, buffer, lba, count);
    if (r == RES_OK) return true;
  }
  stickFails = stickFails + 1;
  return false;
}

// A Cisco switch (C2960CX, IOS 15.2(7)E5) talks to a full-speed device through
// its OHCI driver, and that driver gets the data of a request wrong when the
// device answers at once: measured with the same stick replugged again and
// again - mounted 3 times in 12 with no delay, 6 in 10 at 20 ms, 10 in 10 at
// 40 ms and 60 ms; files read back with a different MD5 each time, its own
// format wrote its heap into the partition table. A flash stick is slow enough
// not to meet this, the board's RAM or an SD card at SPI speed is not. So the
// first chunk of a request is answered no sooner than STICK_FIRST_MS after it
// arrived. TinyUSB hands a request over in chunks of 4 kB; a chunk that follows
// at once (same place, within STICK_CONT_MS) counts as part of the request and
// waits stickNextMs only. Both can be set at run time for measuring
// (/api/stick/delay).
static const uint32_t STICK_FIRST_MS = 60, STICK_CONT_MS = 25;
static volatile uint32_t stickFirstMs = STICK_FIRST_MS, stickNextMs = 0;
static uint32_t stickLastEnd = 0xFFFFFFFF, stickLastAt = 0;

static bool stickIo(bool write, uint8_t *buffer, uint32_t lba, uint32_t count) {
  uint32_t t0 = millis();
  bool cont = lba == stickLastEnd && t0 - stickLastAt < STICK_CONT_MS;
  bool ok = stickIoRaw(write, buffer, lba, count);
  uint32_t want = cont ? stickNextMs : stickFirstMs, used = millis() - t0;
  if (used < want) vTaskDelay(pdMS_TO_TICKS(want - used));
  stickLastEnd = lba + count;
  stickLastAt = millis();
  return ok;
}

// The last requests of the host, for /api/status: which sectors a host reads
// and writes, and what the last two bytes of the first one were (55aa marks a
// partition table or boot sector). Written in the USB task only.
struct StickReq {
  char op;                                 // R/W = READ/WRITE (10), r/w = (6), C capacity, S start/stop, X other
  bool ok;
  uint16_t sig;
  uint32_t lba, off, size;
};
static const int STICK_LOG = 48;
static StickReq stickLog[STICK_LOG];
static volatile uint32_t stickLogN = 0;

static void stickNote(char op, uint32_t lba, uint32_t off, uint32_t size, const uint8_t *buf, bool ok) {
  StickReq &r = stickLog[stickLogN % STICK_LOG];
  r.op = op;
  r.ok = ok;
  r.sig = buf && size >= 512 ? buf[510] << 8 | buf[511] : 0;
  r.lba = lba;
  r.off = off;
  r.size = size;
  stickLogN = stickLogN + 1;
}

static int32_t stickOnRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size, char op = 'R') {
  bool ok = !offset && !(size % 512) && stickIo(false, (uint8_t *)buffer, lba, size / 512);
  stickNote(op, lba, offset, size, (uint8_t *)buffer, ok);
  if (!ok) return -1;
  stickRead = stickRead + size / 512;
  return size;
}

// Copies of the last single-sector writes, as the host's data arrived here:
// a C2960CX writes partition table and boot sector one sector at a time, and
// their 55aa did not come across (GET /api/stick/sectors).
static uint8_t stickCopy[4][512];
static uint32_t stickCopyLba[4];
static volatile uint32_t stickCopyN = 0;

static int32_t stickOnWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size, char op = 'W') {
  if (size == 512) {
    uint32_t k = stickCopyN % 4;
    memcpy(stickCopy[k], buffer, 512);
    stickCopyLba[k] = lba;
    stickCopyN = stickCopyN + 1;
  }
  bool ok = !offset && !(size % 512) && stickIo(true, buffer, lba, size / 512);
  stickNote(op, lba, offset, size, buffer, ok);
  if (!ok) return -1;
  stickWritten = stickWritten + size / 512;
  return size;
}

// Older Cisco switches keep a stick's size in bytes in 32 bits: a 32 GB card
// looked like 1856 MB to a C2960CX, which then found its partition table
// "unexpected" and could not even format the card. A card larger than 4 GB
// whose partitions all end within the first 4 GB, or that has no partition
// table at all, is therefore shown as a 4 GB stick. Otherwise the whole card is
// shown: cutting off a partition would wreck it for every host.
static const uint32_t STICK_MAX = 8386560;   // 4 GiB - 1 MiB, in sectors
static uint32_t stickShown = 0;

static uint16_t stickDescriptor(uint8_t *dst, uint8_t *itf) {
  uint8_t str = tinyusb_add_string_descriptor("RS232 Console SD card");
  uint8_t ep = tinyusb_get_free_duplex_endpoint();
  TU_VERIFY(ep != 0);
  uint8_t d[TUD_MSC_DESC_LEN] = {TUD_MSC_DESCRIPTOR(*itf, str, ep, (uint8_t)(0x80 | ep), 64)};
  *itf += 1;
  memcpy(dst, d, sizeof d);
  return sizeof d;
}

static void stickText(uint8_t *dst, const char *src, size_t n) {   // SCSI strings are padded with spaces
  memset(dst, ' ', n);
  memcpy(dst, src, min(strlen(src), n));
}

static uint32_t be(const uint8_t *p, int n) {
  uint32_t v = 0;
  while (n--) v = v << 8 | *p++;
  return v;
}

static void putBe(uint8_t *p, uint32_t v, int n) {
  while (n--) p[n] = v, v >>= 8;
}

// Commands TinyUSB does not handle itself. A negative answer makes it fail the
// command with the sense set here, as SCSI expects.
static int32_t stickScsi(uint8_t lun, const uint8_t *cmd, uint8_t *buf, uint16_t size) {
  stickSeen[cmd[0] >> 5] = stickSeen[cmd[0] >> 5] | 1u << (cmd[0] & 31);
  if (cmd[0] != 0x08 && cmd[0] != 0x0A) stickNote('X', cmd[0], 0, size, nullptr, true);
  switch (cmd[0]) {
    case 0x1E:                             // PREVENT ALLOW MEDIUM REMOVAL
    case 0x35:                             // SYNCHRONIZE CACHE (10): nothing is held back here
      return 0;
    case 0x2F:                             // VERIFY (10): the sectors are there; comparing is not offered
      if (cmd[1] & 0x02) break;
      return 0;
    case 0x08:                             // READ (6)
    case 0x0A: {                           // WRITE (6)
      if (!stickMedia) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return -1;
      }
      uint32_t lba = be(cmd + 1, 3) & 0x1FFFFF, n = cmd[4] ? cmd[4] : 256;
      if (n * 512 > CFG_TUD_MSC_BUFSIZE || lba + n > stickShown) break;
      int32_t r = cmd[0] == 0x08 ? stickOnRead(lba, 0, buf, n * 512, 'r') : stickOnWrite(lba, 0, buf, n * 512, 'w');
      if (r < 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, cmd[0] == 0x08 ? 0x11 : 0x0C, 0x00);
        return -1;
      }
      return r;
    }
    case 0x5A: {                           // MODE SENSE (10): no pages, not write-protected
      uint8_t r[8] = {0, 6, 0, 0, 0, 0, 0, 0};
      uint16_t n = min<uint32_t>(min<uint32_t>(be(cmd + 7, 2), sizeof r), size);
      memcpy(buf, r, n);
      return n;
    }
    case 0x9E:                             // SERVICE ACTION IN (16): READ CAPACITY (16)
      if ((cmd[1] & 0x1F) != 0x10) break;
      {
        uint8_t r[32] = {0};
        putBe(r + 4, stickShown - 1, 4);
        putBe(r + 8, 512, 4);
        uint16_t n = min<uint32_t>(min<uint32_t>(be(cmd + 10, 4), sizeof r), size);
        memcpy(buf, r, n);
        return n;
      }
  }
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
  return -1;
}

static uint32_t stickSize(uint32_t card) {
  if (card <= STICK_MAX) return card;
  static uint8_t mbr[512];
  if (disk_read(drive, mbr, 0, 1) != RES_OK) return card;
  if (mbr[510] != 0x55 || mbr[511] != 0xAA) return STICK_MAX;   // no partition table: blank, whoever formats it gets 4 GB
  if (!memcmp(mbr + 54, "FAT", 3) || !memcmp(mbr + 82, "FAT", 3)) return card;   // a file system without a partition table
  uint32_t end = 0;
  for (int i = 0; i < 4; i++) {
    const uint8_t *e = mbr + 446 + 16 * i;
    if (!e[4]) continue;                     // empty entry
    if (e[4] == 0xEE) return card;           // GPT
    uint32_t start = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
    uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
    if (start + len > end) end = start + len;
  }
  return end && end <= STICK_MAX ? STICK_MAX : card;
}

const char *stickStart(bool ram, uint32_t ramMb) {
  if (lent) return nullptr;
  if (ram && !ramAlloc()) return TR("not enough memory", "nicht genug Speicher", "memória insuficiente");
  for (uint8_t p = 0; p < MAX_PORTS; p++) logStop(p);
  uint32_t sectors = !ram ? 0 : ramMb ? min(ramMb, 4095u) * 2048 : RAM_SECTORS;
  bool raw = ram;
  if (!ram) {
    BusGuard guard;
    if (ok) {
      drive = DriveOf::get(SD);
      sectors = SD.numSectors();
      if (SD.sectorSize() != 512) sectors = 0;
    } else {
      // No file system the board can use - a host is about to format the card,
      // or just failed to. The card itself may well be fine: lend it as it is,
      // without mounting anything.
      // sdcard_init() only registers the driver; the card itself is started
      // by disk_initialize(), which a mount would call.
      SD.end();
      drive = 0xFF;
      for (int hz : {10000000, 4000000}) {
        uint8_t d = sdcard_init(PIN_SD_CS, &spi, hz);
        if (d == 0xFF) continue;
        if (!(disk_initialize(d) & STA_NOINIT) && sdcard_sector_size(d) == 512) {
          drive = d;
          sectors = sdcard_num_sectors(d);
          break;
        }
        sdcard_uninit(d);
      }
      if (drive == 0xFF) return TR("no SD card", "keine SD-Karte", "sem cartão SD");
      raw = true;
    }
    if (sectors) sectors = stickSize(sectors);
  }
  if (!ram && (drive == 0xFF || !sectors)) return TR("the card cannot be read", "die Karte lässt sich nicht lesen", "o cartão não pode ser lido");
  Serial.printf("[SD]   USB-Stick-Modus: %lu Sektoren%s, die Debug-Konsole am USB endet hier\n", (unsigned long)sectors,
                ram ? " (im RAM, ohne Karte)" : raw ? " (ohne Dateisystem)" : "");
  Serial.flush();
  ok = false;
  lent = true;
  stickEjected = false;
  stickRead = stickWritten = 0;
  stickRetries = stickFails = 0;
  stickShown = sectors;
  memset((void *)stickSeen, 0, sizeof stickSeen);
  stickLogN = 0;
  Power::forceSaver(0);                    // the USB controller wants the full clock
  stickMedia = true;
  stickDevice();                           // only now is there anything at the port: a stick with its medium in it
  return nullptr;
}

// The board's own USB device: one interface, mass storage.
static void stickDevice() {
  tinyusb_enable_interface(USB_INTERFACE_MSC, TUD_MSC_DESC_LEN, stickDescriptor);
  USB.manufacturerName("RS232 Console");
  USB.productName("RS232 Console SD card");
  // A plain stick: the class is given by its one interface (0/0/0). The core's
  // default is a composite device (0xEF/2/1, IAD), and a Cisco C2960CX refuses
  // that with "USB_ENUM_UNSUPPORTED_DEVICE" before it looks at the interface.
  USB.usbClass(0);
  USB.usbSubClass(0);
  USB.usbProtocol(0);
  USB.begin();                             // takes the port away from the USB serial/JTAG unit
  ownUsb = true;
}

// The chip's serial/JTAG unit must not meet a switch. It answers requests for
// strings it does not have with nothing or with scraps of its ROM instead of
// refusing them (measured: string 5 comes back with 0 bytes, 6 with 239, 7
// with a single 0xFF), and the IOS of a Cisco C2960CX turns such an answer
// into a string of some 250 characters that it writes over its heap: "Corrupted
// magic value in in-use chunk", "Software-forced reload" (30 Oct 2026, twice).
// So unless the debug console is asked for (settings.usbDebug), the port is
// dead outside stick mode: it goes to the USB controller, which is not started
// - no pull-up on D+, a host sees no device at all, there is nothing to get
// wrong. Stick mode starts the controller (stickDevice), and the host sees a
// stick being plugged in, medium and all.
// Which unit owns the port lives in the RTC domain: through a restart the
// serial/JTAG unit does not come back. Power-on clears the choice, and the
// chip then puts the serial/JTAG unit on the port by itself - for the half
// second until this runs, long enough for a host to read all of it (measured).
// That is why the bootloader is ours (bootloader/): it does the two lines
// below some tens of milliseconds after power-on, sooner than a host talks to
// a device it has just seen.
void usbStart(bool debug) {
  if (debug) {
    usbRestore();
    return;
  }
  RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;  // the choice below counts, not the eFuse ...
  RTCCNTL.usb_conf.sw_usb_phy_sel = 1;     // ... and it is the USB controller (what usb_hal_init() sets, too)
  ownUsb = true;
}

// Diagnosis: the host sees the stick unplugged and plugged in again.
void stickSetDelay(uint32_t first, uint32_t next) {
  stickFirstMs = first;
  stickNextMs = next;
}

bool stickReplug() {
  if (!lent) return false;
  stickLogN = 0;
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(1500));
  tud_connect();
  return true;
}

bool usbOwn() { return ownUsb; }
bool unreadable() { return rawCard && !ok && !lent; }
bool usbHostSeen() { return ownUsb && tud_mounted(); }

// The port as the chip's serial/JTAG unit: debug console, flashing over the
// cable. Chosen here in so many words (sw_hw_usb_phy_sel set), not by falling
// back to the chip's default - the bootloader takes a port that nobody has
// chosen away again at every start (bootloader/bootloader_components/usb_port).
static void usbRestore() {
  USB_SERIAL_JTAG.conf0.phy_sel = 0;       // the internal transceiver ...
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
  USB_SERIAL_JTAG.conf0.dp_pullup = 1;
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
  RTCCNTL.usb_conf.sw_usb_phy_sel = 0;     // ... wired to the serial/JTAG unit ...
  RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;  // ... because this says so (as usb_phy_ll_int_jtag_enable() does)
}

Stick stick() {
  Stick s;
  s.on = lent;
  s.host = lent && tud_mounted();
  s.ejected = stickEjected;
  s.readKb = stickRead / 2;
  s.writtenKb = stickWritten / 2;
  s.retries = stickRetries;
  s.sizeMb = stickShown / 2048;
  s.fails = stickFails;
  for (int i = 0; i < 8; i++) s.cmds[i] = stickSeen[i];
  return s;
}

String stickSectors() {
  String out = "[";
  uint32_t n = stickCopyN, first = n > 4 ? n - 4 : 0;
  char hex[3];
  for (uint32_t i = first; i < n; i++) {
    out += (i > first ? ",{\"lba\":" : "{\"lba\":") + String(stickCopyLba[i % 4]) + ",\"hex\":\"";
    for (int b = 0; b < 512; b++) {
      snprintf(hex, sizeof hex, "%02x", stickCopy[i % 4][b]);
      out += hex;
    }
    out += "\"}";
  }
  return out + "]";
}

String stickTrace() {
  String out;
  uint32_t n = stickLogN, first = n > STICK_LOG ? n - STICK_LOG : 0;
  char line[48];
  for (uint32_t i = first; i < n; i++) {
    const StickReq &r = stickLog[i % STICK_LOG];
    snprintf(line, sizeof line, "%c %lu+%lu %lu %s %04x\n", r.op, (unsigned long)r.lba, (unsigned long)r.off,
             (unsigned long)r.size, r.ok ? "ok" : "FAIL", r.sig);
    out += line;
  }
  return out;
}

}  // namespace Sd

// ---- TinyUSB's mass storage callbacks (one drive: the card in stick mode)
extern "C" {

uint8_t tud_msc_get_maxlun_cb(void) { return 1; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t rev[4]) {
  (void)lun;
  Sd::stickText(vendor, "RS232", 8);
  Sd::stickText(product, "Console SD card", 16);
  Sd::stickText(rev, "1.0", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (Sd::stickMedia) return true;
  tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
  return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
  (void)lun;
  Sd::stickNote('C', Sd::stickShown, 0, 0, nullptr, Sd::stickMedia);
  *count = Sd::stickMedia ? Sd::stickShown : 0;
  *size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool eject) {
  (void)lun;
  (void)power;
  Sd::stickNote('S', start, eject, power, nullptr, true);
  if (eject) {                             // an ejected stick has no medium until the host loads it again
    Sd::stickEjected = !start;
    Sd::stickMedia = start;
  }
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  (void)lun;
  return Sd::stickMedia ? Sd::stickOnRead(lba, offset, buffer, size) : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
  (void)lun;
  return Sd::stickMedia ? Sd::stickOnWrite(lba, offset, buffer, size) : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const cmd[16], void *buffer, uint16_t size) {
  return Sd::stickScsi(lun, cmd, (uint8_t *)buffer, size);
}

}  // extern "C"
