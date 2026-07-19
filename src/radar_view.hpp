#pragma once

#include "tracker.hpp"

#include <lvgl.h>

class RadarView {
 public:
  static constexpr int32_t kSize = 480;

  bool create(lv_obj_t *parent, Tracker *tracker, float rangeNm);
  void setRangeNm(float rangeNm);
  void redraw();

 private:
  static void onClicked(lv_event_t *e);

  void drawBackground(lv_layer_t *layer);
  void drawAircraft(lv_layer_t *layer, const Aircraft &ac, bool selected);
  void nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const;
  void pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const;
  lv_color_t colorForAircraft(const Aircraft &ac) const;
  lv_opa_t trailOpacity(const Aircraft &ac, uint8_t ageIndex, uint8_t count) const;

  Tracker *tracker_ = nullptr;
  lv_obj_t *canvas_ = nullptr;
  uint8_t *buf_ = nullptr;
  float rangeNm_ = 25.0f;
  float pxPerNm_ = 1.0f;
};
