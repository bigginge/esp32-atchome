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

#include "settings.hpp"
#include "net_control.hpp"
#include "geo_ip.hpp"
#include "crowpanel_display.hpp"
#include "gt911.hpp"
#include "api_client.hpp"
#include "tracker.hpp"
#include "radar_view.hpp"
#include "info_panel.hpp"
#include "settings_screen.hpp"

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
static SettingsScreen settingsScreen;

// snapshot: network-task fetch buffer. selCopy: UI copy of the selection.
static Aircraft snapshot[kMaxAircraft];
static Aircraft selCopy;

// Shared state between the UI task (core 1) and the network task (core 0).
static SemaphoreHandle_t gMutex = nullptr;
static TaskHandle_t gNetTaskHandle = nullptr;
static volatile bool gDataDirty = false;
static volatile int gNetStatus = 0;  // 0 ok, 1 wifi reconnecting, 2 fetch failed
static volatile unsigned long gLastFetchOkMs = 0;
static volatile bool gHadFirstFetch = false;
static volatile uint32_t gFreeInternal = 0;  // free internal heap (bytes)
// Set when a settings save invalidated the tracked aircraft; clears the UI's
// cached render state so the next paint is unconditional.
static bool gUiResetRequested = false;

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

// ===== Setup-mode worker (core 0) =====
// These run only while networkTask is parked, so they can take over the WiFi
// stack without racing an in-flight fetch.

