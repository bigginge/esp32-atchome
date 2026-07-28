/**
 * Host-side checks for the sweep beam's geometry and compositing.
 *
 * These exist because the sweep is the one part of the render path whose
 * correctness is not obvious by reading it, and whose failure mode is subtle:
 * a pixel that no pass writes keeps whatever was in the framebuffer two frames
 * ago, which on screen is a single lit dot the beam leaves behind and never
 * clears. That is invisible in a screenshot and obvious after a minute of
 * watching, which is the worst possible combination to debug on a device.
 *
 * Three real bugs were found here before the code ever reached hardware: a
 * half-pixel bias in sectorRowSpan(), a zero-opacity slice that was skipped
 * rather than copied, and adjacent slices deriving their shared edge ray twice
 * and disagreeing about it by a rounding step.
 *
 * Run with `make -C tests/host`.
 */
#include <math.h>
#include <stdio.h>

#include "lv_stub.hpp"
#include "radar_view.hpp"

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    ++failures;
  }
}

constexpr float kPi = 3.14159265358979f;

float wrap360(float d) {
  d = fmodf(d, 360.0f);
  return d < 0.0f ? d + 360.0f : d;
}

/** The reference the fast path is checked against: one atan2 per pixel. */
bool bruteInside(int x, int y, float b0, float span) {
  const float c = RadarView::kSize * 0.5f;
  const float dx = x + 0.5f - c;
  const float dy = y + 0.5f - c;
  const float r = static_cast<float>(RadarView::kSweepRadius);
  if (dx * dx + dy * dy > r * r) return false;
  // Bearings run clockwise from north; screen y grows downwards.
  const float bearing = wrap360(atan2f(dx, -dy) * 180.0f / kPi);
  return wrap360(bearing - b0) <= span;
}

void testOpacityAndWrap() {
  check(RadarView::sweepOpacity(0.0f) > RadarView::sweepOpacity(10.0f),
        "beam is brightest at the leading edge");
  check(RadarView::sweepOpacity(RadarView::kSweepTailDeg + 0.1f) == 0,
        "beam is dark past the tail");
  check(RadarView::degreesBehind(350.0f, 10.0f) == 20.0f,
        "degreesBehind wraps through north");
  check(RadarView::degreesBehind(10.0f, 10.0f) == 0.0f,
        "degreesBehind is zero on the head");
  check(RadarView::sweepAngleAt(RadarView::kSweepPeriodMs - 1) > 359.0f &&
            RadarView::sweepAngleAt(0) == 0.0f,
        "sweep angle wraps cleanly at the period");
}

/** sectorRowSpan() must return exactly the sector, from two half-plane bounds
 *  per row rather than an angle per pixel. */
void testSectorRowSpan() {
  int compared = 0;
  int mismatches = 0;
  // Chosen to land on and either side of the axes, where a bounding ray goes
  // horizontal and the row constraint stops being solvable for x.
  const float bases[] = {0.0f,   17.0f,  45.0f,  89.9f,  90.0f,  90.1f,
                         133.0f, 180.0f, 213.0f, 270.0f, 315.0f, 359.0f};
  const float spans[] = {2.0f, 3.055f, 30.0f, 55.0f, 89.0f, 90.0f, 120.0f, 169.0f};

  for (float b0 : bases) {
    for (float span : spans) {
      const float b1 = wrap360(b0 + span);
      const float d0x = sinf(b0 * kPi / 180.0f), d0y = -cosf(b0 * kPi / 180.0f);
      const float d1x = sinf(b1 * kPi / 180.0f), d1y = -cosf(b1 * kPi / 180.0f);

      for (int y = 0; y < RadarView::kSize; ++y) {
        int32_t lo = 0, hi = 0;
        const bool any = RadarView::sectorRowSpan(d0x, d0y, d1x, d1y, y, &lo, &hi);
        for (int x = 0; x < RadarView::kSize; ++x) {
          const bool want = bruteInside(x, y, b0, span);
          const bool got = any && x >= lo && x <= hi;
          ++compared;
          if (want == got) continue;

          // Disagreement is only tolerated within a pixel of a boundary, where
          // the two methods legitimately round differently.
          const float c = RadarView::kSize * 0.5f;
          const float dx = x + 0.5f - c, dy = y + 0.5f - c;
          const float dist = sqrtf(dx * dx + dy * dy);
          const float behind = wrap360(wrap360(atan2f(dx, -dy) * 180.0f / kPi) - b0);
          const float toEdge =
              fminf(fminf(behind, fabsf(behind - span)) * kPi / 180.0f * dist,
                    fabsf(dist - static_cast<float>(RadarView::kSweepRadius)));
          if (toEdge > 1.0f) {
            if (mismatches < 5) {
              printf("  b0=%.1f span=%.1f (%d,%d) want=%d got=%d edge=%.2fpx\n", b0,
                     span, x, y, want, got, toEdge);
            }
            ++mismatches;
          }
        }
      }
    }
  }
  printf("sectorRowSpan: %d pixels compared, %d mismatches away from an edge\n",
         compared, mismatches);
  check(mismatches == 0, "sectorRowSpan matches a brute-force atan2 sector");
}

