#pragma once

#include "aircraft.hpp"
#include "theme.hpp"

#include <lvgl.h>

class RadarView {
 public:
  static constexpr int32_t kSize = theme::layout::kRadarSize;
  static constexpr int kRings = 4;

  // ---- Sweep beam ----------------------------------------------------------
  // A rotating PPI beam: a hot leading edge with a wedge of phosphor decay
  // behind it, composited under the aircraft.
  //
  // COST MODEL, because these four numbers are the whole performance story and
  // it is worth being able to reason about a change before flashing it. Every
  // sweep step repaints the wedge swept since the last one *plus* the whole
  // tail, because the tail's opacity is a function of angular distance from the
  // head and therefore every pixel of it changes when the head moves. That is
  //
  //     px/step ~= pi * kSweepRadius^2 * (kSweepTailDeg + step) / 360
  //
  // at roughly 4.7 Mpx/s for the read-blend-write (PSRAM to PSRAM, contended
  // with the LCD scan-out -- see benchPsram() in main.cpp), plus ~40% for the
  // slack in the bounding rects handed to LVGL. At the defaults below that is
  // ~25 kpx, ~7 ms, twelve and a half times a second: about a fifth of one core.
  //
  // So: a longer tail or a faster step both cost linearly. Widening the tail
  // past 170 deg is not merely expensive, it is wrong -- the row-span solver in
  // sectorRowSpan() assumes a convex sector.
  static constexpr bool kSweepEnabled = true;
  static constexpr uint32_t kSweepPeriodMs = 6000;  // one revolution
  static constexpr uint32_t kSweepStepMs = 80;      // 12.5 steps/s
  static constexpr float kSweepTailDeg = 55.0f;
  static constexpr int32_t kSweepRadius = kSize / 2 - 8;  // == the outer ring

  bool create(lv_obj_t *parent, float rangeNm);
  void setRangeNm(float rangeNm);

  /** Point the view at the array to render on the next redraw(). The array is
   *  owned by the caller (the UI task's private render copy) and must stay
   *  valid until the following redraw() returns. */
  void setSnapshot(const Aircraft *list, size_t count, const char *selectedHex);
  void redraw();

  /** If the user tapped since the last call, returns the tap position in
   *  east/north nm from home and clears the pending flag. */
  bool consumePendingClick(float *eastNm, float *northNm);

  /** Force the next redraw to repaint the whole view. Needed whenever
   *  something outside the moving parts changed: new data (trails grow), a
   *  range change, or the first paint. Ordinary frames only invalidate the
   *  symbols and labels, which is the point -- between fetches an aircraft
   *  moves about a pixel a second, so almost nothing needs repainting. */
  void markFullRepaint() { fullRepaint_ = true; }

  /** New positions arrived, so the trails grew. Cheaper than a full repaint:
   *  only the trail extents need restoring, not the whole view. A full repaint
   *  here cost ~200 ms and landed every 5 seconds, on every fetch. */
  void markDataChanged() { trailsChanged_ = true; }

 private:
  /** The canvas is a few hundred px square, so int16 is ample. Half the size of
   *  lv_area_t, which matters because RadarView is a global and these land in
   *  .bss — internal SRAM, the scarce resource here. */
  struct Rect {
    int16_t x1, y1, x2, y2;
  };

  /** One aircraft resolved to canvas pixels, ready to draw. */
  struct Placed {
    const Aircraft *ac;
    int32_t x, y;     // symbol centre
    int16_t symbolR;  // half-extent of the symbol's bounding box
    bool selected;
    Rect box;  // symbol + trail extent, for dirty tracking and clip culling
    // How much the beam is currently flaring this blip, 0 when the beam is
    // elsewhere. Quantised in buildDrawOrder() so that a symbol is only
    // reinvalidated when the flare visibly steps, not on every sweep frame.
    uint8_t lit;
  };

  /** Last frame's label placement, so a label that is still free keeps its
   *  spot instead of hopping when the distance ordering shuffles. */
  struct AnchorMemo {
    char hex[8];
    uint8_t form, anchor;
  };

