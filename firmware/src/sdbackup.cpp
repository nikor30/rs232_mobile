#include "sdbackup.h"
#include "config.h"
#include "sdcard.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <nvs.h>
#include <vector>

namespace SdBackup {

static const char MAGIC[8] = {'R', 'S', '2', '3', '2', 'B', 'K', '1'};
static const char *NVS_NS = "rs232";                 // settings.cpp
static const char *const DIRS[] = {"/c", "/tls"};    // configs.cpp, certs.cpp
static const size_t MAX_SIZE = 512 * 1024;

// Everything of the backup lives in the external RAM: a JSON tree of a few
// hundred small pieces would cut up the internal heap (c2.cpp, big()).
struct ExtAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static ExtAllocator ext;

static uint8_t *extAlloc(size_t n) { return (uint8_t *)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static bool b64(const uint8_t *data, size_t len, JsonVariant out) {
  size_t n = 0;
  mbedtls_base64_encode(nullptr, 0, &n, data, len);
  char *s = (char *)extAlloc(n + 1);
  if (!s || mbedtls_base64_encode((uint8_t *)s, n + 1, &n, data, len)) { free(s); return false; }
  s[n] = 0;
  out.set(s);                                         // copied into the document
  free(s);
  return true;
}

static uint8_t *unb64(const char *s, size_t &len) {
  size_t n = strlen(s), cap = n * 3 / 4 + 4;
  uint8_t *d = extAlloc(cap);
  if (d && mbedtls_base64_decode(d, cap, &len, (const uint8_t *)s, n)) { free(d); return nullptr; }
  return d;
}

static void dumpNvs(JsonArray out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  nvs_iterator_t it = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_ANY);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    JsonObject e = out.add<JsonObject>();
    e["k"] = info.key;
    e["t"] = (int)info.type;
    switch (info.type) {
      case NVS_TYPE_U8:  { uint8_t v;  nvs_get_u8(h, info.key, &v);  e["v"] = v; break; }
      case NVS_TYPE_I8:  { int8_t v;   nvs_get_i8(h, info.key, &v);  e["v"] = v; break; }
      case NVS_TYPE_U16: { uint16_t v; nvs_get_u16(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_I16: { int16_t v;  nvs_get_i16(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_U32: { uint32_t v; nvs_get_u32(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_I32: { int32_t v;  nvs_get_i32(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_U64: { uint64_t v; nvs_get_u64(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_I64: { int64_t v;  nvs_get_i64(h, info.key, &v); e["v"] = v; break; }
      case NVS_TYPE_STR: {
        size_t n = 0;
        nvs_get_str(h, info.key, nullptr, &n);
        char *s = (char *)extAlloc(n);
        if (s && nvs_get_str(h, info.key, s, &n) == ESP_OK) e["v"] = s;
        free(s);
        break;
      }
      case NVS_TYPE_BLOB: {
        size_t n = 0;
        nvs_get_blob(h, info.key, nullptr, &n);
        uint8_t *b = extAlloc(n);
        if (b && nvs_get_blob(h, info.key, b, &n) == ESP_OK) b64(b, n, e["v"].to<JsonVariant>());
        free(b);
        break;
      }
      default: break;
    }
    it = nvs_entry_next(it);
  }
  nvs_close(h);
}

// Two settings are never taken from a backup: whether the control center may
// change things here, and unattended access. Both are granted at the device
// only (CLAUDE.md) - a restore must not hand them out from afar.
static bool keepHere(const char *k) { return !strcmp(k, "c2ctl") || !strcmp(k, "c2una"); }

static bool restoreNvs(JsonArray in) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  uint8_t ctl = 0;
  uint32_t una = 0;
  nvs_get_u8(h, "c2ctl", &ctl);
  nvs_get_u32(h, "c2una", &una);
  nvs_erase_all(h);
  nvs_set_u8(h, "c2ctl", ctl);
  nvs_set_u32(h, "c2una", una);
  bool ok = true;
  for (JsonObject e : in) {
    const char *k = e["k"] | "";
    if (!*k || strlen(k) > 15 || keepHere(k)) continue;
    esp_err_t r = ESP_OK;
    switch ((int)(e["t"] | 0)) {
      case NVS_TYPE_U8:  r = nvs_set_u8(h, k, e["v"].as<uint8_t>()); break;
      case NVS_TYPE_I8:  r = nvs_set_i8(h, k, e["v"].as<int8_t>()); break;
      case NVS_TYPE_U16: r = nvs_set_u16(h, k, e["v"].as<uint16_t>()); break;
      case NVS_TYPE_I16: r = nvs_set_i16(h, k, e["v"].as<int16_t>()); break;
      case NVS_TYPE_U32: r = nvs_set_u32(h, k, e["v"].as<uint32_t>()); break;
      case NVS_TYPE_I32: r = nvs_set_i32(h, k, e["v"].as<int32_t>()); break;
      case NVS_TYPE_U64: r = nvs_set_u64(h, k, e["v"].as<uint64_t>()); break;
      case NVS_TYPE_I64: r = nvs_set_i64(h, k, e["v"].as<int64_t>()); break;
      case NVS_TYPE_STR: r = nvs_set_str(h, k, e["v"] | ""); break;
      case NVS_TYPE_BLOB: {
        size_t n = 0;
        uint8_t *b = unb64(e["v"] | "", n);
        r = b ? nvs_set_blob(h, k, b, n) : ESP_FAIL;
        free(b);
        break;
      }
      default: continue;
    }
    ok &= r == ESP_OK;
  }
  ok &= nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

static bool dumpFiles(JsonArray out) {
  for (const char *dirName : DIRS) {
    File dir = LittleFS.open(dirName);
    if (!dir || !dir.isDirectory()) continue;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      size_t n = f.size();
      uint8_t *b = extAlloc(n);
      if (!b) return false;
      size_t got = f.read(b, n);
      JsonObject e = out.add<JsonObject>();
      e["p"] = String(dirName) + "/" + f.name();
      bool ok = got == n && b64(b, n, e["d"].to<JsonVariant>());
      free(b);
      if (!ok) return false;
    }
  }
  return true;
}

static bool restoreFiles(JsonArray in) {
  for (const char *dirName : DIRS) {                  // what is there now goes first
    File dir = LittleFS.open(dirName);
    std::vector<String> names;
    if (dir && dir.isDirectory())
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) names.push_back(String(dirName) + "/" + f.name());
    if (dir) dir.close();
    for (auto &n : names) LittleFS.remove(n);
    LittleFS.mkdir(dirName);
  }
  bool ok = true;
  for (JsonObject e : in) {
    String p = e["p"] | "";
    if (!p.startsWith("/c/") && !p.startsWith("/tls/")) continue;
    if (p.indexOf("..") >= 0) continue;
    size_t n = 0;
    uint8_t *b = unb64(e["d"] | "", n);
    File f = b ? LittleFS.open(p, "w") : File();
    ok &= f && f.write(b, n) == n;
    if (f) f.close();
    free(b);
  }
  return ok;
}

const char *write(const uint8_t key[32], String &info) {
  if (!Sd::mounted()) return "no SD card in the device";
  JsonDocument d(&ext);
  d["v"] = 1;
  d["fw"] = FW_VERSION;
  d["board"] = FW_BOARD_ID;
  dumpNvs(d["nvs"].to<JsonArray>());
  if (!dumpFiles(d["files"].to<JsonArray>())) return "not enough memory for the backup";
  if (d.overflowed()) return "not enough memory for the backup";
  size_t plain = measureJson(d);
  if (plain > MAX_SIZE) return "the backup is too large";
  size_t total = sizeof(MAGIC) + 12 + 16 + plain;
  uint8_t *buf = extAlloc(total);
  if (!buf) return "not enough memory for the backup";
  uint8_t *nonce = buf + sizeof(MAGIC), *tag = nonce + 12, *body = tag + 16;
  serializeJson(d, (char *)body, plain);
  memcpy(buf, MAGIC, sizeof(MAGIC));
  esp_fill_random(nonce, 12);
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = !mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) &&
            !mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plain, nonce, 12, (const uint8_t *)MAGIC, sizeof(MAGIC), body, body, 16, tag);
  mbedtls_gcm_free(&g);
  const char *err = ok ? Sd::writeRoot(FILE_NAME, buf, total) : "encrypting the backup failed";
  memset(buf, 0, total);
  free(buf);
  if (!err) info = String("backup written to the SD card: ") + total + " bytes, " + d["nvs"].size() + " settings, " + d["files"].size() + " files";
  return err;
}

const char *restore(const uint8_t key[32], String &info) {
  uint8_t *buf = nullptr;
  size_t total = 0;
  const char *err = Sd::readRoot(FILE_NAME, buf, total, MAX_SIZE + 64);
  if (err) return err;
  if (total < sizeof(MAGIC) + 28 || memcmp(buf, MAGIC, sizeof(MAGIC))) { free(buf); return "the file on the SD card is no backup of this kind"; }
  uint8_t *nonce = buf + sizeof(MAGIC), *tag = nonce + 12, *body = tag + 16;
  size_t plain = total - sizeof(MAGIC) - 28;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = !mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) &&
            !mbedtls_gcm_auth_decrypt(&g, plain, nonce, 12, (const uint8_t *)MAGIC, sizeof(MAGIC), tag, 16, body, body);
  mbedtls_gcm_free(&g);
  if (!ok) { free(buf); return "the backup does not open with this key (another device's, or damaged)"; }
  JsonDocument d(&ext);
  DeserializationError je = deserializeJson(d, (const char *)body, plain);
  memset(buf, 0, total);
  free(buf);
  if (je || (d["v"] | 0) != 1) return "the backup is not readable";
  if (strcmp(d["board"] | "", FW_BOARD_ID)) return "the backup is from another kind of board";
  bool nv = restoreNvs(d["nvs"].as<JsonArray>()), fs = restoreFiles(d["files"].as<JsonArray>());
  if (!nv || !fs) return "the backup could not be restored completely - check the settings";
  info = String("restored from the SD card: ") + d["nvs"].size() + " settings, " + d["files"].size() + " files, restarting";
  return nullptr;
}

}  // namespace SdBackup
