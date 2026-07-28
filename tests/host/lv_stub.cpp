/**
 * Just enough LVGL and Arduino to link src/radar_view.cpp on a workstation.
 *
 * Nothing here draws. The point is to reach the arithmetic in radar_view.cpp --
 * the sector solver and the framebuffer composite -- which is portable C++ and
 * does not care that it is usually running on an S3. Everything LVGL is asked
 * for is either a value the geometry needs (colour conversion, strides) or a
 * no-op the geometry never reads.
 */
#include <lvgl.h>
#include <stdint.h>
#include <string.h>

#include "frame_probe.hpp"
#include "log.hpp"
#include "lv_stub.hpp"

namespace lvstub {
lv_area_t objCoords = {0, 0, 0, 0};
}  // namespace lvstub

uint32_t millis() { return 0; }
uint32_t micros() { return 0; }

TeeLog Log;
void TeeLog::begin(unsigned long) {}
size_t TeeLog::write(uint8_t) { return 0; }
size_t TeeLog::write(const uint8_t *, size_t size) { return size; }

namespace probe {
Acc g[kSlotCount];
uint32_t gFrames;
}  // namespace probe

// Declared by LV_FONT_CUSTOM_DECLARE in lv_conf.h; never dereferenced here.
const lv_font_t atc_sans_14 = {};
const lv_font_t atc_sans_16 = {};
const lv_font_t atc_sans_20 = {};
const lv_font_t atc_num_44 = {};

extern "C" {

lv_color_t lv_color_hex(uint32_t c) {
  lv_color_t r;
  r.red = static_cast<uint8_t>(c >> 16);
  r.green = static_cast<uint8_t>(c >> 8);
  r.blue = static_cast<uint8_t>(c);
  return r;
}
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
  lv_color_t c;
  c.red = r;
  c.green = g;
  c.blue = b;
  return c;
}
uint16_t lv_color_to_u16(lv_color_t c) {
  return static_cast<uint16_t>(((c.red & 0xF8) << 8) | ((c.green & 0xFC) << 3) |
                               (c.blue >> 3));
}
lv_color_t lv_color_hsv_to_rgb(uint16_t, uint8_t, uint8_t) {
  return lv_color_hex(0x808080);
}

uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t) { return w * 2; }
uint32_t lv_tick_get() { return 0; }

void lv_draw_arc(lv_layer_t *, const lv_draw_arc_dsc_t *) {}
void lv_draw_arc_dsc_init(lv_draw_arc_dsc_t *d) { memset(d, 0, sizeof(*d)); }
void lv_draw_image(lv_layer_t *, const lv_draw_image_dsc_t *, const lv_area_t *) {}
void lv_draw_image_dsc_init(lv_draw_image_dsc_t *d) { memset(d, 0, sizeof(*d)); }
void lv_draw_label(lv_layer_t *, const lv_draw_label_dsc_t *, const lv_area_t *) {}
void lv_draw_label_dsc_init(lv_draw_label_dsc_t *d) { memset(d, 0, sizeof(*d)); }
void lv_draw_line(lv_layer_t *, const lv_draw_line_dsc_t *) {}
void lv_draw_line_dsc_init(lv_draw_line_dsc_t *d) { memset(d, 0, sizeof(*d)); }
void lv_draw_rect(lv_layer_t *, const lv_draw_rect_dsc_t *, const lv_area_t *) {}
void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *d) { memset(d, 0, sizeof(*d)); }
void lv_text_get_size(lv_point_t *sz, const char *txt, const lv_font_t *, int32_t,
                      int32_t, int32_t, lv_text_flag_t) {
  sz->x = static_cast<int32_t>(strlen(txt)) * 8;
  sz->y = 16;
}

lv_obj_t *lv_canvas_create(lv_obj_t *) { return nullptr; }
void lv_canvas_fill_bg(lv_obj_t *, lv_color_t, lv_opa_t) {}
void lv_canvas_finish_layer(lv_obj_t *, lv_layer_t *) {}
void lv_canvas_init_layer(lv_obj_t *, lv_layer_t *) {}
void lv_canvas_set_buffer(lv_obj_t *, void *, int32_t, int32_t, lv_color_format_t) {}

lv_event_code_t lv_event_get_code(lv_event_t *) { return LV_EVENT_ALL; }
lv_layer_t *lv_event_get_layer(lv_event_t *) { return nullptr; }
void *lv_event_get_user_data(lv_event_t *) { return nullptr; }
void lv_event_set_cover_res(lv_event_t *, lv_cover_res_t) {}
lv_indev_t *lv_indev_active() { return nullptr; }
void lv_indev_get_point(const lv_indev_t *, lv_point_t *) {}

lv_obj_t *lv_obj_create(lv_obj_t *) { return nullptr; }
lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *, lv_event_cb_t, lv_event_code_t, void *) {
  return nullptr;
}
void lv_obj_add_flag(lv_obj_t *, lv_obj_flag_t) {}
void lv_obj_delete(lv_obj_t *) {}
void lv_obj_get_coords(const lv_obj_t *, lv_area_t *a) { *a = lvstub::objCoords; }
lv_obj_t *lv_obj_get_parent(const lv_obj_t *) { return nullptr; }
lv_result_t lv_obj_invalidate(const lv_obj_t *) { return LV_RESULT_OK; }
lv_result_t lv_obj_invalidate_area(const lv_obj_t *, const lv_area_t *) {
  return LV_RESULT_OK;
}
void lv_obj_remove_flag(lv_obj_t *, lv_obj_flag_t) {}
void lv_obj_remove_style_all(lv_obj_t *) {}
void lv_obj_set_pos(lv_obj_t *, int32_t, int32_t) {}
void lv_obj_set_size(lv_obj_t *, int32_t, int32_t) {}

}  // extern "C"