  /** A label the solver placed, resolved to pixels and ready to paint.
   *
   *  The anchor search is global -- it depends on every symbol and every label
   *  already placed -- so it must run exactly once per frame. Painting, by
   *  contrast, runs once per invalidated region. Keeping the solved result here
   *  is what lets the two run at different rates. */
  struct LabelPlacement {
    Rect rect;
    char text[24];
    int16_t leadX1, leadY1, leadX2, leadY2;
    uint8_t orderIdx;  // index into order_, for colour and selection
    bool hasLeader;
  };

  static constexpr size_t kAnchorCount = 12;
  static constexpr size_t kLabelForms = 3;
  // Sizing bound only: legend + HOME + kRings ring labels, with headroom for
  // the chrome still to come. The count that drawLabels() indexes against is
  // staticBlockerCount_, recorded by seedStaticBlockers() -- do not use this
  // constant for that, or adding one blocker silently shifts every index.
  static constexpr size_t kStaticBlockerCap = 12;
  static constexpr size_t kMaxBlockers = kStaticBlockerCap + kMaxAircraft * 2;

  static void onClicked(lv_event_t *e);
  static void onDraw(lv_event_t *e);
  static void onCoverCheck(lv_event_t *e);

  /** Rasterise the static chrome into bgCache_. Uses a transient hidden
   *  lv_canvas -- the only public route to drawing into an arbitrary buffer.
   *  Runs at create and whenever the range changes, never per frame. */
  void renderBackgroundCache();

  /** Re-encode bgCache_ as per-row RLE in *internal* SRAM. The restore is the
   *  dominant per-pixel cost of every draw pass, and there are ~7 passes per
   *  paint, so this is the hottest read in the view: sourcing it from SRAM
   *  instead of PSRAM removes ~450 KB of PSRAM reads per pass and moves the
   *  write off the memcpy path (9.4 MB/s) onto the fill path (15.2 MB/s).
   *
   *  Best-effort. Returns false and leaves bgRleValid_ clear if the encoding
   *  does not fit the internal-SRAM budget or either allocation fails, in
   *  which case every caller falls back to the bgCache_ memcpy unchanged. That
   *  fallback is why this is safe to land without the host tests. */
  bool buildBackgroundRle();
  /** Decode background pixels [x1..x2] of local row `row` into `dst`.
   *  Coordinates are local to the cache, i.e. already offset by ox_/oy_. */
  void rleSpan(int32_t row, int32_t x1, int32_t x2, uint16_t *dst) const;
  /** Paint straight into the display's layer, i.e. the framebuffer. */
  void paintDirect(lv_layer_t *layer);

  /** Rects this frame touches: symbol boxes, label boxes and leader lines. */
  void collectDirty();
  /** Invalidate the areas whose pixels differ from what is already in the
   *  buffer. On a content frame that is last frame's rects and this frame's, so
   *  the old pixels are restored from the background cache and the new ones
   *  drawn. On a sweep-only frame it is just the swept wedge plus any blip
   *  whose flare stepped -- nothing else moved. */
  void invalidateDirty(bool contentChanged);
  /** True if `r` (local coords) lies outside the region being painted. LVGL
   *  calls the draw handler once per invalidated region, so without this every
   *  region would build draw tasks for every aircraft on screen. */
  bool culled(const Rect &r) const;

  void drawBackground(lv_layer_t *layer);
  void drawLegend(lv_layer_t *layer);
  /** Radius in px of range ring `i` (1..kRings). */
  int32_t ringRadius(int i) const;
  /** Box occupied by ring `i`'s nm caption. drawBackground() draws the text and
   *  seedStaticBlockers() reserves the space; both must agree, so both call
   *  this rather than repeating the placement maths. */
  Rect ringLabelRect(int i) const;
  void nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const;
  void pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const;
  lv_color_t colorForAircraft(const Aircraft &ac) const;
  lv_opa_t trailOpacity(const Aircraft &ac, uint8_t ageIndex, uint8_t count,
                        bool selected) const;

