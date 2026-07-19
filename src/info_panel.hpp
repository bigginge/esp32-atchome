#pragma once

#include "aircraft.hpp"

#include <lvgl.h>

class InfoPanel {
 public:
  static constexpr int32_t kWidth = 320;
  static constexpr int32_t kHeight = 480;

  void create(lv_obj_t *parent);
  void showAircraft(const Aircraft *aircraft);
  void showStatus(const char *status);

 private:
  void setField(lv_obj_t *valueLabel, const char *value);

  lv_obj_t *panel_ = nullptr;
  lv_obj_t *statusLabel_ = nullptr;
  lv_obj_t *aircraft_ = nullptr;
  lv_obj_t *flight_ = nullptr;
  lv_obj_t *distance_ = nullptr;
  lv_obj_t *origin_ = nullptr;
  lv_obj_t *destination_ = nullptr;
  lv_obj_t *fieldsCont_ = nullptr;
};
