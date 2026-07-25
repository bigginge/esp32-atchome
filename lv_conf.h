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

#define LV_USE_FLEX 1
#define LV_USE_CANVAS 1
#define LV_USE_LABEL 1

#endif /* LV_CONF_H */
