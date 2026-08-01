#include "radar_view.hpp"
#include "log.hpp"

#include "frame_probe.hpp"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

/** Geometry of the altitude legend.
 *
 *  drawLegend() paints the backdrop and seedStaticBlockers() reserves the same
 *  box so no aircraft label is placed underneath it (the legend draws last and
 *  would paint over one). Those two rects used to be written out separately,
 *  held together by nothing but a comment. One struct, two readers. */
struct LegendGeom {
  int32_t barX, barW;      // gradient bar
  int32_t barTop, barBot;  // gradient extent
  int32_t x1, y1, x2, y2;  // backdrop == the label blocker
};

constexpr LegendGeom kLegend = [] {
  constexpr int32_t barW = 8;
  constexpr int32_t top = 46;
  constexpr int32_t bot = 206;
  return LegendGeom{RadarView::kSize - 16, barW,
                    top,                   bot,
                    RadarView::kSize - 48, top - 20,
                    RadarView::kSize - 2,  bot + 6};
}();

constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;

// The three intensities the beam is made of, all 0..255.
//   kBeamPeak  the wedge, at its brightest. Low on purpose: it passes over
//              range rings and trails and must not compete with them.
//   kEdgeOpa   the leading slice, which is what reads as "a beam" rather than
//              as the bright end of a gradient.
//   kFlarePeak how far a blip is pulled towards the beam colour as it is
//              crossed. Half is about the limit before the altitude colour --
//              the actual information -- stops being readable.
constexpr float kBeamPeak = 92.0f;
constexpr uint32_t kEdgeOpa = 170;
constexpr float kFlarePeak = 140.0f;
// Below this a ray is treated as exactly horizontal, which is a separate case
// in sectorRowSpan() because the row constraint stops being solvable for x.
constexpr float kRayEps = 1e-6f;

/** Blend `n` RGB565 pixels of `src` towards a fixed colour and write to `dst`.
 *
 *  The colour and its alpha are constant for a whole angular slice of the beam,
 *  so the caller pre-multiplies them once and this loop is three multiplies and
 *  a shift per pixel. Alpha is on a 0..256 scale, not 0..255, so that `inv`
 *  and `a` sum to exactly 256 and the shift is exact at both ends -- on a 5-bit
 *  channel a rounding error is a visible band. */
inline void blendSpan(uint16_t *dst, const uint16_t *src, int32_t n, uint32_t fr,
                      uint32_t fg, uint32_t fb, uint32_t inv) {
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t s = src[i];
    const uint32_t r = ((((s >> 11) & 0x1F) * inv) + fr) >> 8;
    const uint32_t g = ((((s >> 5) & 0x3F) * inv) + fg) >> 8;
    const uint32_t b = (((s & 0x1F) * inv) + fb) >> 8;
    dst[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }
}

/** Wrap to [0, 360). */
inline float wrapDeg(float deg) {
  deg = fmodf(deg, 360.0f);
  return deg < 0.0f ? deg + 360.0f : deg;
}

/** Mix `fg` into `bg` by `mix`/255. lv_color_mix() is not public API in LVGL 9,
 *  and this is the whole of it. */
inline lv_color_t mixColor(lv_color_t fg, lv_color_t bg, uint8_t mix) {
  const uint32_t inv = 255u - mix;
  return lv_color_make(
      static_cast<uint8_t>((fg.red * mix + bg.red * inv) / 255u),
      static_cast<uint8_t>((fg.green * mix + bg.green * inv) / 255u),
      static_cast<uint8_t>((fg.blue * mix + bg.blue * inv) / 255u));
}

// Draw a text label straight onto the canvas layer. LVGL only duplicates the
// text when text_local is set, so we set it and let LVGL copy — that keeps a
// caller-side stack buffer safe to reuse for the next label.
void drawText(lv_layer_t *layer, int32_t x, int32_t y, int32_t w,
              const char *text, lv_color_t color, lv_opa_t opa,
              const lv_font_t *font, lv_text_align_t align) {
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = opa;
  dsc.font = font;
  dsc.text = text;
  dsc.text_local = 1;
  dsc.text_length = strlen(text);
  dsc.align = align;
  lv_area_t area = {x, y, x + w, y + static_cast<int32_t>(font->line_height) + 2};
  lv_draw_label(layer, &dsc, &area);
}

}  // namespace

lv_color_t altitudeColor(int altitudeFt) {
  if (altitudeFt == kAltitudeUnknown) {
    return theme::c(theme::kGround);
  }
  const float n = altitudeNorm(altitudeFt);
  // Monotonic hue sweep, green-cyan (low) → magenta (high).
  const uint16_t hue = static_cast<uint16_t>(lroundf(150.0f + n * 170.0f));
  return lv_color_hsv_to_rgb(hue, 82, 98);
}

