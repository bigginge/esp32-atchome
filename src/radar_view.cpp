#include "radar_view.hpp"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kBgColor = 0x0B1218;

// Draw a text label straight onto the canvas layer. LVGL only duplicates the
// text when text_local is set, so we set it and let LVGL copy — that keeps a
// caller-side stack buffer safe to reuse for the next label.
void drawText(lv_layer_t *layer, int32_t x, int32_t y, int32_t w,
              const char *text, lv_color_t color, lv_opa_t opa,
              const lv_font_t *font, lv_text_align_t align) {
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = color;
  dsc.opa = opa;
  dsc.font = font;
  dsc.text = text;
  dsc.text_local = 1;
  dsc.text_length = strlen(text);
  dsc.align = align;
  lv_area_t area = {x, y, x + w, y + static_cast<int32_t>(font->line_height) + 2};
  lv_draw_label(layer, &dsc, &area);
}

}  // namespace

lv_color_t altitudeColor(int altitudeFt) {
  if (altitudeFt <= 0) {
    return lv_color_hex(0x8A9AAA);  // ground / unknown
  }
  const float n = altitudeNorm(altitudeFt);
  // Monotonic hue sweep, green-cyan (low) → magenta (high).
  const uint16_t hue = static_cast<uint16_t>(lroundf(150.0f + n * 170.0f));
  return lv_color_hsv_to_rgb(hue, 82, 98);
}

bool RadarView::create(lv_obj_t *parent, float rangeNm) {
  setRangeNm(rangeNm);

  const size_t bufBytes =
      static_cast<size_t>(kSize) * static_cast<size_t>(kSize) * sizeof(uint16_t);
  buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf_ == nullptr) {
    Serial.println("[radar] Failed to allocate canvas buffer in PSRAM");
    return false;
  }

  canvas_ = lv_canvas_create(parent);
  lv_obj_set_size(canvas_, kSize, kSize);
  lv_obj_set_pos(canvas_, 0, 0);
  lv_canvas_set_buffer(canvas_, buf_, kSize, kSize, LV_COLOR_FORMAT_RGB565);
  lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas_, onClicked, LV_EVENT_CLICKED, this);

  redraw();
  return true;
}

void RadarView::setRangeNm(float rangeNm) {
  rangeNm_ = rangeNm > 1.0f ? rangeNm : 1.0f;
  pxPerNm_ = static_cast<float>(kSize / 2 - 8) / rangeNm_;
}

void RadarView::setSnapshot(const Aircraft *list, size_t count,
                            const char *selectedHex) {
  snap_ = list;
  snapCount_ = count;
  if (selectedHex != nullptr) {
    strncpy(selectedHex_, selectedHex, sizeof(selectedHex_) - 1);
    selectedHex_[sizeof(selectedHex_) - 1] = '\0';
  } else {
    selectedHex_[0] = '\0';
  }
}

bool RadarView::consumePendingClick(float *eastNm, float *northNm) {
  if (!pendingClick_) {
    return false;
  }
  pendingClick_ = false;
  if (eastNm != nullptr) *eastNm = pendingEast_;
  if (northNm != nullptr) *northNm = pendingNorth_;
  return true;
}

void RadarView::nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const {
  *x = static_cast<int32_t>(lroundf(kSize * 0.5f + eastNm * pxPerNm_));
  *y = static_cast<int32_t>(lroundf(kSize * 0.5f - northNm * pxPerNm_));
}

void RadarView::pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const {
  *eastNm = (static_cast<float>(x) - kSize * 0.5f) / pxPerNm_;
  *northNm = (kSize * 0.5f - static_cast<float>(y)) / pxPerNm_;
}

lv_color_t RadarView::colorForAircraft(const Aircraft &ac) const {
  return altitudeColor(ac.altitudeFt);
}

lv_opa_t RadarView::trailOpacity(const Aircraft &ac, uint8_t ageIndex,
                                 uint8_t count) const {
  // Lower altitude → more distinct trails.
  const float altFactor = 1.0f - altitudeNorm(ac.altitudeFt);
  const float base = 40.0f + altFactor * 180.0f;
  const float ageFade =
      count <= 1 ? 1.0f
                 : static_cast<float>(ageIndex + 1) / static_cast<float>(count);
  return static_cast<lv_opa_t>(lroundf(base * ageFade));
}