  /** Everything that affects a pixel: positions, altitude (colour), selection.
   *  Compared against the last painted frame to decide whether to paint at
   *  all. Cheap to compute -- it runs over order_, which buildDrawOrder() has
   *  just filled in. */
  uint32_t paintSignature() const;

  // ---- Sweep beam ----------------------------------------------------------
  /** Leading-edge bearing at `nowMs`, degrees clockwise from north. Derived
   *  from the clock rather than accumulated, so a dropped frame slips the beam
   *  rather than slowing it. */
  static float sweepAngleAt(uint32_t nowMs);
  /** Angular distance from `headDeg` backwards to `bearingDeg`, in [0, 360). */
  static float degreesBehind(float bearingDeg, float headDeg);
  /** Beam opacity `behindDeg` behind the leading edge; 0 past the tail. */
  static lv_opa_t sweepOpacity(float behindDeg);
  /** Row span of the circular sector between two rays, at local row `y`.
   *  The rays are passed as direction vectors because the caller hoists the
   *  trigonometry out of the row loop. Requires a sector of at most 180 deg:
   *  that is what makes the sector convex, and therefore what makes the
   *  intersection with a row a single interval. */
  static bool sectorRowSpan(float d0x, float d0y, float d1x, float d1y,
                            int32_t y, int32_t *xlo, int32_t *xhi);
  /** Bounding rects of the wedge swept from `fromDeg` to `toDeg` (including
   *  the tail behind both), banded by row so they hug the wedge. */
  void buildSweepRegions(float fromDeg, float toDeg);
  /** Restore the background over the clip region and composite the beam into
   *  it, writing straight into the layer's buffer. Returns false if the layer
   *  is not the plain RGB565 framebuffer this knows how to write, in which case
   *  the caller falls back to lv_draw_image. */
  bool compositeBackground(lv_layer_t *layer);

  void buildDrawOrder();
  void seedStaticBlockers();
  uint8_t trailBudget(bool selected) const;
  void drawTrail(lv_layer_t *layer, const Placed &p);
  void drawSymbol(lv_layer_t *layer, const Placed &p);
  void layoutLabels();
  void paintLabels(lv_layer_t *layer);
  bool anchorRect(const Placed &p, uint8_t anchor, int32_t w, int32_t h,
                  Rect *out) const;
  bool blocked(const Rect &r, size_t skip) const;
  void addBlocker(const Rect &r);
  uint8_t rememberedAnchor(const char *hex, uint8_t *form) const;
  static bool rectsOverlap(const Rect &a, const Rect &b);
  static void formatLabel(const Aircraft &ac, uint8_t form, char *buf, size_t n);

  lv_obj_t *obj_ = nullptr;
  // The static chrome, rasterised once. Painted per frame with lv_draw_image,
  // which takes the same per-row memcpy fast path as the old canvas blit but
  // only for the clipped region -- that is the whole point of the change.
  uint8_t *bgCache_ = nullptr;
  lv_image_dsc_t bgImg_ = {};
  bool bgValid_ = false;

  // ---- Background RLE, in internal SRAM ------------------------------------
  // Token stream, one run list per row, indexed by bgRleRow_[row]:
  //   FILL  [count][value]                 count < 0x8000, covers `count` px
  //   LIT   [0x8000|count][v0]..[vN-1]     `count` literal pixels
  // A run of 3+ identical pixels is cheaper as a FILL; anything shorter is
  // folded into a LIT block, which amortises its one-token header across the
  // whole stretch of dissimilar pixels. Rows here are a handful of flat runs
  // plus scattered literals (crosshairs, arcs, captions), which is the shape
  // this encoding is for.
  uint16_t *bgRle_ = nullptr;
  uint32_t *bgRleRow_ = nullptr;
  /** One decoded row. The beam blends *over* the background, so pass two needs
   *  random-access source pixels, not just a fill -- it decodes into this and
   *  blends from it, which keeps that read in SRAM too. */
  uint16_t *bgLine_ = nullptr;
  bool bgRleValid_ = false;
  // Screen-absolute origin of the view. Drawing now goes into the display's
  // layer rather than a canvas of our own, so every local coordinate needs it.
  // Zero while rendering the cache, which is in local coordinates.
  int32_t ox_ = 0;
  int32_t oy_ = 0;
  float rangeNm_ = 25.0f;
  float pxPerNm_ = 1.0f;

