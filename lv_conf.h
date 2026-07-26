#ifndef LV_CONF_H
#define LV_CONF_H

/* CrowPanel 7" RGB565 panel */
#define LV_COLOR_DEPTH 16

// LVGL allocations are routed to PSRAM via custom core allocators
// (src/lv_psram_alloc.cpp). This frees ~128 KB of internal SRAM (the old
// builtin static pool) for the WiFi/TLS stack — HTTPS fetches were failing
// from internal-heap exhaustion.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
// LV_MEM_SIZE is unused with the custom allocator (kept only for reference).
#define LV_MEM_SIZE (48 * 1024U)

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

#define LV_USE_OS LV_OS_NONE

#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// The stock theme is light, which would put grey chrome on the app's dark
// background the moment the settings menu adds a button or a textarea.
#define LV_THEME_DEFAULT_DARK 1

// Button-matrix keys auto-repeat on long press (only the keyboard's *control*
// keys carry NO_REPEAT), and a deliberate press on the GT911 easily crosses the
// 400 ms default, duplicating characters. See also the NO_REPEAT ctrl maps in
// settings_screen.cpp.
#define LV_INDEV_LONG_PRESS_TIME 700

#define LV_USE_FLEX 1
#define LV_USE_CANVAS 1
#define LV_USE_LABEL 1

// Used by the settings menu. These all default to 1 when left undefined, but
// the list above reads as "the only widgets in use" -- keep it truthful.
#define LV_USE_BUTTON 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LIST 1
#define LV_USE_SPINNER 1
#define LV_USE_ARC 1
#define LV_USE_IMAGE 1

#endif /* LV_CONF_H */