/** Every pixel of the swept wedge has to fall inside a band handed to LVGL, or
 *  it is never invalidated and the beam streaks. */
void testSweepRegionsCoverWedge() {
  RadarView rv;
  int uncovered = 0;
  for (float from = 0.0f; from < 360.0f; from += 7.0f) {
    const float to = wrap360(from + 5.0f);
    rv.buildSweepRegions(from, to);
    const float span = wrap360(to - from) + RadarView::kSweepTailDeg;
    const float b0 = wrap360(to - span);

    for (int y = 0; y < RadarView::kSize; ++y) {
      for (int x = 0; x < RadarView::kSize; ++x) {
        if (!bruteInside(x, y, b0, span)) continue;
        bool covered = false;
        for (size_t i = 0; i < rv.sweepRegionCount_ && !covered; ++i) {
          const RadarView::Rect &r = rv.sweepRegion_[i];
          covered = x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
        }
        if (!covered) {
          if (uncovered < 5) printf("  uncovered from=%.0f (%d,%d)\n", from, x, y);
          ++uncovered;
        }
      }
    }
  }
  printf("buildSweepRegions: %d wedge pixels outside every band\n", uncovered);
  check(uncovered == 0, "sweep bands cover the whole swept wedge");
}

/** Dead area the row-banding costs. Reported, not asserted: it is the tuning
 *  number behind kSweepBands, not a contract. */