void RadarView::drawBackground(lv_layer_t *layer) {
  const int32_t cx = kSize / 2;
  const int32_t cy = kSize / 2;
  const lv_color_t ringColor = lv_color_hex(0x2A3A4A);
  const lv_color_t crossColor = lv_color_hex(0x1E2A36);
  const lv_color_t labelColor = lv_color_hex(0x5A6A7A);
  const lv_color_t compassColor = lv_color_hex(0x8A9AAA);

  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = crossColor;
  line.width = 1;
  line.opa = LV_OPA_COVER;

  line.p1.x = 0;
  line.p1.y = cy;
  line.p2.x = kSize - 1;
  line.p2.y = cy;
  lv_draw_line(layer, &line);

  line.p1.x = cx;
  line.p1.y = 0;
  line.p2.x = cx;
  line.p2.y = kSize - 1;
  lv_draw_line(layer, &line);

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.color = ringColor;
  arc.width = 1;
  arc.opa = LV_OPA_COVER;
  arc.start_angle = 0;
  arc.end_angle = 360;

  const int rings = 4;
  for (int i = 1; i <= rings; ++i) {
    const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(rings));
    const int32_t radius = static_cast<int32_t>(lroundf(nm * pxPerNm_));
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = static_cast<int16_t>(radius);
    lv_draw_arc(layer, &arc);

    // Ring range label, just above the ring on the vertical axis.
    char nmBuf[8];
    snprintf(nmBuf, sizeof(nmBuf), "%d", static_cast<int>(lroundf(nm)));
    drawText(layer, cx + 5, cy - radius - 1, 40, nmBuf, labelColor, LV_OPA_COVER,
             &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
  }

  // Compass markers.
  drawText(layer, cx - 16, 3, 32, "N", compassColor, LV_OPA_COVER,
           &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
  drawText(layer, cx - 16, kSize - 22, 32, "S", compassColor, LV_OPA_COVER,
           &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
  drawText(layer, kSize - 22, cy - 10, 20, "E", compassColor, LV_OPA_COVER,
           &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
  drawText(layer, 2, cy - 10, 20, "W", compassColor, LV_OPA_COVER,
           &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

  // Home marker + label.
  lv_draw_rect_dsc_t home;
  lv_draw_rect_dsc_init(&home);
  home.bg_color = lv_color_hex(0xC8D0D8);
  home.bg_opa = LV_OPA_COVER;
  home.radius = LV_RADIUS_CIRCLE;
  lv_area_t homeArea = {cx - 3, cy - 3, cx + 3, cy + 3};
  lv_draw_rect(layer, &home, &homeArea);
  drawText(layer, cx - 24, cy + 6, 48, "HOME", lv_color_hex(0x9AAAB8),
           LV_OPA_COVER, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
}

void RadarView::drawAircraft(lv_layer_t *layer, const Aircraft &ac, bool selected) {
  const lv_color_t color = colorForAircraft(ac);

  // Trail: oldest → newest
  if (ac.trailCount >= 2) {
    lv_draw_line_dsc_t trail;
    lv_draw_line_dsc_init(&trail);
    trail.color = color;
    trail.width = selected ? 2 : 1;
    trail.round_start = 1;
    trail.round_end = 1;

    for (uint8_t i = 0; i + 1 < ac.trailCount; ++i) {
      const uint8_t idx0 =
          static_cast<uint8_t>((ac.trailHead + kTrailLen - ac.trailCount + i) % kTrailLen);
      const uint8_t idx1 =
          static_cast<uint8_t>((ac.trailHead + kTrailLen - ac.trailCount + i + 1) % kTrailLen);
      int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      nmToPixel(ac.trail[idx0].eastNm, ac.trail[idx0].northNm, &x0, &y0);
      nmToPixel(ac.trail[idx1].eastNm, ac.trail[idx1].northNm, &x1, &y1);
      trail.opa = trailOpacity(ac, i, ac.trailCount);
      trail.p1.x = x0;
      trail.p1.y = y0;
      trail.p2.x = x1;
      trail.p2.y = y1;
      lv_draw_line(layer, &trail);
    }
  }

  int32_t x = 0, y = 0;
  nmToPixel(ac.eastNm, ac.northNm, &x, &y);

  const float rad = (ac.trackDeg - 90.0f) * static_cast<float>(DEG_TO_RAD);
  const float fx = cosf(rad);
  const float fy = sinf(rad);
  const float sx = -fy;  // perpendicular (right wing) direction
  const float sy = fx;

  if (selected) {
    lv_draw_arc_dsc_t halo;
    lv_draw_arc_dsc_init(&halo);
    halo.color = lv_color_hex(0xFFFFFF);
    halo.width = 2;
    halo.opa = LV_OPA_80;
    halo.center.x = x;
    halo.center.y = y;
    halo.radius = 15;
    halo.start_angle = 0;
    halo.end_angle = 360;
    lv_draw_arc(layer, &halo);
  }

  // Aircraft glyph: fuselage + wings + tailplane as rounded lines.
  const float s = selected ? 1.35f : 1.0f;
  const float noseLen = 9.0f * s;
  const float tailLen = 6.0f * s;
  const float wingSpan = 7.5f * s;
  const float wingAt = 0.5f * s;
  const float tailSpan = 3.5f * s;
  const float tailAt = -5.0f * s;

  lv_draw_line_dsc_t glyph;
  lv_draw_line_dsc_init(&glyph);
  glyph.color = color;
  glyph.opa = LV_OPA_COVER;
  glyph.width = selected ? 3 : 2;
  glyph.round_start = 1;
  glyph.round_end = 1;

  auto drawSeg = [&](float ax, float ay, float bx, float by) {
    glyph.p1.x = static_cast<int32_t>(lroundf(ax));
    glyph.p1.y = static_cast<int32_t>(lroundf(ay));
    glyph.p2.x = static_cast<int32_t>(lroundf(bx));
    glyph.p2.y = static_cast<int32_t>(lroundf(by));
    lv_draw_line(layer, &glyph);
  };

  // Fuselage
  drawSeg(x - fx * tailLen, y - fy * tailLen, x + fx * noseLen, y + fy * noseLen);
  // Wings
  drawSeg(x + fx * wingAt + sx * wingSpan, y + fy * wingAt + sy * wingSpan,
          x + fx * wingAt - sx * wingSpan, y + fy * wingAt - sy * wingSpan);
  // Tailplane
  drawSeg(x + fx * tailAt + sx * tailSpan, y + fy * tailAt + sy * tailSpan,
          x + fx * tailAt - sx * tailSpan, y + fy * tailAt - sy * tailSpan);

  // Label: callsign (or hex) + flight level.
  char lbl[24];
  const char *id = ac.callsign[0] != '\0' ? ac.callsign : ac.hex;
  if (ac.altitudeFt > 0) {
    snprintf(lbl, sizeof(lbl), "%s %d", id, (ac.altitudeFt + 50) / 100);
  } else {
    snprintf(lbl, sizeof(lbl), "%s", id);
  }
  drawText(layer, x + 11, y - 8, 100, lbl,
           selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB8C4D0),
           selected ? LV_OPA_COVER : LV_OPA_80, &lv_font_montserrat_14,
           LV_TEXT_ALIGN_LEFT);
}

void RadarView::drawLegend(lv_layer_t *layer) {
  const int32_t x0 = kSize - 16;
  const int32_t barW = 8;
  const int32_t yTop = 46;
  const int32_t yBot = 206;
  const int32_t height = yBot - yTop;

  // Backdrop for legibility.
  lv_draw_rect_dsc_t back;
  lv_draw_rect_dsc_init(&back);
  back.bg_color = lv_color_hex(kBgColor);
  back.bg_opa = 190;
  back.radius = 4;
  lv_area_t backArea = {kSize - 48, yTop - 20, kSize - 2, yBot + 6};
  lv_draw_rect(layer, &back, &backArea);

  drawText(layer, kSize - 48, yTop - 19, 46, "kft", lv_color_hex(0x7A8A9A),
           LV_OPA_COVER, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

  // Gradient bar (top = highest altitude).
  const int segs = 40;
  lv_draw_rect_dsc_t seg;
  lv_draw_rect_dsc_init(&seg);
  seg.bg_opa = LV_OPA_COVER;
  for (int i = 0; i < segs; ++i) {
    const float t = 1.0f - (static_cast<float>(i) + 0.5f) / static_cast<float>(segs);
    seg.bg_color = altitudeColor(static_cast<int>(t * kMaxAltitudeFt));
    const int32_t y1 = yTop + i * height / segs;
    const int32_t y2 = yTop + (i + 1) * height / segs;
    lv_area_t segArea = {x0, y1, x0 + barW, y2};
    lv_draw_rect(layer, &seg, &segArea);
  }

  // Tick labels: 0,10,20,30,40 kft.
  for (int k = 0; k <= 40; k += 10) {
    const float frac = static_cast<float>(k * 1000) / static_cast<float>(kMaxAltitudeFt);
    const int32_t y = yBot - static_cast<int32_t>(lroundf(frac * height)) - 8;
    char t[4];
    snprintf(t, sizeof(t), "%d", k);
    drawText(layer, kSize - 46, y, 26, t, lv_color_hex(0x9AAAB8), LV_OPA_COVER,
             &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
  }
}

void RadarView::redraw() {
  if (canvas_ == nullptr) {
    return;
  }

  lv_canvas_fill_bg(canvas_, lv_color_hex(kBgColor), LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(canvas_, &layer);

  drawBackground(&layer);

  const bool hasSelection = selectedHex_[0] != '\0';
  const Aircraft *selected = nullptr;
  for (size_t i = 0; i < snapCount_; ++i) {
    const Aircraft &ac = snap_[i];
    if (hasSelection && strcasecmp(selectedHex_, ac.hex) == 0) {
      selected = &ac;
      continue;
    }
    drawAircraft(&layer, ac, false);
  }
  if (selected != nullptr) {
    drawAircraft(&layer, *selected, true);
  }

  drawLegend(&layer);

  lv_canvas_finish_layer(canvas_, &layer);
}

void RadarView::onClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }

  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr || self->canvas_ == nullptr) {
    return;
  }

  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point;
  lv_indev_get_point(indev, &point);

  lv_area_t coords;
  lv_obj_get_coords(self->canvas_, &coords);
  const int32_t localX = point.x - coords.x1;
  const int32_t localY = point.y - coords.y1;

  self->pixelToNm(localX, localY, &self->pendingEast_, &self->pendingNorth_);
  self->pendingClick_ = true;
}