bool RadarView::create(lv_obj_t *parent, float rangeNm) {
  setRangeNm(rangeNm);

  const uint32_t stride = lv_draw_buf_width_to_stride(kSize, LV_COLOR_FORMAT_RGB565);
  const size_t bufBytes = static_cast<size_t>(stride) * static_cast<size_t>(kSize);
  bgCache_ = static_cast<uint8_t *>(
      heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (bgCache_ == nullptr) {
    Log.println("[radar] Failed to allocate background cache in PSRAM");
    return false;
  }

  bgImg_.header.magic = LV_IMAGE_HEADER_MAGIC;
  bgImg_.header.cf = LV_COLOR_FORMAT_RGB565;
  bgImg_.header.flags = 0;
  bgImg_.header.w = kSize;
  bgImg_.header.h = kSize;
  bgImg_.header.stride = stride;
  bgImg_.data = bgCache_;
  bgImg_.data_size = bufBytes;

  // A plain object, not a canvas: we paint it ourselves in LV_EVENT_DRAW_MAIN,
  // straight into the display's layer. That removes both the per-frame canvas
  // clear and the canvas -> framebuffer blit, which together were ~106 ms of a
  // ~200 ms frame. remove_style_all also drops the theme's background, so LVGL
  // does not fill the rect before we paint over it.
  obj_ = lv_obj_create(parent);
  lv_obj_remove_style_all(obj_);
  lv_obj_set_size(obj_, kSize, kSize);
  lv_obj_set_pos(obj_, theme::layout::kRadarX, theme::layout::kRadarY);
  lv_obj_clear_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj_, onClicked, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(obj_, onDraw, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_event_cb(obj_, onCoverCheck, LV_EVENT_COVER_CHECK, this);

  renderBackgroundCache();
  redraw();
  return true;
}

void RadarView::setRangeNm(float rangeNm) {
  const float clamped = rangeNm > 1.0f ? rangeNm : 1.0f;
  if (clamped == rangeNm_ && bgValid_) {
    return;  // a save that did not touch the radius costs nothing
  }
  rangeNm_ = clamped;
  pxPerNm_ = static_cast<float>(kSize / 2 - 8) / rangeNm_;
  // Ring radii and their nm captions are baked into the cache.
  bgValid_ = false;
}

void RadarView::renderBackgroundCache() {
  if (bgCache_ == nullptr || obj_ == nullptr) {
    return;
  }
  // lv_canvas is the only public way to rasterise into a buffer we own. This
  // one is transient and hidden: lv_canvas_finish_layer() drives the draw
  // dispatch from the layer's own task list regardless of visibility, and its
  // trailing lv_obj_invalidate() is a no-op while hidden, so nothing of this
  // ever reaches the screen.
  lv_obj_t *scratch = lv_canvas_create(lv_obj_get_parent(obj_));
  lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_buffer(scratch, bgCache_, kSize, kSize, LV_COLOR_FORMAT_RGB565);
  // Fill with the *page* colour, then lay a rounded rect of the radar colour on
  // top. The corners end up painted with the page colour, so the view reads as
  // a rounded card while remaining a fully opaque rectangle -- which is what
  // keeps the LV_EVENT_COVER_CHECK claim honest and stops LVGL filling the
  // screen background underneath every frame.
  lv_canvas_fill_bg(scratch, theme::c(theme::kBgApp), LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(scratch, &layer);
  ox_ = 0;  // the cache is in local coordinates
  oy_ = 0;

  lv_draw_rect_dsc_t card;
  lv_draw_rect_dsc_init(&card);
  card.bg_color = theme::c(theme::kRadarBg);
  card.bg_opa = LV_OPA_COVER;
  card.radius = theme::kRadLg;
  card.border_color = theme::c(theme::kBorder);
  card.border_width = 1;
  card.border_opa = LV_OPA_60;
  lv_area_t full = {0, 0, kSize - 1, kSize - 1};
  lv_draw_rect(&layer, &card, &full);

  drawBackground(&layer);
  lv_canvas_finish_layer(scratch, &layer);

  lv_obj_delete(scratch);
  bgValid_ = true;

  // Re-encode after every re-render: the range change that invalidated the
  // cache changed the ring radii and their captions, so the old runs describe
  // the wrong picture. Failure here is not fatal -- bgCache_ is still valid and
  // every read falls back to it.
  buildBackgroundRle();
}

void RadarView::setSnapshot(const Aircraft *list, size_t count,
                            const char *selectedHex) {
  snap_ = list;
  snapCount_ = count;
  if (selectedHex != nullptr) {
    strncpy(selectedHex_, selectedHex, sizeof(selectedHex_) - 1);
    selectedHex_[sizeof(selectedHex_) - 1] = '\0';
  } else {
    selectedHex_[0] = '\0';
  }
}

bool RadarView::consumePendingClick(float *eastNm, float *northNm) {
  if (!pendingClick_) {
    return false;
  }
  pendingClick_ = false;
  if (eastNm != nullptr) *eastNm = pendingEast_;
  if (northNm != nullptr) *northNm = pendingNorth_;
  return true;
}

void RadarView::nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const {
  *x = static_cast<int32_t>(lroundf(kSize * 0.5f + eastNm * pxPerNm_));
  *y = static_cast<int32_t>(lroundf(kSize * 0.5f - northNm * pxPerNm_));
}

void RadarView::pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const {
  *eastNm = (static_cast<float>(x) - kSize * 0.5f) / pxPerNm_;
  *northNm = (kSize * 0.5f - static_cast<float>(y)) / pxPerNm_;
}

lv_color_t RadarView::colorForAircraft(const Aircraft &ac) const {
  return altitudeColor(ac.altitudeFt);
}

lv_opa_t RadarView::trailOpacity(const Aircraft &ac, uint8_t ageIndex,
                                 uint8_t count, bool selected) const {
  // Lower altitude → more distinct trails.
  const float altFactor = 1.0f - altitudeNorm(ac.altitudeFt);
  const float base = 40.0f + altFactor * 180.0f;
  const float ageFade =
      count <= 1 ? 1.0f
                 : static_cast<float>(ageIndex + 1) / static_cast<float>(count);
  // The crowding fade is what the selection needs to stand out against, so it
  // is exempt from it.
  const float crowd = selected ? 1.0f : trailScale_;
  return static_cast<lv_opa_t>(lroundf(base * ageFade * crowd));
}

// Trails are the biggest source of crossing lines on a busy screen, and each
// segment is a draw task. Shortening them as the count climbs is both a
// legibility and a throughput win — it pays for the symbol casing below.
uint8_t RadarView::trailBudget(bool selected) const {
  if (selected) return kTrailLen;
  if (orderCount_ <= 8) return kTrailLen;
  if (orderCount_ <= 16) return 8;
  if (orderCount_ <= 24) return 5;
  return 3;
}

// ===== Sweep beam ==========================================================

float RadarView::sweepAngleAt(uint32_t nowMs) {
  return wrapDeg(360.0f * static_cast<float>(nowMs % kSweepPeriodMs) /
                 static_cast<float>(kSweepPeriodMs));
}

float RadarView::degreesBehind(float bearingDeg, float headDeg) {
  return wrapDeg(headDeg - bearingDeg);
}

lv_opa_t RadarView::sweepOpacity(float behindDeg) {
  if (behindDeg < 0.0f || behindDeg > kSweepTailDeg) {
    return 0;
  }
  const float t = 1.0f - behindDeg / kSweepTailDeg;  // 1 at the leading edge
  // Quadratic rather than linear: a linear ramp reads as a grey triangle, the
  // squared one as light decaying, which is what a phosphor tube actually does.
  return static_cast<lv_opa_t>(lroundf(kBeamPeak * t * t));
}

bool RadarView::sectorRowSpan(float d0x, float d0y, float d1x, float d1y,
                              int32_t y, int32_t *xlo, int32_t *xhi) {
  constexpr float kCentre = kSize * 0.5f;
  const float dy = static_cast<float>(y) + 0.5f - kCentre;
  const float inside =
      static_cast<float>(kSweepRadius) * static_cast<float>(kSweepRadius) - dy * dy;
  if (inside <= 0.0f) {
    return false;  // the row misses the disc entirely
  }
  const float xr = sqrtf(inside);
  float lo = -xr;
  float hi = xr;

  // Bearings run clockwise from north and screen y grows downwards, so the
  // direction of bearing b is (sin b, -cos b) and the cross product
  // ux*vy - uy*vx is positive exactly when v is clockwise of u. A sector of at
  // most 180 deg is then the intersection of two half-planes: clockwise of the
  // trailing ray, and counter-clockwise of the leading one. Each is linear in
  // x once the row fixes y, so each contributes one bound.
  //
  //   clockwise of d0:            d0x*dy - d0y*x >= 0  ->  d0y*x <= d0x*dy
  if (d0y > kRayEps) {
    const float bound = d0x * dy / d0y;
    if (bound < hi) hi = bound;
  } else if (d0y < -kRayEps) {
    const float bound = d0x * dy / d0y;
    if (bound > lo) lo = bound;
  } else if (d0x * dy < 0.0f) {
    return false;  // ray is horizontal: the row is wholly on the wrong side
  }
  //   counter-clockwise of d1:    x*d1y - dy*d1x >= 0  ->  x*d1y >= dy*d1x
  if (d1y > kRayEps) {
    const float bound = dy * d1x / d1y;
    if (bound > lo) lo = bound;
  } else if (d1y < -kRayEps) {
    const float bound = dy * d1x / d1y;
    if (bound < hi) hi = bound;
  } else if (-dy * d1x < 0.0f) {
    return false;
  }

  if (lo > hi) {
    return false;
  }
  // lo and hi are continuous x offsets from the centre, and the row was solved
  // for the centre of the row, so the pixel to compare is the one whose centre
  // lies in the span: pixel x covers x+0.5. Dropping that half pixel biases
  // every span one column clockwise, which is a whole pixel of smear at the
  // rim and enough to leave the odd unpainted pixel behind the beam.
  *xlo = static_cast<int32_t>(ceilf(kCentre + lo - 0.5f));
  *xhi = static_cast<int32_t>(floorf(kCentre + hi - 0.5f));
  if (*xlo < 0) *xlo = 0;
  if (*xhi > kSize - 1) *xhi = kSize - 1;
  return *xlo <= *xhi;
}

void RadarView::buildSweepRegions(float fromDeg, float toDeg) {
  sweepRegionCount_ = 0;
  if (!kSweepEnabled) {
    return;
  }

  // Everything between where the beam was painted and where it is now differs
  // from the buffer, and so does the whole tail behind both -- the tail's
  // opacity is a function of distance from the head, so moving the head
  // rewrites all of it. One wedge covers both.
  const float span = wrapDeg(toDeg - fromDeg) + kSweepTailDeg;
  if (span >= 170.0f) {
    // Past this the sector stops being convex and sectorRowSpan()'s
    // single-interval assumption fails. Only reachable after a long stall (the
    // settings wizard, a slow fetch), where a full repaint is right anyway.
    fullRepaint_ = true;
    return;
  }
  const float b1 = toDeg;
  const float b0 = wrapDeg(toDeg - span);
  const float d0x = sinf(b0 * kDeg2Rad), d0y = -cosf(b0 * kDeg2Rad);
  const float d1x = sinf(b1 * kDeg2Rad), d1y = -cosf(b1 * kDeg2Rad);

  // Grouped into kSweepBands contiguous bands of roughly equal row count. A
  // wedge is a poor fit for its own bounding box -- a thin diagonal one is
  // worst, at four or five times its area -- and banding by row is the
  // subdivision that actually helps. Banding by angle does not: it turns one
  // bad sliver into several worse ones.
  //
  // Two scans rather than a cached row table: kSize rows of extents would be
  // ~1.8 KB of stack in a function reached from the Arduino loop task, whose
  // stack is 8 KB in total (see the note in Tracker::mergeSnapshot). The span
  // solve is a dozen flops.
  int32_t firstRow = -1;
  int32_t lastRow = -1;
  for (int32_t y = 0; y < kSize; ++y) {
    int32_t lo = 0, hi = 0;
    if (!sectorRowSpan(d0x, d0y, d1x, d1y, y, &lo, &hi)) {
      continue;
    }
    if (firstRow < 0) firstRow = y;
    lastRow = y;
  }
  if (firstRow < 0) {
    return;
  }

  const int32_t rows = lastRow - firstRow + 1;
  for (size_t band = 0; band < kSweepBands; ++band) {
    const int32_t y0 = firstRow + static_cast<int32_t>(band) * rows /
                                      static_cast<int32_t>(kSweepBands);
    const int32_t y1 = firstRow + static_cast<int32_t>(band + 1) * rows /
                                      static_cast<int32_t>(kSweepBands) - 1;
    int32_t x1 = kSize, x2 = -1;
    for (int32_t y = y0; y <= y1; ++y) {
      int32_t lo = 0, hi = 0;
      if (!sectorRowSpan(d0x, d0y, d1x, d1y, y, &lo, &hi)) continue;
      if (lo < x1) x1 = lo;
      if (hi > x2) x2 = hi;
    }
    if (x2 < x1) {
      continue;
    }
    sweepRegion_[sweepRegionCount_++] = {
        static_cast<int16_t>(x1), static_cast<int16_t>(y0),
        static_cast<int16_t>(x2), static_cast<int16_t>(y1)};
  }
}

namespace {

/** Encode one row. Writes tokens to `out` and returns the count written; with
 *  `out == nullptr` it only counts, which is how the exact allocation size is
 *  known before allocating. The two modes share this one body deliberately --
 *  a measure pass that can disagree with the write pass is a buffer overrun. */
size_t encodeRleRow(const uint16_t *src, int32_t w, uint16_t *out) {
  size_t n = 0;
  int32_t x = 0;
  while (x < w) {
    int32_t run = 1;
    while (x + run < w && src[x + run] == src[x]) ++run;

    if (run >= 3) {
      if (out != nullptr) {
        out[n] = static_cast<uint16_t>(run);
        out[n + 1] = src[x];
      }
      n += 2;
      x += run;
      continue;
    }

    // Gather everything up to the next run of 3+ into one literal block.
    const int32_t start = x;
    while (x < w) {
      int32_t r = 1;
      while (x + r < w && src[x + r] == src[x]) ++r;
      if (r >= 3) break;
      x += r;
    }
    int32_t len = x - start;
    // 0x7FFF is the token's payload limit; rows are 448 px so this never trips
    // in practice, but a silent truncation here would corrupt every row after.
    while (len > 0) {
      const int32_t chunk = len > 0x7FFF ? 0x7FFF : len;
      if (out != nullptr) {
        out[n] = static_cast<uint16_t>(0x8000 | chunk);
        memcpy(&out[n + 1], &src[x - len], static_cast<size_t>(chunk) * 2);
      }
      n += 1 + static_cast<size_t>(chunk);
      len -= chunk;
    }
  }
  return n;
}

}  // namespace

void RadarView::rleSpan(int32_t row, int32_t x1, int32_t x2, uint16_t *dst) const {
  const uint16_t *p = bgRle_ + bgRleRow_[row];
  int32_t x = 0;
  while (x <= x2) {
    const uint16_t tok = *p++;
    const int32_t n = tok & 0x7FFF;
    if (tok & 0x8000) {
      if (x + n > x1) {
        const int32_t s = LV_MAX(x1, x);
        const int32_t e = LV_MIN(x2, x + n - 1);
        memcpy(dst + (s - x1), p + (s - x), static_cast<size_t>(e - s + 1) * 2);
      }
      p += n;
    } else {
      const uint16_t v = *p++;
      if (x + n > x1) {
        const int32_t s = LV_MAX(x1, x);
        const int32_t e = LV_MIN(x2, x + n - 1);
        uint16_t *d = dst + (s - x1);
        for (int32_t i = 0, c = e - s; i <= c; ++i) d[i] = v;
      }
    }
    x += n;
  }
}

bool RadarView::buildBackgroundRle() {
  bgRleValid_ = false;
  heap_caps_free(bgRle_);
  heap_caps_free(bgRleRow_);
  heap_caps_free(bgLine_);
  bgRle_ = nullptr;
  bgRleRow_ = nullptr;
  bgLine_ = nullptr;

  if (bgCache_ == nullptr) {
    return false;
  }
  const size_t srcStride = bgImg_.header.stride;
  auto rowPtr = [&](int32_t y) {
    return reinterpret_cast<const uint16_t *>(bgCache_ +
                                              static_cast<size_t>(y) * srcStride);
  };

  size_t total = 0;
  for (int32_t y = 0; y < kSize; ++y) {
    total += encodeRleRow(rowPtr(y), kSize, nullptr);
  }

  // The point of the exercise is to get this read out of PSRAM, so it is
  // internal SRAM or nothing -- MALLOC_CAP_INTERNAL, never a plain malloc that
  // could quietly satisfy itself from PSRAM and leave us slower than the
  // memcpy we replaced.
  const size_t tokenBytes = total * sizeof(uint16_t);
  bgRle_ = static_cast<uint16_t *>(
      heap_caps_malloc(tokenBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  bgRleRow_ = static_cast<uint32_t *>(
      heap_caps_malloc(kSize * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  bgLine_ = static_cast<uint16_t *>(
      heap_caps_malloc(kSize * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (bgRle_ == nullptr || bgRleRow_ == nullptr || bgLine_ == nullptr) {
    Log.printf("[radar] bg RLE needs %u B internal, unavailable -- using PSRAM cache\n",
               static_cast<unsigned>(tokenBytes));
    heap_caps_free(bgRle_);
    heap_caps_free(bgRleRow_);
    heap_caps_free(bgLine_);
    bgRle_ = nullptr;
    bgRleRow_ = nullptr;
    bgLine_ = nullptr;
    return false;
  }

  size_t at = 0;
  for (int32_t y = 0; y < kSize; ++y) {
    bgRleRow_[y] = static_cast<uint32_t>(at);
    at += encodeRleRow(rowPtr(y), kSize, bgRle_ + at);
  }
  bgRleValid_ = true;

  // Verify the decode reproduces the cache exactly, every row, before anything
  // draws from it. This costs one pass at init and is the only pixel-identity
  // check available on-device; tests/host covers the geometry but never sees
  // this buffer. A mismatch falls back rather than shipping a corrupt frame.
  for (int32_t y = 0; y < kSize; ++y) {
    rleSpan(y, 0, kSize - 1, bgLine_);
    if (memcmp(bgLine_, rowPtr(y), static_cast<size_t>(kSize) * 2) != 0) {
      Log.printf("[radar] bg RLE self-check failed at row %d -- using PSRAM cache\n",
                 static_cast<int>(y));
      bgRleValid_ = false;
      return false;
    }
  }

  Log.printf("[radar] bg RLE %u B internal (was %u B PSRAM, %.1fx smaller)\n",
             static_cast<unsigned>(tokenBytes),
             static_cast<unsigned>(srcStride * kSize),
             static_cast<double>(srcStride * kSize) / static_cast<double>(tokenBytes));
  return true;
}

#if ATC_BG_RLE_AB

void RadarView::abReport(unsigned long nowMs) {
  static unsigned long last = 0;
  if (last == 0) {
    last = nowMs;
    return;
  }
  if (nowMs - last < 10000) {
    return;
  }
  last = nowMs;
  if (abN_[0] == 0 || abN_[1] == 0) {
    return;
  }
  const double psram = static_cast<double>(abSum_[0]) / abN_[0];
  const double rle = static_cast<double>(abSum_[1]) / abN_[1];
  Log.printf("[bgab] psram=%.0fus n=%lu  rle=%.0fus n=%lu  delta=%+.1f%%\n", psram,
             static_cast<unsigned long>(abN_[0]), rle,
             static_cast<unsigned long>(abN_[1]), (rle - psram) / psram * 100.0);
  abSum_[0] = abSum_[1] = 0;
  abN_[0] = abN_[1] = 0;
}

bool RadarView::compositeBackground(lv_layer_t *layer) {
  // Only meaningful once both paths are actually available; before that the
  // RLE arm would silently be the memcpy arm and the comparison would read as
  // a dead heat rather than as "not measured".
  if (bgRle_ == nullptr || bgLine_ == nullptr) {
    return compositeBackgroundImpl(layer);
  }
  if (++abCalls_ % kAbBlock == 0) {
    abArm_ = !abArm_;
  }
  const bool saved = bgRleValid_;
  bgRleValid_ = abArm_;
  const uint32_t t0 = micros();
  const bool r = compositeBackgroundImpl(layer);
  const uint32_t dt = micros() - t0;
  bgRleValid_ = saved;
  abSum_[abArm_ ? 1 : 0] += dt;
  abN_[abArm_ ? 1 : 0]++;
  abReport(millis());
  return r;
}

#else

bool RadarView::compositeBackground(lv_layer_t *layer) {
  return compositeBackgroundImpl(layer);
}

#endif  // ATC_BG_RLE_AB

bool RadarView::compositeBackgroundImpl(lv_layer_t *layer) {
  lv_draw_buf_t *buf = layer->draw_buf;
  if (bgCache_ == nullptr || !bgValid_ || buf == nullptr || buf->data == nullptr ||
      layer->color_format != LV_COLOR_FORMAT_RGB565) {
    return false;  // caller falls back to lv_draw_image
  }

  lv_area_t coords;
  lv_obj_get_coords(obj_, &coords);
  // The clip region, in screen coordinates, cropped to the view.
  const int32_t ax1 = LV_MAX(layer->_clip_area.x1, coords.x1);
  const int32_t ay1 = LV_MAX(layer->_clip_area.y1, coords.y1);
  const int32_t ax2 = LV_MIN(layer->_clip_area.x2, coords.x2);
  const int32_t ay2 = LV_MIN(layer->_clip_area.y2, coords.y2);
  if (ax1 > ax2 || ay1 > ay2) {
    return true;  // nothing of the view is in this pass
  }

  const size_t dstStride = buf->header.stride;
  const size_t srcStride = bgImg_.header.stride;
  uint8_t *const dstBase = buf->data;
  const int32_t bufX = layer->buf_area.x1;
  const int32_t bufY = layer->buf_area.y1;

  // Screen coordinates in, buffer offsets out. buf_area is the whole display in
  // DIRECT render mode, but going through it rather than assuming that keeps
  // this correct if the layer is ever a partial one.
  auto dstAt = [&](int32_t x, int32_t y) -> uint16_t * {
    return reinterpret_cast<uint16_t *>(
               dstBase + static_cast<size_t>(y - bufY) * dstStride) +
           (x - bufX);
  };
  auto srcAt = [&](int32_t x, int32_t y) -> const uint16_t * {
    return reinterpret_cast<const uint16_t *>(
               bgCache_ + static_cast<size_t>(y - oy_) * srcStride) +
           (x - ox_);
  };
  // Background pixels [x1..x2] of screen row y, straight into the layer. Out of
  // the SRAM run list when there is one, otherwise the original PSRAM memcpy.
  auto restore = [&](int32_t x1, int32_t x2, int32_t y) {
    if (bgRleValid_) {
      rleSpan(y - oy_, x1 - ox_, x2 - ox_, dstAt(x1, y));
    } else {
      memcpy(dstAt(x1, y), srcAt(x1, y), static_cast<size_t>(x2 - x1 + 1) * 2);
    }
  };

  // With the beam off this is only the background restore, which is what the
  // whole function was before the beam existed: a row-at-a-time copy out of the
  // cache, clipped to the region LVGL asked for.
  if (!sweepRunning_ || !kSweepEnabled) {
    for (int32_t y = ay1; y <= ay2; ++y) {
      restore(ax1, ax2, y);
    }
    return true;
  }

  // The beam is painted as constant-opacity angular slices, so opacity can vary
  // with angle without an atan2 for every one of ~25 000 pixels. Ray directions
  // are computed once per slice *boundary*, and neighbouring slices index the
  // same entry for the edge they share. Deriving that edge twice -- once as one
  // slice's leading ray, once as the next one's trailing ray -- leaves the two
  // spans disagreeing by a rounding step, and a pixel in the gap is written by
  // neither. Two of them, across a full revolution, was what this cost.
  constexpr int kSlices = 18;
  constexpr float kSliceDeg = kSweepTailDeg / kSlices;
  float rayX[kSlices + 1];
  float rayY[kSlices + 1];
  for (int i = 0; i <= kSlices; ++i) {
    const float b = wrapDeg(sweepDeg_ - static_cast<float>(i) * kSliceDeg);
    rayX[i] = sinf(b * kDeg2Rad);
    rayY[i] = -cosf(b * kDeg2Rad);
  }

  // Pass one: restore the background, skipping the span the beam is about to
  // overwrite anyway. Writing those pixels twice would cost as much again as
  // the blend itself. Ray 0 is the leading edge and ray kSlices the end of the
  // tail, so the wedge is bounded by the same two floats its outermost slices
  // use.
  //
  // The skipped span is deliberately one pixel narrower at each end than the
  // wedge. Pass two rebuilds the same boundary per slice, and while the two
  // agree to within a rounding step, "within a rounding step" is exactly a
  // pixel that neither pass writes -- which on screen is a lit pixel left
  // behind by the beam, forever. Two pixels a row is a rounding error in the
  // frame budget; a stale one is not.
  for (int32_t y = ay1; y <= ay2; ++y) {
    int32_t lo = 0, hi = 0;
    if (!sectorRowSpan(rayX[kSlices], rayY[kSlices], rayX[0], rayY[0], y - oy_, &lo,
                       &hi)) {
      restore(ax1, ax2, y);
      continue;
    }
    const int32_t wlo = LV_MAX(lo + ox_, ax1);
    const int32_t whi = LV_MIN(hi + ox_, ax2);
    if (wlo > whi) {  // the wedge is on this row but not in this region
      restore(ax1, ax2, y);
      continue;
    }
    restore(ax1, wlo, y);
    restore(whi, ax2, y);
  }

  // Pass two: the beam itself, slice by slice.
  probe::Scope _(probe::kSweep);
  for (int s = 0; s < kSlices; ++s) {
    const float behind = (static_cast<float>(s) + 0.5f) * kSliceDeg;
    // Not skipped when this rounds to zero, tempting though that is: pass one
    // left the whole wedge unwritten, so the faintest slice is still the only
    // thing that will put background back under the beam's trailing edge. At
    // alpha zero the blend below is an exact copy, which is precisely what that
    // slice needs.
    const lv_opa_t opa = sweepOpacity(behind);
    // The leading slice is the beam itself: brighter, and a different colour,
    // which is what makes it read as an edge rather than as the top of a ramp.
    const uint16_t col = lv_color_to_u16(
        theme::c(s == 0 ? theme::kSweepEdge : theme::kSweepBeam));
    const uint32_t a = s == 0 ? kEdgeOpa : static_cast<uint32_t>(opa);
    const uint32_t a256 = a + (a >> 7);  // 0..255 -> 0..256, exact at both ends
    const uint32_t inv = 256u - a256;
    const uint32_t fr = ((col >> 11) & 0x1F) * a256;
    const uint32_t fg = ((col >> 5) & 0x3F) * a256;
    const uint32_t fb = (col & 0x1F) * a256;

    for (int32_t y = ay1; y <= ay2; ++y) {
      int32_t lo = 0, hi = 0;
      if (!sectorRowSpan(rayX[s + 1], rayY[s + 1], rayX[s], rayY[s], y - oy_, &lo,
                         &hi)) {
        continue;
      }
      const int32_t x1 = LV_MAX(lo + ox_, ax1);
      const int32_t x2 = LV_MIN(hi + ox_, ax2);
      if (x1 > x2) {
        continue;
      }
      // The blend reads the background per pixel, so the RLE has to be decoded
      // rather than filled straight out. Decoding into bgLine_ first still wins:
      // the span is written once to SRAM and read back from SRAM, where the
      // memcpy path read every one of these pixels from PSRAM.
      if (bgRleValid_) {
        rleSpan(y - oy_, x1 - ox_, x2 - ox_, bgLine_);
        blendSpan(dstAt(x1, y), bgLine_, x2 - x1 + 1, fr, fg, fb, inv);
      } else {
        blendSpan(dstAt(x1, y), srcAt(x1, y), x2 - x1 + 1, fr, fg, fb, inv);
      }
    }
  }
  return true;
}

// ===========================================================================

void RadarView::buildDrawOrder() {
  orderCount_ = 0;
  blockerCount_ = 0;

  const bool hasSelection = selectedHex_[0] != '\0';

  for (size_t i = 0; i < snapCount_ && orderCount_ < kMaxAircraft; ++i) {
    const Aircraft &ac = snap_[i];
    int32_t x = 0, y = 0;
    nmToPixel(ac.eastNm, ac.northNm, &x, &y);

    // Off-canvas: LVGL would clip these anyway, so skip the draw tasks.
    if (x < -24 || x > kSize + 24 || y < -24 || y > kSize + 24) {
      continue;
    }

    Placed p;
    p.ac = &ac;
    p.x = x;
    p.y = y;
    p.selected = hasSelection && strcasecmp(selectedHex_, ac.hex) == 0;
    // Glyph nose reaches 9 px; selected scales x1.35 and adds a 15 px halo.
    p.symbolR = p.selected ? 18 : 10;

    // Beam flare. A real PPI brightens a return as the beam crosses it and lets
    // it decay, and that -- not the wedge -- is what makes a sweep read as a
    // sweep rather than a rotating shadow. Quantised to 16 levels because the
    // level is what decides whether the symbol has to be reinvalidated on a
    // sweep-only frame; at full resolution every blip in the tail would repaint
    // every frame for a change nobody can see.
    p.lit = 0;
    if (sweepRunning_ && kSweepEnabled) {
      const float bearing = wrapDeg(atan2f(ac.eastNm, ac.northNm) / kDeg2Rad);
      const float behind = degreesBehind(bearing, sweepDeg_);
      if (behind <= kSweepTailDeg) {
        const float t = 1.0f - behind / kSweepTailDeg;
        const uint32_t flare = static_cast<uint32_t>(lroundf(kFlarePeak * t * t));
        p.lit = static_cast<uint8_t>((flare & 0xF0u) + (flare >> 4));
      }
    }

    // Extent of everything this aircraft paints. +2 covers the glyph casing,
    // which is drawn two pixels wider than the core stroke.
    const int16_t r = static_cast<int16_t>(p.symbolR + 2);
    p.box = {static_cast<int16_t>(x - r), static_cast<int16_t>(y - r),
             static_cast<int16_t>(x + r), static_cast<int16_t>(y + r)};
    const uint8_t used = ac.trailCount < kTrailLen ? ac.trailCount : kTrailLen;
    for (uint8_t t = 0; t < used; ++t) {
      int32_t tx = 0, ty = 0;
      nmToPixel(ac.trail[t].eastNm, ac.trail[t].northNm, &tx, &ty);
      if (tx - 1 < p.box.x1) p.box.x1 = static_cast<int16_t>(tx - 1);
      if (ty - 1 < p.box.y1) p.box.y1 = static_cast<int16_t>(ty - 1);
      if (tx + 1 > p.box.x2) p.box.x2 = static_cast<int16_t>(tx + 1);
      if (ty + 1 > p.box.y2) p.box.y2 = static_cast<int16_t>(ty + 1);
    }

    // Insertion sort by altitude ascending, so high aircraft draw last and
    // paint over low ones — looking down from above, higher is nearer the eye.
    // The selection is forced to the tail so its halo is never overpainted.
    size_t at = orderCount_;
    if (!p.selected) {
      while (at > 0 && (order_[at - 1].selected ||
                        order_[at - 1].ac->altitudeFt > ac.altitudeFt)) {
        order_[at] = order_[at - 1];
        --at;
      }
    }
    order_[at] = p;
    ++orderCount_;
  }

  // Label priority: the selection first, then nearest outwards. This is a
  // "what is over my house" display, and Tracker::selectNearest() already
  // treats distance as the measure of interest.
  for (size_t i = 0; i < orderCount_; ++i) {
    size_t at = i;
    while (at > 0) {
      const Placed &prev = order_[labelOrder_[at - 1]];
      const Placed &cur = order_[i];
      const bool prevWins =
          prev.selected ||
          (!cur.selected && prev.ac->distanceNm <= cur.ac->distanceNm);
      if (prevWins) break;
      labelOrder_[at] = labelOrder_[at - 1];
      --at;
    }
    labelOrder_[at] = static_cast<uint8_t>(i);
  }

  trailScale_ = orderCount_ <= 8    ? 1.00f
                : orderCount_ <= 16 ? 0.80f
                : orderCount_ <= 24 ? 0.65f
                                    : 0.50f;
}

int32_t RadarView::ringRadius(int i) const {
  const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(kRings));
  return static_cast<int32_t>(lroundf(nm * pxPerNm_));
}

RadarView::Rect RadarView::ringLabelRect(int i) const {
  const int32_t cx = kSize / 2;
  const int32_t ly = kSize / 2 - ringRadius(i) - 1;
  return {static_cast<int16_t>(cx + 5), static_cast<int16_t>(ly),
          static_cast<int16_t>(cx + 19), static_cast<int16_t>(ly + 17)};
}

// Local coordinates throughout: this is only ever rasterised into bgCache_ by
// renderBackgroundCache(), never into the display layer, so it needs no origin
// offset. Everything below that paints live does.
void RadarView::drawBackground(lv_layer_t *layer) {
  const int32_t cx = kSize / 2;
  const int32_t cy = kSize / 2;
  const lv_color_t ringColor = theme::c(theme::kRing);
  const lv_color_t crossColor = theme::c(theme::kCross);
  const lv_color_t labelColor = theme::c(theme::kRingLabel);
  const lv_color_t compassColor = theme::c(theme::kCompass);

  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = crossColor;
  line.width = 1;
  line.opa = LV_OPA_COVER;

  line.p1.x = 0;
  line.p1.y = cy;
  line.p2.x = kSize - 1;
  line.p2.y = cy;
  lv_draw_line(layer, &line);

  line.p1.x = cx;
  line.p1.y = 0;
  line.p2.x = cx;
  line.p2.y = kSize - 1;
  lv_draw_line(layer, &line);

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.color = ringColor;
  arc.width = 1;
  arc.opa = LV_OPA_COVER;
  arc.start_angle = 0;
  arc.end_angle = 360;

  for (int i = 1; i <= kRings; ++i) {
    const int32_t radius = ringRadius(i);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = static_cast<int16_t>(radius);
    lv_draw_arc(layer, &arc);

    // Ring range label, just above the ring on the vertical axis.
    const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(kRings));
    const Rect box = ringLabelRect(i);
    char nmBuf[8];
    snprintf(nmBuf, sizeof(nmBuf), "%d", static_cast<int>(lroundf(nm)));
    drawText(layer, box.x1, box.y1, 40, nmBuf, labelColor, LV_OPA_COVER,
             theme::fontRadar(), LV_TEXT_ALIGN_LEFT);
  }

  // Compass markers.
  drawText(layer, cx - 16, 3, 32, "N", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, cx - 16, kSize - 22, 32, "S", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, kSize - 22, cy - 10, 20, "E", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, 2, cy - 10, 20, "W", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);

  // Home marker + label.
  lv_draw_rect_dsc_t home;
  lv_draw_rect_dsc_init(&home);
  home.bg_color = theme::c(theme::kHomeDot);
  home.bg_opa = LV_OPA_COVER;
  home.radius = LV_RADIUS_CIRCLE;
  lv_area_t homeArea = {cx - 3, cy - 3, cx + 3, cy + 3};
  lv_draw_rect(layer, &home, &homeArea);
  drawText(layer, cx - 24, cy + 6, 48, "HOME", theme::c(theme::kHomeCaption),
           LV_OPA_COVER, theme::fontRadar(), LV_TEXT_ALIGN_CENTER);
}

void RadarView::drawTrail(lv_layer_t *layer, const Placed &p) {
  const Aircraft &ac = *p.ac;
  const uint8_t budget = trailBudget(p.selected);
  const uint8_t used = ac.trailCount < budget ? ac.trailCount : budget;
  if (used < 2) {
    return;
  }

  lv_draw_line_dsc_t trail;
  lv_draw_line_dsc_init(&trail);
  trail.color = colorForAircraft(ac);
  trail.width = p.selected ? 2 : 1;
  trail.round_start = 1;
  trail.round_end = 1;

  // Newest `used` points only, so the age fade still spans the whole trail.
  for (uint8_t i = 0; i + 1 < used; ++i) {
    const uint8_t idx0 =
        static_cast<uint8_t>((ac.trailHead + kTrailLen - used + i) % kTrailLen);
    const uint8_t idx1 =
        static_cast<uint8_t>((ac.trailHead + kTrailLen - used + i + 1) % kTrailLen);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    nmToPixel(ac.trail[idx0].eastNm, ac.trail[idx0].northNm, &x0, &y0);
    nmToPixel(ac.trail[idx1].eastNm, ac.trail[idx1].northNm, &x1, &y1);
    // Per segment, not per trail: a trail's bounding box is large enough that
    // testing it as a whole lets a region at one end of it build a draw task
    // for every segment at the other end, which LVGL then clips away.
    const Rect seg = {static_cast<int16_t>(LV_MIN(x0, x1) - 1),
                      static_cast<int16_t>(LV_MIN(y0, y1) - 1),
                      static_cast<int16_t>(LV_MAX(x0, x1) + 1),
                      static_cast<int16_t>(LV_MAX(y0, y1) + 1)};
    if (culled(seg)) {
      continue;
    }
    trail.opa = trailOpacity(ac, i, used, p.selected);
    trail.p1.x = x0 + ox_;
    trail.p1.y = y0 + oy_;
    trail.p2.x = x1 + ox_;
    trail.p2.y = y1 + oy_;
    lv_draw_line(layer, &trail);
  }
}

void RadarView::drawSymbol(lv_layer_t *layer, const Placed &p) {
  const Aircraft &ac = *p.ac;
  lv_color_t color = colorForAircraft(ac);
  if (p.lit != 0) {
    // Towards the beam's own colour rather than to white: white is the
    // selection, and a flare that reached it would read as "this one is
    // selected" twelve times a minute.
    color = mixColor(theme::c(theme::kSweepEdge), color, p.lit);
  }
  const int32_t x = p.x + ox_;
  const int32_t y = p.y + oy_;

  const float rad = (ac.trackDeg - 90.0f) * static_cast<float>(DEG_TO_RAD);
  const float fx = cosf(rad);
  const float fy = sinf(rad);
  const float sx = -fy;  // perpendicular (right wing) direction
  const float sy = fx;

  if (p.selected) {
    lv_draw_arc_dsc_t halo;
    lv_draw_arc_dsc_init(&halo);
    halo.color = theme::c(theme::kSelection);
    halo.width = 2;
    halo.opa = LV_OPA_80;
    halo.center.x = x;
    halo.center.y = y;
    halo.radius = 15;
    halo.start_angle = 0;
    halo.end_angle = 360;
    lv_draw_arc(layer, &halo);
  }

  // Aircraft glyph: fuselage + wings + tailplane as rounded lines.
  const float s = p.selected ? 1.35f : 1.0f;
  const float noseLen = 9.0f * s;
  const float tailLen = 6.0f * s;
  const float wingSpan = 7.5f * s;
  const float wingAt = 0.5f * s;
  const float tailSpan = 3.5f * s;
  const float tailAt = -5.0f * s;

  struct Seg {
    float ax, ay, bx, by;
  };
  const Seg segs[3] = {
      // Fuselage
      {x - fx * tailLen, y - fy * tailLen, x + fx * noseLen, y + fy * noseLen},
      // Wings
      {x + fx * wingAt + sx * wingSpan, y + fy * wingAt + sy * wingSpan,
       x + fx * wingAt - sx * wingSpan, y + fy * wingAt - sy * wingSpan},
      // Tailplane
      {x + fx * tailAt + sx * tailSpan, y + fy * tailAt + sy * tailSpan,
       x + fx * tailAt - sx * tailSpan, y + fy * tailAt - sy * tailSpan},
  };

  // Two complete passes: every casing first, then every fill. Interleaving
  // would let the wing casing eat the fuselage colour. The casing is what
  // keeps overlapping aircraft distinguishable in a cluster.
  const int32_t coreW = p.selected ? 3 : 2;
  for (int pass = 0; pass < 2; ++pass) {
    lv_draw_line_dsc_t glyph;
    lv_draw_line_dsc_init(&glyph);
    // Round caps only on the visible stroke. LVGL draws each one as a separate
    // radius-masked fill, which costs a PSRAM allocation and a circle mask for
    // a 3x3 box; the casing pass alone was half of them, for ends that the fill
    // pass covers anyway.
    glyph.round_start = pass == 1;
    glyph.round_end = pass == 1;
    glyph.opa = LV_OPA_COVER;
    glyph.color = pass == 0 ? theme::c(theme::kRadarBg) : color;
    glyph.width = pass == 0 ? coreW + 2 : coreW;

    for (const Seg &seg : segs) {
      glyph.p1.x = static_cast<int32_t>(lroundf(seg.ax));
      glyph.p1.y = static_cast<int32_t>(lroundf(seg.ay));
      glyph.p2.x = static_cast<int32_t>(lroundf(seg.bx));
      glyph.p2.y = static_cast<int32_t>(lroundf(seg.by));
      lv_draw_line(layer, &glyph);
    }
  }
}

bool RadarView::rectsOverlap(const Rect &a, const Rect &b) {
  return !(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

void RadarView::addBlocker(const Rect &r) {
  if (blockerCount_ < kMaxBlockers) {
    blockers_[blockerCount_++] = r;
  }
}

// `skip` exempts one blocker — an aircraft's label is allowed to sit against
// its own symbol, which anchor 0 (the historical placement) always does.
bool RadarView::blocked(const Rect &r, size_t skip) const {
  for (size_t i = 0; i < blockerCount_; ++i) {
    if (i != skip && rectsOverlap(r, blockers_[i])) {
      return true;
    }
  }
  return false;
}

void RadarView::seedStaticBlockers() {
  const int32_t cx = kSize / 2;
  const int32_t cy = kSize / 2;

  // The legend is drawn after the labels, so it would paint over any label that
  // strayed under it.
  addBlocker({static_cast<int16_t>(kLegend.x1), static_cast<int16_t>(kLegend.y1),
              static_cast<int16_t>(kLegend.x2), static_cast<int16_t>(kLegend.y2)});
  // HOME marker plus its caption.
  addBlocker({static_cast<int16_t>(cx - 26), static_cast<int16_t>(cy - 8),
              static_cast<int16_t>(cx + 26), static_cast<int16_t>(cy + 24)});

  // Ring range labels sit in a column just right of the vertical axis.
  for (int i = 1; i <= kRings; ++i) {
    addBlocker(ringLabelRect(i));
  }

  // drawLabels() identifies "the aircraft's own symbol" by index, so it needs
  // to know where the static run ended rather than assuming a count.
  staticBlockerCount_ = blockerCount_;
}

void RadarView::formatLabel(const Aircraft &ac, uint8_t form, char *buf, size_t n) {
  const char *id = ac.callsign[0] != '\0' ? ac.callsign : ac.hex;
  const bool hasAlt = ac.altitudeFt != kAltitudeUnknown;
  const int fl = (ac.altitudeFt + 50) / 100;

  switch (form) {
    case 0:  // Identity + flight level.
      if (hasAlt) {
        snprintf(buf, n, "%s %d", id, fl);
      } else {
        snprintf(buf, n, "%s", id);
      }
      break;
    case 1:  // Identity only.
      snprintf(buf, n, "%s", id);
      break;
    default:  // Flight level only, or fall back to identity.
      if (hasAlt) {
        snprintf(buf, n, "%d", fl);
      } else {
        snprintf(buf, n, "%s", id);
      }
      break;
  }
}

bool RadarView::anchorRect(const Placed &p, uint8_t anchor, int32_t w, int32_t h,
                           Rect *out) const {
  const int32_t x = p.x;
  const int32_t y = p.y;
  const int32_t r = p.symbolR;
  const int32_t g = 4;   // gap between symbol box and label
  const int32_t f = 16;  // extra offset for the outer ring of anchors
  int32_t lx = 0, ly = 0;

  switch (anchor) {
    // Anchor 0 reproduces the historical placement exactly, so an uncrowded
    // sky looks unchanged.
    case 0:  lx = x + 11;         ly = y - 8;             break;
    case 1:  lx = x - r - g - w;  ly = y - h / 2 + 1;     break;
    case 2:  lx = x - w / 2;      ly = y - r - g - h;     break;
    case 3:  lx = x - w / 2;      ly = y + r + g;         break;
    case 4:  lx = x + r;          ly = y - r - h;         break;
    case 5:  lx = x + r;          ly = y + r;             break;
    case 6:  lx = x - r - w;      ly = y - r - h;         break;
    case 7:  lx = x - r - w;      ly = y + r;             break;
    case 8:  lx = x + r + f;      ly = y - r - f - h;     break;
    case 9:  lx = x + r + f;      ly = y + r + f;         break;
    case 10: lx = x - r - f - w;  ly = y - r - f - h;     break;
    default: lx = x - r - f - w;  ly = y + r + f;         break;
  }

  // Padded for the backdrop; this is what gets collision-tested and stored.
  const int32_t x1 = lx - 3;
  const int32_t y1 = ly - 1;
  const int32_t x2 = lx + w + 3;
  const int32_t y2 = ly + h + 1;
  if (x1 < 2 || y1 < 2 || x2 > kSize - 3 || y2 > kSize - 3) {
    return false;
  }

  out->x1 = static_cast<int16_t>(x1);
  out->y1 = static_cast<int16_t>(y1);
  out->x2 = static_cast<int16_t>(x2);
  out->y2 = static_cast<int16_t>(y2);
  return true;
}

uint8_t RadarView::rememberedAnchor(const char *hex, uint8_t *form) const {
  for (size_t i = 0; i < memoCount_; ++i) {
    if (strcasecmp(memo_[i].hex, hex) == 0) {
      *form = memo_[i].form;
      return memo_[i].anchor;
    }
  }
  *form = 0xFF;
  return 0xFF;
}

void RadarView::layoutLabels() {
  labelCount_ = 0;
  seedStaticBlockers();

  // Every symbol blocks, including ones we have not placed a label for yet —
  // otherwise a label could land on an aircraft further down the order.
  for (size_t i = 0; i < orderCount_; ++i) {
    const Placed &p = order_[i];
    addBlocker({static_cast<int16_t>(p.x - p.symbolR),
                static_cast<int16_t>(p.y - p.symbolR),
                static_cast<int16_t>(p.x + p.symbolR),
                static_cast<int16_t>(p.y + p.symbolR)});
  }

  // Read every hysteresis hint before writing any, since the new placements
  // overwrite memo_ in place.
  uint8_t hintForm[kMaxAircraft];
  uint8_t hintAnchor[kMaxAircraft];
  for (size_t i = 0; i < orderCount_; ++i) {
    hintAnchor[i] = rememberedAnchor(order_[i].ac->hex, &hintForm[i]);
  }

  size_t written = 0;

  for (size_t k = 0; k < orderCount_; ++k) {
    const size_t idx = labelOrder_[k];
    const Placed &p = order_[idx];
    const size_t ownSymbol = staticBlockerCount_ + idx;

    char lbl[24];
    Rect rect = {0, 0, 0, 0};
    bool placed = false;
    uint8_t usedForm = 0;
    uint8_t usedAnchor = 0;

    // Prefer last frame's spot if it is still free, so labels do not hop
    // around when the distance ordering shuffles between frames.
    if (hintAnchor[idx] < kAnchorCount && hintForm[idx] < kLabelForms) {
      formatLabel(*p.ac, hintForm[idx], lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      if (anchorRect(p, hintAnchor[idx], sz.x, sz.y, &rect) && !blocked(rect, ownSymbol)) {
        placed = true;
        usedForm = hintForm[idx];
        usedAnchor = hintAnchor[idx];
      }
    }

    // Otherwise walk the ladder: full label, then shorter forms, dropping the
    // label entirely if nothing fits. This thins clusters locally while
    // sparse regions keep their full labels.
    for (uint8_t form = 0; !placed && form < kLabelForms; ++form) {
      formatLabel(*p.ac, form, lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      for (uint8_t a = 0; a < kAnchorCount; ++a) {
        if (anchorRect(p, a, sz.x, sz.y, &rect) && !blocked(rect, ownSymbol)) {
          placed = true;
          usedForm = form;
          usedAnchor = a;
          break;
        }
      }
    }

    if (!placed) {
      if (!p.selected) {
        continue;  // dropped — the symbol is still drawn
      }
      // The selection always keeps its label; it has a halo and a backdrop,
      // so it stays readable even if it has to overlap something.
      formatLabel(*p.ac, 0, lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      const int32_t lx = p.x + 11;
      const int32_t ly = p.y - 8;
      rect = {static_cast<int16_t>(lx - 3), static_cast<int16_t>(ly - 1),
              static_cast<int16_t>(lx + sz.x + 3),
              static_cast<int16_t>(ly + sz.y + 1)};
      usedForm = 0;
      usedAnchor = 0;
    } else {
      formatLabel(*p.ac, usedForm, lbl, sizeof(lbl));
    }

    // Record the solved placement; painting happens later, possibly more than
    // once, from labels_ alone.
    if (labelCount_ < kMaxAircraft) {
      LabelPlacement &lp = labels_[labelCount_++];
      lp.rect = rect;
      strncpy(lp.text, lbl, sizeof(lp.text) - 1);
      lp.text[sizeof(lp.text) - 1] = '\0';
      lp.orderIdx = static_cast<uint8_t>(idx);
      lp.hasLeader = false;

      // A label on the outer ring of anchors has been pushed clear of its
      // symbol, so tie it back with a leader line.
      if (usedAnchor >= 8) {
        const int32_t tx = p.x < rect.x1 ? rect.x1 : (p.x > rect.x2 ? rect.x2 : p.x);
        const int32_t ty = p.y < rect.y1 ? rect.y1 : (p.y > rect.y2 ? rect.y2 : p.y);
        const float dx = static_cast<float>(tx - p.x);
        const float dy = static_cast<float>(ty - p.y);
        const float len = sqrtf(dx * dx + dy * dy);
        if (len > p.symbolR) {
          lp.hasLeader = true;
          lp.leadX1 = static_cast<int16_t>(
              p.x + static_cast<int32_t>(lroundf(dx / len * p.symbolR)));
          lp.leadY1 = static_cast<int16_t>(
              p.y + static_cast<int32_t>(lroundf(dy / len * p.symbolR)));
          lp.leadX2 = static_cast<int16_t>(tx);
          lp.leadY2 = static_cast<int16_t>(ty);
        }
      }
    }

    addBlocker(rect);

    if (written < kMaxAircraft) {
      strncpy(memo_[written].hex, p.ac->hex, sizeof(memo_[written].hex) - 1);
      memo_[written].hex[sizeof(memo_[written].hex) - 1] = '\0';
      memo_[written].form = usedForm;
      memo_[written].anchor = usedAnchor;
      ++written;
    }
  }

  memoCount_ = written;
}

uint32_t RadarView::paintSignature() const {
  uint32_t h = 2166136261u ^ static_cast<uint32_t>(orderCount_);
  for (size_t i = 0; i < orderCount_; ++i) {
    const Placed &p = order_[i];
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.x);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.y);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.ac->altitudeFt);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.selected ? 1 : 0);
  }
  return h;
}

bool RadarView::culled(const Rect &r) const {
  return r.x2 < clip_.x1 || clip_.x2 < r.x1 || r.y2 < clip_.y1 || clip_.y2 < r.y1;
}

void RadarView::collectDirty() {
  dirtyCount_ = 0;

  // Symbol boxes only in the common case: trails do not move between fetches,
  // and a trail's extent is large enough that including it every frame would
  // invalidate most of the view. On the frames where new positions arrived the
  // trails did grow, so those frames use the full per-aircraft extent instead.
  for (size_t i = 0; i < orderCount_ && dirtyCount_ < kMaxDirty; ++i) {
    const Placed &p = order_[i];
    if (trailsChanged_) {
      dirty_[dirtyCount_++] = p.box;
    } else {
      const int16_t r = static_cast<int16_t>(p.symbolR + 2);
      dirty_[dirtyCount_++] = {static_cast<int16_t>(p.x - r), static_cast<int16_t>(p.y - r),
                               static_cast<int16_t>(p.x + r), static_cast<int16_t>(p.y + r)};
    }
  }

  for (size_t i = 0; i < labelCount_ && dirtyCount_ < kMaxDirty; ++i) {
    const LabelPlacement &lp = labels_[i];
    Rect r = lp.rect;
    if (lp.hasLeader) {
      // The leader runs from the symbol to the label, through pixels neither
      // rect covers.
      if (lp.leadX1 < r.x1) r.x1 = lp.leadX1;
      if (lp.leadY1 < r.y1) r.y1 = lp.leadY1;
      if (lp.leadX1 > r.x2) r.x2 = lp.leadX1;
      if (lp.leadY1 > r.y2) r.y2 = lp.leadY1;
    }
    dirty_[dirtyCount_++] = r;
  }
}

void RadarView::invalidateDirty(bool contentChanged) {
  lv_area_t coords;
  lv_obj_get_coords(obj_, &coords);

  auto area = [](const Rect &r) -> int32_t {
    return (static_cast<int32_t>(r.x2) - r.x1 + 1) * (static_cast<int32_t>(r.y2) - r.y1 + 1);
  };
  auto merged = [](const Rect &a, const Rect &b) -> Rect {
    return {a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
            a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2};
  };

  Rect region[kMaxDirty * 2 + kSweepBands];
  size_t n = 0;
  if (contentChanged) {
    // Both sets: last frame's rects to restore the background under where
    // things were, this frame's to draw where they now are.
    for (size_t i = 0; i < prevDirtyCount_; ++i) region[n++] = prevDirty_[i];
    for (size_t i = 0; i < dirtyCount_; ++i) region[n++] = dirty_[i];
  } else {
    // Nothing moved, so the only aircraft that differ from what is already in
    // the buffer are the ones whose beam flare stepped. Everything else in the
    // view is already correct and repainting it would be the sweep paying for
    // the whole frame, twelve times a second.
    for (size_t i = 0; i < orderCount_ && n < kMaxAircraft; ++i) {
      const Placed &p = order_[i];
      if (p.lit == litPainted_[i]) {
        continue;
      }
      const int16_t r = static_cast<int16_t>(p.symbolR + 2);
      region[n++] = {static_cast<int16_t>(p.x - r), static_cast<int16_t>(p.y - r),
                     static_cast<int16_t>(p.x + r), static_cast<int16_t>(p.y + r)};
    }
  }
  for (size_t i = 0; i < sweepRegionCount_; ++i) region[n++] = sweepRegion_[i];
  if (n == 0) {
    return;
  }

  // Merge down to a handful of regions before invalidating. LVGL renders each
  // invalid area as its own pass, and each pass re-runs the whole draw handler
  // -- including restoring the background. Measured at ~5-8 ms per pass, so two
  // dozen tight rects cost far more than a few loose ones: going from 24
  // regions to 4 was worth more than the tighter clipping it gave up.
  //
  // The exact merge below is O(n^3), which is invisible at the 28 rects a
  // half-dozen aircraft produce and ruinous at the 132 a full sky plus the
  // sweep can: ~500 000 pair tests, tens of milliseconds, inside the frame it
  // is trying to make cheaper. Bucketing by position first caps n at 16 for a
  // pass that is linear in the rect count. A bucket is a quarter of the view
  // across, so this gives up very little -- and only where the exact merge was
  // never affordable in the first place.
  if (n > kMergeExactLimit) {
    Rect cell[kMergeGrid * kMergeGrid];
    bool used[kMergeGrid * kMergeGrid] = {false};
    for (size_t i = 0; i < n; ++i) {
      const int32_t cx = (static_cast<int32_t>(region[i].x1) + region[i].x2) / 2;
      const int32_t cy = (static_cast<int32_t>(region[i].y1) + region[i].y2) / 2;
      const int32_t gx = LV_CLAMP(0, cx * static_cast<int32_t>(kMergeGrid) / kSize,
                                  static_cast<int32_t>(kMergeGrid) - 1);
      const int32_t gy = LV_CLAMP(0, cy * static_cast<int32_t>(kMergeGrid) / kSize,
                                  static_cast<int32_t>(kMergeGrid) - 1);
      const size_t c = static_cast<size_t>(gy) * kMergeGrid + static_cast<size_t>(gx);
      cell[c] = used[c] ? merged(cell[c], region[i]) : region[i];
      used[c] = true;
    }
    n = 0;
    for (size_t c = 0; c < kMergeGrid * kMergeGrid; ++c) {
      if (used[c]) region[n++] = cell[c];
    }
  }

  const size_t maxRegions = kMaxRegions + (sweepRegionCount_ > 0 ? kSweepBands : 0);
  while (n > maxRegions) {
    // Cheapest pair to merge: the one adding least dead area.
    size_t bi = 0, bj = 1;
    int32_t best = INT32_MAX;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = i + 1; j < n; ++j) {
        const int32_t cost = area(merged(region[i], region[j])) - area(region[i]) -
                             area(region[j]);
        if (cost < best) {
          best = cost;
          bi = i;
          bj = j;
        }
      }
    }
    region[bi] = merged(region[bi], region[bj]);
    region[bj] = region[--n];
  }

  for (size_t i = 0; i < n; ++i) {
    lv_area_t a = {region[i].x1 + coords.x1 - 1, region[i].y1 + coords.y1 - 1,
                   region[i].x2 + coords.x1 + 1, region[i].y2 + coords.y1 + 1};
    lv_obj_invalidate_area(obj_, &a);
  }
}

void RadarView::paintLabels(lv_layer_t *layer) {
  for (size_t i = 0; i < labelCount_; ++i) {
    const LabelPlacement &lp = labels_[i];
    if (culled(lp.rect) && !(lp.hasLeader && !culled(order_[lp.orderIdx].box))) {
      continue;
    }
    const Placed &p = order_[lp.orderIdx];

    if (lp.hasLeader) {
      lv_draw_line_dsc_t lead;
      lv_draw_line_dsc_init(&lead);
      lead.color = colorForAircraft(*p.ac);
      lead.width = 1;
      lead.opa = LV_OPA_50;
      lead.p1.x = lp.leadX1 + ox_;
      lead.p1.y = lp.leadY1 + oy_;
      lead.p2.x = lp.leadX2 + ox_;
      lead.p2.y = lp.leadY2 + oy_;
      lv_draw_line(layer, &lead);
    }

    // Backdrop, so 14 px text stays readable over rings and trails —
    // collision avoidance cannot help with those.
    lv_draw_rect_dsc_t back;
    lv_draw_rect_dsc_init(&back);
    back.bg_color = theme::c(theme::kRadarBg);
    back.bg_opa = p.selected ? 210 : 165;
    back.radius = 3;
    lv_area_t backArea = {lp.rect.x1 + ox_, lp.rect.y1 + oy_, lp.rect.x2 + ox_,
                          lp.rect.y2 + oy_};
    lv_draw_rect(layer, &back, &backArea);

    drawText(layer, lp.rect.x1 + 3 + ox_, lp.rect.y1 + 1 + oy_,
             lp.rect.x2 - lp.rect.x1 - 6, lp.text,
             theme::c(p.selected ? theme::kSelection : theme::kRadarLabel),
             LV_OPA_COVER, theme::fontRadar(), LV_TEXT_ALIGN_LEFT);
  }
}

void RadarView::drawLegend(lv_layer_t *layer) {
  // The legend is static but is not baked into the background cache, because it
  // has to paint after the symbols for aircraft never to obscure it. That left
  // it redrawing in full on every pass: a translucent rounded backdrop over
  // ~8 800 px (a masked read-modify-write, the most expensive shape here) plus
  // 40 gradient segments and six labels, three or four times per paint, almost
  // always for a region nowhere near it.
  if (culled({static_cast<int16_t>(kLegend.x1), static_cast<int16_t>(kLegend.y1),
              static_cast<int16_t>(kLegend.x2), static_cast<int16_t>(kLegend.y2)})) {
    return;
  }

  const int32_t height = kLegend.barBot - kLegend.barTop;

  // Backdrop for legibility. Its extent is also what seedStaticBlockers()
  // reserves, so both read kLegend rather than repeating the arithmetic.
  lv_draw_rect_dsc_t back;
  lv_draw_rect_dsc_init(&back);
  back.bg_color = theme::c(theme::kRadarBg);
  back.bg_opa = 190;
  back.radius = theme::kRadSm;
  lv_area_t backArea = {kLegend.x1 + ox_, kLegend.y1 + oy_, kLegend.x2 + ox_,
                        kLegend.y2 + oy_};
  lv_draw_rect(layer, &back, &backArea);

  drawText(layer, kLegend.x1 + ox_, kLegend.y1 + 1 + oy_, 46, "kft",
           theme::c(theme::kTextMuted), LV_OPA_COVER, theme::fontRadar(),
           LV_TEXT_ALIGN_CENTER);

  // Gradient bar (top = highest altitude).
  const int segs = 40;
  lv_draw_rect_dsc_t seg;
  lv_draw_rect_dsc_init(&seg);
  seg.bg_opa = LV_OPA_COVER;
  for (int i = 0; i < segs; ++i) {
    const float t = 1.0f - (static_cast<float>(i) + 0.5f) / static_cast<float>(segs);
    seg.bg_color = altitudeColor(static_cast<int>(t * kMaxAltitudeFt));
    const int32_t y1 = kLegend.barTop + i * height / segs;
    const int32_t y2 = kLegend.barTop + (i + 1) * height / segs;
    lv_area_t segArea = {kLegend.barX + ox_, y1 + oy_,
                         kLegend.barX + kLegend.barW + ox_, y2 + oy_};
    lv_draw_rect(layer, &seg, &segArea);
  }

  // Tick labels: 0,10,20,30,40 kft.
  for (int k = 0; k <= 40; k += 10) {
    const float frac = static_cast<float>(k * 1000) / static_cast<float>(kMaxAltitudeFt);
    const int32_t y = kLegend.barBot - static_cast<int32_t>(lroundf(frac * height)) - 8;
    char t[4];
    snprintf(t, sizeof(t), "%d", k);
    drawText(layer, kLegend.barX - 30 + ox_, y + oy_, 26, t,
             theme::c(theme::kLegendTick), LV_OPA_COVER, theme::fontRadar(),
             LV_TEXT_ALIGN_RIGHT);
  }
}

void RadarView::redraw() {
  if (obj_ == nullptr) {
    return;
  }
  if (!bgValid_) {
    renderBackgroundCache();
  }

  // The beam steps on its own clock, independently of whether any aircraft
  // moved. Derived from the tick rather than accumulated, so a frame the loop
  // was too busy to serve slips the beam instead of slowing it down.
  const uint32_t now = lv_tick_get();
  const bool wasRunning = sweepRunning_;
  const bool sweepStep =
      kSweepEnabled && (!wasRunning || now - sweepStepMs_ >= kSweepStepMs);
  if (sweepStep) {
    sweepDeg_ = sweepAngleAt(now);
    sweepRunning_ = true;  // read by buildDrawOrder() below, for the blip flare
  }

  // Solve the frame here; the actual painting happens in onDraw(), which LVGL
  // calls back during lv_timer_handler() once per invalidated region.
  buildDrawOrder();

  // Nothing moved by a whole pixel? Then there is nothing to paint *of the
  // aircraft*. At 25 nm range a 450 kt aircraft crosses about one pixel per
  // second, so without this the view repaints ten-plus times per pixel of real
  // movement. That cost two visible artefacts: the label solver re-ran every
  // frame and could pick a different anchor each time (labels flickering), and
  // the repaints starved lv_timer_handler, so the info panel's marquee
  // animation stepped irregularly.
  //
  // The sweep does not change that reasoning, it just splits it in two. A sweep
  // step still has to paint, but only the wedge -- and crucially it must not
  // re-run the anchor solver, or the beam would bring the label flicker back
  // with it at twelve frames a second.
  const uint32_t sig = paintSignature();
  const bool contentChanged =
      fullRepaint_ || trailsChanged_ || !hasPainted_ || sig != lastPaintSig_;
  if (!contentChanged && !sweepStep) {
    return;
  }
  lastPaintSig_ = sig;
  hasPainted_ = true;

  if (sweepStep) {
    // From where the beam was when the buffer was last painted, not from where
    // it was last frame: a frame the gate above skipped never reached the
    // screen, and the pixels it would have written are still stale.
    buildSweepRegions(wasRunning ? sweepPaintedDeg_ : sweepDeg_, sweepDeg_);
    sweepStepMs_ = now;
    sweepPaintedDeg_ = sweepDeg_;
  } else {
    sweepRegionCount_ = 0;
  }

  if (contentChanged) {
    // Only the frames that move an aircraft pay for the anchor solver.
    probe::Scope solveScope(probe::kTasks);
    layoutLabels();
    collectDirty();
  }

  if (fullRepaint_) {
    fullRepaint_ = false;
    lv_obj_invalidate(obj_);
  } else {
    invalidateDirty(contentChanged);
  }
  trailsChanged_ = false;
  if (contentChanged) {
    memcpy(prevDirty_, dirty_, dirtyCount_ * sizeof(Rect));
    prevDirtyCount_ = dirtyCount_;
  }
  for (size_t i = 0; i < orderCount_; ++i) {
    litPainted_[i] = order_[i].lit;
  }
  probe::countFrame();
}

void RadarView::onCoverCheck(lv_event_t *e) {
  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  // The background image is opaque and covers the whole object, so LVGL can
  // skip everything underneath -- including filling the screen background,
  // which would be a full-area write we would then paint straight over.
  lv_event_set_cover_res(e, self != nullptr && self->bgValid_ ? LV_COVER_RES_COVER
                                                             : LV_COVER_RES_NOT_COVER);
}

void RadarView::onDraw(lv_event_t *e) {
  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr) {
    return;
  }
  probe::Scope _(probe::kRaster);
  self->paintDirect(lv_event_get_layer(e));
}

void RadarView::paintDirect(lv_layer_t *layer) {
  lv_area_t coords;
  lv_obj_get_coords(obj_, &coords);
  ox_ = coords.x1;
  oy_ = coords.y1;

  // The region LVGL is asking for, in local coordinates, so the draw loops can
  // skip work that would be clipped away anyway.
  clip_ = {static_cast<int16_t>(layer->_clip_area.x1 - ox_),
           static_cast<int16_t>(layer->_clip_area.y1 - oy_),
           static_cast<int16_t>(layer->_clip_area.x2 - ox_),
           static_cast<int16_t>(layer->_clip_area.y2 - oy_)};

  // Restores the static chrome and erases the previous frame in one pass, and
  // composites the beam into the same pass -- which is the whole reason it is
  // done by hand rather than with lv_draw_image. With LV_USE_OS none, an
  // lv_draw_*() call only queues a task; the queue is not rasterised until
  // after this handler returns. So anything written directly here lands
  // underneath everything drawn through LVGL below, which is exactly the layer
  // order the beam wants: over the rings, under the aircraft.
  if (!compositeBackground(layer)) {
    if (bgValid_) {
      lv_draw_image_dsc_t img;
      lv_draw_image_dsc_init(&img);
      img.src = &bgImg_;
      img.opa = LV_OPA_COVER;
      lv_draw_image(layer, &img, &coords);
    }
  }

  for (size_t i = 0; i < orderCount_; ++i) {
    if (!culled(order_[i].box)) drawTrail(layer, order_[i]);
  }
  for (size_t i = 0; i < orderCount_; ++i) {
    // Against the symbol's own box, not order_[i].box -- that one is stretched
    // to cover the trail, and testing symbols against it builds a glyph's six
    // line tasks for every region the trail happens to reach.
    const Placed &p = order_[i];
    const int16_t r = static_cast<int16_t>(p.symbolR + 2);
    const Rect symBox = {static_cast<int16_t>(p.x - r), static_cast<int16_t>(p.y - r),
                         static_cast<int16_t>(p.x + r), static_cast<int16_t>(p.y + r)};
    if (!culled(symBox)) drawSymbol(layer, p);
  }
  paintLabels(layer);
  // Stays live rather than baked into the cache: it is drawn last so aircraft
  // never obscure it, which is only true if it paints after the symbols.
  drawLegend(layer);
}

void RadarView::onClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }

  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr || self->obj_ == nullptr) {
    return;
  }

  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point;
  lv_indev_get_point(indev, &point);

  lv_area_t coords;
  lv_obj_get_coords(self->obj_, &coords);
  const int32_t localX = point.x - coords.x1;
  const int32_t localY = point.y - coords.y1;

  self->pixelToNm(localX, localY, &self->pendingEast_, &self->pendingNorth_);
  self->pendingClick_ = true;
}
