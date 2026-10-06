#include "ble.h"

#if HAS_BLE
#include "settings.h"
#include "serial_bridge.h"
#include "net.h"
#include <NimBLEDevice.h>
#include <WiFi.h>

namespace Ble {

static const uint8_t PORT = 0;                 // the port this link is bridged to
static const char *UUID_SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";   // Nordic UART Service
static const char *UUID_RX  = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";   // phone -> device
static const char *UUID_TX  = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";   // device -> phone

static const size_t TX_QUEUE_SIZE = 4096;      // serial bytes waiting for the phone
static const size_t RX_QUEUE_SIZE = 1024;      // typed bytes waiting for the serial line

static NimBLEServer *server = nullptr;
static NimBLECharacteristic *txChar = nullptr;
static bool running = false;
static uint32_t pin = 0;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool secure = false;           // encrypted + authenticated
static volatile uint32_t connectedAt = 0;
static const uint32_t PAIR_TIMEOUT_MS = 60000; // an unpaired link may not occupy the single slot for longer

static uint8_t txQ[TX_QUEUE_SIZE];
static size_t txHead = 0, txLen = 0;
static uint32_t txDropped = 0;

static uint8_t rxQ[RX_QUEUE_SIZE];             // filled in the NimBLE task, emptied in loop()
static size_t rxHead = 0, rxLen = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t rxDropped = 0;

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    conn = info.getConnHandle();
    secure = false;
    connectedAt = millis();
    Serial.printf("[BT]   verbunden: %s - warte auf Kopplung\n", info.getAddress().toString().c_str());
    NimBLEDevice::startSecurity(info.getConnHandle());
    Net::markDirty();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    conn = BLE_HS_CONN_HANDLE_NONE;
    secure = false;
    Serial.printf("[BT]   getrennt (Grund 0x%X)\n", reason);
    Net::markDirty();
  }
  uint32_t onPassKeyDisplay() override { return pin; }
  // Whether the link ended up secure is read from the connection in loop();
  // this event only serves to turn away a pairing that came out too weak.
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    if (info.isEncrypted() && info.isAuthenticated()) return;
    Serial.println("[BT]   abgewiesen: Kopplung ohne PIN oder unverschluesselt");
    if (server) server->disconnect(info.getConnHandle());
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    if (!info.isEncrypted() || !info.isAuthenticated()) return;
    portENTER_CRITICAL(&rxMux);
    for (size_t i = 0; i < v.length(); i++) {
      if (rxLen == RX_QUEUE_SIZE) { rxDropped = rxDropped + (v.length() - i); break; }
      rxQ[(rxHead + rxLen) % RX_QUEUE_SIZE] = v.data()[i];
      rxLen++;
    }
    portEXIT_CRITICAL(&rxMux);
  }
};

static ServerCb serverCb;
static RxCb rxCb;

static void start() {
  if (running) return;
  WiFi.setSleep(true);                   // must be on before Bluetooth joins a WiFi station on the radio (see net.cpp)
  NimBLEDevice::init(settings.apSsid.c_str());
  NimBLEDevice::setSecurityAuth(true, true, true);            // bonding, MITM protection, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);     // we show the PIN, the phone types it
  NimBLEDevice::setSecurityPasskey(pin);
  NimBLEDevice::setMTU(247);

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCb, false);
  server->advertiseOnDisconnect(true);
  NimBLEService *svc = server->createService(UUID_SVC);
  txChar = svc->createCharacteristic(UUID_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                              NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  NimBLECharacteristic *rx = svc->createCharacteristic(
      UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
               NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(&rxCb);
  server->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);         // first: the name then goes into the scan response,
  adv->addServiceUUID(UUID_SVC);         // the 128 bit UUID fills the advertisement itself
  adv->setName(settings.apSsid.c_str());
  adv->setMinInterval(400);              // 250..400 ms instead of the default 30..60 ms: a phone still
  adv->setMaxInterval(640);              // finds the name within a second or two, the radio sends a tenth as often
  adv->start();
  running = true;
  txLen = 0;
  Serial.printf("[BT]   an: \"%s\", PIN %06lu, Port %u\n", settings.apSsid.c_str(), (unsigned long)pin, PORT + 1);
}

