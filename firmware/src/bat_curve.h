#pragma once
#include <stddef.h>
#include <stdint.h>

// Battery voltage -> percent, including the user's calibration. Plain C++
// without Arduino so that tests/bat_curve_test.cpp runs it on the PC.

namespace BatCurve {

// 1S LiPo, voltage -> percent (light load)
static const uint16_t LIPO_MV[]  = {3300, 3500, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800,
                                    3820, 3840, 3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200};
static const uint8_t  LIPO_PCT[] = {0, 2, 5, 10, 15, 20, 25, 30, 35, 40,
                                    45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
// NiCd / NiMH, 4 cells in series (1.0 V empty ... 1.35 V full per cell). The
// discharge curve is very flat, so the percentage is only a rough guide.
static const uint16_t NICD_MV[]  = {4000, 4400, 4600, 4720, 4800, 4880, 5000, 5200, 5400};
static const uint8_t  NICD_PCT[] = {0, 5, 12, 25, 40, 55, 75, 90, 100};

// How far a calibration point may lie from the curve's own end, and how far
// the two points must be apart. Keeps a slip of the finger (0 % set on a full
// cell) from producing a useless display.
static const uint16_t CAL_RANGE_MV = 500;
static const uint16_t CAL_MIN_SPAN_MV = 300;

inline uint8_t lookup(const uint16_t *mvs, const uint8_t *pcts, size_t n, uint16_t v) {
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

// The voltages the curve of this battery type calls 0 % and 100 %.
inline uint16_t defaultEmptyMv(uint8_t type) { return type == 1 ? NICD_MV[0] : LIPO_MV[0]; }
inline uint16_t defaultFullMv(uint8_t type) {
  return type == 1 ? NICD_MV[sizeof(NICD_MV) / sizeof(NICD_MV[0]) - 1] : LIPO_MV[sizeof(LIPO_MV) / sizeof(LIPO_MV[0]) - 1];
}

// A calibration point of 0 means "not calibrated, use the curve's own end".
// Returns nullptr or what is wrong with the pair.
inline const char *calError(uint8_t type, uint16_t emptyMv, uint16_t fullMv) {
  int e0 = defaultEmptyMv(type), f0 = defaultFullMv(type);
  int e = emptyMv ? emptyMv : e0, f = fullMv ? fullMv : f0;
  if (e < e0 - CAL_RANGE_MV || e > e0 + CAL_RANGE_MV) return "0-%-Punkt passt nicht zu diesem Akkutyp";
  if (f < f0 - CAL_RANGE_MV || f > f0 + CAL_RANGE_MV) return "100-%-Punkt passt nicht zu diesem Akkutyp";
  if (f - e < CAL_MIN_SPAN_MV) return "0 % und 100 % liegen zu dicht beieinander";
  return nullptr;
}

// Percent for a measured voltage. The calibration stretches the measured range
// emptyMv..fullMv onto the curve's own range, so the shape of the discharge
// curve in between is kept. A pair calError() rejects is ignored.
inline uint8_t percent(uint8_t type, uint16_t mv, uint16_t emptyMv, uint16_t fullMv) {
  const uint16_t *mvs = type == 1 ? NICD_MV : LIPO_MV;
  const uint8_t *pcts = type == 1 ? NICD_PCT : LIPO_PCT;
  size_t n = type == 1 ? sizeof(NICD_MV) / sizeof(NICD_MV[0]) : sizeof(LIPO_MV) / sizeof(LIPO_MV[0]);
  int32_t e0 = mvs[0], f0 = mvs[n - 1];
  if (calError(type, emptyMv, fullMv)) emptyMv = fullMv = 0;
  int32_t e = emptyMv ? emptyMv : e0, f = fullMv ? fullMv : f0;
  if (mv <= e) return 0;
  if (mv >= f) return 100;
  int32_t v = e0 + ((int32_t)mv - e) * (f0 - e0) / (f - e);
  return lookup(mvs, pcts, n, (uint16_t)v);
}

}  // namespace BatCurve
