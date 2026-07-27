#include "radar_view.hpp"
#include "log.hpp"

#include "frame_probe.hpp"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

/** Geometry of the altitude legend.
 *
 *  drawLegend() paints the backdrop and seedStaticBlockers() reserves the same
 *  box so no aircraft label is placed underneath it (the legend draws last and
 *  would paint over one). Those two rects used to be written out separately,
 *  held together by nothing but a comment. One struct, two readers. */
struct LegendGeom {
  int32_t barX, barW;      // gradient bar
  int32_t barTop, barBot;  // gradient extent
  int32_t x1, y1, x2, y2;  // backdrop == the label blocker
};

constexpr LegendGeom kLegend = [] {
  constexpr int32_t barW = 8;
  constexpr int32_t top = 46;
  constexpr int32_t bot = 206;
  return LegendGeom{RadarView::kSize - 16, barW,
                    top,                   bot,
                    RadarView::kSize - 48, top - 20,
                    RadarView::kSize - 2,  bot + 6};
}();

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
    return theme::c(theme::kGround);
  }
  const float n = altitudeNorm(altitudeFt);
  // Monotonic hue sweep, green-cyan (low) → magenta (high).
  const uint16_t hue = static_cast<uint16_t>(lroundf(150.0f + n * 170.0f));
  return lv_color_hsv_to_rgb(hue, 82, 98);
}

