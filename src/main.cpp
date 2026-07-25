#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>
#include <PCA9557.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>

#include "config.h"
#include "crowpanel_display.hpp"
#include "gt911.hpp"
#include "api_client.hpp"
#include "tracker.hpp"
#include "radar_view.hpp"
#include "info_panel.hpp"

static constexpr uint16_t kHorRes = 800;
static constexpr uint16_t kVerRes = 480;

static constexpr int kTouchSda = 19;
static constexpr int kTouchScl = 20;
static constexpr uint32_t kI2cHz = 300000;

// Animation cadence. The panel refreshes at ~24 Hz and the flush waits on
// VSYNC (tear-free), so this is an upper bound; idle frames are skipped.
static constexpr unsigned long kRadarFrameMs = 50;

static constexpr unsigned long kEnrichmentSuccessIntervalMs = 1200;
static constexpr unsigned long kEnrichmentRetryIntervalMs = 5000;

static CrowPanelDisplay lcd;
static PCA9557 ioExpander(0x18, &Wire);
static GT911 touch;

static Tracker tracker;
static RadarView radarView;
static InfoPanel infoPanel;

// snapshot: network-task fetch buffer. selCopy: UI copy of the selection.
static Aircraft snapshot[kMaxAircraft];
static Aircraft selCopy;

// Shared state between the UI task (core 1) and the network task (core 0).
static SemaphoreHandle_t gMutex = nullptr;
static volatile bool gDataDirty = false;
static volatile int gNetStatus = 0;  // 0 ok, 1 wifi reconnecting, 2 fetch failed
static volatile unsigned long gLastFetchOkMs = 0;
static volatile bool gHadFirstFetch = false;
static volatile uint32_t gFreeInternal = 0;  // free internal heap (bytes)

static uint32_t lvTickCb() { return millis(); }

static void dispFlush(lv_display_t *disp, const lv_area_t * /*area*/, uint8_t *pxMap) {
  // DIRECT render mode: pxMap is one of the two framebuffers. Present it once
  // per refresh and wait for the flip so LVGL never draws into the scanned FB.
  if (lv_display_flush_is_last(disp)) {
    esp_lcd_panel_draw_bitmap(lcd.panel(), 0, 0, kHorRes, kVerRes, pxMap);
    lcd.waitVsync();
  }
  lv_display_flush_ready(disp);
}