  const Aircraft *snap_ = nullptr;
  size_t snapCount_ = 0;
  char selectedHex_[8] = {0};

  Placed order_[kMaxAircraft];        // altitude ascending, selected forced last
  uint8_t labelOrder_[kMaxAircraft];  // indices into order_: selected, then nearest
  size_t orderCount_ = 0;
  Rect blockers_[kMaxBlockers];
  size_t blockerCount_ = 0;
  size_t staticBlockerCount_ = 0;  // set by seedStaticBlockers()
  float trailScale_ = 1.0f;
  AnchorMemo memo_[kMaxAircraft];
  size_t memoCount_ = 0;
  LabelPlacement labels_[kMaxAircraft];
  size_t labelCount_ = 0;

  // Symbol box + label box per aircraft, this frame and last.
  static constexpr size_t kMaxDirty = kMaxAircraft * 2;
  // How many separate invalid areas to hand LVGL. Each costs a full draw pass
  // with a fixed overhead on top of its area-proportional cost, so this trades
  // clipping tightness against pass count. Measured at 6-7 aircraft, before the
  // legend and per-segment culling below cut the fixed part: 2 regions => 8.0
  // fps, 4 => 11.6 fps, 8 => 9.9 fps. The sweep needs its own bands on top,
  // and LVGL only merges invalid areas when doing so *shrinks* them, so these
  // two budgets add rather than compete.
  static constexpr size_t kMaxRegions = 4;
  // The swept wedge is banded by row before being handed over, because a wedge
  // fits its own bounding box badly. Dead area against the true wedge, measured
  // over a full revolution by reportBandOverhead() in tests/host:
  //
  //   bands   2      3      4      5      6      8
  //   mean    1.57x  1.43x  1.34x  1.28x  1.24x  1.18x
  //   worst   1.87x  1.59x  1.44x  1.36x  1.30x  1.22x
  //
  // Four is where the curve flattens: going to eight buys 14% of the wedge's
  // pixels back and costs four more draw passes to do it.
  static constexpr size_t kSweepBands = 4;
  // Above this many rects, bucket by position before the exact pairwise merge.
  // See invalidateDirty(): the exact merge is O(n^3) and n runs to 4*kMaxAircraft.
  static constexpr size_t kMergeExactLimit = 24;
  static constexpr size_t kMergeGrid = 4;  // kMergeGrid^2 buckets
  Rect dirty_[kMaxDirty];
  size_t dirtyCount_ = 0;
  Rect prevDirty_[kMaxDirty];
  size_t prevDirtyCount_ = 0;
  bool fullRepaint_ = true;
  bool trailsChanged_ = false;
  uint32_t lastPaintSig_ = 0;
  bool hasPainted_ = false;
  // The region currently being painted, in local coordinates.
  Rect clip_ = {0, 0, 0, 0};

  // Leading edge of the beam, and where it was when the buffer was last
  // painted. The gap between the two is what has to be repainted, so they are
  // separate: a frame the paint gate skipped still has to be caught up on.
  float sweepDeg_ = 0.0f;
  float sweepPaintedDeg_ = 0.0f;
  uint32_t sweepStepMs_ = 0;
  bool sweepRunning_ = false;
  Rect sweepRegion_[kSweepBands];
  size_t sweepRegionCount_ = 0;
  // Last painted flare level per draw-order slot. Only meaningful while the
  // content is unchanged, which is exactly when it is read -- a content frame
  // repaints every symbol anyway.
  uint8_t litPainted_[kMaxAircraft] = {0};

  volatile bool pendingClick_ = false;
  float pendingEast_ = 0.0f;
  float pendingNorth_ = 0.0f;
};

/** Colour for an altitude (feet). Shared by the radar and the legend so they
 *  always agree. Ground / unknown altitude renders grey. */
lv_color_t altitudeColor(int altitudeFt);