bool RadarView::create(lv_obj_t *parent, float rangeNm) {
  setRangeNm(rangeNm);

  const uint32_t stride = lv_draw_buf_width_to_stride(kSize, LV_COLOR_FORMAT_RGB565);
  const size_t bufBytes = static_cast<size_t>(stride) * static_cast<size_t>(kSize);
  bgCache_ = static_cast<uint8_t *>(
      heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (bgCache_ == nullptr) {
    Log.println("[radar] Failed to allocate background cache in PSRAM");
    return false;
  }

  bgImg_.header.magic = LV_IMAGE_HEADER_MAGIC;
  bgImg_.header.cf = LV_COLOR_FORMAT_RGB565;
  bgImg_.header.flags = 0;
  bgImg_.header.w = kSize;
  bgImg_.header.h = kSize;
  bgImg_.header.stride = stride;
  bgImg_.data = bgCache_;
  bgImg_.data_size = bufBytes;

  // A plain object, not a canvas: we paint it ourselves in LV_EVENT_DRAW_MAIN,
  // straight into the display's layer. That removes both the per-frame canvas
  // clear and the canvas -> framebuffer blit, which together were ~106 ms of a
  // ~200 ms frame. remove_style_all also drops the theme's background, so LVGL
  // does not fill the rect before we paint over it.
  obj_ = lv_obj_create(parent);
  lv_obj_remove_style_all(obj_);
  lv_obj_set_size(obj_, kSize, kSize);
  lv_obj_set_pos(obj_, theme::layout::kRadarX, theme::layout::kRadarY);
  lv_obj_clear_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj_, onClicked, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(obj_, onDraw, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_event_cb(obj_, onCoverCheck, LV_EVENT_COVER_CHECK, this);

  renderBackgroundCache();
  redraw();
  return true;
}

void RadarView::setRangeNm(float rangeNm) {
  const float clamped = rangeNm > 1.0f ? rangeNm : 1.0f;
  if (clamped == rangeNm_ && bgValid_) {
    return;  // a save that did not touch the radius costs nothing
  }
  rangeNm_ = clamped;
  pxPerNm_ = static_cast<float>(kSize / 2 - 8) / rangeNm_;
  // Ring radii and their nm captions are baked into the cache.
  bgValid_ = false;
}

void RadarView::renderBackgroundCache() {
  if (bgCache_ == nullptr || obj_ == nullptr) {
    return;
  }
  // lv_canvas is the only public way to rasterise into a buffer we own. This
  // one is transient and hidden: lv_canvas_finish_layer() drives the draw
  // dispatch from the layer's own task list regardless of visibility, and its
  // trailing lv_obj_invalidate() is a no-op while hidden, so nothing of this
  // ever reaches the screen.
  lv_obj_t *scratch = lv_canvas_create(lv_obj_get_parent(obj_));
  lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_buffer(scratch, bgCache_, kSize, kSize, LV_COLOR_FORMAT_RGB565);
  // Fill with the *page* colour, then lay a rounded rect of the radar colour on
  // top. The corners end up painted with the page colour, so the view reads as
  // a rounded card while remaining a fully opaque rectangle -- which is what
  // keeps the LV_EVENT_COVER_CHECK claim honest and stops LVGL filling the
  // screen background underneath every frame.
  lv_canvas_fill_bg(scratch, theme::c(theme::kBgApp), LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(scratch, &layer);
  ox_ = 0;  // the cache is in local coordinates
  oy_ = 0;

  lv_draw_rect_dsc_t card;
  lv_draw_rect_dsc_init(&card);
  card.bg_color = theme::c(theme::kRadarBg);
  card.bg_opa = LV_OPA_COVER;
  card.radius = theme::kRadLg;
  card.border_color = theme::c(theme::kBorder);
  card.border_width = 1;
  card.border_opa = LV_OPA_60;
  lv_area_t full = {0, 0, kSize - 1, kSize - 1};
  lv_draw_rect(&layer, &card, &full);

  drawBackground(&layer);
  lv_canvas_finish_layer(scratch, &layer);

  lv_obj_delete(scratch);
  bgValid_ = true;
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
                                 uint8_t count, bool selected) const {
  // Lower altitude → more distinct trails.
  const float altFactor = 1.0f - altitudeNorm(ac.altitudeFt);
  const float base = 40.0f + altFactor * 180.0f;
  const float ageFade =
      count <= 1 ? 1.0f
                 : static_cast<float>(ageIndex + 1) / static_cast<float>(count);
  // The crowding fade is what the selection needs to stand out against, so it
  // is exempt from it.
  const float crowd = selected ? 1.0f : trailScale_;
  return static_cast<lv_opa_t>(lroundf(base * ageFade * crowd));
}

// Trails are the biggest source of crossing lines on a busy screen, and each
// segment is a draw task. Shortening them as the count climbs is both a
// legibility and a throughput win — it pays for the symbol casing below.
uint8_t RadarView::trailBudget(bool selected) const {
  if (selected) return kTrailLen;
  if (orderCount_ <= 8) return kTrailLen;
  if (orderCount_ <= 16) return 8;
  if (orderCount_ <= 24) return 5;
  return 3;
}

void RadarView::buildDrawOrder() {
  orderCount_ = 0;
  blockerCount_ = 0;

  const bool hasSelection = selectedHex_[0] != '\0';

  for (size_t i = 0; i < snapCount_ && orderCount_ < kMaxAircraft; ++i) {
    const Aircraft &ac = snap_[i];
    int32_t x = 0, y = 0;
    nmToPixel(ac.eastNm, ac.northNm, &x, &y);

    // Off-canvas: LVGL would clip these anyway, so skip the draw tasks.
    if (x < -24 || x > kSize + 24 || y < -24 || y > kSize + 24) {
      continue;
    }

    Placed p;
    p.ac = &ac;
    p.x = x;
    p.y = y;
    p.selected = hasSelection && strcasecmp(selectedHex_, ac.hex) == 0;
    // Glyph nose reaches 9 px; selected scales x1.35 and adds a 15 px halo.
    p.symbolR = p.selected ? 18 : 10;

    // Extent of everything this aircraft paints. +2 covers the glyph casing,
    // which is drawn two pixels wider than the core stroke.
    const int16_t r = static_cast<int16_t>(p.symbolR + 2);
    p.box = {static_cast<int16_t>(x - r), static_cast<int16_t>(y - r),
             static_cast<int16_t>(x + r), static_cast<int16_t>(y + r)};
    const uint8_t used = ac.trailCount < kTrailLen ? ac.trailCount : kTrailLen;
    for (uint8_t t = 0; t < used; ++t) {
      int32_t tx = 0, ty = 0;
      nmToPixel(ac.trail[t].eastNm, ac.trail[t].northNm, &tx, &ty);
      if (tx - 1 < p.box.x1) p.box.x1 = static_cast<int16_t>(tx - 1);
      if (ty - 1 < p.box.y1) p.box.y1 = static_cast<int16_t>(ty - 1);
      if (tx + 1 > p.box.x2) p.box.x2 = static_cast<int16_t>(tx + 1);
      if (ty + 1 > p.box.y2) p.box.y2 = static_cast<int16_t>(ty + 1);
    }

    // Insertion sort by altitude ascending, so high aircraft draw last and
    // paint over low ones — looking down from above, higher is nearer the eye.
    // The selection is forced to the tail so its halo is never overpainted.
    size_t at = orderCount_;
    if (!p.selected) {
      while (at > 0 && (order_[at - 1].selected ||
                        order_[at - 1].ac->altitudeFt > ac.altitudeFt)) {
        order_[at] = order_[at - 1];
        --at;
      }
    }
    order_[at] = p;
    ++orderCount_;
  }

  // Label priority: the selection first, then nearest outwards. This is a
  // "what is over my house" display, and Tracker::selectNearest() already
  // treats distance as the measure of interest.
  for (size_t i = 0; i < orderCount_; ++i) {
    size_t at = i;
    while (at > 0) {
      const Placed &prev = order_[labelOrder_[at - 1]];
      const Placed &cur = order_[i];
      const bool prevWins =
          prev.selected ||
          (!cur.selected && prev.ac->distanceNm <= cur.ac->distanceNm);
      if (prevWins) break;
      labelOrder_[at] = labelOrder_[at - 1];
      --at;
    }
    labelOrder_[at] = static_cast<uint8_t>(i);
  }

  trailScale_ = orderCount_ <= 8    ? 1.00f
                : orderCount_ <= 16 ? 0.80f
                : orderCount_ <= 24 ? 0.65f
                                    : 0.50f;
}

int32_t RadarView::ringRadius(int i) const {
  const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(kRings));
  return static_cast<int32_t>(lroundf(nm * pxPerNm_));
}

RadarView::Rect RadarView::ringLabelRect(int i) const {
  const int32_t cx = kSize / 2;
  const int32_t ly = kSize / 2 - ringRadius(i) - 1;
  return {static_cast<int16_t>(cx + 5), static_cast<int16_t>(ly),
          static_cast<int16_t>(cx + 19), static_cast<int16_t>(ly + 17)};
}

// Local coordinates throughout: this is only ever rasterised into bgCache_ by
// renderBackgroundCache(), never into the display layer, so it needs no origin
// offset. Everything below that paints live does.
void RadarView::drawBackground(lv_layer_t *layer) {
  const int32_t cx = kSize / 2;
  const int32_t cy = kSize / 2;
  const lv_color_t ringColor = theme::c(theme::kRing);
  const lv_color_t crossColor = theme::c(theme::kCross);
  const lv_color_t labelColor = theme::c(theme::kRingLabel);
  const lv_color_t compassColor = theme::c(theme::kCompass);

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

  for (int i = 1; i <= kRings; ++i) {
    const int32_t radius = ringRadius(i);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = static_cast<int16_t>(radius);
    lv_draw_arc(layer, &arc);

    // Ring range label, just above the ring on the vertical axis.
    const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(kRings));
    const Rect box = ringLabelRect(i);
    char nmBuf[8];
    snprintf(nmBuf, sizeof(nmBuf), "%d", static_cast<int>(lroundf(nm)));
    drawText(layer, box.x1, box.y1, 40, nmBuf, labelColor, LV_OPA_COVER,
             theme::fontRadar(), LV_TEXT_ALIGN_LEFT);
  }

  // Compass markers.
  drawText(layer, cx - 16, 3, 32, "N", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, cx - 16, kSize - 22, 32, "S", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, kSize - 22, cy - 10, 20, "E", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);
  drawText(layer, 2, cy - 10, 20, "W", compassColor, LV_OPA_COVER,
           theme::fontBody(), LV_TEXT_ALIGN_CENTER);

  // Home marker + label.
  lv_draw_rect_dsc_t home;
  lv_draw_rect_dsc_init(&home);
  home.bg_color = theme::c(theme::kHomeDot);
  home.bg_opa = LV_OPA_COVER;
  home.radius = LV_RADIUS_CIRCLE;
  lv_area_t homeArea = {cx - 3, cy - 3, cx + 3, cy + 3};
  lv_draw_rect(layer, &home, &homeArea);
  drawText(layer, cx - 24, cy + 6, 48, "HOME", theme::c(theme::kHomeCaption),
           LV_OPA_COVER, theme::fontRadar(), LV_TEXT_ALIGN_CENTER);
}

void RadarView::drawTrail(lv_layer_t *layer, const Placed &p) {
  const Aircraft &ac = *p.ac;
  const uint8_t budget = trailBudget(p.selected);
  const uint8_t used = ac.trailCount < budget ? ac.trailCount : budget;
  if (used < 2) {
    return;
  }

  lv_draw_line_dsc_t trail;
  lv_draw_line_dsc_init(&trail);
  trail.color = colorForAircraft(ac);
  trail.width = p.selected ? 2 : 1;
  trail.round_start = 1;
  trail.round_end = 1;

  // Newest `used` points only, so the age fade still spans the whole trail.
  for (uint8_t i = 0; i + 1 < used; ++i) {
    const uint8_t idx0 =
        static_cast<uint8_t>((ac.trailHead + kTrailLen - used + i) % kTrailLen);
    const uint8_t idx1 =
        static_cast<uint8_t>((ac.trailHead + kTrailLen - used + i + 1) % kTrailLen);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    nmToPixel(ac.trail[idx0].eastNm, ac.trail[idx0].northNm, &x0, &y0);
    nmToPixel(ac.trail[idx1].eastNm, ac.trail[idx1].northNm, &x1, &y1);
    trail.opa = trailOpacity(ac, i, used, p.selected);
    trail.p1.x = x0 + ox_;
    trail.p1.y = y0 + oy_;
    trail.p2.x = x1 + ox_;
    trail.p2.y = y1 + oy_;
    lv_draw_line(layer, &trail);
  }
}

void RadarView::drawSymbol(lv_layer_t *layer, const Placed &p) {
  const Aircraft &ac = *p.ac;
  const lv_color_t color = colorForAircraft(ac);
  const int32_t x = p.x + ox_;
  const int32_t y = p.y + oy_;

  const float rad = (ac.trackDeg - 90.0f) * static_cast<float>(DEG_TO_RAD);
  const float fx = cosf(rad);
  const float fy = sinf(rad);
  const float sx = -fy;  // perpendicular (right wing) direction
  const float sy = fx;

  if (p.selected) {
    lv_draw_arc_dsc_t halo;
    lv_draw_arc_dsc_init(&halo);
    halo.color = theme::c(theme::kSelection);
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
  const float s = p.selected ? 1.35f : 1.0f;
  const float noseLen = 9.0f * s;
  const float tailLen = 6.0f * s;
  const float wingSpan = 7.5f * s;
  const float wingAt = 0.5f * s;
  const float tailSpan = 3.5f * s;
  const float tailAt = -5.0f * s;

  struct Seg {
    float ax, ay, bx, by;
  };
  const Seg segs[3] = {
      // Fuselage
      {x - fx * tailLen, y - fy * tailLen, x + fx * noseLen, y + fy * noseLen},
      // Wings
      {x + fx * wingAt + sx * wingSpan, y + fy * wingAt + sy * wingSpan,
       x + fx * wingAt - sx * wingSpan, y + fy * wingAt - sy * wingSpan},
      // Tailplane
      {x + fx * tailAt + sx * tailSpan, y + fy * tailAt + sy * tailSpan,
       x + fx * tailAt - sx * tailSpan, y + fy * tailAt - sy * tailSpan},
  };

  // Two complete passes: every casing first, then every fill. Interleaving
  // would let the wing casing eat the fuselage colour. The casing is what
  // keeps overlapping aircraft distinguishable in a cluster.
  const int32_t coreW = p.selected ? 3 : 2;
  for (int pass = 0; pass < 2; ++pass) {
    lv_draw_line_dsc_t glyph;
    lv_draw_line_dsc_init(&glyph);
    glyph.round_start = 1;
    glyph.round_end = 1;
    glyph.opa = LV_OPA_COVER;
    glyph.color = pass == 0 ? theme::c(theme::kRadarBg) : color;
    glyph.width = pass == 0 ? coreW + 2 : coreW;

    for (const Seg &seg : segs) {
      glyph.p1.x = static_cast<int32_t>(lroundf(seg.ax));
      glyph.p1.y = static_cast<int32_t>(lroundf(seg.ay));
      glyph.p2.x = static_cast<int32_t>(lroundf(seg.bx));
      glyph.p2.y = static_cast<int32_t>(lroundf(seg.by));
      lv_draw_line(layer, &glyph);
    }
  }
}

bool RadarView::rectsOverlap(const Rect &a, const Rect &b) {
  return !(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

void RadarView::addBlocker(const Rect &r) {
  if (blockerCount_ < kMaxBlockers) {
    blockers_[blockerCount_++] = r;
  }
}

// `skip` exempts one blocker — an aircraft's label is allowed to sit against
// its own symbol, which anchor 0 (the historical placement) always does.
bool RadarView::blocked(const Rect &r, size_t skip) const {
  for (size_t i = 0; i < blockerCount_; ++i) {
    if (i != skip && rectsOverlap(r, blockers_[i])) {
      return true;
    }
  }
  return false;
}

void RadarView::seedStaticBlockers() {
  const int32_t cx = kSize / 2;
  const int32_t cy = kSize / 2;

  // The legend is drawn after the labels, so it would paint over any label that
  // strayed under it.
  addBlocker({static_cast<int16_t>(kLegend.x1), static_cast<int16_t>(kLegend.y1),
              static_cast<int16_t>(kLegend.x2), static_cast<int16_t>(kLegend.y2)});
  // HOME marker plus its caption.
  addBlocker({static_cast<int16_t>(cx - 26), static_cast<int16_t>(cy - 8),
              static_cast<int16_t>(cx + 26), static_cast<int16_t>(cy + 24)});

  // Ring range labels sit in a column just right of the vertical axis.
  for (int i = 1; i <= kRings; ++i) {
    addBlocker(ringLabelRect(i));
  }

  // drawLabels() identifies "the aircraft's own symbol" by index, so it needs
  // to know where the static run ended rather than assuming a count.
  staticBlockerCount_ = blockerCount_;
}

void RadarView::formatLabel(const Aircraft &ac, uint8_t form, char *buf, size_t n) {
  const char *id = ac.callsign[0] != '\0' ? ac.callsign : ac.hex;
  const bool hasAlt = ac.altitudeFt > 0;
  const int fl = (ac.altitudeFt + 50) / 100;

  switch (form) {
    case 0:  // Identity + flight level.
      if (hasAlt) {
        snprintf(buf, n, "%s %d", id, fl);
      } else {
        snprintf(buf, n, "%s", id);
      }
      break;
    case 1:  // Identity only.
      snprintf(buf, n, "%s", id);
      break;
    default:  // Flight level only, or fall back to identity.
      if (hasAlt) {
        snprintf(buf, n, "%d", fl);
      } else {
        snprintf(buf, n, "%s", id);
      }
      break;
  }
}

bool RadarView::anchorRect(const Placed &p, uint8_t anchor, int32_t w, int32_t h,
                           Rect *out) const {
  const int32_t x = p.x;
  const int32_t y = p.y;
  const int32_t r = p.symbolR;
  const int32_t g = 4;   // gap between symbol box and label
  const int32_t f = 16;  // extra offset for the outer ring of anchors
  int32_t lx = 0, ly = 0;

  switch (anchor) {
    // Anchor 0 reproduces the historical placement exactly, so an uncrowded
    // sky looks unchanged.
    case 0:  lx = x + 11;         ly = y - 8;             break;
    case 1:  lx = x - r - g - w;  ly = y - h / 2 + 1;     break;
    case 2:  lx = x - w / 2;      ly = y - r - g - h;     break;
    case 3:  lx = x - w / 2;      ly = y + r + g;         break;
    case 4:  lx = x + r;          ly = y - r - h;         break;
    case 5:  lx = x + r;          ly = y + r;             break;
    case 6:  lx = x - r - w;      ly = y - r - h;         break;
    case 7:  lx = x - r - w;      ly = y + r;             break;
    case 8:  lx = x + r + f;      ly = y - r - f - h;     break;
    case 9:  lx = x + r + f;      ly = y + r + f;         break;
    case 10: lx = x - r - f - w;  ly = y - r - f - h;     break;
    default: lx = x - r - f - w;  ly = y + r + f;         break;
  }

  // Padded for the backdrop; this is what gets collision-tested and stored.
  const int32_t x1 = lx - 3;
  const int32_t y1 = ly - 1;
  const int32_t x2 = lx + w + 3;
  const int32_t y2 = ly + h + 1;
  if (x1 < 2 || y1 < 2 || x2 > kSize - 3 || y2 > kSize - 3) {
    return false;
  }

  out->x1 = static_cast<int16_t>(x1);
  out->y1 = static_cast<int16_t>(y1);
  out->x2 = static_cast<int16_t>(x2);
  out->y2 = static_cast<int16_t>(y2);
  return true;
}

uint8_t RadarView::rememberedAnchor(const char *hex, uint8_t *form) const {
  for (size_t i = 0; i < memoCount_; ++i) {
    if (strcasecmp(memo_[i].hex, hex) == 0) {
      *form = memo_[i].form;
      return memo_[i].anchor;
    }
  }
  *form = 0xFF;
  return 0xFF;
}

void RadarView::layoutLabels() {
  labelCount_ = 0;
  seedStaticBlockers();

  // Every symbol blocks, including ones we have not placed a label for yet —
  // otherwise a label could land on an aircraft further down the order.
  for (size_t i = 0; i < orderCount_; ++i) {
    const Placed &p = order_[i];
    addBlocker({static_cast<int16_t>(p.x - p.symbolR),
                static_cast<int16_t>(p.y - p.symbolR),
                static_cast<int16_t>(p.x + p.symbolR),
                static_cast<int16_t>(p.y + p.symbolR)});
  }

  // Read every hysteresis hint before writing any, since the new placements
  // overwrite memo_ in place.
  uint8_t hintForm[kMaxAircraft];
  uint8_t hintAnchor[kMaxAircraft];
  for (size_t i = 0; i < orderCount_; ++i) {
    hintAnchor[i] = rememberedAnchor(order_[i].ac->hex, &hintForm[i]);
  }

  size_t written = 0;

  for (size_t k = 0; k < orderCount_; ++k) {
    const size_t idx = labelOrder_[k];
    const Placed &p = order_[idx];
    const size_t ownSymbol = staticBlockerCount_ + idx;

    char lbl[24];
    Rect rect = {0, 0, 0, 0};
    bool placed = false;
    uint8_t usedForm = 0;
    uint8_t usedAnchor = 0;

    // Prefer last frame's spot if it is still free, so labels do not hop
    // around when the distance ordering shuffles between frames.
    if (hintAnchor[idx] < kAnchorCount && hintForm[idx] < kLabelForms) {
      formatLabel(*p.ac, hintForm[idx], lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      if (anchorRect(p, hintAnchor[idx], sz.x, sz.y, &rect) && !blocked(rect, ownSymbol)) {
        placed = true;
        usedForm = hintForm[idx];
        usedAnchor = hintAnchor[idx];
      }
    }

    // Otherwise walk the ladder: full label, then shorter forms, dropping the
    // label entirely if nothing fits. This thins clusters locally while
    // sparse regions keep their full labels.
    for (uint8_t form = 0; !placed && form < kLabelForms; ++form) {
      formatLabel(*p.ac, form, lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      for (uint8_t a = 0; a < kAnchorCount; ++a) {
        if (anchorRect(p, a, sz.x, sz.y, &rect) && !blocked(rect, ownSymbol)) {
          placed = true;
          usedForm = form;
          usedAnchor = a;
          break;
        }
      }
    }

    if (!placed) {
      if (!p.selected) {
        continue;  // dropped — the symbol is still drawn
      }
      // The selection always keeps its label; it has a halo and a backdrop,
      // so it stays readable even if it has to overlap something.
      formatLabel(*p.ac, 0, lbl, sizeof(lbl));
      lv_point_t sz;
      lv_text_get_size(&sz, lbl, theme::fontRadar(), 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      const int32_t lx = p.x + 11;
      const int32_t ly = p.y - 8;
      rect = {static_cast<int16_t>(lx - 3), static_cast<int16_t>(ly - 1),
              static_cast<int16_t>(lx + sz.x + 3),
              static_cast<int16_t>(ly + sz.y + 1)};
      usedForm = 0;
      usedAnchor = 0;
    } else {
      formatLabel(*p.ac, usedForm, lbl, sizeof(lbl));
    }

    // Record the solved placement; painting happens later, possibly more than
    // once, from labels_ alone.
    if (labelCount_ < kMaxAircraft) {
      LabelPlacement &lp = labels_[labelCount_++];
      lp.rect = rect;
      strncpy(lp.text, lbl, sizeof(lp.text) - 1);
      lp.text[sizeof(lp.text) - 1] = '\0';
      lp.orderIdx = static_cast<uint8_t>(idx);
      lp.hasLeader = false;

      // A label on the outer ring of anchors has been pushed clear of its
      // symbol, so tie it back with a leader line.
      if (usedAnchor >= 8) {
        const int32_t tx = p.x < rect.x1 ? rect.x1 : (p.x > rect.x2 ? rect.x2 : p.x);
        const int32_t ty = p.y < rect.y1 ? rect.y1 : (p.y > rect.y2 ? rect.y2 : p.y);
        const float dx = static_cast<float>(tx - p.x);
        const float dy = static_cast<float>(ty - p.y);
        const float len = sqrtf(dx * dx + dy * dy);
        if (len > p.symbolR) {
          lp.hasLeader = true;
          lp.leadX1 = static_cast<int16_t>(
              p.x + static_cast<int32_t>(lroundf(dx / len * p.symbolR)));
          lp.leadY1 = static_cast<int16_t>(
              p.y + static_cast<int32_t>(lroundf(dy / len * p.symbolR)));
          lp.leadX2 = static_cast<int16_t>(tx);
          lp.leadY2 = static_cast<int16_t>(ty);
        }
      }
    }

    addBlocker(rect);

    if (written < kMaxAircraft) {
      strncpy(memo_[written].hex, p.ac->hex, sizeof(memo_[written].hex) - 1);
      memo_[written].hex[sizeof(memo_[written].hex) - 1] = '\0';
      memo_[written].form = usedForm;
      memo_[written].anchor = usedAnchor;
      ++written;
    }
  }

  memoCount_ = written;
}

uint32_t RadarView::paintSignature() const {
  uint32_t h = 2166136261u ^ static_cast<uint32_t>(orderCount_);
  for (size_t i = 0; i < orderCount_; ++i) {
    const Placed &p = order_[i];
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.x);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.y);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.ac->altitudeFt);
    h = (h * 16777619u) ^ static_cast<uint32_t>(p.selected ? 1 : 0);
  }
  return h;
}

bool RadarView::culled(const Rect &r) const {
  return r.x2 < clip_.x1 || clip_.x2 < r.x1 || r.y2 < clip_.y1 || clip_.y2 < r.y1;
}

void RadarView::collectDirty() {
  dirtyCount_ = 0;

  // Symbol boxes only in the common case: trails do not move between fetches,
  // and a trail's extent is large enough that including it every frame would
  // invalidate most of the view. On the frames where new positions arrived the
  // trails did grow, so those frames use the full per-aircraft extent instead.
  for (size_t i = 0; i < orderCount_ && dirtyCount_ < kMaxDirty; ++i) {
    const Placed &p = order_[i];
    if (trailsChanged_) {
      dirty_[dirtyCount_++] = p.box;
    } else {
      const int16_t r = static_cast<int16_t>(p.symbolR + 2);
      dirty_[dirtyCount_++] = {static_cast<int16_t>(p.x - r), static_cast<int16_t>(p.y - r),
                               static_cast<int16_t>(p.x + r), static_cast<int16_t>(p.y + r)};
    }
  }

  for (size_t i = 0; i < labelCount_ && dirtyCount_ < kMaxDirty; ++i) {
    const LabelPlacement &lp = labels_[i];
    Rect r = lp.rect;
    if (lp.hasLeader) {
      // The leader runs from the symbol to the label, through pixels neither
      // rect covers.
      if (lp.leadX1 < r.x1) r.x1 = lp.leadX1;
      if (lp.leadY1 < r.y1) r.y1 = lp.leadY1;
      if (lp.leadX1 > r.x2) r.x2 = lp.leadX1;
      if (lp.leadY1 > r.y2) r.y2 = lp.leadY1;
    }
    dirty_[dirtyCount_++] = r;
  }
}

void RadarView::invalidateDirty() {
  lv_area_t coords;
  lv_obj_get_coords(obj_, &coords);

  // Both sets: last frame's rects to restore the background under where things
  // were, this frame's to draw where they now are.
  Rect region[kMaxDirty * 2];
  size_t n = 0;
  for (size_t i = 0; i < prevDirtyCount_; ++i) region[n++] = prevDirty_[i];
  for (size_t i = 0; i < dirtyCount_; ++i) region[n++] = dirty_[i];
  if (n == 0) {
    return;
  }

  // Merge down to a handful of regions before invalidating. LVGL renders each
  // invalid area as its own pass, and each pass re-runs the whole draw handler
  // -- including setting up the background image draw. Measured at ~5-8 ms per
  // pass, so two dozen tight rects cost far more than a few loose ones: going
  // from 24 regions to 4 was worth more than the tighter clipping it gave up.
  auto area = [](const Rect &r) -> int32_t {
    return (static_cast<int32_t>(r.x2) - r.x1 + 1) * (static_cast<int32_t>(r.y2) - r.y1 + 1);
  };
  auto merged = [](const Rect &a, const Rect &b) -> Rect {
    return {a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
            a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2};
  };

  while (n > kMaxRegions) {
    // Cheapest pair to merge: the one adding least dead area.
    size_t bi = 0, bj = 1;
    int32_t best = INT32_MAX;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = i + 1; j < n; ++j) {
        const int32_t cost = area(merged(region[i], region[j])) - area(region[i]) -
                             area(region[j]);
        if (cost < best) {
          best = cost;
          bi = i;
          bj = j;
        }
      }
    }
    region[bi] = merged(region[bi], region[bj]);
    region[bj] = region[--n];
  }

  for (size_t i = 0; i < n; ++i) {
    lv_area_t a = {region[i].x1 + coords.x1 - 1, region[i].y1 + coords.y1 - 1,
                   region[i].x2 + coords.x1 + 1, region[i].y2 + coords.y1 + 1};
    lv_obj_invalidate_area(obj_, &a);
  }
}

void RadarView::paintLabels(lv_layer_t *layer) {
  for (size_t i = 0; i < labelCount_; ++i) {
    const LabelPlacement &lp = labels_[i];
    if (culled(lp.rect) && !(lp.hasLeader && !culled(order_[lp.orderIdx].box))) {
      continue;
    }
    const Placed &p = order_[lp.orderIdx];

    if (lp.hasLeader) {
      lv_draw_line_dsc_t lead;
      lv_draw_line_dsc_init(&lead);
      lead.color = colorForAircraft(*p.ac);
      lead.width = 1;
      lead.opa = LV_OPA_50;
      lead.p1.x = lp.leadX1 + ox_;
      lead.p1.y = lp.leadY1 + oy_;
      lead.p2.x = lp.leadX2 + ox_;
      lead.p2.y = lp.leadY2 + oy_;
      lv_draw_line(layer, &lead);
    }

    // Backdrop, so 14 px text stays readable over rings and trails —
    // collision avoidance cannot help with those.
    lv_draw_rect_dsc_t back;
    lv_draw_rect_dsc_init(&back);
    back.bg_color = theme::c(theme::kRadarBg);
    back.bg_opa = p.selected ? 210 : 165;
    back.radius = 3;
    lv_area_t backArea = {lp.rect.x1 + ox_, lp.rect.y1 + oy_, lp.rect.x2 + ox_,
                          lp.rect.y2 + oy_};
    lv_draw_rect(layer, &back, &backArea);

    drawText(layer, lp.rect.x1 + 3 + ox_, lp.rect.y1 + 1 + oy_,
             lp.rect.x2 - lp.rect.x1 - 6, lp.text,
             theme::c(p.selected ? theme::kSelection : theme::kRadarLabel),
             LV_OPA_COVER, theme::fontRadar(), LV_TEXT_ALIGN_LEFT);
  }
}

void RadarView::drawLegend(lv_layer_t *layer) {
  const int32_t height = kLegend.barBot - kLegend.barTop;

  // Backdrop for legibility. Its extent is also what seedStaticBlockers()
  // reserves, so both read kLegend rather than repeating the arithmetic.
  lv_draw_rect_dsc_t back;
  lv_draw_rect_dsc_init(&back);
  back.bg_color = theme::c(theme::kRadarBg);
  back.bg_opa = 190;
  back.radius = theme::kRadSm;
  lv_area_t backArea = {kLegend.x1 + ox_, kLegend.y1 + oy_, kLegend.x2 + ox_,
                        kLegend.y2 + oy_};
  lv_draw_rect(layer, &back, &backArea);

  drawText(layer, kLegend.x1 + ox_, kLegend.y1 + 1 + oy_, 46, "kft",
           theme::c(theme::kTextMuted), LV_OPA_COVER, theme::fontRadar(),
           LV_TEXT_ALIGN_CENTER);

  // Gradient bar (top = highest altitude).
  const int segs = 40;
  lv_draw_rect_dsc_t seg;
  lv_draw_rect_dsc_init(&seg);
  seg.bg_opa = LV_OPA_COVER;
  for (int i = 0; i < segs; ++i) {
    const float t = 1.0f - (static_cast<float>(i) + 0.5f) / static_cast<float>(segs);
    seg.bg_color = altitudeColor(static_cast<int>(t * kMaxAltitudeFt));
    const int32_t y1 = kLegend.barTop + i * height / segs;
    const int32_t y2 = kLegend.barTop + (i + 1) * height / segs;
    lv_area_t segArea = {kLegend.barX + ox_, y1 + oy_,
                         kLegend.barX + kLegend.barW + ox_, y2 + oy_};
    lv_draw_rect(layer, &seg, &segArea);
  }

  // Tick labels: 0,10,20,30,40 kft.
  for (int k = 0; k <= 40; k += 10) {
    const float frac = static_cast<float>(k * 1000) / static_cast<float>(kMaxAltitudeFt);
    const int32_t y = kLegend.barBot - static_cast<int32_t>(lroundf(frac * height)) - 8;
    char t[4];
    snprintf(t, sizeof(t), "%d", k);
    drawText(layer, kLegend.barX - 30 + ox_, y + oy_, 26, t,
             theme::c(theme::kLegendTick), LV_OPA_COVER, theme::fontRadar(),
             LV_TEXT_ALIGN_RIGHT);
  }
}

void RadarView::redraw() {
  if (obj_ == nullptr) {
    return;
  }
  if (!bgValid_) {
    renderBackgroundCache();
  }

  // Solve the frame here; the actual painting happens in onDraw(), which LVGL
  // calls back during lv_timer_handler() once per invalidated region.
  buildDrawOrder();

  // Nothing moved by a whole pixel? Then there is nothing to paint. At 25 nm
  // range a 450 kt aircraft crosses about one pixel per second, so without this
  // the view repaints ten-plus times per pixel of real movement. That cost two
  // visible artefacts: the label solver re-ran every frame and could pick a
  // different anchor each time (labels flickering), and the repaints starved
  // lv_timer_handler, so the info panel's marquee animation stepped
  // irregularly. Both are fixed by simply not painting an identical frame.
  const uint32_t sig = paintSignature();
  if (!fullRepaint_ && !trailsChanged_ && hasPainted_ && sig == lastPaintSig_) {
    return;
  }
  lastPaintSig_ = sig;
  hasPainted_ = true;

  // Only the frames that actually paint pay for the anchor solver.
  probe::Scope solveScope(probe::kTasks);
  layoutLabels();
  collectDirty();

  if (fullRepaint_) {
    fullRepaint_ = false;
    lv_obj_invalidate(obj_);
  } else {
    invalidateDirty();
  }
  trailsChanged_ = false;
  memcpy(prevDirty_, dirty_, dirtyCount_ * sizeof(Rect));
  prevDirtyCount_ = dirtyCount_;
  probe::countFrame();
}

void RadarView::onCoverCheck(lv_event_t *e) {
  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  // The background image is opaque and covers the whole object, so LVGL can
  // skip everything underneath -- including filling the screen background,
  // which would be a full-area write we would then paint straight over.
  lv_event_set_cover_res(e, self != nullptr && self->bgValid_ ? LV_COVER_RES_COVER
                                                             : LV_COVER_RES_NOT_COVER);
}

void RadarView::onDraw(lv_event_t *e) {
  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr) {
    return;
  }
  probe::Scope _(probe::kRaster);
  self->paintDirect(lv_event_get_layer(e));
}

void RadarView::paintDirect(lv_layer_t *layer) {
  lv_area_t coords;
  lv_obj_get_coords(obj_, &coords);
  ox_ = coords.x1;
  oy_ = coords.y1;

  // The region LVGL is asking for, in local coordinates, so the draw loops can
  // skip work that would be clipped away anyway.
  clip_ = {static_cast<int16_t>(layer->_clip_area.x1 - ox_),
           static_cast<int16_t>(layer->_clip_area.y1 - oy_),
           static_cast<int16_t>(layer->_clip_area.x2 - ox_),
           static_cast<int16_t>(layer->_clip_area.y2 - oy_)};

  if (bgValid_) {
    // Restores the static chrome and erases the previous frame in one pass.
    // LVGL clips this to the invalid region, so it costs only what changed.
    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = &bgImg_;
    img.opa = LV_OPA_COVER;
    lv_draw_image(layer, &img, &coords);
  }

  for (size_t i = 0; i < orderCount_; ++i) {
    if (!culled(order_[i].box)) drawTrail(layer, order_[i]);
  }
  for (size_t i = 0; i < orderCount_; ++i) {
    if (!culled(order_[i].box)) drawSymbol(layer, order_[i]);
  }
  paintLabels(layer);
  // Stays live rather than baked into the cache: it is drawn last so aircraft
  // never obscure it, which is only true if it paints after the symbols.
  drawLegend(layer);
}

void RadarView::onClicked(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }

  auto *self = static_cast<RadarView *>(lv_event_get_user_data(e));
  if (self == nullptr || self->obj_ == nullptr) {
    return;
  }

  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point;
  lv_indev_get_point(indev, &point);

  lv_area_t coords;
  lv_obj_get_coords(self->obj_, &coords);
  const int32_t localX = point.x - coords.x1;
  const int32_t localY = point.y - coords.y1;

  self->pixelToNm(localX, localY, &self->pendingEast_, &self->pendingNorth_);
  self->pendingClick_ = true;
}
