#include "info_panel.hpp"

#include <stdio.h>
#include <string.h>

static lv_obj_t *addField(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut) {
  lv_obj_t *block = lv_obj_create(parent);
  lv_obj_set_width(block, LV_PCT(100));
  lv_obj_set_height(block, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(block, 0, 0);
  lv_obj_set_style_pad_all(block, 0, 0);
  lv_obj_set_style_pad_bottom(block, 10, 0);
  lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(block, 2, 0);
  lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *captionLabel = lv_label_create(block);
  lv_label_set_text(captionLabel, caption);
  lv_obj_set_style_text_color(captionLabel, lv_color_hex(0x7A8A9A), 0);
  lv_obj_set_style_text_font(captionLabel, &lv_font_montserrat_14, 0);

  lv_obj_t *valueLabel = lv_label_create(block);
  lv_label_set_text(valueLabel, "—");
  lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(valueLabel, LV_PCT(100));
  lv_obj_set_style_text_color(valueLabel, lv_color_hex(0xE8EEF4), 0);
  lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_16, 0);
  *valueOut = valueLabel;
  return block;
}

void InfoPanel::create(lv_obj_t *parent) {
  panel_ = lv_obj_create(parent);
  lv_obj_set_size(panel_, kWidth, kHeight);
  lv_obj_set_pos(panel_, 480, 0);
  lv_obj_set_style_bg_color(panel_, lv_color_hex(0x121A22), 0);
  lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel_, 0, 0);
  lv_obj_set_style_radius(panel_, 0, 0);
  lv_obj_set_style_pad_left(panel_, 16, 0);
  lv_obj_set_style_pad_right(panel_, 16, 0);
  lv_obj_set_style_pad_top(panel_, 16, 0);
  lv_obj_set_style_pad_bottom(panel_, 16, 0);
  lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel_, 8, 0);
  lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel_);
  lv_label_set_text(title, "Aircraft");
  lv_obj_set_style_text_color(title, lv_color_hex(0xF0F4F8), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

  statusLabel_ = lv_label_create(panel_);
  lv_label_set_text(statusLabel_, "Connecting...");
  lv_label_set_long_mode(statusLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel_, LV_PCT(100));
  lv_obj_set_style_text_color(statusLabel_, lv_color_hex(0xA0B0C0), 0);
  lv_obj_set_style_text_font(statusLabel_, &lv_font_montserrat_14, 0);

  fieldsCont_ = lv_obj_create(panel_);
  lv_obj_set_width(fieldsCont_, LV_PCT(100));
  lv_obj_set_flex_grow(fieldsCont_, 1);
  lv_obj_set_style_bg_opa(fieldsCont_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(fieldsCont_, 0, 0);
  lv_obj_set_style_pad_all(fieldsCont_, 0, 0);
  lv_obj_set_flex_flow(fieldsCont_, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(fieldsCont_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(fieldsCont_, LV_OBJ_FLAG_SCROLLABLE);

  addField(fieldsCont_, "Manufacturer", &manufacturer_);
  addField(fieldsCont_, "Type", &type_);
  addField(fieldsCont_, "Registration", &registration_);
  addField(fieldsCont_, "Flight", &flight_);
  addField(fieldsCont_, "Distance", &distance_);
  addField(fieldsCont_, "Origin", &origin_);
  addField(fieldsCont_, "Destination", &destination_);
}

void InfoPanel::setField(lv_obj_t *valueLabel, const char *value) {
  if (valueLabel == nullptr) {
    return;
  }
  if (value == nullptr || value[0] == '\0') {
    lv_label_set_text(valueLabel, "—");
  } else {
    lv_label_set_text(valueLabel, value);
  }
}

void InfoPanel::showStatus(const char *status) {
  if (statusLabel_ != nullptr) {
    lv_label_set_text(statusLabel_, status != nullptr ? status : "");
    lv_obj_clear_flag(statusLabel_, LV_OBJ_FLAG_HIDDEN);
  }
  if (fieldsCont_ != nullptr) {
    lv_obj_add_flag(fieldsCont_, LV_OBJ_FLAG_HIDDEN);
  }
}

void InfoPanel::showAircraft(const Aircraft *aircraft) {
  if (aircraft == nullptr) {
    showStatus("No aircraft nearby");
    return;
  }

  if (statusLabel_ != nullptr) {
    lv_obj_add_flag(statusLabel_, LV_OBJ_FLAG_HIDDEN);
  }
  if (fieldsCont_ != nullptr) {
    lv_obj_clear_flag(fieldsCont_, LV_OBJ_FLAG_HIDDEN);
  }

  const char *typeText = aircraft->typeDescription[0] != '\0' ? aircraft->typeDescription
                                                              : aircraft->typeCode;
  const char *flightText =
      aircraft->callsign[0] != '\0' ? aircraft->callsign : aircraft->hex;

  setField(manufacturer_, aircraft->manufacturer);
  setField(type_, typeText);
  setField(registration_, aircraft->registration);
  setField(flight_, flightText);

  char distBuf[32];
  snprintf(distBuf, sizeof(distBuf), "%.1f nm", aircraft->distanceNm);
  setField(distance_, distBuf);

  setField(origin_, aircraft->origin);
  setField(destination_, aircraft->destination);
}
