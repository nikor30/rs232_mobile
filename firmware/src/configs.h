#pragma once
#include <Arduino.h>

// Stored configuration snippets for provisioning devices (web UI tab "Konfig").
// LittleFS on the "spiffs" partition, one file per config: first line = name.
namespace Configs {
  bool begin();
  bool mounted();
  String listJson();                                    // {"ok":..,"used":..,"total":..,"configs":[{name,size}]}
  bool read(const String &name, String &text);
  const char *save(const String &name, const String &text, const String &oldName);   // nullptr = ok
  bool remove(const String &name);
  static const size_t MAX_TEXT = 32768;
  static const size_t MAX_NAME = 48;
}
