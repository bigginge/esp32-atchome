#pragma once

#include "aircraft.hpp"

#include <lvgl.h>

class InfoPanel {
 public:
  static constexpr int32_t kWidth = 320;
  static constexpr int32_t kHeight = 480;

  void create(lv_obj_t *parent);
  void setStats(size_t inRange, long secondsSinceUpdate);
  void showAircraft(const Aircraft *aircraft);
  void showStatus(const char *status);
  void updateClock();

 private:
  // Hides the whole caption+value block when the value is empty.
  void setField(lv_obj_t *valueLabel, const char *value);
  // Hides only the label itself; for labels that share a block with siblings.
  void setLabelText(lv_obj_t *label, const char *value);

  lv_obj_t *panel_ = nullptr;
  lv_obj_t *clockContainer_ = nullptr;
  lv_obj_t *timeLabel_ = nullptr;
  lv_obj_t *dateLabel_ = nullptr;
  lv_obj_t *statsLabel_ = nullptr;
  lv_obj_t *statusLabel_ = nullptr;
  lv_obj_t *aircraft_ = nullptr;
  lv_obj_t *flight_ = nullptr;
  lv_obj_t *distance_ = nullptr;
  lv_obj_t *altitude_ = nullptr;
  lv_obj_t *speed_ = nullptr;
  lv_obj_t *routeBlock_ = nullptr;
  lv_obj_t *routeCodes_ = nullptr;
  lv_obj_t *originName_ = nullptr;
  lv_obj_t *destName_ = nullptr;
  lv_obj_t *fieldsCont_ = nullptr;
};
