#include <cstdio>
#include <cstring>

// the shipped header, not a copy
#include "../firmware/src/bat_curve.h"

static int fails = 0;

static void eq(const char *what, int got, int want) {
  if (got == want) return;
  printf("FAIL %s: got %d, want %d\n", what, got, want);
  fails++;
}

static void ok(const char *what, bool cond) {
  if (cond) return;
  printf("FAIL %s\n", what);
  fails++;
}

int main() {
  using namespace BatCurve;

  // ---- without calibration: the curve as it always was
  eq("lipo 3300", percent(0, 3300, 0, 0), 0);
  eq("lipo 3000", percent(0, 3000, 0, 0), 0);
  eq("lipo 3840", percent(0, 3840, 0, 0), 50);
  eq("lipo 4050", percent(0, 4050, 0, 0), 83);     // the reading of a full cell on the Waveshare board
  eq("lipo 4200", percent(0, 4200, 0, 0), 100);
  eq("lipo 4300", percent(0, 4300, 0, 0), 100);
  eq("nicd 4000", percent(1, 4000, 0, 0), 0);
  eq("nicd 4800", percent(1, 4800, 0, 0), 40);
  eq("nicd 5400", percent(1, 5400, 0, 0), 100);

  // ---- 100 % calibrated to what the board reads on a full cell
  eq("full=4050 at 4050", percent(0, 4050, 0, 4050), 100);
  eq("full=4050 at 4100", percent(0, 4100, 0, 4050), 100);
  eq("full=4050 at 4049", percent(0, 4049, 0, 4050), 100);   // rounds up, never above
  eq("full=4050 at 3300", percent(0, 3300, 0, 4050), 0);
  ok("full=4050 at 4000 below 100", percent(0, 4000, 0, 4050) < 100);
  ok("full=4050 raises the middle", percent(0, 3800, 0, 4050) > percent(0, 3800, 0, 0));

  // ---- 0 % calibrated
  eq("empty=3450 at 3450", percent(0, 3450, 3450, 0), 0);
  eq("empty=3450 at 3400", percent(0, 3400, 3450, 0), 0);
  eq("empty=3450 at 4200", percent(0, 4200, 3450, 0), 100);
  ok("empty=3450 lowers the middle", percent(0, 3800, 3450, 0) < percent(0, 3800, 0, 0));

  // ---- both points, and the result never runs backwards
  eq("both at empty", percent(0, 3400, 3400, 4050), 0);
  eq("both at full", percent(0, 4050, 3400, 4050), 100);
  for (int type = 0; type < 2; type++) {
    uint16_t e = type ? 4100 : 3400, f = type ? 5300 : 4050;
    int last = 0;
    for (int mv = 2500; mv <= 6000; mv++) {
      int p = percent(type, mv, e, f);
      if (p < last || p > 100) { printf("FAIL monotonic type %d at %d mV: %d after %d\n", type, mv, p, last); fails++; break; }
      last = p;
    }
    eq("reaches 100", last, 100);
  }

  // ---- validation
  ok("no calibration is valid", calError(0, 0, 0) == nullptr);
  ok("full 4050 valid", calError(0, 0, 4050) == nullptr);
  ok("empty 3000 valid", calError(0, 3000, 0) == nullptr);
  ok("full 3600 rejected", calError(0, 0, 3600) != nullptr);
  ok("full 4800 rejected", calError(0, 0, 4800) != nullptr);
  ok("empty 2700 rejected", calError(0, 2700, 0) != nullptr);
  ok("empty 3900 rejected", calError(0, 3900, 0) != nullptr);
  ok("too close rejected", calError(0, 3600, 3800) != nullptr);
  ok("nicd 4100/5300 valid", calError(1, 4100, 5300) == nullptr);
  ok("lipo values on nicd rejected", calError(1, 3400, 4050) != nullptr);
  // a rejected pair (e.g. left over in the flash after the type changed) falls back to the plain curve
  eq("invalid pair ignored", percent(1, 4800, 3400, 4050), percent(1, 4800, 0, 0));

  if (fails) { printf("%d FAILED\n", fails); return 1; }
  printf("bat_curve: all tests passed\n");
  return 0;
}
