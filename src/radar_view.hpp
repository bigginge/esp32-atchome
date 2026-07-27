#pragma once

#include "aircraft.hpp"
#include "theme.hpp"

#include <lvgl.h>

class RadarView {
 public:
  static constexpr int32_t kSize = theme::layout::kRadarSize;
  static constexpr int kRings = 4;

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

  /** Advance the PPI sweep on wall-clock time and, if it has moved far enough
   *  to be worth repainting, invalidate the band it swept through.
   *
   *  Call this every loop() iteration. It is deliberately independent of
   *  redraw(), which skips painting entirely when nothing moved a whole pixel;
   *  that skip is what leaves any headroom at all, and the sweep must not
   *  defeat it. Running, the sweep costs roughly half of it: loop() sits at
   *  45-150/s with the sweep on versus 150-250/s with it off. The sweep
   *  contributes exactly one extra
   *  invalid region per tick (each region costs ~5 ms of fixed draw-pass
   *  overhead), covering the union of where the wedge was and where it now is.
   *  The background cache is what erases the old wedge, which is why the old
   *  extent has to be part of that union. */
  void tickSweep(unsigned long nowMs);

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
  /** Paint straight into the display's layer, i.e. the framebuffer. */
  void paintDirect(lv_layer_t *layer);

  /** Rects this frame touches: symbol boxes, label boxes and leader lines. */
  void collectDirty();
  /** Invalidate last frame's rects and this frame's, so the old pixels are
   *  restored from the background cache and the new ones drawn. */
  void invalidateDirty();
  /** True if `r` (local coords) lies outside the region being painted. LVGL
   *  calls the draw handler once per invalidated region, so without this every
   *  region would build draw tasks for every aircraft on screen. */
  bool culled(const Rect &r) const;

  void drawBackground(lv_layer_t *layer);
  void drawLegend(lv_layer_t *layer);

  // ---- PPI sweep -----------------------------------------------------------
  // One line to switch the whole feature off.
  static constexpr bool kSweepEnabled = true;
  // Outer annulus rather than a full pie. It covers roughly a third of the
  // pixels *and* declutters the busy centre, where HOME and the densest labels
  // live -- and because the inner radius clips the near-centre corner off the
  // wedge's bounding box, it also shrinks the invalidated region, which is what
  // actually costs money here. Several real PPI displays do exactly this.
  static constexpr bool kSweepAnnulusOnly = true;
  // 6 s per revolution rather than the 4 s originally planned. A tick costs a
  // whole extra LVGL refresh cycle -- ~7-11 ms rasterising the wedge, ~8 ms
  // restoring the background over the band, plus a ~20 ms VSYNC wait on a
  // 24 Hz panel, so roughly 35-40 ms all in. The tick *rate* is therefore the
  // lever, not the pixel count. Slowing the revolution keeps the angular step,
  // and so the apparent smoothness, identical while cutting the rate to 8/s.
  static constexpr float kSweepPeriodMs = 6000.0f;  // one revolution
  static constexpr float kSweepTailDeg = 50.0f;
  // Repaint every 7.5 deg: 8 ticks/s at 6 s/rev. Keeping tail + step under
  // 90 deg also holds lv_draw_arc_get_area() on its tight two-quadrant path
  // rather than its full-circle fallback.
  static constexpr float kSweepStepDeg = 7.5f;
  static constexpr int kSweepSegments = 4;
  static constexpr uint8_t kSweepOpa = 110;  // leading segment; the tail fades
  // Governor rungs: 0 = full rate, each rung halves it, and this one is "off".
  static constexpr uint8_t kSweepOffRung = 3;

  void drawSweep(lv_layer_t *layer);
  /** Local-coordinate extent of the whole wedge with its head at `headDeg`
   *  (LVGL angle convention: 0 = east, increasing clockwise on screen). */
  Rect sweepWedgeBox(float headDeg) const;
  /** How far the head must move before the sweep is worth a repaint -- i.e.
   *  the sweep's frame rate, not its rotation rate, which is wall-clock driven
   *  and never varies. Widens when loop() is under pressure, in the same spirit
   *  as trailBudget() thinning the trails: the sweep is decoration and the
   *  aircraft are the product, so the sweep gives ground first. */
  float sweepStepDeg() const;
  /** Invalidate the union of two wedge extents as a single region. */
  void invalidateSweepBand(const Rect &prev, const Rect &now);
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
  // with a fixed overhead of ~5 ms on top of its area-proportional cost, so
  // this trades clipping tightness against pass count. Measured at 6-7
  // aircraft: 2 regions => 8.0 fps, 4 => 11.6 fps, 8 => 9.9 fps.
  static constexpr size_t kMaxRegions = 4;
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

  // Sweep state. sweepDeg_ accumulates continuously; sweepDrawnDeg_ is the
  // angle that was last invalidated and is therefore what paintDirect() draws,
  // so what is on screen always matches what was asked to be repainted.
  float sweepDeg_ = 270.0f;       // 270 in LVGL angles == due north
  float sweepDrawnDeg_ = 270.0f;
  unsigned long sweepLastMs_ = 0;
  bool sweepStarted_ = false;
  bool sweepVisible_ = false;  // false until the first tick, and at kSweepOffRung
  Rect sweepBox_ = {0, 0, 0, 0};
  // Frame-rate governor: 0 = full rate, each rung halves it, kSweepOffRung is off.
  unsigned long sweepRateAtMs_ = 0;
  uint16_t sweepCalls_ = 0;
  uint32_t sweepHzAvg_ = 0;  // EMA of the loop rate; 0 until the first window
  uint8_t sweepRung_ = 0;

  volatile bool pendingClick_ = false;
  float pendingEast_ = 0.0f;
  float pendingNorth_ = 0.0f;
};

/** Colour for an altitude (feet). Shared by the radar and the legend so they
 *  always agree. Ground / unknown altitude renders grey. */
lv_color_t altitudeColor(int altitudeFt);
