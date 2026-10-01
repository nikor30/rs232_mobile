#include "power.h"
#include "config.h"
#include "settings.h"

// Battery monitoring only (voltage divider to an ADC pin, web UI -> Ports).
// Over-discharge protection is up to the battery/charger hardware.

namespace Power {

static float filt = 0;
static uint32_t lastSample = 0;

// 1S LiPo, voltage -> percent (light load)
static const uint16_t LIPO_MV[]  = {3300, 3500, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800,
                                    3820, 3840, 3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200};
static const uint8_t  LIPO_PCT[] = {0, 2, 5, 10, 15, 20, 25, 30, 35, 40,
                                    45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
// NiCd / NiMH, 4 cells in series (1.0 V empty ... 1.35 V full per cell). The
// discharge curve is very flat, so the percentage is only a rough guide.
static const uint16_t NICD_MV[]  = {4000, 4400, 4600, 4720, 4800, 4880, 5000, 5200, 5400};
static const uint8_t  NICD_PCT[] = {0, 5, 12, 25, 40, 55, 75, 90, 100};

bool measured() { return settings.batPin >= 0; }
bool hasLbo() { return settings.batLbo >= 0; }

// Chargers like the Adafruit PowerBoost pull LBO low (open drain) below ~3.2 V.
// That is a hardware threshold from the charger itself and stays valid even
// without a voltage divider, so it counts as "low" on its own.
bool lowSignal() { return hasLbo() && digitalRead(settings.batLbo) == LOW; }
const char *typeName() { return settings.batType == 1 ? "NiCd/NiMH 4 Zellen" : "LiPo 1S"; }

static uint16_t sampleMv() {
  if (!measured()) return 0;
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(settings.batPin);
  return (uint16_t)((acc / 8) * (settings.batDiv / 10.0f) * BAT_CAL);
}

void begin() {
  if (hasLbo()) pinMode(settings.batLbo, INPUT_PULLUP);
  if (!measured()) return;
  analogSetPinAttenuation(settings.batPin, ADC_11db);
  filt = sampleMv();
}

bool present() { return measured() && filt >= (settings.batType == 1 ? 3000 : 2500); }
uint16_t mv() { return (uint16_t)filt; }

static uint8_t lookup(const uint16_t *mvs, const uint8_t *pcts, size_t n, uint16_t v) {
  if (v <= mvs[0]) return 0;
  if (v >= mvs[n - 1]) return 100;
  for (size_t i = 1; i < n; i++) {
    if (v < mvs[i]) {
      float f = float(v - mvs[i - 1]) / float(mvs[i] - mvs[i - 1]);
      return pcts[i - 1] + (uint8_t)(f * (pcts[i] - pcts[i - 1]) + 0.5f);
    }
  }
  return 100;
}

uint8_t pct() {
  if (!present()) return 0;
  if (settings.batType == 1) return lookup(NICD_MV, NICD_PCT, sizeof(NICD_MV) / sizeof(NICD_MV[0]), mv());
  return lookup(LIPO_MV, LIPO_PCT, sizeof(LIPO_MV) / sizeof(LIPO_MV[0]), mv());
}

bool low() { return lowSignal() || (present() && pct() <= BAT_LOW_PCT); }

void loop() {
  if (!measured() || millis() - lastSample < 1000) return;
  lastSample = millis();
  float s = sampleMv();
  // follow quickly when a battery is (un)plugged, otherwise smooth
  if (fabsf(s - filt) > 400) filt = s;
  else filt = filt * 0.85f + s * 0.15f;
}

}  // namespace Power
