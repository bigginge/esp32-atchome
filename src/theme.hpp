#pragma once

#include <lvgl.h>

/**
 * Single source of truth for the app's visual language.
 *
 * Colours are raw 0xRRGGBB constants rather than lv_color_t: the radar draws
 * through lv_draw_*_dsc_t and wants values in constant expressions, and
 * lv_color_hex() is not constexpr in LVGL 9. c() bridges to the widget side.
 *
 * Before this file the palette lived in three places -- a named block in
 * settings_screen.cpp whose comment claimed it "matches the radar / info panel
 * palette", plus inline literals in both of those. That was a promise the
 * compiler could not check.
 */
namespace theme {

inline lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }

// ---- Surfaces --------------------------------------------------------------
// An elevation ramp, darkest first. The page has to sit below the radar
// interior, or the radar card reads as a hole rather than a raised surface.
constexpr uint32_t kBgApp = 0x060A0E;      // page, visible in the gutter
constexpr uint32_t kSurface = 0x121A22;    // card fill
constexpr uint32_t kSurfaceHi = 0x1E2A36;  // raised: buttons, stat tiles
constexpr uint32_t kBorder = 0x223040;     // 1 px card hairline
// The radar interior. drawSymbol's glyph casing and drawLabels' backdrops paint
// with this same value to punch through rings and trails, so they must agree;
// route all three through this token, never a literal.
constexpr uint32_t kRadarBg = 0x0B1218;

// ---- Text ------------------------------------------------------------------
constexpr uint32_t kText = 0xE8EEF4;
constexpr uint32_t kTextDim = 0xA0B0C0;
constexpr uint32_t kTextMuted = 0x7A8A9A;

// ---- Accents ---------------------------------------------------------------
constexpr uint32_t kAccent = 0x2D7FF9;
constexpr uint32_t kDanger = 0xE05B4B;
constexpr uint32_t kSelection = 0xFFFFFF;  // selection halo + its label text

// ---- Radar chrome ----------------------------------------------------------
constexpr uint32_t kRing = 0x2A3A4A;
constexpr uint32_t kCross = 0x1E2A36;
constexpr uint32_t kRingLabel = 0x5A6A7A;
constexpr uint32_t kCompass = 0x8A9AAA;
constexpr uint32_t kHomeDot = 0xC8D0D8;
constexpr uint32_t kHomeCaption = 0x9AAAB8;
constexpr uint32_t kRadarLabel = 0xD8E2EC;  // unselected aircraft label text
constexpr uint32_t kLegendTick = 0x9AAAB8;
constexpr uint32_t kGround = 0x8A9AAA;  // altitude <= 0 / unknown

// ---- Spacing scale (4 px base) ---------------------------------------------
constexpr int32_t kSp1 = 4;
constexpr int32_t kSp2 = 8;
constexpr int32_t kSp3 = 12;
constexpr int32_t kSp4 = 16;
constexpr int32_t kSp6 = 24;
constexpr int32_t kSp8 = 32;

// ---- Radius scale ----------------------------------------------------------
constexpr int32_t kRadSm = 4;
constexpr int32_t kRadMd = 8;
constexpr int32_t kRadLg = 12;

// ---- Fonts -----------------------------------------------------------------
// Accessors rather than direct references, so swapping the family is a change
// here instead of at ~30 call sites. Declared by LV_FONT_CUSTOM_DECLARE in
// lv_conf.h, which is inside lvgl.h's extern "C" block -- do not redeclare
// them here, or the mangling mismatch surfaces as a baffling link error.
inline const lv_font_t *fontCaption() { return &atc_sans_14; }
inline const lv_font_t *fontBody() { return &atc_sans_16; }
inline const lv_font_t *fontTitle() { return &atc_sans_20; }
// Digits and colon only, monospaced so the seconds field does not jitter.
inline const lv_font_t *fontClock() { return &atc_num_44; }
inline const lv_font_t *fontRadar() { return &atc_sans_14; }

// ---- Shared styles ---------------------------------------------------------
// Each lv_style_t is a 12-byte handle; the property storage behind it is
// lv_malloc'd and therefore lands in PSRAM. So these cost ~12 B of .bss apiece
// and *save* PSRAM heap: a local style costs a separate malloc per object plus
// a realloc on every lv_obj_set_style_* call.
extern lv_style_t stylePlain;    // transparent, borderless, unpadded container
extern lv_style_t styleCard;     // kSurface fill, large radius, hairline border
extern lv_style_t styleRaised;   // kSurfaceHi fill with a radius (stat tiles)
extern lv_style_t styleCaption;  // small muted uppercase-ish label
extern lv_style_t styleValue;    // body-weight readout
extern lv_style_t styleButton;
extern lv_style_t styleButtonPrimary;
extern lv_style_t styleField;  // textarea

/** Must run before any object is created -- call from initLvgl(). */
void init();

// ---- Layout ----------------------------------------------------------------
// Everything derives from kGutter, so the dashboard framing is one constant.
namespace layout {

constexpr int32_t kScreenW = 800;
constexpr int32_t kScreenH = 480;

constexpr int32_t kGutter = kSp4;  // 0 = edge-to-edge (original); 16 = cards

constexpr int32_t kRadarSize = kScreenH - 2 * kGutter;
constexpr int32_t kRadarX = kGutter;
constexpr int32_t kRadarY = kGutter;

constexpr int32_t kPanelX = kRadarX + kRadarSize + kGutter;
constexpr int32_t kPanelY = kGutter;
constexpr int32_t kPanelW = kScreenW - kPanelX - kGutter;
constexpr int32_t kPanelH = kRadarSize;

// The radar is square and the panel takes what is left; if that stops being
// true the two will silently overlap rather than fail to build.
static_assert(kRadarSize > 0 && kPanelW > 0, "gutter too large for the screen");
static_assert(kRadarY + kRadarSize + kGutter == kScreenH, "radar is not square in the gutter");

}  // namespace layout

}  // namespace theme
