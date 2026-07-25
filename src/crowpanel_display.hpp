#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

/** CrowPanel ESP32-S3 7" (DIS08070H) RGB panel driven by the ESP-IDF esp_lcd
 *  driver with two PSRAM framebuffers and VSYNC page-flip (tear-free).
 *  Touch (GT911) is handled separately in gt911.hpp. */
class CrowPanelDisplay {
 public:
  static constexpr uint16_t kWidth = 800;
  static constexpr uint16_t kHeight = 480;

  bool init();
  void setBrightness(uint8_t level);

  esp_lcd_panel_handle_t panel() const { return panel_; }
  void *fb0() const { return fb0_; }
  void *fb1() const { return fb1_; }
  size_t fbSizeBytes() const {
    return static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);
  }

  /** Block until the next VSYNC (after a draw_bitmap flip completes). */
  void waitVsync();

 private:
  esp_lcd_panel_handle_t panel_ = nullptr;
  void *fb0_ = nullptr;
  void *fb1_ = nullptr;
};
