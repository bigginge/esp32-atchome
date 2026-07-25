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
  static void onClicked(lv_event_t *e);

  void drawBackground(lv_layer_t *layer);
  void drawAircraft(lv_layer_t *layer, const Aircraft &ac, bool selected);
  void drawLegend(lv_layer_t *layer);
  void nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const;
  void pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const;
  lv_color_t colorForAircraft(const Aircraft &ac) const;
  lv_opa_t trailOpacity(const Aircraft &ac, uint8_t ageIndex, uint8_t count) const;

  lv_obj_t *canvas_ = nullptr;
  uint8_t *buf_ = nullptr;
  float rangeNm_ = 25.0f;
  float pxPerNm_ = 1.0f;

  const Aircraft *snap_ = nullptr;
  size_t snapCount_ = 0;
  char selectedHex_[8] = {0};

  volatile bool pendingClick_ = false;
  float pendingEast_ = 0.0f;
  float pendingNorth_ = 0.0f;
};

/** Colour for an altitude (feet). Shared by the radar and the legend so they
 *  always agree. Ground / unknown altitude renders grey. */
lv_color_t altitudeColor(int altitudeFt);
