#pragma once

#include <Arduino.h>

/**
 * Frame-time probes for the render path.
 *
 * Serial only, deliberately: an on-screen overlay would be a label whose text
 * changes every 2 s, invalidating a region of the info panel and perturbing the
 * very thing being measured.
 *
 * MEASUREMENT TRAP -- read before trusting any number here. With
 * LV_USE_OS LV_OS_NONE the lv_draw_*() calls only build a linked list of draw
 * tasks; the rasterisation all happens inside lv_canvas_finish_layer(). So a
 * Scope around a draw stage measures task *creation* (essentially the PSRAM
 * malloc cost), and every stage's raster time lands together in kRaster.
 * Attributing raster time per stage needs a temporary isolation build that runs
 * init_layer / draw / finish_layer once per stage -- compile with
 * -DATC_ISOLATE_STAGES=1, see RadarView::redraw().
 *
 * ---------------------------------------------------------------------------
 * BASELINE, captured 2026-07-26 on the 480x480 canvas at 6-7 aircraft, before
 * any render work. Averages in microseconds.
 *
 *   clear (lv_canvas_fill_bg)          27 000
 *   background + legend raster         45 000   (isolation build)
 *   trails + symbols + labels raster   34 000   (isolation build)
 *   lv_timer_handler                  105 000   (incl. vsync below)
 *     of which vsync (idle)            18 000
 *     => canvas -> framebuffer blit    ~87 000
 *   task creation                       8 000
 *   present (draw_bitmap)                  30
 *
 *   achieved 4.6 fps -- NOT the 20 fps that kRadarFrameMs = 50 implies. The
 *   frame budget is spent, not throttled: ~106 ms rendering the canvas plus
 *   ~87 ms blitting it to the framebuffer. Everything here is PSRAM-bandwidth
 *   bound; the canvas is paid for twice, once to draw and once to blit.
 *
 * Contended PSRAM bandwidth, measured at boot with the LCD scanning out
 * (see benchPsram() in main.cpp): memset 15.2 MB/s, memcpy 9.4 MB/s. Those two
 * figures explain every number above -- lv_canvas_fill_bg's 27 ms *is* memset
 * speed for 460 KB, and the blit takes LVGL's per-row lv_memcpy fast path
 * already. There was no micro-optimisation to find, only fewer pixels to touch.
 *
 * AFTER the direct-draw refactor (no canvas, background cache, dirty rects
 * merged to 4 regions): 11.6 paints/s, i.e. 2.5x. clear gone, ~4 draw passes
 * at ~14 ms. But lv_timer_handler still averaged ~80-160 ms because the loop
 * was running at only ~13 iterations/s -- saturated. Painting every frame
 * starved LVGL's animation stepping, which is what made the info panel's
 * marquee jitter, and re-ran the label anchor solver ten-plus times per pixel
 * of real movement, which is what made labels flicker.
 *
 * AFTER additionally skipping frames where nothing moved a whole pixel:
 *   loop           ~200 iterations/s   (was ~13)
 *   lv_timer_handler  ~3 ms avg        (was ~80-160)
 *   radar paints      6.5 /s           (was 11.6, and 4.6 originally)
 *   tasks (solver)    ~3 ms, painting frames only
 *   rast              ~12 ms per pass
 *
 * The paint rate went *down* and that is the improvement: 6.5 paints/s is far
 * more than 1 px/s of aircraft movement needs, and the freed CPU is what makes
 * animations smooth. Judge this system by `loop`, not by `fps`.
 *
 * FINAL, with the 448 px card layout and the subset fonts:
 *   loop           150-250 /s
 *   lv_timer_handler  2-7 ms avg
 *   radar paints      ~6 /s
 *   tasks (solver)    ~4.7 ms
 *   rast              ~15 ms per pass, ~4 passes per paint
 *   flash             1 390 354 B (was 1 512 342 with Montserrat)
 *
 * Two traps found along the way, both worth remembering because neither shows
 * up as a slow function -- they show up as a collapsed `loop` rate:
 *   1. A transparent or rounded container breaks LVGL's cover check, so a
 *      repaint inside it walks all the way up to the screen background. The
 *      info panel root must stay opaque, square and border-free, exactly like
 *      the settings wizard root already documents.
 *   2. LV_LABEL_LONG_SCROLL_CIRCULAR over LV_SIZE_CONTENT blocks re-runs the
 *      flex layout on every animation step. Eight of them cost ~80 ms per
 *      lv_timer_handler call and took `loop` from ~200/s to ~12/s.
 * ---------------------------------------------------------------------------
 */

#ifndef ATC_PROFILE
#define ATC_PROFILE 1
#endif

namespace probe {

enum Slot : uint8_t {
  kClear = 0,  // canvas clear (fill_bg, later the background restore)
  kSweep,      // sweep task creation
  kTasks,      // trails + symbols + labels task creation
  kRaster,     // lv_canvas_finish_layer -- where the drawing actually happens
  kLvgl,       // lv_timer_handler: layout, widget redraw, canvas blit
  kPresent,    // esp_lcd_panel_draw_bitmap (cache write-back + arm the flip)
  kVsync,      // waitVsync -- idle time, NOT cost; kept separate so it does not
               // inflate the apparent frame cost
  kSlotCount
};

#if ATC_PROFILE

struct Acc {
  uint32_t sum;
  uint32_t max;
  uint32_t n;
};

extern Acc g[kSlotCount];
extern uint32_t gFrames;

inline void add(Slot s, uint32_t us) {
  Acc &a = g[s];
  a.sum += us;
  a.n++;
  if (us > a.max) a.max = us;
}

/** Scoped timer: `probe::Scope _(probe::kClear);` */
struct Scope {
  Slot slot;
  uint32_t t0;
  explicit Scope(Slot s) : slot(s), t0(micros()) {}
  ~Scope() { add(slot, micros() - t0); }
};

inline void countFrame() { gFrames++; }

/** Prints and resets every 2 s. Call once per loop(); it self-throttles. */
void report(unsigned long nowMs);

#else

struct Scope {
  explicit Scope(Slot) {}
};
inline void add(Slot, uint32_t) {}
inline void countFrame() {}
inline void report(unsigned long) {}

#endif

}  // namespace probe
