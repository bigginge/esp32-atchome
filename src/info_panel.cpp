#include "info_panel.hpp"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// The panel is a fixed 320x480 with no scrolling, so every block must have a
// deterministic height: values are single-line marquees rather than wrapping
// labels, otherwise a long airport name pushes the route off the bottom.
static constexpr int32_t kBlockPadBottom = 8;

static lv_obj_t *makeBlock(lv_obj_t *parent) {
  lv_obj_t *block = lv_obj_create(parent);
  lv_obj_set_width(block, LV_PCT(100));
  lv_obj_set_height(block, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(block, 0, 0);
  lv_obj_set_style_pad_all(block, 0, 0);
  lv_obj_set_style_pad_bottom(block, kBlockPadBottom, 0);
  lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(block, 2, 0);
  lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
  return block;
}

static lv_obj_t *makeCaption(lv_obj_t *parent, const char *caption) {
  lv_obj_t *captionLabel = lv_label_create(parent);
  lv_label_set_text(captionLabel, caption);
  lv_obj_set_style_text_color(captionLabel, lv_color_hex(0x7A8A9A), 0);
  lv_obj_set_style_text_font(captionLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(captionLabel, 1, 0);
  return captionLabel;
}

static lv_obj_t *makeValue(lv_obj_t *parent, const lv_font_t *font) {
  lv_obj_t *valueLabel = lv_label_create(parent);
  lv_label_set_text(valueLabel, "—");
  lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(valueLabel, LV_PCT(100));
  lv_obj_set_style_text_color(valueLabel, lv_color_hex(0xE8EEF4), 0);
  lv_obj_set_style_text_font(valueLabel, font, 0);
  return valueLabel;
}

static lv_obj_t *addField(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut) {
  lv_obj_t *block = makeBlock(parent);
  makeCaption(block, caption);
  *valueOut = makeValue(block, &lv_font_montserrat_16);
  return block;
}

/** Row holding the short numeric readouts side by side. */
static lv_obj_t *addMetricRow(lv_obj_t *parent) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_bottom(row, kBlockPadBottom, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

static void addMetric(lv_obj_t *row, const char *caption, lv_obj_t **valueOut) {
  lv_obj_t *col = lv_obj_create(row);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

  makeCaption(col, caption);
  *valueOut = makeValue(col, &lv_font_montserrat_16);
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

  statusLabel_ = lv_label_create(panel_);
  lv_label_set_text(statusLabel_, "Connecting...");
  lv_label_set_long_mode(statusLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel_, LV_PCT(100));
  // Grows so the stats line below stays pinned to the bottom of the panel.
  lv_obj_set_flex_grow(statusLabel_, 1);
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

  addField(fieldsCont_, "AIRCRAFT", &aircraft_);
  addField(fieldsCont_, "FLIGHT", &flight_);

  lv_obj_t *metrics = addMetricRow(fieldsCont_);
  addMetric(metrics, "DIST", &distance_);
  addMetric(metrics, "ALT", &altitude_);
  addMetric(metrics, "SPD", &speed_);

  // Route is one block: the codes headline the leg, the airport names sit
  // beneath it and marquee when they are too long for the panel.
  routeBlock_ = makeBlock(fieldsCont_);
  makeCaption(routeBlock_, "ROUTE");
  routeCodes_ = makeValue(routeBlock_, &lv_font_montserrat_20);
  originName_ = makeValue(routeBlock_, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(originName_, lv_color_hex(0xA0B0C0), 0);
  destName_ = makeValue(routeBlock_, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(destName_, lv_color_hex(0xA0B0C0), 0);

  // Promote the primary aircraft line for a clearer hierarchy.
  lv_obj_set_style_text_font(aircraft_, &lv_font_montserrat_20, 0);

  // Last child of the panel: the growing body above pushes it to the bottom.
  statsLabel_ = lv_label_create(panel_);
  lv_label_set_text(statsLabel_, "");
  lv_obj_set_style_text_color(statsLabel_, lv_color_hex(0x7A8A9A), 0);
  lv_obj_set_style_text_font(statsLabel_, &lv_font_montserrat_14, 0);
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

void InfoPanel::setLabelText(lv_obj_t *label, const char *value) {
  if (label == nullptr) {
    return;
  }
  if (value == nullptr || value[0] == '\0') {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(label, value);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
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

  if (aircraft->originIcao[0] == '\0' && aircraft->destinationIcao[0] == '\0') {
    lv_obj_add_flag(routeBlock_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(routeBlock_, LV_OBJ_FLAG_HIDDEN);

  // ASCII only: the built-in montserrat fonts carry 0x20-0x7F plus the LV_SYMBOL
  // glyphs, so an em dash here would render as a missing-glyph box.
  buf = aircraft->originIcao[0] ? aircraft->originIcao : "???";
  buf += " " LV_SYMBOL_RIGHT " ";
  buf += aircraft->destinationIcao[0] ? aircraft->destinationIcao : "???";
  lv_label_set_text(routeCodes_, buf.c_str());

  // Until the airport-name lookups land, origin/destination still hold the raw
  // ICAO codes (see fetchRouteInfo) — don't repeat them under the codes line.
  const bool haveOriginName = strcmp(aircraft->origin, aircraft->originIcao) != 0;
  const bool haveDestName = strcmp(aircraft->destination, aircraft->destinationIcao) != 0;
  setLabelText(originName_, haveOriginName ? aircraft->origin : "");
  setLabelText(destName_, haveDestName ? aircraft->destination : "");
}