static int runScan() {
  // Deliberately no WiFi.disconnect() first: esp_wifi_scan_start works while
  // associated, which keeps Cancel a true no-op. It does stall TCP for the
  // duration, which is harmless *because* the task is parked -- if the parking
  // ever goes away, this is where the mysterious fetch failures come from.
  // 120 ms/channel instead of the 300 ms default turns a ~4 s sweep into ~1.7 s.
  const int16_t n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                                      /*passive=*/false, /*max_ms_per_chan=*/120);
  ScanEntry *out = netctl::scanBuffer();
  size_t k = 0;
  for (int16_t i = 0; i < n && k < kMaxScanEntries; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;  // hidden AP; reachable via "Other network..."
    }
    bool duplicate = false;
    for (size_t j = 0; j < k; ++j) {
      if (strcmp(out[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;  // same network on 2.4 and 5 GHz, or a mesh node
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    strncpy(out[k].ssid, ssid.c_str(), sizeof(out[k].ssid) - 1);
    out[k].ssid[sizeof(out[k].ssid) - 1] = '\0';
    const int32_t rssi = WiFi.RSSI(i);
    out[k].rssi = static_cast<int8_t>(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
    out[k].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    ++k;
  }
  // The driver calloc's its record array from internal heap; release it now
  // rather than holding it for however long the wizard stays open.
  WiFi.scanDelete();

  // Strongest first (insertion sort; k <= 24).
  for (size_t a = 1; a < k; ++a) {
    const ScanEntry tmp = out[a];
    size_t b = a;
    while (b > 0 && out[b - 1].rssi < tmp.rssi) {
      out[b] = out[b - 1];
      --b;
    }
    out[b] = tmp;
  }

  netctl::setScanCount(k);
  Serial.printf("[setup] scan: %u networks\n", static_cast<unsigned>(k));
  return static_cast<int>(k);
}

static int runConnect() {
  const char *ssid = nullptr;
  const char *pass = nullptr;
  netctl::connectArgs(&ssid, &pass);
  Serial.printf("[setup] connecting to \"%s\"\n", ssid);

  WiFi.disconnect(false);
  WiFi.begin(ssid, pass[0] != '\0' ? pass : nullptr);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  const int status = static_cast<int>(WiFi.status());
  if (status == WL_CONNECTED) {
    Serial.printf("[setup] connected %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("[setup] connect failed, status %d\n", status);
  }
  return status;
}

static void serveSetupCommand() {
  SetupCmd cmd = SetupCmd::None;
  if (!netctl::takeCommand(&cmd)) {
    return;
  }

  int result = 0;
  switch (cmd) {
    case SetupCmd::Scan:
      result = runScan();
      break;
    case SetupCmd::Connect:
      result = runConnect();
      break;
    case SetupCmd::Geolocate:
      result = fetchIpLocation(netctl::geoBuffer()) ? 1 : 0;
      break;
    case SetupCmd::None:
      break;
  }
  netctl::completeCommand(result);
}

// ===== Network task (core 0): all HTTP lives here so the UI never blocks. =====

static void networkTask(void * /*arg*/) {
  char lastEnrichedHex[8] = {0};
  unsigned long lastEnrichAttempt = 0;
  bool lastEnrichFailed = false;
  unsigned long lastFetch = 0;
  uint32_t seenEpoch = netctl::epoch();

  for (;;) {
    // Park for the settings menu. This check has to stay at the very top of
    // the loop: it is the one point where no TLS transaction is in flight and
    // no lock is held, which is what makes handing the WiFi stack to the UI
    // safe without suspending this task.
    if (netctl::setupModeRequested()) {
      netctl::acknowledgeParked(true);
      serveSetupCommand();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    netctl::acknowledgeParked(false);

    if (seenEpoch != netctl::epoch()) {
      seenEpoch = netctl::epoch();
      lastFetch = 0;             // refetch now, against the new home location
      lastEnrichedHex[0] = '\0';
    }

    // Re-read every pass: the settings menu can change any of these from the
    // UI core while this task is running.
    double homeLat = 0.0;
    double homeLon = 0.0;
    int radiusNm = 0;
    char ssid[33];
    char pass[64];
    settings::snapshotForNetwork(&homeLat, &homeLon, &radiusNm, ssid, sizeof(ssid), pass,
                                 sizeof(pass));

    if (WiFi.status() != WL_CONNECTED) {
      gNetStatus = 1;
      gDataDirty = true;
      WiFi.disconnect();
      WiFi.begin(ssid, pass[0] != '\0' ? pass : nullptr);
      const unsigned long start = millis();
      // Bail out the moment the menu is opened. Users reach for the gear
      // precisely because WiFi is broken, so this is the common case, and
      // without the check parking here would take 15 s instead of 250 ms.
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000 &&
             !netctl::setupModeRequested()) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (netctl::setupModeRequested()) {
        continue;
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
    if (lastFetch == 0 || now - lastFetch >= settings::kRefreshIntervalMs) {
      lastFetch = now;
      gFreeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      Serial.printf("[mem] free internal %u B before fetch\n",
                    static_cast<unsigned>(gFreeInternal));
      size_t count = 0;
      const bool ok = fetchNearbyAircraft(static_cast<float>(homeLat),
                                          static_cast<float>(homeLon), radiusNm, snapshot,
                                          kMaxAircraft, &count);
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

/** Blocking connect that keeps the UI alive by pumping LVGL while it waits.
 *  UI task only. Leaves NTP to the caller so the boot path and the network
 *  task's reconnect path stay in step. */
static bool connectWiFi(const char *ssid, const char *pass, uint32_t timeoutMs) {
  infoPanel.showStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass != nullptr && pass[0] != '\0' ? pass : nullptr);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    lv_timer_handler();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WiFi] Connection failed");
  infoPanel.showStatus("WiFi failed");
  return false;
}

/** Applies the fallout of a settings save. Home lat/lon is the origin of every
 *  aircraft's east/north offsets *and* of its trail history, so a location
 *  change invalidates all of it and there is no in-place fix: the only correct
 *  answer is to throw the tracked set away and refetch. UI task only. */
static void applySettingsChange() {
  if (!settingsScreen.consumeSettingsChanged()) {
    return;
  }

  netctl::bumpEpoch();  // makes the network task refetch immediately

  xSemaphoreTake(gMutex, portMAX_DELAY);
  tracker.clear();
  xSemaphoreGive(gMutex);

  // radarView is exclusively UI-owned, so this is deliberately outside the lock.
  radarView.setRangeNm(static_cast<float>(settings::get().radiusNm));

  gUiResetRequested = true;
  gDataDirty = true;
  gHadFirstFetch = false;
  gLastFetchOkMs = 0;
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
  if (!radarView.create(screen, static_cast<float>(settings::get().radiusNm))) {
    return false;
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("===== ESP32 ATC Home =====");

  gMutex = xSemaphoreCreateMutex();

  // Before anything reads the radius or the credentials.
  settings::load();
  {
    const AppSettings &cfg = settings::get();
    Serial.printf("Home: %.6f, %.6f  radius %d nm\n", cfg.latitude, cfg.longitude,
                  cfg.radiusNm);
  }

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

  // Before the first WiFi call. The driver otherwise runs in persistent mode
  // and rewrites nvs.net80211 on every WiFi.begin() -- including the one the
  // reconnect loop issues every 15 s while the AP is down.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  // Networking on core 0 (WiFi's core); the Arduino loop (UI + LVGL) owns core 1.
  // Started before the wizard so the wizard can use it as its scan / connect /
  // geolocate worker; it parks immediately and costs nothing. Setup mode is
  // requested *first* so the task parks on its very first pass and never races
  // the boot connect below for the WiFi stack.
  netctl::enterSetupMode();
  xTaskCreatePinnedToCore(networkTask, "net", 16384, nullptr, 5, &gNetTaskHandle, 0);

  // The worker stays parked for this whole loop: connectWiFi() runs on the UI
  // task, and an unparked worker would see a disconnected radio and issue its
  // own competing WiFi.begin().
  bool connected = false;
  for (;;) {
    if (settings::hasWifi()) {
      const AppSettings &cfg = settings::get();
      connected = connectWiFi(cfg.ssid, cfg.password, 20000);
    }
    if (connected && settings::get().locationConfirmed) {
      break;
    }

    settingsScreen.open(connected ? SettingsStep::Location : SettingsStep::Network,
                        connected ? nullptr : "Set up a network to get started");
    while (settingsScreen.isOpen()) {
      lv_timer_handler();
      delay(5);
    }
    applySettingsChange();
    connected = WiFi.status() == WL_CONNECTED;
  }

  netctl::exitSetupMode();
  configTzTime("GMT0BST,M3.5.0,M10.5.0", "pool.ntp.org");

  Serial.printf("[mem] free internal after init: %u B\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  Serial.println("Ready.");
}

void loop() {
  lv_timer_handler();

  // ===== Settings wizard =====
  // While it is open, skip the rest of loop(): RadarView::redraw() rasterises
  // the full 480x480 PSRAM canvas every 50 ms whether or not anything will
  // blit it, and the wizard's overlay hides it anyway.
  static bool inSetupMode = false;
  if (settingsScreen.isOpen()) {
    delay(5);
    return;
  }
  if (inSetupMode) {
    inSetupMode = false;
    applySettingsChange();
    netctl::exitSetupMode();
  }
  if (infoPanel.consumeSettingsRequest()) {
    // Opened here rather than in the gear's event callback: doing it there
    // would re-enter lv_timer_handler() from inside an LVGL event, which
    // corrupts the display refresh rather than failing cleanly.
    radarView.consumePendingClick(nullptr, nullptr);  // drop a tap made just before
    inSetupMode = true;
    netctl::enterSetupMode();
    settingsScreen.open(SettingsStep::Location, nullptr);
    return;
  }

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

  if (gUiResetRequested) {
    gUiResetRequested = false;
    // The redraw below is gated on everPainted, so without clearing it a radius
    // change with no *moving* aircraft would leave the old rings on screen.
    everPainted = false;
    uiSelHex[0] = '\0';
    uiHasSel = false;
    uiCount = 0;
  }

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