void reportBandOverhead() {
  RadarView rv;
  double worst = 0.0, total = 0.0;
  int n = 0;
  for (float from = 0.0f; from < 360.0f; from += 3.0f) {
    const float to = wrap360(from + 5.0f);
    rv.buildSweepRegions(from, to);
    const float span = wrap360(to - from) + RadarView::kSweepTailDeg;
    long banded = 0;
    for (size_t i = 0; i < rv.sweepRegionCount_; ++i) {
      const RadarView::Rect &r = rv.sweepRegion_[i];
      banded += static_cast<long>(r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
    }
    const double wedge = kPi * RadarView::kSweepRadius * RadarView::kSweepRadius *
                         span / 360.0;
    const double ratio = banded / wedge;
    if (ratio > worst) worst = ratio;
    total += ratio;
    ++n;
  }
  printf("band overhead: mean %.2fx wedge area, worst %.2fx (%zu bands)\n",
         total / n, worst, static_cast<size_t>(RadarView::kSweepBands));
}

/**
 * The composite is the only thing that puts background back, so it must write
 * every pixel of the region LVGL asked for. In DIRECT double-buffered mode an
 * unwritten pixel inside an invalid area keeps content from two frames ago.
 */
void testCompositeWritesEveryPixel() {
  constexpr int32_t kScreenW = 800, kScreenH = 480;
  const int32_t S = RadarView::kSize;
  const int32_t ox = theme::layout::kRadarX;
  const int32_t oy = theme::layout::kRadarY;
  static uint16_t fb[kScreenW * kScreenH];
  static uint16_t bg[RadarView::kSize * RadarView::kSize];
  constexpr uint16_t kSentinel = 0xDEAD;

  RadarView rv;
  rv.obj_ = reinterpret_cast<lv_obj_t *>(1);  // only ever passed to the stub
  rv.bgCache_ = reinterpret_cast<uint8_t *>(bg);
  rv.bgImg_.header.stride = S * 2;
  rv.bgValid_ = true;
  rv.ox_ = ox;
  rv.oy_ = oy;
  rv.sweepRunning_ = true;
  lvstub::objCoords = {ox, oy, ox + S - 1, oy + S - 1};

  lv_draw_buf_t db = {};
  db.header.stride = kScreenW * 2;
  db.data = reinterpret_cast<uint8_t *>(fb);
  lv_layer_t layer = {};
  layer.draw_buf = &db;
  layer.buf_area = {0, 0, kScreenW - 1, kScreenH - 1};
  layer.color_format = LV_COLOR_FORMAT_RGB565;

  // A distinct value per pixel, so "was this written?" and "from the right
  // source pixel?" are both answerable.
  for (int32_t y = 0; y < S; ++y) {
    for (int32_t x = 0; x < S; ++x) {
      bg[y * S + x] = static_cast<uint16_t>((y * 37 + x * 11) | 1);
    }
  }

  // The whole view, a band, a vertical strip, and a small square: between them
  // these cut the wedge every way LVGL's region merging can.
  const lv_area_t clips[] = {
      {ox, oy, ox + S - 1, oy + S - 1},
      {ox, oy + 100, ox + S - 1, oy + 200},
      {ox + 180, oy, ox + 300, oy + S - 1},
      {ox + 200, oy + 200, ox + 260, oy + 260},
  };

  int stale = 0, altered = 0;
  for (float deg = 0.0f; deg < 360.0f; deg += 11.0f) {
    rv.sweepDeg_ = deg;
    for (const lv_area_t &clip : clips) {
      for (int32_t i = 0; i < kScreenW * kScreenH; ++i) fb[i] = kSentinel;
      layer._clip_area = clip;
      if (!rv.compositeBackground(&layer)) {
        check(false, "compositeBackground took the lv_draw_image fallback");
        return;
      }
      const float tailStart = wrap360(deg - RadarView::kSweepTailDeg);
      for (int32_t y = clip.y1; y <= clip.y2; ++y) {
        for (int32_t x = clip.x1; x <= clip.x2; ++x) {
          if (fb[y * kScreenW + x] == kSentinel) {
            if (stale < 5) printf("  stale deg=%.0f (%d,%d)\n", deg, x, y);
            ++stale;
            continue;
          }
          const bool inWedge =
              bruteInside(x - ox, y - oy, tailStart, RadarView::kSweepTailDeg);
          // A pixel within a degree of the wedge edge may go either way.
          const bool nearEdge =
              bruteInside(x - ox, y - oy, wrap360(tailStart - 1.0f),
                          RadarView::kSweepTailDeg + 2.0f) != inWedge;
          if (!inWedge && !nearEdge &&
              fb[y * kScreenW + x] != bg[(y - oy) * S + (x - ox)]) {
            if (altered < 5) printf("  altered outside wedge deg=%.0f (%d,%d)\n", deg, x, y);
            ++altered;
          }
        }
      }
    }
  }
  printf("composite: %d stale pixels, %d altered outside the wedge\n", stale, altered);
  check(stale == 0, "composite writes every pixel of the clip region");
  check(altered == 0, "composite leaves pixels outside the wedge untouched");
}

}  // namespace

int main() {
  testOpacityAndWrap();
  testSectorRowSpan();
  testSweepRegionsCoverWedge();
  reportBandOverhead();
  testCompositeWritesEveryPixel();
  printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", failures);
  return failures != 0;
}
