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
    trail.p1.x = x0;
    trail.p1.y = y0;
    trail.p2.x = x1;
    trail.p2.y = y1;
    lv_draw_line(layer, &trail);
  }
}

void RadarView::drawSymbol(lv_layer_t *layer, const Placed &p) {
  const Aircraft &ac = *p.ac;
  const lv_color_t color = colorForAircraft(ac);
  const int32_t x = p.x;
  const int32_t y = p.y;

  const float rad = (ac.trackDeg - 90.0f) * static_cast<float>(DEG_TO_RAD);
  const float fx = cosf(rad);
  const float fy = sinf(rad);
  const float sx = -fy;  // perpendicular (right wing) direction
  const float sy = fx;

  if (p.selected) {
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
    glyph.color = pass == 0 ? lv_color_hex(kBgColor) : color;
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

  // The legend is drawn after the labels, so it would paint over any label
  // that strayed under it. Must match drawLegend's backdrop rect.
  addBlocker({kSize - 48, 26, kSize - 2, 212});
  // HOME marker plus its caption.
  addBlocker({static_cast<int16_t>(cx - 26), static_cast<int16_t>(cy - 8),
              static_cast<int16_t>(cx + 26), static_cast<int16_t>(cy + 24)});

  // Ring range labels sit in a column just right of the vertical axis.
  const int rings = 4;
  for (int i = 1; i <= rings; ++i) {
    const float nm = rangeNm_ * (static_cast<float>(i) / static_cast<float>(rings));
    const int32_t radius = static_cast<int32_t>(lroundf(nm * pxPerNm_));
    const int32_t ly = cy - radius - 1;
    addBlocker({static_cast<int16_t>(cx + 5), static_cast<int16_t>(ly),
                static_cast<int16_t>(cx + 19), static_cast<int16_t>(ly + 17)});
  }
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

void RadarView::drawLabels(lv_layer_t *layer) {
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
    const size_t ownSymbol = kStaticBlockers + idx;

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
      lv_text_get_size(&sz, lbl, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX,
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
      lv_text_get_size(&sz, lbl, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX,
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
      lv_text_get_size(&sz, lbl, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX,
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

    // A label on the outer ring of anchors has been pushed clear of its
    // symbol, so tie it back with a leader line.
    if (usedAnchor >= 8) {
      int32_t tx = p.x < rect.x1 ? rect.x1 : (p.x > rect.x2 ? rect.x2 : p.x);
      int32_t ty = p.y < rect.y1 ? rect.y1 : (p.y > rect.y2 ? rect.y2 : p.y);
      const float dx = static_cast<float>(tx - p.x);
      const float dy = static_cast<float>(ty - p.y);
      const float len = sqrtf(dx * dx + dy * dy);
      if (len > p.symbolR) {
        lv_draw_line_dsc_t lead;
        lv_draw_line_dsc_init(&lead);
        lead.color = colorForAircraft(*p.ac);
        lead.width = 1;
        lead.opa = LV_OPA_50;
        lead.p1.x = p.x + static_cast<int32_t>(lroundf(dx / len * p.symbolR));
        lead.p1.y = p.y + static_cast<int32_t>(lroundf(dy / len * p.symbolR));
        lead.p2.x = tx;
        lead.p2.y = ty;
        lv_draw_line(layer, &lead);
      }
    }

    // Backdrop, so 14 px text stays readable over rings and trails —
    // collision avoidance cannot help with those.
    lv_draw_rect_dsc_t back;
    lv_draw_rect_dsc_init(&back);
    back.bg_color = lv_color_hex(kBgColor);
    back.bg_opa = p.selected ? 210 : 165;
    back.radius = 3;
    lv_area_t backArea = {rect.x1, rect.y1, rect.x2, rect.y2};
    lv_draw_rect(layer, &back, &backArea);

    drawText(layer, rect.x1 + 3, rect.y1 + 1, rect.x2 - rect.x1 - 6, lbl,
             p.selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xD8E2EC),
             LV_OPA_COVER, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);

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

  // Draw in layers rather than per-aircraft, so no aircraft's trail or symbol
  // can land on top of another's label.
  buildDrawOrder();
  for (size_t i = 0; i < orderCount_; ++i) {
    drawTrail(&layer, order_[i]);
  }
  for (size_t i = 0; i < orderCount_; ++i) {
    drawSymbol(&layer, order_[i]);
  }
  drawLabels(&layer);

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
