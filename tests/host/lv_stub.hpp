#pragma once

#include <lvgl.h>

/** Knobs the stubbed LVGL exposes to tests. */
namespace lvstub {

/** What lv_obj_get_coords() reports. The composite maps screen coordinates to
 *  both the framebuffer and the background cache through this, so a test that
 *  wants realistic offsets has to set it. */
extern lv_area_t objCoords;

}  // namespace lvstub
