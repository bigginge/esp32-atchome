#include "info_panel.hpp"

#include "theme.hpp"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// The panel is a fixed height with no scrolling, so every block must have a
// bounded height: values ellipsize once they hit their line budget rather than
// wrapping without limit, otherwise a long airport name pushes the route off
// the bottom. See makeValue() for how that bound is enforced.
//
// Blocks are separated by the parent's pad_row alone. They used to also carry a
// pad_bottom of their own, which stacked with the gap -- and the gap itself was
// an 11 px default inherited from lv_theme_default, because neither flex column
// below set one. Roughly 19 px between every block, none of it chosen here.
static constexpr int32_t kBlockGap = theme::kSp3;

/** Transparent grouping container: the shared half of every block below. */
static lv_obj_t *makeContainer(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_add_style(obj, &theme::stylePlain, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

static lv_obj_t *makeBlock(lv_obj_t *parent) {
  lv_obj_t *block = makeContainer(parent);
  lv_obj_set_width(block, LV_PCT(100));
  lv_obj_set_height(block, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(block, 2, 0);
  return block;
}

static lv_obj_t *makeCaption(lv_obj_t *parent, const char *caption) {
  lv_obj_t *captionLabel = lv_label_create(parent);
  lv_label_set_text(captionLabel, caption);
  lv_obj_add_style(captionLabel, &theme::styleCaption, 0);
  return captionLabel;
}

/**
 * A readout, bounded to `lines` lines: it wraps up to that many, then
 * ellipsizes.
 *
 * The max_height is what makes LV_LABEL_LONG_DOT work at all. LVGL only
 * inserts the dots when the text overflows a *constrained* height
 * (lv_label.c: `size.y > lv_area_get_height(&txt_coords)`), and the label
 * default height is LV_SIZE_CONTENT, which just grows to whatever the text
 * needs -- so the condition never fires and the text silently wraps instead.
 * Capping the height via max_height is what the dots branch tests against
 * (lv_label.c applies it to the label's reported self-size). Without it the
 * long-mode setting below is decorative, and a long airport name pushes the
 * route off the bottom of the panel.
 *
 * This caps rather than fixes the height, so a one-line value still occupies
 * one line and its block shrinks to match; `lines` is a worst case, not a
 * reservation.
 *
 * These were circular-scroll marquees. Measured, that was very expensive: the
 * animation steps continuously, and because the panel is built from
 * LV_SIZE_CONTENT blocks with percentage widths, each step re-runs the flex
 * layout for the column and repaints through the card beneath. lv_timer_handler
 * cost ~80 ms per call even on iterations where the radar did not paint, and
 * the main loop fell from ~60 to ~12 iterations a second. The animation was
 * starving itself -- which is exactly what the visible jitter was.
 */
static lv_obj_t *makeValue(lv_obj_t *parent, const lv_font_t *font, int32_t lines = 1) {
  lv_obj_t *valueLabel = lv_label_create(parent);
  lv_label_set_text(valueLabel, "—");
  lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_width(valueLabel, LV_PCT(100));
  lv_obj_add_style(valueLabel, &theme::styleValue, 0);
  lv_obj_set_style_text_font(valueLabel, font, 0);
  // text_line_space is 0 everywhere in this app, so N lines is exactly N line
  // heights. Revisit this if that ever stops being true.
  lv_obj_set_style_max_height(valueLabel, lines * lv_font_get_line_height(font), 0);
  return valueLabel;
}

/** The font must be passed in rather than restyled afterwards: makeValue sizes
 *  the label's line budget from it, so a later font swap would leave the two
 *  disagreeing and clip the value. */
static lv_obj_t *addField(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut,
                          const lv_font_t *font) {
  lv_obj_t *block = makeBlock(parent);
  makeCaption(block, caption);
  *valueOut = makeValue(block, font);
  return block;
}

/** Row holding the short numeric readouts side by side. */
static lv_obj_t *addMetricRow(lv_obj_t *parent) {
  lv_obj_t *row = makeContainer(parent);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, theme::kSp2, 0);
  return row;
}

/** One of the three short readouts, on its own raised surface. Giving them
 *  tiles rather than bare text is what makes the numbers read as the primary
 *  data on the panel. */
static void addMetric(lv_obj_t *row, const char *caption, lv_obj_t **valueOut) {
  lv_obj_t *col = lv_obj_create(row);
  lv_obj_add_style(col, &theme::styleRaised, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_style_pad_all(col, theme::kSp2, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);

  makeCaption(col, caption);
  *valueOut = makeValue(col, theme::fontBody());
}

void InfoPanel::create(lv_obj_t *parent) {
  // A column of cards over the page colour. The panel paints that colour itself
  // rather than being transparent: it must be an opaque, square, border-free
  // rectangle so LVGL's cover check stops the redraw walk here rather than
  // propagating every repaint up to the screen background. The settings wizard
  // root relies on the same property, for the same reason.
  panel_ = lv_obj_create(parent);
  lv_obj_add_style(panel_, &theme::stylePlain, 0);
  lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel_, theme::c(theme::kBgApp), 0);
  lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
  lv_obj_set_size(panel_, kWidth, kHeight);
  lv_obj_set_pos(panel_, theme::layout::kPanelX, theme::layout::kPanelY);
  lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel_, theme::kSp3, 0);

  // Large clock display at the top.
  clockContainer_ = lv_obj_create(panel_);
  lv_obj_add_style(clockContainer_, &theme::styleCard, 0);
  lv_obj_clear_flag(clockContainer_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(clockContainer_, LV_SIZE_CONTENT);
  lv_obj_set_width(clockContainer_, LV_PCT(100));
  lv_obj_set_flex_flow(clockContainer_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(clockContainer_, theme::kSp1, 0);

  timeLabel_ = lv_label_create(clockContainer_);
  lv_label_set_text(timeLabel_, "--:--:--");
  lv_obj_set_style_text_color(timeLabel_, theme::c(theme::kText), 0);
  lv_obj_set_style_text_font(timeLabel_, theme::fontClock(), 0);

  // The date shares its row with the stats readout. The stats used to be the
  // panel's last child, which cost a line plus a column gap -- 30 px that the
  // route block needs far more than the clock card needs the whitespace. The
  // date is ~90 px of a 270 px row and the settings gear floats over the *time*
  // row above, so the right-hand side here is free.
  lv_obj_t *dateRow = makeContainer(clockContainer_);
  lv_obj_set_width(dateRow, LV_PCT(100));
  lv_obj_set_height(dateRow, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(dateRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dateRow, theme::kSp2, 0);

  dateLabel_ = lv_label_create(dateRow);
  lv_label_set_text(dateLabel_, "------------");
  lv_obj_add_style(dateLabel_, &theme::styleCaption, 0);
  lv_obj_set_style_text_letter_space(dateLabel_, 0, 0);

  statsLabel_ = lv_label_create(dateRow);
  lv_label_set_text(statsLabel_, "");
  lv_label_set_long_mode(statsLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_flex_grow(statsLabel_, 1);
  lv_obj_set_style_max_height(statsLabel_, lv_font_get_line_height(theme::fontCaption()), 0);
  lv_obj_set_style_text_align(statsLabel_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(statsLabel_, theme::c(theme::kTextMuted), 0);
  lv_obj_set_style_text_font(statsLabel_, theme::fontCaption(), 0);

  // Second card, and the panel's last child: whichever of the status line or the
  // detail fields is showing. It grows into all the height the clock leaves.
  lv_obj_t *bodyCard = lv_obj_create(panel_);
  lv_obj_add_style(bodyCard, &theme::styleCard, 0);
  lv_obj_clear_flag(bodyCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(bodyCard, LV_PCT(100));
  lv_obj_set_flex_grow(bodyCard, 1);
  lv_obj_set_flex_flow(bodyCard, LV_FLEX_FLOW_COLUMN);
  // Explicit, because lv_theme_default's card style otherwise supplies an
  // 11 px pad_row here that nothing in this file asked for.
  lv_obj_set_style_pad_row(bodyCard, kBlockGap, 0);

  statusLabel_ = lv_label_create(bodyCard);
  lv_label_set_text(statusLabel_, "Connecting...");
  lv_label_set_long_mode(statusLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel_, LV_PCT(100));
  lv_obj_set_style_text_color(statusLabel_, theme::c(theme::kTextDim), 0);
  lv_obj_set_style_text_font(statusLabel_, theme::fontCaption(), 0);

  fieldsCont_ = makeContainer(bodyCard);
  lv_obj_set_width(fieldsCont_, LV_PCT(100));
  lv_obj_set_flex_grow(fieldsCont_, 1);
  lv_obj_set_flex_flow(fieldsCont_, LV_FLEX_FLOW_COLUMN);
  // The only separation between blocks; see kBlockGap.
  lv_obj_set_style_pad_row(fieldsCont_, kBlockGap, 0);
  lv_obj_add_flag(fieldsCont_, LV_OBJ_FLAG_HIDDEN);

  // The aircraft line is promoted to the title font for a clearer hierarchy.
  addField(fieldsCont_, "AIRCRAFT", &aircraft_, theme::fontTitle());
  addField(fieldsCont_, "FLIGHT", &flight_, theme::fontBody());

  lv_obj_t *metrics = addMetricRow(fieldsCont_);
  addMetric(metrics, "DIST", &distance_);
  addMetric(metrics, "ALT", &altitude_);
  addMetric(metrics, "SPD", &speed_);

  // Route is one block: the codes headline the leg, the airport names sit
  // beneath it. The names get two lines each and ellipsize beyond that; the line
  // holds roughly 40 characters, so "Chhatrapati Shivaji International, IN" fits
  // on one and only the longest "Airport, City, CC" forms need the second.
  // Measured worst case (both names on two lines) leaves 5 px spare at the
  // bottom of the fields container.
  routeBlock_ = makeBlock(fieldsCont_);
  makeCaption(routeBlock_, "ROUTE");
  routeCodes_ = makeValue(routeBlock_, theme::fontTitle());
  originName_ = makeValue(routeBlock_, theme::fontCaption(), 2);
  lv_obj_set_style_text_color(originName_, theme::c(theme::kTextDim), 0);
  destName_ = makeValue(routeBlock_, theme::fontCaption(), 2);
  lv_obj_set_style_text_color(destName_, theme::c(theme::kTextDim), 0);

  // Settings gear. IGNORE_LAYOUT keeps the flex column from placing it, so it
  // floats over the empty right-hand side of the clock card (the big time
  // hugs the left) without reflowing anything above.
  settingsBtn_ = lv_button_create(panel_);
  lv_obj_add_style(settingsBtn_, &theme::styleButton, 0);
  lv_obj_add_flag(settingsBtn_, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_size(settingsBtn_, 40, 40);
  lv_obj_align(settingsBtn_, LV_ALIGN_TOP_RIGHT, -theme::kSp2, theme::kSp2);
  lv_obj_add_event_cb(settingsBtn_, onSettingsClicked, LV_EVENT_CLICKED, this);

  lv_obj_t *gear = lv_label_create(settingsBtn_);
  lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_color(gear, theme::c(theme::kTextDim), 0);
  lv_obj_center(gear);
}

void InfoPanel::onSettingsClicked(lv_event_t *e) {
  static_cast<InfoPanel *>(lv_event_get_user_data(e))->settingsRequested_ = true;
}

bool InfoPanel::consumeSettingsRequest() {
  if (!settingsRequested_) {
    return false;
  }
  settingsRequested_ = false;
  return true;
}

void InfoPanel::setStats(size_t inRange, long secondsSinceUpdate) {
  if (statsLabel_ == nullptr) return;
  // Shorter than it once was: this now shares the date's row rather than owning
  // a line of the panel, so "updated 12s ago" no longer fits.
  char buf[48];
  if (secondsSinceUpdate < 0) {
    snprintf(buf, sizeof(buf), "%u in range", static_cast<unsigned>(inRange));
  } else {
    snprintf(buf, sizeof(buf), "%u in range " LV_SYMBOL_BULLET " %lds",
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
