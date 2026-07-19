#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>
#include <PCA9557.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "crowpanel_display.hpp"
#include "api_client.hpp"
#include "tracker.hpp"
#include "radar_view.hpp"
#include "info_panel.hpp"

static constexpr uint16_t kHorRes = 800;
static constexpr uint16_t kVerRes = 480;
static constexpr uint16_t kBufLines = 40;

static constexpr int kTouchSda = 19;
static constexpr int kTouchScl = 20;
static constexpr uint32_t kI2cHz = 300000;

static CrowPanelDisplay lcd;
static PCA9557 ioExpander(0x18, &Wire);

static Tracker tracker;
static RadarView radarView;
static InfoPanel infoPanel;

static Aircraft snapshot[kMaxAircraft];
static unsigned long lastFetchMs = 0;
static char lastEnrichedHex[8] = {0};
static unsigned long lastEnrichmentAttemptMs = 0;
static bool lastEnrichmentAttemptFailed = false;
static bool fetchInProgress = false;

static constexpr unsigned long kEnrichmentSuccessIntervalMs = 1200;
static constexpr unsigned long kEnrichmentRetryIntervalMs = 5000;

static uint32_t lvTickCb() { return millis(); }

static void dispFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  const uint32_t w = lv_area_get_width(area);
  const uint32_t h = lv_area_get_height(area);
  lcd.pushImage(area->x1, area->y1, w, h, reinterpret_cast<uint16_t *>(pxMap));
  lv_display_flush_ready(disp);
}

static void touchRead(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
  uint16_t x = 0;
  uint16_t y = 0;
  if (lcd.getTouch(&x, &y)) {
    if (x >= kHorRes) {
      x = kHorRes - 1;
    }
    if (y >= kVerRes) {
      y = kVerRes - 1;
    }
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

static void refreshInfoPanel() {
  infoPanel.showAircraft(tracker.selected());
}

static void enrichSelectedIfNeeded() {
  Aircraft *selected = tracker.selected();
  if (selected == nullptr) {
    lastEnrichedHex[0] = '\0';
    infoPanel.showStatus("No aircraft nearby");
    radarView.redraw();
    return;
  }

  const bool selectionChanged = strcasecmp(lastEnrichedHex, selected->hex) != 0;
  if (selectionChanged) {
    strncpy(lastEnrichedHex, selected->hex, sizeof(lastEnrichedHex) - 1);
    lastEnrichedHex[sizeof(lastEnrichedHex) - 1] = '\0';
    // Permit the first enrichment request for a new selection immediately.
    lastEnrichmentAttemptMs = 0;
    lastEnrichmentAttemptFailed = false;
    refreshInfoPanel();
    radarView.redraw();
  }

  if (selected->detailsLoaded &&
      (selected->routeLoaded || selected->callsign[0] == '\0')) {
    return;
  }

  const unsigned long now = millis();
  const unsigned long retryInterval = lastEnrichmentAttemptFailed
                                          ? kEnrichmentRetryIntervalMs
                                          : kEnrichmentSuccessIntervalMs;
  if (!selectionChanged && now - lastEnrichmentAttemptMs < retryInterval) {
    return;
  }
  lastEnrichmentAttemptMs = now;

  // Perform only one HTTPS request per pass through the loop.  This keeps
  // LVGL and the task watchdog serviced between detail/route/airport calls.
  bool updated = false;
  if (!selected->detailsLoaded) {
    updated = fetchAircraftDetails(*selected);
  } else if (!selected->routeLoaded && selected->callsign[0] != '\0') {
    updated = fetchRouteInfo(*selected) || updated;
  }

  // A failed service must not be retried on every five-millisecond loop.
  lastEnrichmentAttemptFailed = !updated;

  if (updated || selectionChanged) {
    refreshInfoPanel();
    radarView.redraw();
  }
}

static void fetchAndUpdate() {
  if (fetchInProgress) {
    return;
  }
  fetchInProgress = true;

  if (WiFi.status() != WL_CONNECTED) {
    infoPanel.showStatus("WiFi disconnected");
    fetchInProgress = false;
    return;
  }

  infoPanel.showStatus("Scanning...");

  size_t count = 0;
  const bool ok = fetchNearbyAircraft(MY_LATITUDE, MY_LONGITUDE, SEARCH_RADIUS_NM,
                                      snapshot, kMaxAircraft, &count);
  if (!ok) {
    if (tracker.selected() == nullptr) {
      infoPanel.showStatus("Fetch failed");
    } else {
      refreshInfoPanel();
    }
    fetchInProgress = false;
    lastFetchMs = millis();
    return;
  }

  tracker.mergeSnapshot(snapshot, count);
  radarView.redraw();

  if (tracker.selected() == nullptr) {
    infoPanel.showStatus("No aircraft nearby");
    lastEnrichedHex[0] = '\0';
  } else {
    enrichSelectedIfNeeded();
    refreshInfoPanel();
  }

  lastFetchMs = millis();
  fetchInProgress = false;
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
  } else {
    Serial.println("[WiFi] Connection failed");
    infoPanel.showStatus("WiFi failed");
  }
}

static bool initLvgl() {
  lv_init();
  lv_tick_set_cb(lvTickCb);

  const size_t bufBytes = static_cast<size_t>(kHorRes) * kBufLines * sizeof(lv_color_t);
  auto *buf1 = static_cast<lv_color_t *>(
      heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *buf2 = static_cast<lv_color_t *>(
      heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf1 == nullptr || buf2 == nullptr) {
    Serial.println("Failed to allocate LVGL draw buffers in PSRAM");
    return false;
  }

  lv_display_t *disp = lv_display_create(kHorRes, kVerRes);
  lv_display_set_flush_cb(disp, dispFlush);
  lv_display_set_buffers(disp, buf1, buf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchRead);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1218), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  infoPanel.create(screen);
  if (!radarView.create(screen, &tracker, static_cast<float>(SEARCH_RADIUS_NM))) {
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

  Serial.println("Reset touch controller...");
  resetTouchViaPca9557();

  Serial.println("Init display...");
  lcd.init();
  lcd.setBrightness(255);
  lcd.setSwapBytes(true);   // Correct RGB565 byte order: panel expects bytes swapped vs LVGL's output
  lcd.fillScreen(TFT_BLACK);

  Serial.println("Init LVGL...");
  if (!initLvgl()) {
    Serial.println("LVGL init failed");
    return;
  }

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    fetchAndUpdate();
  }

  Serial.println("Ready.");
}

void loop() {
  lv_timer_handler();

  // Refresh info when user taps a different aircraft (radar redraws on click;
  // enrichment runs here so HTTP stays out of the LVGL event callback).
  Aircraft *selected = tracker.selected();
  if (selected != nullptr && strcasecmp(lastEnrichedHex, selected->hex) != 0) {
    enrichSelectedIfNeeded();
  }

  const unsigned long now = millis();
  if (!fetchInProgress && (lastFetchMs == 0 || now - lastFetchMs >= REFRESH_INTERVAL_MS)) {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    if (WiFi.status() == WL_CONNECTED) {
      fetchAndUpdate();
    }
  }

  delay(5);
}
