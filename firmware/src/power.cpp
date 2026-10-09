#include "power.h"
#include "config.h"
#include "settings.h"
#include "bat_curve.h"
#if ARDUINO_USB_MODE
#include "soc/usb_serial_jtag_struct.h"
#define USB_SOF_DETECT 1
#else
#define USB_SOF_DETECT 0
#endif

// Battery monitoring only (voltage divider to an ADC pin, web UI -> Ports).
// Over-discharge protection is up to the battery/charger hardware.

namespace Power {

static float filt = 0;
static uint32_t lastSample = 0;

bool measured() { return settings.batPin >= 0; }
const char *typeName() { return settings.batType == 1 ? "NiCd/NiMH 4 Zellen" : "LiPo 1S"; }

// ---- charging ---------------------------------------------------------------
// The charger on these boards (ETA6096 on the Waveshare) has no status line to
// the processor, so "charging" is concluded from two observations:
//  1. A USB host sends a start-of-frame every millisecond. Seeing the frame
//     number move proves that a computer feeds the port. A plain USB power
//     supply sends nothing and cannot be seen this way.
//  2. The battery voltage. Above CHG_FULL_MV a cell is being held up by the
//     charger (or was until a few minutes ago); a step up or down by
//     CHG_STEP_MV within CHG_STEP_S seconds is the charger being plugged in or
//     pulled. A power supply that is already plugged in at start-up stays
//     unnoticed until the cell reaches CHG_FULL_MV.
// "Full" is only reported after CHG_FULL_HOLD_S at that voltage: the charger
// reaches its end voltage long before the current has tapered off. The second
// way to "full" does not depend on the absolute reading (the board measured
// 4.05 V on a cell that no longer rose): above CHG_FLAT_MIN_MV a cell that is
// still taking current climbs by the minute, so a voltage that has stayed
// within CHG_FLAT_MV for CHG_FLAT_S is a cell the charger is done with.
#define CHG_FULL_MV      4150
#define CHG_FULL_HOLD_S  1800
#define CHG_STEP_MV      40
#define CHG_STEP_S       15
#define CHG_BOOT_S       30      // start-up load changes are not charger steps
#define CHG_FLAT_MIN_MV  4000
#define CHG_FLAT_MV      8
#define CHG_FLAT_S       900

static bool host = false, stepUp = false;
static uint16_t lastSof = 0;
static uint16_t hist[CHG_STEP_S];
static uint8_t histPos = 0, histFill = 0;
static uint32_t highSince = 0;          // seconds the voltage has been at CHG_FULL_MV, 0 = it is not
static uint16_t lastPinMv = 0;
static uint16_t flatRef = 0;            // voltage the cell has stayed at ...
static uint32_t flatSince = 0;          // ... for this many seconds

bool usbHost() { return host; }

// ---- power saving
#define SAVER_AFTER_S 10                // the charge state must have settled (USB frames need a moment after start-up)
static uint8_t batSecs = 0, emptySecs = 0;
static int8_t saverForced = -1;
bool saver() { return saverForced >= 0 ? saverForced == 1 : batSecs >= SAVER_AFTER_S; }
void forceSaver(int8_t mode) { saverForced = mode; }
#ifdef BAT_OFF_MV
bool empty() { return emptySecs >= BAT_OFF_S; }
#else
bool empty() { return false; }
#endif

static void chargeSample() {
#if USB_SOF_DETECT
  uint16_t sof = USB_SERIAL_JTAG.fram_num.sof_frame_index;
  host = sof != lastSof;
  lastSof = sof;
#endif
  if (!present() || settings.batType != 0) { stepUp = false; histFill = 0; highSince = 0; flatSince = 0; return; }
  uint16_t v = (uint16_t)filt;
  if (abs((int)v - (int)flatRef) > CHG_FLAT_MV) { flatRef = v; flatSince = 0; }
  else flatSince++;
  if (millis() < CHG_BOOT_S * 1000UL) return;
  if (histFill == CHG_STEP_S) {
    int d = (int)v - (int)hist[histPos];             // against the value CHG_STEP_S seconds ago
    if (d >= CHG_STEP_MV) stepUp = true;
    else if (d <= -CHG_STEP_MV) stepUp = false;
  } else histFill++;
  hist[histPos] = v;
  histPos = (histPos + 1) % CHG_STEP_S;
  if (v >= CHG_FULL_MV) { if (highSince < UINT32_MAX) highSince++; }
  else highSince = 0;
}

Charge charge() {
  if (!present() || settings.batType != 0) return ON_BATTERY;
  bool external = host || stepUp || highSince > 0;
  if (!external) return ON_BATTERY;
  bool flat = filt >= CHG_FLAT_MIN_MV && flatSince >= CHG_FLAT_S;
  return highSince >= CHG_FULL_HOLD_S || flat ? FULL : CHARGING;
}

const char *chargeName() {
  Charge c = charge();
  return c == CHARGING ? "laedt" : c == FULL ? "voll" : "";
}

String diag() {
  char buf[340];
  if (!measured()) return "keine Messung";
  uint8_t t = settings.batType;
  snprintf(buf, sizeof(buf), "GPIO%d %u mV x %u.%u = %u mV, gefiltert %u mV, %u%% (0 %% = %u mV%s, 100 %% = %u mV%s), %s | USB-Host %s, Sprung %s, >=%u mV seit %lu s, konstant seit %lu s -> %s | Sparmodus %s, CPU %u MHz",
           settings.batPin, lastPinMv, settings.batDiv / 10, settings.batDiv % 10,
           (unsigned)(lastPinMv * (settings.batDiv / 10.0f) * BAT_CAL), mv(), pct(),
           settings.batEmptyMv ? settings.batEmptyMv : BatCurve::defaultEmptyMv(t), settings.batEmptyMv ? " kalibriert" : "",
           settings.batFullMv ? settings.batFullMv : BatCurve::defaultFullMv(t), settings.batFullMv ? " kalibriert" : "",
           present() ? "Akku da" : "kein Akku",
           USB_SOF_DETECT ? (host ? "ja" : "nein") : "n/a", stepUp ? "ja" : "nein", CHG_FULL_MV, (unsigned long)highSince, (unsigned long)flatSince,
           charge() == ON_BATTERY ? "Akkubetrieb" : chargeName(),
           saverForced >= 0 ? (saver() ? "erzwungen" : "gesperrt") : saver() ? "ja" : "nein", (unsigned)getCpuFrequencyMhz());
  return String(buf);
}

static uint16_t sampleMv() {
  if (!measured()) return 0;
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(settings.batPin);
  lastPinMv = acc / 8;
  return (uint16_t)(lastPinMv * (settings.batDiv / 10.0f) * BAT_CAL);
}

void begin() {
  if (!measured()) return;
  analogSetPinAttenuation(settings.batPin, ADC_11db);
  filt = sampleMv();
}

bool present() { return measured() && filt >= (settings.batType == 1 ? 3000 : 2500); }
uint16_t mv() { return (uint16_t)filt; }

uint8_t pct() {
  if (!present()) return 0;
  return BatCurve::percent(settings.batType, mv(), settings.batEmptyMv, settings.batFullMv);
}

// ---- calibration ------------------------------------------------------------
// The curve expects 4.20 V on a full LiPo and 3.30 V on an empty one. What the
// divider and the ADC of one particular board report for these two states is
// stored here (0 = not calibrated) and BatCurve::percent() stretches the curve
// between them. Only the percentage follows; mv(), the charge detection and the
// switch-off threshold keep working on the measured voltage.
uint16_t calEmptyMv() { return settings.batEmptyMv; }
uint16_t calFullMv() { return settings.batFullMv; }

const char *setCal(uint16_t emptyMv, uint16_t fullMv) {
  if (!measured()) return "keine Akku-Messung eingerichtet";
  const char *err = BatCurve::calError(settings.batType, emptyMv, fullMv);
  if (err) return err;
  settings.batEmptyMv = emptyMv;
  settings.batFullMv = fullMv;
  Store::saveBatCal();
  Serial.printf("[PWR]  Akku-Kalibrierung: 0 %% = %u mV%s, 100 %% = %u mV%s -> jetzt %u mV = %u%%\n",
                emptyMv ? emptyMv : BatCurve::defaultEmptyMv(settings.batType), emptyMv ? "" : " (Kennlinie)",
                fullMv ? fullMv : BatCurve::defaultFullMv(settings.batType), fullMv ? "" : " (Kennlinie)", mv(), pct());
  return nullptr;
}

const char *calibrateNow(bool full) {
  if (!present()) return "kein Akku gemessen";
  return full ? setCal(settings.batEmptyMv, mv()) : setCal(mv(), settings.batFullMv);
}

bool low() { return present() && pct() <= BAT_LOW_PCT; }

void loop() {
  if (!measured() || millis() - lastSample < 1000) return;
  lastSample = millis();
  float s = sampleMv();
  // follow quickly when a battery is (un)plugged, otherwise smooth
  if (fabsf(s - filt) > 400) filt = s;
  else filt = filt * 0.85f + s * 0.15f;
  chargeSample();
  bool battery = present() && settings.batType == 0 && charge() == ON_BATTERY;
  batSecs = !battery ? 0 : batSecs < 255 ? batSecs + 1 : 255;
#ifdef BAT_OFF_MV
  emptySecs = !(battery && filt < BAT_OFF_MV) ? 0 : emptySecs < 255 ? emptySecs + 1 : 255;
#endif
}

}  // namespace Power
