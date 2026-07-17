#include <lvgl.h>
#include <FT6236G.h>
#include <Wire.h>
#include <SPI.h>

// Display pins for CrowPanel ESP32 (ILI9488)
#define TFT_CLK   18
#define TFT_MOSI  23
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4
#define TFT_MISO  19

// Touch pins (FT6236 on I2C)
#define TOUCH_SDA 21
#define TOUCH_SCL 22

// Display dimensions
#define DISP_HOR_RES  800
#define DISP_VER_RES  480

static lv_disp_buf_t disp_buf;
static lv_color_t buf[DISP_HOR_RES * 10];

FT6236G touch;
lv_obj_t *rect = NULL;
lv_point_t rect_pos = {100, 100};
lv_point_t rect_size = {100, 100};

void my_disp_flush(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p) {
  lv_disp_flush_ready(disp);
}

void my_touch_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
  if (touch.available()) {
    TS_Point p = touch.getPoint();
    data->point.x = p.x;
    data->point.y = p.y;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void draw_rectangle() {
  if (rect) {
    lv_obj_del(rect);
  }
  
  rect = lv_obj_create(lv_scr_act());
  lv_obj_set_size(rect, rect_size.x, rect_size.y);
  lv_obj_set_pos(rect, rect_pos.x, rect_pos.y);
  lv_obj_set_style_local_bg_color(rect, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 
                                   lv_color_make(255, 0, 0));
}

void lv_touch_callback(lv_obj_t * obj, lv_event_t event) {
  if (event == LV_EVENT_CLICKED) {
    rect_pos.x = random(DISP_HOR_RES - rect_size.x);
    rect_pos.y = random(DISP_VER_RES - rect_size.y);
    draw_rectangle();
    Serial.print("Rectangle moved to: ");
    Serial.print(rect_pos.x);
    Serial.print(", ");
    Serial.println(rect_pos.y);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Starting ESP32 Display and Touch Demo");
  
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  touch.begin(0x38);
  
  SPI.begin(TFT_CLK, TFT_MISO, TFT_MOSI, TFT_CS);
  
  lv_init();
  
  lv_disp_buf_init(&disp_buf, buf, NULL, DISP_HOR_RES * 10);
  
  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.buffer = &disp_buf;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.hor_res = DISP_HOR_RES;
  disp_drv.ver_res = DISP_VER_RES;
  lv_disp_drv_register(&disp_drv);
  
  lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);
  
  draw_rectangle();
  lv_obj_set_event_cb(rect, lv_touch_callback);
  
  Serial.println("Setup complete. Display and touch initialized.");
}

void loop() {
  lv_task_handler();
  delay(5);
}
