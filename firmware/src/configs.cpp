#include "configs.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace Configs {

static bool ok = false;
static const char *DIR_PATH = "/c";

bool begin() {
  ok = LittleFS.begin(true);            // formats the partition on first use
  if (ok) LittleFS.mkdir(DIR_PATH);     // no-op when it exists (exists() would log an error)
  Serial.printf("[CFG] Speicher %s: %u von %u kB belegt\n", ok ? "ok" : "FEHLER",
                ok ? (unsigned)(LittleFS.usedBytes() / 1024) : 0, ok ? (unsigned)(LittleFS.totalBytes() / 1024) : 0);
  return ok;
}

bool mounted() { return ok; }

static String readName(File &f) {
  String n = f.readStringUntil('\n');
  n.trim();
  return n;
}

// path of the file holding config <name>, "" if none
static String findPath(const String &name) {
  if (!ok) return "";
  File dir = LittleFS.open(DIR_PATH);
  if (!dir) return "";
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String path = String(DIR_PATH) + "/" + f.name();
    bool match = readName(f) == name;
    f.close();
    if (match) return path;
  }
  return "";
}

String listJson() {
  JsonDocument d;
  d["ok"] = ok;
  d["used"] = ok ? LittleFS.usedBytes() : 0;
  d["total"] = ok ? LittleFS.totalBytes() : 0;
  d["maxText"] = MAX_TEXT;
  JsonArray a = d["configs"].to<JsonArray>();
  if (ok) {
    File dir = LittleFS.open(DIR_PATH);
    if (dir) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        String n = readName(f);
        JsonObject o = a.add<JsonObject>();
        o["name"] = n;
        o["size"] = (uint32_t)f.size() - (n.length() + 1);
        f.close();
      }
    }
  }
  String s;
  serializeJson(d, s);
  return s;
}

bool read(const String &name, String &text) {
  String path = findPath(name);
  if (path.isEmpty()) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  readName(f);
  text = f.readString();
  f.close();
  return true;
}

static bool validName(const String &n) {
  if (n.isEmpty() || n.length() > MAX_NAME) return false;
  for (size_t i = 0; i < n.length(); i++)
    if ((uint8_t)n[i] < 0x20) return false;
  return true;
}

const char *save(const String &nameIn, const String &text, const String &oldName) {
  if (!ok) return "Speicher nicht verfügbar";
  String name = nameIn;
  name.trim();
  if (!validName(name)) return "Name: 1-48 Zeichen";
  if (text.length() > MAX_TEXT) return "Text zu lang (max. 32 kB)";
  String path = findPath(name);
  if (path.isEmpty() && oldName.length() && oldName != name) path = findPath(oldName);   // rename in place
  if (path.isEmpty()) {
    // first free file number (collected from the directory, not probed with exists())
    static bool used[1000];
    memset(used, 0, sizeof(used));
    File dir = LittleFS.open(DIR_PATH);
    if (dir) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        int n = atoi(f.name());
        if (n > 0 && n < 1000) used[n] = true;
        f.close();
      }
    }
    for (int i = 1; i < 1000; i++) {
      if (used[i]) continue;
      char p[20];
      snprintf(p, sizeof(p), "%s/%03d.txt", DIR_PATH, i);
      path = p;
      break;
    }
    if (path.isEmpty()) return "Zu viele Konfigurationen";
  }
  size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (text.length() + 4096 > freeB) return "Speicher voll";
  File f = LittleFS.open(path, "w");
  if (!f) return "Datei kann nicht geschrieben werden";
  f.print(name);
  f.print('\n');
  size_t w = f.print(text);
  f.close();
  if (w != text.length()) return "Schreibfehler";
  // renamed into an existing file of the new name: drop the old one
  if (oldName.length() && oldName != name) {
    String old = findPath(oldName);
    if (old.length() && old != path) LittleFS.remove(old);
  }
  return nullptr;
}

bool remove(const String &name) {
  String path = findPath(name);
  return path.length() && LittleFS.remove(path);
}

}  // namespace Configs