static void stop() {
  if (!running) return;
  running = false;
  conn = BLE_HS_CONN_HANDLE_NONE;
  secure = false;
  NimBLEDevice::deinit(true);
  server = nullptr;
  txChar = nullptr;
  Serial.println("[BT]   aus");
}

void begin() {
  pin = 100000 + esp_random() % 900000;
  if (settings.bleEnabled) start();
}

void setEnabled(bool on) {
  if (settings.bleEnabled != on) {
    settings.bleEnabled = on;
    Store::save();
  }
  if (on) start(); else stop();
  Net::markDirty();
}

State state() {
  if (!running) return OFF;
  if (conn == BLE_HS_CONN_HANDLE_NONE) return ADVERTISING;
  return secure ? CONNECTED : PAIRING;
}

uint32_t passkey() { return pin; }

void onSerialData(uint8_t port, const uint8_t *data, size_t len) {
  if (port != PORT || !running || !secure) return;
  for (size_t i = 0; i < len; i++) {
    if (txLen == TX_QUEUE_SIZE) { txDropped += len - i; break; }
    txQ[(txHead + txLen) % TX_QUEUE_SIZE] = data[i];
    txLen++;
  }
}

void loop() {
  if (!running) return;

  uint16_t h = conn;
  if (h != BLE_HS_CONN_HANDLE_NONE) {
    NimBLEConnInfo info = server->getPeerInfoByHandle(h);
    bool s = info.isEncrypted() && info.isAuthenticated();
    if (s != secure) {
      secure = s;
      Serial.println(s ? "[BT]   gekoppelt, Verbindung verschluesselt" : "[BT]   Verschluesselung beendet");
      Net::markDirty();
    }
    if (!s && millis() - connectedAt > PAIR_TIMEOUT_MS) {
      Serial.println("[BT]   getrennt: keine Kopplung innerhalb von 60 s");
      connectedAt = millis();
      server->disconnect(h);
    }
  }

  // phone -> serial line, only as much as the port's send queue takes
  uint8_t buf[128];
  size_t n = 0;
  size_t room = Bridge::txFree(PORT);
  if (room > sizeof(buf)) room = sizeof(buf);
  portENTER_CRITICAL(&rxMux);
  while (n < room && rxLen) {
    buf[n++] = rxQ[rxHead];
    rxHead = (rxHead + 1) % RX_QUEUE_SIZE;
    rxLen--;
  }
  uint32_t lost = rxDropped;
  rxDropped = 0;
  portEXIT_CRITICAL(&rxMux);
  if (n) Bridge::write(PORT, buf, n);
  if (lost) Net::onMessage("Bluetooth: " + String(lost) + " Byte Eingabe verworfen (Port zu langsam)");

  // serial line -> phone. notify() says whether the stack took the packet; if
  // not, the bytes stay queued and are tried again on the next pass.
  if (!secure || conn == BLE_HS_CONN_HANDLE_NONE) { txLen = 0; return; }
  uint16_t handle = conn;
  size_t mtu = server->getPeerMTU(handle);
  size_t chunkMax = mtu > 23 ? mtu - 3 : 20;
  if (chunkMax > sizeof(buf)) chunkMax = sizeof(buf);
  for (int pass = 0; pass < 3 && txLen; pass++) {
    size_t c = txLen < chunkMax ? txLen : chunkMax;
    for (size_t i = 0; i < c; i++) buf[i] = txQ[(txHead + i) % TX_QUEUE_SIZE];
    if (!txChar->notify(buf, c, handle)) break;
    txHead = (txHead + c) % TX_QUEUE_SIZE;
    txLen -= c;
  }
  if (txDropped && txLen < TX_QUEUE_SIZE / 2) {
    Net::onMessage("Bluetooth: " + String(txDropped) + " Byte verworfen (Empfaenger zu langsam)");
    txDropped = 0;
  }
}

}  // namespace Ble
#endif
