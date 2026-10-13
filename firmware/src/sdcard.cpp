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
#include <USBMSC.h>
#include "tusb.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/rtc_cntl_struct.h"
#include "ff.h"
#include "diskio.h"
#include "display.h"
#include "power.h"

namespace Sd {

static const uint32_t FLUSH_MS = 2000;     // SD writes are slow: collect, then flush
static const size_t   BUF_SIZE = 1024;

static bool ok = false;
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
static USBMSC msc;
static uint8_t drive = 0xFF;
static volatile bool stickEjected = false;
static volatile uint32_t stickRead = 0, stickWritten = 0;    // sectors

// SD keeps the number of its drive to itself.
struct DriveOf : fs::SDFS {
  static uint8_t get(fs::SDFS &s) { return s.*(&DriveOf::_pdrv); }
};

static int32_t stickOnRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  if (offset || size % 512) return -1;
  BusGuard guard;
  if (disk_read(drive, (uint8_t *)buffer, lba, size / 512) != RES_OK) return -1;
  stickRead = stickRead + size / 512;
  return size;
}

static int32_t stickOnWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
  if (offset || size % 512) return -1;
  BusGuard guard;
  if (disk_write(drive, buffer, lba, size / 512) != RES_OK) return -1;
  stickWritten = stickWritten + size / 512;
  return size;
}

static bool stickOnStartStop(uint8_t power, bool start, bool eject) {
  (void)power;
  if (eject) {
    stickEjected = !start;
    msc.mediaPresent(start);               // an ejected stick has no medium until the host loads it again
  }
  return true;
}

const char *stickStart() {
#if !HAS_USB_STICK
  return TR("not on this board", "nicht an diesem Board", "nao nesta placa");   // its USB pins carry the touch bus
#endif
  if (lent) return nullptr;
  if (!ok) return TR("no SD card", "keine SD-Karte", "sem cartão SD");
  for (uint8_t p = 0; p < MAX_PORTS; p++) logStop(p);
  uint32_t sectors;
  {
    BusGuard guard;
    drive = DriveOf::get(SD);
    sectors = SD.numSectors();
    if (SD.sectorSize() != 512) sectors = 0;
  }
  if (drive == 0xFF || !sectors) return TR("the card cannot be read", "die Karte lässt sich nicht lesen", "o cartão não pode ser lido");
  Serial.printf("[SD]   USB-Stick-Modus: %lu Sektoren, die Debug-Konsole am USB endet hier\n", (unsigned long)sectors);
  Serial.flush();
  ok = false;
  lent = true;
  stickEjected = false;
  stickRead = stickWritten = 0;
  Power::forceSaver(0);                    // the USB controller wants the full clock
  msc.vendorID("RS232");
  msc.productID("Console SD card");
  msc.productRevision("1.0");
  msc.onRead(stickOnRead);
  msc.onWrite(stickOnWrite);
  msc.onStartStop(stickOnStartStop);
  msc.mediaPresent(true);
  msc.begin(sectors, 512);
  USB.manufacturerName("RS232 Console");
  USB.productName("RS232 Console SD card");
  USB.begin();                             // takes the port away from the USB serial/JTAG unit
  return nullptr;
}

// Which unit owns the USB pins is kept in the RTC domain: it survives every
// restart, only a power cut clears it. Without this the port stays dead after
// stick mode - no debug console, no flashing over the cable.
void usbRestore() {
#if !HAS_USB_STICK
  return;                                  // the USB pins are not a USB port here (lcd_ui.cpp)
#endif
  USB_SERIAL_JTAG.conf0.phy_sel = 0;       // the internal transceiver ...
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
  RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 0;  // ... wired to the serial/JTAG unit, as after power-on
  RTCCNTL.usb_conf.sw_usb_phy_sel = 0;
}

Stick stick() {
  Stick s;
  s.on = lent;
  s.host = lent && tud_mounted();
  s.ejected = stickEjected;
  s.readKb = stickRead / 2;
  s.writtenKb = stickWritten / 2;
  return s;
}

}  // namespace Sd
