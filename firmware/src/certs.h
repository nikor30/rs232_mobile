#pragma once
#include <Arduino.h>

// Certificates for the WLAN client (802.1X EAP-TLS) and HTTPS.
// Stored as PEM in LittleFS /tls/. Uploads may be PEM, DER, PKCS#7 (.p7b) or
// PKCS#12 (.p12/.pfx, AES as well as legacy 3DES/RC2 encryption); encrypted keys
// are decrypted with the given password and stored unencrypted (without flash
// encryption anyone with the board can read them - see README).
namespace Certs {
  enum Slot : uint8_t { CA = 0, CLIENT = 1, HTTPS = 2, SLOTS = 3 };

  void begin();                                  // after LittleFS is mounted (Configs::begin)
  void loop();                                   // finishes key generation running in the background
  void wipe();                                   // factory reset: all certificates + keys

  // data = file content. Returns false + msg (German) on error, true + summary on success.
  bool upload(Slot slot, const uint8_t *data, size_t len, const String &password, String &msg);
  bool remove(Slot slot);

  bool hasCert(Slot s);
  bool hasKey(Slot s);
  bool ready(Slot s);                            // CA: certificate; others: certificate + matching key
  bool serverUsable(Slot s);                     // certificate may be used by a TLS server (EKU)
  String subjectCN(Slot s);                      // common name of the (first) certificate
  String summary(Slot s);                        // "CN=... bis 2027-05-01"
  // thread-safe copies (the HTTPS task reads while the web UI may upload)
  void copy(Slot s, String &certPem, String &keyPem);
  uint32_t generation();                         // changes whenever a certificate/key changes

  String infoJson();                             // all slots + CSR state + device CA

  // key pair + certificate signing request, generated on the device (key never leaves it)
  bool csrStart(const String &subject, const String &sans, bool rsa, String &err);
  String csrPem();
  bool busy();                                   // key generation running

  // HTTPS certificate issued by the device's own CA when none was uploaded.
  // Re-issued when names/addresses change or it is about to expire.
  void ensureAutoHttps(const String &hostname, const String &caName, const String &staIp);
  bool httpsIsAuto();
  String deviceCaPem();

  // wall clock (no RTC): learned from the browser; enables expiry warnings
  void setTime(uint32_t epoch);
  bool timeKnown();
}
