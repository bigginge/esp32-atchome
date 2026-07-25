#include "info_panel.hpp"

#include <Arduino.h>
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

  // Large clock display at the top.
  clockContainer_ = lv_obj_create(panel_);
  lv_obj_set_height(clockContainer_, LV_SIZE_CONTENT);
  lv_obj_set_width(clockContainer_, LV_PCT(100));
  lv_obj_set_style_bg_color(clockContainer_, lv_color_hex(0x1E2A36), 0);
  lv_obj_set_style_bg_opa(clockContainer_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(clockContainer_, 8, 0);
  lv_obj_set_style_border_width(clockContainer_, 0, 0);
  lv_obj_set_style_pad_all(clockContainer_, 16, 0);
  lv_obj_set_flex_flow(clockContainer_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(clockContainer_, 4, 0);

  timeLabel_ = lv_label_create(clockContainer_);
  lv_label_set_text(timeLabel_, "--:--:--");
  lv_obj_set_style_text_color(timeLabel_, lv_color_hex(0xE8EEF4), 0);
  lv_obj_set_style_text_font(timeLabel_, &lv_font_montserrat_48, 0);

  dateLabel_ = lv_label_create(clockContainer_);
  lv_label_set_text(dateLabel_, "------------");
  lv_obj_set_style_text_color(dateLabel_, lv_color_hex(0x7A8A9A), 0);
  lv_obj_set_style_text_font(dateLabel_, &lv_font_montserrat_14, 0);

  locationLabel_ = lv_label_create(clockContainer_);
  lv_label_set_text(locationLabel_, LV_SYMBOL_GPS " —");
  lv_obj_set_style_text_color(locationLabel_, lv_color_hex(0xA0B0C0), 0);
  lv_obj_set_style_text_font(locationLabel_, &lv_font_montserrat_14, 0);

  statsLabel_ = lv_label_create(clockContainer_);
  lv_label_set_text(statsLabel_, "");
  lv_obj_set_style_text_color(statsLabel_, lv_color_hex(0x7A8A9A), 0);
  lv_obj_set_style_text_font(statsLabel_, &lv_font_montserrat_14, 0);

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

  addField(fieldsCont_, "Aircraft", &aircraft_);
  addField(fieldsCont_, "Flight", &flight_);
  addField(fieldsCont_, LV_SYMBOL_GPS " Distance", &distance_);
  addField(fieldsCont_, LV_SYMBOL_UP " Altitude", &altitude_);
  addField(fieldsCont_, LV_SYMBOL_REFRESH " Speed", &speed_);
  addField(fieldsCont_, "Origin", &origin_);
  addField(fieldsCont_, "Destination", &destination_);

  // Promote the primary aircraft line for a clearer hierarchy.
  lv_obj_set_style_text_font(aircraft_, &lv_font_montserrat_20, 0);
}

void InfoPanel::setLocation(const char *name) {
  if (locationLabel_ == nullptr) return;
  char buf[64];
  snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %s",
           (name != nullptr && name[0] != '\0') ? name : "Home");
  lv_label_set_text(locationLabel_, buf);
}

void InfoPanel::setStats(size_t inRange, long secondsSinceUpdate) {
  if (statsLabel_ == nullptr) return;
  char buf[48];
  if (secondsSinceUpdate < 0) {
    snprintf(buf, sizeof(buf), "%u in range", static_cast<unsigned>(inRange));
  } else {
    snprintf(buf, sizeof(buf), "%u in range " LV_SYMBOL_BULLET " updated %lds ago",
             static_cast<unsigned>(inRange), secondsSinceUpdate);
  }
  lv_label_set_text(statsLabel_, buf);
}

void InfoPanel::updateClock() {
  if (timeLabel_ == nullptr || dateLabel_ == nullptr) return;

  struct tm timeinfo;
  // Non-blocking: never stall the UI waiting for NTP. Once time is set this
  // returns immediately; before sync it simply leaves the placeholder text.
  if (!getLocalTime(&timeinfo, 5)) {
    return;
  }
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  lv_label_set_text(timeLabel_, buf);

  const char months[] = "Jan\0Feb\0Mar\0Apr\0May\0Jun\0Jul\0Aug\0Sep\0Oct\0Nov\0Dec";
  char dateBuf[32];
  snprintf(dateBuf, sizeof(dateBuf), "%d %s %d", timeinfo.tm_mday, months + (timeinfo.tm_mon * 4), 1900 + timeinfo.tm_year);
  lv_label_set_text(dateLabel_, dateBuf);
}

void InfoPanel::setField(lv_obj_t *valueLabel, const char *value) {
  if (valueLabel == nullptr) {
    return;
  }
  lv_obj_t *block = lv_obj_get_parent(valueLabel);
  if (value == nullptr || value[0] == '\0') {
    lv_obj_add_flag(block, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(valueLabel, value);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_HIDDEN);
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

  String buf;
  buf.reserve(128);

  if (aircraft->manufacturer[0]) buf += aircraft->manufacturer;
  if (typeText[0]) {
    if (buf.length()) buf += " ";
    buf += typeText;
  }
  if (aircraft->registration[0]) {
    if (buf.length()) buf += " ";
    buf += "(";
    buf += aircraft->registration;
    buf += ")";
  }
  setField(aircraft_, buf.c_str());

  buf = "";
  if (aircraft->registeredOwner[0]) {
    buf += aircraft->registeredOwner;
    buf += " ";
  }
  if (flightText[0]) buf += flightText;
  setField(flight_, buf.c_str());

  char distBuf[32];
  snprintf(distBuf, sizeof(distBuf), "%.1f nm", aircraft->distanceNm);
  setField(distance_, distBuf);

  char altBuf[32];
  snprintf(altBuf, sizeof(altBuf), "%d ft", aircraft->altitudeFt);
  setField(altitude_, altBuf);

  if (aircraft->groundSpeedKts > 0.0f) {
    char spdBuf[32];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f kt", aircraft->groundSpeedKts);
    setField(speed_, spdBuf);
  } else {
    setField(speed_, "");
  }

  buf = "";
  if (aircraft->origin[0]) buf += aircraft->origin;
  if (aircraft->originIcao[0]) {
    if (buf.length() > 0 && strcmp(aircraft->origin, aircraft->originIcao) != 0) {
      buf += " (";
      buf += aircraft->originIcao;
      buf += ")";
    } else if (buf.length() == 0) {
      buf += aircraft->originIcao;
    }
  }
  setField(origin_, buf.c_str());

  buf = "";
  if (aircraft->destination[0]) buf += aircraft->destination;
  if (aircraft->destinationIcao[0]) {
    if (buf.length() > 0 && strcmp(aircraft->destination, aircraft->destinationIcao) != 0) {
      buf += " (";
      buf += aircraft->destinationIcao;
      buf += ")";
    } else if (buf.length() == 0) {
      buf += aircraft->destinationIcao;
    }
  }
  setField(destination_, buf.c_str());
}