static void touchRead(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
  uint16_t x = 0;
  uint16_t y = 0;
  if (touch.read(&x, &y)) {
    if (x >= kHorRes) x = kHorRes - 1;
    if (y >= kVerRes) y = kVerRes - 1;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void resetTouchViaPca9557() {
  Wire.begin(kTouchSda, kTouchScl);
  Wire.setClock(kI2cHz);

  ioExpander.pinMode(0, OUTPUT);
  ioExpander.pinMode(1, OUTPUT);
  ioExpander.digitalWrite(0, LOW);
  ioExpander.digitalWrite(1, LOW);
  delay(20);
  ioExpander.digitalWrite(0, HIGH);
  delay(100);
  ioExpander.pinMode(1, INPUT);
}

// ===== Network task (core 0): all HTTP lives here so the UI never blocks. =====

static void networkTask(void * /*arg*/) {
  char lastEnrichedHex[8] = {0};
  unsigned long lastEnrichAttempt = 0;
  bool lastEnrichFailed = false;
  unsigned long lastFetch = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      gNetStatus = 1;
      gDataDirty = true;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      const unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Reconnected %s\n", WiFi.localIP().toString().c_str());
        configTzTime("GMT0BST,M3.5.0,M10.5.0", "pool.ntp.org");
        gNetStatus = 0;
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }

    const unsigned long now = millis();
    if (lastFetch == 0 || now - lastFetch >= REFRESH_INTERVAL_MS) {
      lastFetch = now;
      gFreeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      Serial.printf("[mem] free internal %u B before fetch\n",
                    static_cast<unsigned>(gFreeInternal));
      size_t count = 0;
      const bool ok = fetchNearbyAircraft(MY_LATITUDE, MY_LONGITUDE, SEARCH_RADIUS_NM,
                                          snapshot, kMaxAircraft, &count);
      if (ok) {
        xSemaphoreTake(gMutex, portMAX_DELAY);
        tracker.mergeSnapshot(snapshot, count);
        xSemaphoreGive(gMutex);
        gNetStatus = 0;
        gLastFetchOkMs = millis();
        gHadFirstFetch = true;
        gDataDirty = true;
      } else {
        gNetStatus = 2;
        gDataDirty = true;
      }
    }

    // Enrich the current selection, one HTTP step per pass.
    Aircraft sel;
    bool hasSel = false;
    xSemaphoreTake(gMutex, portMAX_DELAY);
    {
      const Aircraft *s = tracker.selected();
      if (s != nullptr) {
        sel = *s;
        hasSel = true;
      }
    }
    xSemaphoreGive(gMutex);

    if (hasSel) {
      const bool changed = strcasecmp(lastEnrichedHex, sel.hex) != 0;
      if (changed) {
        strncpy(lastEnrichedHex, sel.hex, sizeof(lastEnrichedHex) - 1);
        lastEnrichedHex[sizeof(lastEnrichedHex) - 1] = '\0';
        lastEnrichAttempt = 0;
        lastEnrichFailed = false;
      }

      const bool needs =
          !(sel.detailsLoaded && (sel.routeLoaded || sel.callsign[0] == '\0'));
      if (needs) {
        const unsigned long n2 = millis();
        const unsigned long interval =
            lastEnrichFailed ? kEnrichmentRetryIntervalMs : kEnrichmentSuccessIntervalMs;
        if (changed || n2 - lastEnrichAttempt >= interval) {
          lastEnrichAttempt = n2;
          bool updated = false;
          if (!sel.detailsLoaded) {
            updated = fetchAircraftDetails(sel);
          } else if (!sel.routeLoaded && sel.callsign[0] != '\0') {
            updated = fetchRouteInfo(sel);
          }
          lastEnrichFailed = !updated;
          if (updated) {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            tracker.applyEnrichment(sel);
            xSemaphoreGive(gMutex);
            gDataDirty = true;
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void connectWiFi() {
  infoPanel.showStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    lv_timer_handler();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected %s\n", WiFi.localIP().toString().c_str());
    configTzTime("GMT0BST,M3.5.0,M10.5.0", "pool.ntp.org");
  } else {
    Serial.println("[WiFi] Connection failed");
    infoPanel.showStatus("WiFi failed");
  }
}

static bool initLvgl() {
  lv_init();
  lv_tick_set_cb(lvTickCb);

  lv_display_t *disp = lv_display_create(kHorRes, kVerRes);
  lv_display_set_flush_cb(disp, dispFlush);
  // The two RGB framebuffers ARE the LVGL draw buffers (DIRECT mode + VSYNC
  // page-flip = tear-free). No separate draw buffer needed.
  lv_display_set_buffers(disp, lcd.fb0(), lcd.fb1(), lcd.fbSizeBytes(),
                         LV_DISPLAY_RENDER_MODE_DIRECT);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchRead);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1218), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  infoPanel.create(screen);
  if (!radarView.create(screen, static_cast<float>(SEARCH_RADIUS_NM))) {
    return false;
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("===== ESP32 ATC Home =====");
  Serial.printf("Home: %.4f, %.4f  radius %d nm\n", MY_LATITUDE, MY_LONGITUDE,
                SEARCH_RADIUS_NM);

  gMutex = xSemaphoreCreateMutex();

  Serial.println("Reset touch controller...");
  resetTouchViaPca9557();

  Serial.println("Init display...");
  if (!lcd.init()) {
    Serial.println("Display init failed");
    return;
  }
  lcd.setBrightness(255);

  Serial.println("Init LVGL...");
  if (!initLvgl()) {
    Serial.println("LVGL init failed");
    return;
  }

  connectWiFi();

  // Networking on core 0 (WiFi's core); the Arduino loop (UI + LVGL) owns core 1.
  xTaskCreatePinnedToCore(networkTask, "net", 16384, nullptr, 5, nullptr, 0);

  Serial.printf("[mem] free internal after init: %u B\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  Serial.println("Ready.");
}

void loop() {
  lv_timer_handler();

  // Handle a pending touch selection (radar records it; mutate under the lock).
  float ce = 0.0f;
  float cn = 0.0f;
  if (radarView.consumePendingClick(&ce, &cn)) {
    xSemaphoreTake(gMutex, portMAX_DELAY);
    tracker.selectNearestTo(ce, cn, 3.0f);
    xSemaphoreGive(gMutex);
    gDataDirty = true;
  }

  const unsigned long now = millis();

  static unsigned long lastFrameMs = 0;
  static size_t uiCount = 0;
  static bool uiHasSel = false;
  static bool everPainted = false;
  static char uiSelHex[8] = {0};
  static unsigned long lastClockMs = 0;

  // Animate + redraw the radar (under the lock; only when something changed).
  if (now - lastFrameMs >= kRadarFrameMs) {
    const float dt = lastFrameMs == 0 ? 0.0f : (now - lastFrameMs) / 1000.0f;
    lastFrameMs = now;

    xSemaphoreTake(gMutex, portMAX_DELAY);
    tracker.updatePositions(dt);
    const size_t n = tracker.count();
    const Aircraft *arr = tracker.aircraft();
    const Aircraft *sel = tracker.selected();
    uiHasSel = sel != nullptr;

    bool moving = false;
    for (size_t i = 0; i < n; ++i) {
      if (arr[i].groundSpeedKts > 0.0f) {
        moving = true;
        break;
      }
    }
    if (moving || gDataDirty || !everPainted) {
      radarView.setSnapshot(arr, n, uiHasSel ? sel->hex : "");
      radarView.redraw();
      everPainted = true;
    }
    uiCount = n;
    if (uiHasSel) {
      selCopy = *sel;
    }
    xSemaphoreGive(gMutex);
  }

  // Refresh the info-panel body on data / selection change.
  char curSel[8] = {0};
  if (uiHasSel) {
    strncpy(curSel, selCopy.hex, sizeof(curSel) - 1);
  }
  const bool selChanged = strcasecmp(uiSelHex, curSel) != 0;
  if (gDataDirty || selChanged) {
    gDataDirty = false;
    strncpy(uiSelHex, curSel, sizeof(uiSelHex) - 1);
    uiSelHex[sizeof(uiSelHex) - 1] = '\0';

    if (gNetStatus == 1) {
      infoPanel.showStatus("WiFi reconnecting...");
    } else if (uiHasSel) {
      infoPanel.showAircraft(&selCopy);
    } else {
      char buf[48];
      const unsigned kb = gFreeInternal / 1024;
      if (!gHadFirstFetch) {
        snprintf(buf, sizeof(buf), "Scanning... (free %uk)", kb);
      } else if (gNetStatus == 2) {
        snprintf(buf, sizeof(buf), "Fetch failed (free %uk)", kb);
      } else {
        snprintf(buf, sizeof(buf), "No aircraft nearby");
      }
      infoPanel.showStatus(buf);
    }
  }

  // Clock + stats once per second.
  if (now - lastClockMs >= 1000) {
    lastClockMs = now;
    infoPanel.updateClock();
    const long secsAgo =
        gLastFetchOkMs == 0 ? -1 : static_cast<long>((millis() - gLastFetchOkMs) / 1000);
    infoPanel.setStats(uiCount, secsAgo);
  }

  delay(2);
}
