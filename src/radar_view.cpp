#include "radar_view.hpp"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

bool RadarView::create(lv_obj_t *parent, Tracker *tracker, float rangeNm) {
  tracker_ = tracker;
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

void RadarView::nmToPixel(float eastNm, float northNm, int32_t *x, int32_t *y) const {
  *x = static_cast<int32_t>(lroundf(kSize * 0.5f + eastNm * pxPerNm_));
  *y = static_cast<int32_t>(lroundf(kSize * 0.5f - northNm * pxPerNm_));
}

void RadarView::pixelToNm(int32_t x, int32_t y, float *eastNm, float *northNm) const {
  *eastNm = (static_cast<float>(x) - kSize * 0.5f) / pxPerNm_;
  *northNm = (kSize * 0.5f - static_cast<float>(y)) / pxPerNm_;
}

lv_color_t RadarView::colorForAircraft(const Aircraft &ac) const {
  const float n = altitudeNorm(ac.altitudeFt);
  // Low altitude → darker; high altitude → lighter. HSV s/v are 0..100.
  const uint8_t v = static_cast<uint8_t>(lroundf(28.0f + n * 72.0f));
  const uint8_t s = static_cast<uint8_t>(lroundf(92.0f - n * 28.0f));
  return lv_color_hsv_to_rgb(static_cast<uint16_t>(ac.hue) * 360u / 255u, s, v);
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
  const lv_color_t crossColor = lv_color_hex(0x3A4A5A);

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
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = static_cast<int16_t>(lroundf(nm * pxPerNm_));
    lv_draw_arc(layer, &arc);
  }

  // Home marker
  lv_draw_rect_dsc_t home;
  lv_draw_rect_dsc_init(&home);
  home.bg_color = lv_color_hex(0xC8D0D8);
  home.bg_opa = LV_OPA_COVER;
  home.radius = LV_RADIUS_CIRCLE;
  lv_area_t homeArea = {cx - 3, cy - 3, cx + 3, cy + 3};
  lv_draw_rect(layer, &home, &homeArea);
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
      int32_t x0 = 0;
      int32_t y0 = 0;
      int32_t x1 = 0;
      int32_t y1 = 0;
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

  int32_t x = 0;
  int32_t y = 0;
  nmToPixel(ac.eastNm, ac.northNm, &x, &y);

  const float rad = (ac.trackDeg - 90.0f) * static_cast<float>(DEG_TO_RAD);
  const float c = cosf(rad);
  const float s = sinf(rad);
  const float len = selected ? 11.0f : 9.0f;
  const float half = selected ? 6.0f : 5.0f;

  // Nose, left wing, right wing in screen space (track 0° = north / up).
  const float noseX = x + c * len;
  const float noseY = y + s * len;
  const float leftX = x - c * (len * 0.45f) - s * half;
  const float leftY = y - s * (len * 0.45f) + c * half;
  const float rightX = x - c * (len * 0.45f) + s * half;
  const float rightY = y - s * (len * 0.45f) - c * half;

  if (selected) {
    lv_draw_arc_dsc_t halo;
    lv_draw_arc_dsc_init(&halo);
    halo.color = lv_color_hex(0xFFFFFF);
    halo.width = 2;
    halo.opa = LV_OPA_80;
    halo.center.x = x;
    halo.center.y = y;
    halo.radius = 14;
    halo.start_angle = 0;
    halo.end_angle = 360;
    lv_draw_arc(layer, &halo);
  }

  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  tri.color = color;
  tri.opa = LV_OPA_COVER;
  tri.p[0].x = noseX;
  tri.p[0].y = noseY;
  tri.p[1].x = leftX;
  tri.p[1].y = leftY;
  tri.p[2].x = rightX;
  tri.p[2].y = rightY;
  lv_draw_triangle(layer, &tri);
}

void RadarView::redraw() {
  if (canvas_ == nullptr || tracker_ == nullptr) {
    return;
  }

  lv_canvas_fill_bg(canvas_, lv_color_hex(0x0B1218), LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(canvas_, &layer);

  drawBackground(&layer);

  const Aircraft *selected = tracker_->selected();
  for (size_t i = 0; i < tracker_->count(); ++i) {
    const Aircraft &ac = tracker_->aircraft()[i];
    const bool isSelected =
        selected != nullptr && strcasecmp(selected->hex, ac.hex) == 0;
    if (!isSelected) {
      drawAircraft(&layer, ac, false);
    }
  }
  if (selected != nullptr) {
    drawAircraft(&layer, *selected, true);
  }

  lv_canvas_finish_layer(canvas_, &layer);
}

void RadarView::onClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }

  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr || self->tracker_ == nullptr || self->canvas_ == nullptr) {
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

  float eastNm = 0.0f;
  float northNm = 0.0f;
  self->pixelToNm(localX, localY, &eastNm, &northNm);

  const float hitNm = 3.0f;
  if (self->tracker_->selectNearestTo(eastNm, northNm, hitNm) != nullptr) {
    self->redraw();
  }
}
