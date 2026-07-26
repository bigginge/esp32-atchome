#pragma once

#include "aircraft.hpp"

#include <lvgl.h>

class RadarView {
 public:
  static constexpr int32_t kSize = 480;

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

 private:
  /** The canvas is 480x480, so int16 is ample. Half the size of lv_area_t,
   *  which matters because RadarView is a global and these land in .bss —
   *  internal SRAM, the scarce resource here. */
  struct Rect {
    int16_t x1, y1, x2, y2;
  };

  /** One aircraft resolved to canvas pixels, ready to draw. */
  struct Placed {
    const Aircraft *ac;
    int32_t x, y;     // symbol centre
    int16_t symbolR;  // half-extent of the symbol's bounding box
    bool selected;
  };

  /** Last frame's label placement, so a label that is still free keeps its
   *  spot instead of hopping when the distance ordering shuffles. */
  struct AnchorMemo {
    char hex[8];
    uint8_t form, anchor;
  };

  static constexpr size_t kAnchorCount = 12;
  static constexpr size_t kLabelForms = 3;
  static constexpr size_t kStaticBlockers = 6;  // legend + HOME + 4 ring labels
  static constexpr size_t kMaxBlockers = kStaticBlockers + kMaxAircraft * 2;

  static void onClicked(lv_event_t *e);

  void drawBackground(lv_layer_t *layer);
  void drawLegend(lv_layer_t *layer);
  void nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const;
  void pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const;
  lv_color_t colorForAircraft(const Aircraft &ac) const;
  lv_opa_t trailOpacity(const Aircraft &ac, uint8_t ageIndex, uint8_t count,
                        bool selected) const;

  void buildDrawOrder();
  void seedStaticBlockers();
  uint8_t trailBudget(bool selected) const;
  void drawTrail(lv_layer_t *layer, const Placed &p);
  void drawSymbol(lv_layer_t *layer, const Placed &p);
  void drawLabels(lv_layer_t *layer);
  bool anchorRect(const Placed &p, uint8_t anchor, int32_t w, int32_t h,
                  Rect *out) const;
  bool blocked(const Rect &r, size_t skip) const;
  void addBlocker(const Rect &r);
  uint8_t rememberedAnchor(const char *hex, uint8_t *form) const;
  static bool rectsOverlap(const Rect &a, const Rect &b);
  static void formatLabel(const Aircraft &ac, uint8_t form, char *buf, size_t n);

  lv_obj_t *canvas_ = nullptr;
  uint8_t *buf_ = nullptr;
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
  float trailScale_ = 1.0f;
  AnchorMemo memo_[kMaxAircraft];
  size_t memoCount_ = 0;

  volatile bool pendingClick_ = false;
  float pendingEast_ = 0.0f;
  float pendingNorth_ = 0.0f;
};

/** Colour for an altitude (feet). Shared by the radar and the legend so they
 *  always agree. Ground / unknown altitude renders grey. */
lv_color_t altitudeColor(int altitudeFt);
