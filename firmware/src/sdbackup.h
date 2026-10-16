#pragma once
#include <Arduino.h>

// The device's own backup on its SD card, encrypted with a key that only the
// control center holds (server/internal/server/backup.go): settings with their
// passwords, stored configurations and the certificates. Without the server the
// file on the card is useless to whoever takes the card; with it, the device -
// or a replacement for it - gets everything back.
//
//   /rs232-backup.bin   "RS232BK1" | 12 bytes nonce | 16 bytes tag | AES-256-GCM(JSON)
namespace SdBackup {
  static const char *const FILE_NAME = "rs232-backup.bin";
  const char *write(const uint8_t key[32], String &info);     // nullptr = written; info: what
  const char *restore(const uint8_t key[32], String &info);   // nullptr = restored - restart then
}
