#include "frame_probe.hpp"
#include "log.hpp"

#if ATC_PROFILE

#include <esp_heap_caps.h>

namespace probe {

Acc g[kSlotCount] = {};
uint32_t gFrames = 0;

static constexpr unsigned long kReportMs = 2000;

void report(unsigned long nowMs) {
  static unsigned long last = 0;
  static bool primed = false;
  if (!primed) {
    // Skip the first window: it straddles boot, where the first paint and the
    // WiFi connect make every average meaningless.
    primed = true;
    last = nowMs;
    return;
  }
  const unsigned long elapsed = nowMs - last;
  if (elapsed < kReportMs) {
    return;
  }
  last = nowMs;

  static const char *const kName[kSlotCount] = {"clear", "sweep", "tasks", "rast",
                                                "lvgl",  "pres",  "vsync"};

  // avg/max microseconds per slot. n is per-slot because the radar frame runs
  // at a different cadence from lv_timer_handler.
  // g[kLvgl].n is the loop() iteration count: lv_timer_handler runs once per
  // pass. Comparing it against gFrames (actual radar paints) is what tells you
  // whether the loop has headroom to step LVGL's animations smoothly, or is
  // saturated by painting.
  Log.printf("[frame] radar=%lu loop=%lu", static_cast<unsigned long>(gFrames),
             static_cast<unsigned long>(g[kLvgl].n));
  // avg/max xN. N is printed because without it these averages cannot be
  // reconciled against the window: a stage that runs several times per
  // lv_timer_handler call (rast, once per draw pass) contributes avg*N, not
  // avg, and the difference is where unattributed time hides.
  for (int i = 0; i < kSlotCount; ++i) {
    const uint32_t avg = g[i].n ? g[i].sum / g[i].n : 0;
    Log.printf(" %s=%lu/%lu x%lu", kName[i], static_cast<unsigned long>(avg),
                  static_cast<unsigned long>(g[i].max),
                  static_cast<unsigned long>(g[i].n));
    g[i] = {0, 0, 0};
  }
  const float fps = elapsed ? (1000.0f * static_cast<float>(gFrames) /
                               static_cast<float>(elapsed))
                            : 0.0f;
  Log.printf(" fps=%.1f psram=%uk\n", fps,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  gFrames = 0;
}

}  // namespace probe

#endif  // ATC_PROFILE
