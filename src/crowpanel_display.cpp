#include "crowpanel_display.hpp"
#include "log.hpp"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

SemaphoreHandle_t s_vsyncSem = nullptr;

// Runs in ISR context: signal the flush waiter that a frame flip completed.
bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t /*panel*/,
                       const esp_lcd_rgb_panel_event_data_t * /*edata*/,
                       void * /*ctx*/) {
  BaseType_t highTaskWoken = pdFALSE;
  if (s_vsyncSem != nullptr) {
    xSemaphoreGiveFromISR(s_vsyncSem, &highTaskWoken);
  }
  return highTaskWoken == pdTRUE;
}

constexpr int kBacklightPin = 2;

}  // namespace

bool CrowPanelDisplay::init() {
  s_vsyncSem = xSemaphoreCreateBinary();
  if (s_vsyncSem == nullptr) {
    Log.println("[lcd] vsync sem alloc failed");
    return false;
  }

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  // 12 MHz pixel clock (the panel flickers above ~12-14 MHz).
  cfg.timings.pclk_hz = 12 * 1000 * 1000;
  cfg.timings.h_res = kWidth;
  cfg.timings.v_res = kHeight;
  cfg.timings.hsync_pulse_width = 48;
  cfg.timings.hsync_back_porch = 40;
  cfg.timings.hsync_front_porch = 40;
  cfg.timings.vsync_pulse_width = 31;
  cfg.timings.vsync_back_porch = 13;
  cfg.timings.vsync_front_porch = 1;
  cfg.timings.flags.pclk_active_neg = 1;

  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;              // double framebuffer → tear-free page-flip
  // Bounce buffers feed the LCD DMA from fast internal SRAM (refilled from
  // PSRAM in bursts). Without them the direct PSRAM scan-out underruns under
  // bus contention (framebuffers + radar canvas + LVGL heap all in PSRAM),
  // which shows as flashing/jitter. 20 lines divides the frame evenly.
  cfg.bounce_buffer_size_px = kWidth * 20;
  cfg.flags.fb_in_psram = 1;

  cfg.hsync_gpio_num = 39;
  cfg.vsync_gpio_num = 40;
  cfg.de_gpio_num = 41;
  cfg.pclk_gpio_num = 0;
  cfg.disp_gpio_num = -1;

  // RGB565 data lines: d0..d4 = B0..B4, d5..d10 = G0..G5, d11..d15 = R0..R4
  // (same mapping the previous LovyanGFX config used).
  const int dataPins[16] = {15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45};
  for (int i = 0; i < 16; ++i) {
    cfg.data_gpio_nums[i] = dataPins[i];
  }

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel_);
  if (err != ESP_OK && cfg.bounce_buffer_size_px != 0) {
    // Some IDF builds reject bounce buffers combined with a double framebuffer.
    // Fall back to double-FB without bounce (tear-free, may flicker).
    Log.println("[lcd] bounce+double-FB rejected; retrying without bounce");
    cfg.bounce_buffer_size_px = 0;
    err = esp_lcd_new_rgb_panel(&cfg, &panel_);
  }
  if (err != ESP_OK) {
    Log.println("[lcd] esp_lcd_new_rgb_panel failed");
    return false;
  }

  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  esp_lcd_rgb_panel_register_event_callbacks(panel_, &cbs, nullptr);

  if (esp_lcd_panel_reset(panel_) != ESP_OK ||
      esp_lcd_panel_init(panel_) != ESP_OK) {
    Log.println("[lcd] panel reset/init failed");
    return false;
  }

  if (esp_lcd_rgb_panel_get_frame_buffer(panel_, 2, &fb0_, &fb1_) != ESP_OK) {
    Log.println("[lcd] get_frame_buffer failed");
    return false;
  }

  // Clear both framebuffers so no garbage shows before the first LVGL flush.
  memset(fb0_, 0, fbSizeBytes());
  memset(fb1_, 0, fbSizeBytes());

  return true;
}

void CrowPanelDisplay::waitVsync() {
  if (s_vsyncSem != nullptr) {
    // Drain any stale signal, then wait for the next genuine VSYNC so the flip
    // is applied before LVGL reuses the buffer (steady pacing, no tearing).
    xSemaphoreTake(s_vsyncSem, 0);
    xSemaphoreTake(s_vsyncSem, pdMS_TO_TICKS(100));
  }
}

void CrowPanelDisplay::setBrightness(uint8_t level) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(kBacklightPin, 5000, 8);
    attached = true;
  }
  ledcWrite(kBacklightPin, level);
}
