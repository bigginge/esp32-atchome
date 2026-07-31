#include <Arduino.h>
#include "log.hpp"
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>
#include <PCA9557.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
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
#include "theme.hpp"
#include "frame_probe.hpp"

static constexpr uint16_t kHorRes = theme::layout::kScreenW;
static constexpr uint16_t kVerRes = theme::layout::kScreenH;

static constexpr int kTouchSda = 19;
static constexpr int kTouchScl = 20;
static constexpr uint32_t kI2cHz = 300000;

// Animation cadence. The panel refreshes at ~24 Hz and the flush waits on
// VSYNC (tear-free), so this is an upper bound; idle frames are skipped.
//
// 40 ms rather than 50 so that it divides RadarView::kSweepStepMs: the beam
// steps on a whole number of these ticks, which is what stops it alternating
// between a long and a short step and reading as a stutter.
static constexpr unsigned long kRadarFrameMs = 40;

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
    {
      probe::Scope _(probe::kPresent);
      esp_lcd_panel_draw_bitmap(lcd.panel(), 0, 0, kHorRes, kVerRes, pxMap);
    }
    // Timed separately: this is idle time waiting for the panel, not work. At
    // ~24 Hz it averages ~21 ms and would swamp every other number.
    {
      probe::Scope _(probe::kVsync);
      lcd.waitVsync();
    }
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

// The active regulatory domain, e.g. "GB ch 1-13". Worth printing rather than
// assuming: the policy is AUTO, so an associated station reports the AP's
// country, not necessarily the one we asked for. Read-only, so either core may
// call it.
static const char *describeCountry(char *buf, size_t size) {
  wifi_country_t c;
  if (esp_wifi_get_country(&c) != ESP_OK) {
    snprintf(buf, size, "country unknown");
  } else {
    snprintf(buf, size, "%.2s ch %u-%u", c.cc, static_cast<unsigned>(c.schan),
             static_cast<unsigned>(c.schan + c.nchan - 1));
  }
  return buf;
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

  // out[0..k) is kept sorted strongest-first as we go rather than sorted at the
  // end, so that when more than kMaxScanEntries unique SSIDs are in range we can
  // evict the weakest instead of keeping whichever ones the driver happened to
  // report first.
  for (int16_t i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;  // hidden AP; reachable via "Other network..."
    }
    const int32_t raw = WiFi.RSSI(i);
    const int8_t rssi = static_cast<int8_t>(raw < -127 ? -127 : (raw > 0 ? 0 : raw));
    // Per-BSS, before dedup and before the cap: the channel column is how you
    // tell whether the regulatory domain is letting the sweep reach 12/13.
    Log.printf("[setup]   ch %2d  %4d dBm  %s\n", static_cast<int>(WiFi.channel(i)),
                  static_cast<int>(rssi), ssid.c_str());

    size_t dup = k;
    for (size_t j = 0; j < k; ++j) {
      if (strcmp(out[j].ssid, ssid.c_str()) == 0) {
        dup = j;  // same network on 2.4 and 5 GHz, or a mesh node
        break;
      }
    }

    size_t slot;
    if (dup < k) {
      // One row per SSID, and the strongest sighting sets what we show -- both
      // fields together, so the row describes one BSS rather than mixing the
      // signal of one band with the security of another.
      if (rssi <= out[dup].rssi) {
        continue;
      }
      out[dup].rssi = rssi;
      out[dup].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      slot = dup;
    } else {
      if (k < kMaxScanEntries) {
        slot = k++;
      } else if (rssi > out[k - 1].rssi) {
        slot = k - 1;  // full, and this beats the weakest we kept
      } else {
        continue;
      }
      strncpy(out[slot].ssid, ssid.c_str(), sizeof(out[slot].ssid) - 1);
      out[slot].ssid[sizeof(out[slot].ssid) - 1] = '\0';
      out[slot].rssi = rssi;
      out[slot].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }

    // Shift the touched entry forward until the order holds again.
    const ScanEntry tmp = out[slot];
    size_t b = slot;
    while (b > 0 && out[b - 1].rssi < tmp.rssi) {
      out[b] = out[b - 1];
      --b;
    }
    out[b] = tmp;
  }
  // The driver calloc's its record array from internal heap; release it now
  // rather than holding it for however long the wizard stays open.
  WiFi.scanDelete();

  netctl::setScanCount(k);

  char domain[24];
  Log.printf("[setup] scan: %u networks (%s)\n", static_cast<unsigned>(k),
                describeCountry(domain, sizeof(domain)));
  return static_cast<int>(k);
}

static int runConnect() {
  const char *ssid = nullptr;
  const char *pass = nullptr;
  netctl::connectArgs(&ssid, &pass);
  Log.printf("[setup] connecting to \"%s\"\n", ssid);

  WiFi.disconnect(false);
  WiFi.begin(ssid, pass[0] != '\0' ? pass : nullptr);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  const int status = static_cast<int>(WiFi.status());
  if (status == WL_CONNECTED) {
    Log.printf("[setup] connected %s\n", WiFi.localIP().toString().c_str());
  } else {
    Log.printf("[setup] connect failed, status %d\n", status);
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
        Log.printf("[WiFi] Reconnected %s\n", WiFi.localIP().toString().c_str());
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
      Log.printf("[mem] free internal %u B before fetch\n",
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
    Log.printf("[WiFi] Connected %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Log.println("[WiFi] Connection failed");
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

#if ATC_BENCH
/**
 * The actual CPU clock, counted rather than asked for.
 *
 * Every "cycles per pixel" figure in PERFORMANCE.md is quoted against 240 MHz,
 * and both the board default (`build.f_cpu`) and the shipped sdkconfig
 * (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`, `CONFIG_PM_ENABLE` unset) agree. But
 * `build.f_cpu` is only a -DF_CPU define and the sdkconfig is what the
 * *precompiled* libraries were built with -- exactly the pair of assumptions
 * that turned out to be wrong about the PSRAM clock. So count CCOUNT against
 * micros() and read the answer off the silicon.
 */
static void reportClocks() {
  const uint32_t c0 = ESP.getCycleCount();
  const uint32_t t0 = micros();
  delay(200);
  const uint32_t c1 = ESP.getCycleCount();
  const uint32_t t1 = micros();
  const double mhz = static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
  Log.printf("[clk] api=%u MHz  measured=%.1f MHz  xtal=%u MHz  apb=%u Hz\n",
             static_cast<unsigned>(getCpuFrequencyMhz()), mhz,
             static_cast<unsigned>(getXtalFrequencyMhz()),
             static_cast<unsigned>(getApbFrequency()));
}

/**
 * What PSRAM bandwidth is actually available with the LCD scanning out?
 *
 * The panel reads 800*480*2 B at ~24 Hz = ~18 MB/s continuously through the
 * bounce buffers, so the figures here are the *contended* numbers -- which are
 * the only ones that matter for judging the render path.
 */
static void benchPsram() {
  constexpr size_t kBytes = 448u * 448u * 2u;
  uint8_t *a = static_cast<uint8_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM));
  uint8_t *b = static_cast<uint8_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM));
  if (a == nullptr || b == nullptr) {
    Log.println("[bench] alloc failed");
    heap_caps_free(a);
    heap_caps_free(b);
    return;
  }
  const float mb = static_cast<float>(kBytes) / (1024.0f * 1024.0f);

  uint32_t t = micros();
  memset(a, 0, kBytes);
  const uint32_t tMemset = micros() - t;

  t = micros();
  memcpy(b, a, kBytes);
  const uint32_t tMemcpy = micros() - t;

  // The shape lv_canvas_fill_bg uses: a scalar 16-bit store loop.
  t = micros();
  uint16_t *p = reinterpret_cast<uint16_t *>(a);
  for (size_t i = 0; i < kBytes / 2; ++i) p[i] = 0x1234;
  const uint32_t tStore16 = micros() - t;

  // Row-wise copy, the shape a dirty-rect restore would use (128 px rows).
  t = micros();
  for (size_t y = 0; y < 448; ++y) {
    memcpy(b + y * 896, a + y * 896, 256);
  }
  const uint32_t tRows = micros() - t;

  Log.printf("[bench] %u KB: memset=%lu us (%.1f MB/s) memcpy=%lu us (%.1f MB/s) "
             "store16=%lu us (%.1f MB/s) rows448x256B=%lu us\n",
             static_cast<unsigned>(kBytes / 1024),
             static_cast<unsigned long>(tMemset), mb * 1e6f / tMemset,
             static_cast<unsigned long>(tMemcpy), mb * 1e6f / tMemcpy,
             static_cast<unsigned long>(tStore16), mb * 1e6f / tStore16,
             static_cast<unsigned long>(tRows));

  heap_caps_free(a);
  heap_caps_free(b);
}

/**
 * The same shapes against *internal* SRAM, and against PSRAM at the same size.
 *
 * This is the comparison the probe harness never had. Every conclusion in
 * PERFORMANCE.md rests on "the render path is memory-bound, not compute-bound",
 * and that has only ever been inferred -- from blendSpan running at roughly
 * memcpy speed. Running identical loops over both memories settles it directly,
 * and the ratio bounds how much *any* change to the memory system can be worth:
 * if PSRAM is 20x slower than SRAM, the bus is the wall and Tier 1 has room; if
 * it is 2x, there is far less on the table than that section claims.
 *
 * Small buffers deliberately -- they have to fit internal SRAM alongside the
 * background RLE, and both arms use the same size so the comparison is honest.
 *
 * READ THE PSRAM ARM CAREFULLY. At 8 KB both buffers sit inside the 32 KB data
 * cache, so the "psram" figures here are cache-resident throughput, NOT bus
 * throughput -- measured 173.8 MB/s memset against benchPsram()'s 17.7 MB/s for
 * the same operation at 392 KB. That 10x gap between the same memory hit two
 * ways is the most useful number either benchmark produces:
 *
 *   - it settles the bus-bound question outright. Streaming PSRAM runs ~8.8 MB/s
 *     against internal SRAM's ~308 MB/s, roughly 35x, so PERFORMANCE.md's
 *     central claim is correct and was never actually measured before this.
 *   - it says making a working set *fit* is worth as much as making the bus
 *     faster, and costs no build-system migration. That is exactly what the
 *     background RLE did: 400 KB of streaming PSRAM became 36 KB of SRAM.
 *
 * So do not read this as a PSRAM-vs-SRAM bus comparison; it is not one, and
 * sizing the buffers up to make it one is impossible -- internal SRAM cannot
 * hold a buffer big enough to defeat the cache.
 */
static void benchSram() {
  constexpr size_t kBytes = 8u * 1024u;
  constexpr int kReps = 256;  // small buffers, so repeat to get past timer noise
  uint8_t *ps = static_cast<uint8_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM));
  uint8_t *pd = static_cast<uint8_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM));
  uint8_t *is = static_cast<uint8_t *>(
      heap_caps_malloc(kBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  uint8_t *id = static_cast<uint8_t *>(
      heap_caps_malloc(kBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (ps == nullptr || pd == nullptr || is == nullptr || id == nullptr) {
    Log.printf("[bench2] alloc failed: %u B free internal, largest block %u B\n",
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    heap_caps_free(ps); heap_caps_free(pd); heap_caps_free(is); heap_caps_free(id);
    return;
  }
  const float mb = static_cast<float>(kBytes) * kReps / (1024.0f * 1024.0f);

  auto timeSet = [&](uint8_t *p) {
    const uint32_t t = micros();
    for (int i = 0; i < kReps; ++i) memset(p, i, kBytes);
    return micros() - t;
  };
  auto timeCpy = [&](uint8_t *d, uint8_t *s) {
    const uint32_t t = micros();
    for (int i = 0; i < kReps; ++i) memcpy(d, s, kBytes);
    return micros() - t;
  };

  const uint32_t psSet = timeSet(ps);
  const uint32_t isSet = timeSet(is);
  const uint32_t ppCpy = timeCpy(pd, ps);   // PSRAM -> PSRAM
  const uint32_t iiCpy = timeCpy(id, is);   // SRAM  -> SRAM
  const uint32_t piCpy = timeCpy(id, ps);   // PSRAM -> SRAM, i.e. the RLE's old read

  Log.printf("[bench2] %uKB x%d  memset: psram=%.1f sram=%.1f MB/s (%.1fx)  "
             "memcpy: p->p=%.1f s->s=%.1f p->s=%.1f MB/s (s->s is %.1fx p->p)\n",
             static_cast<unsigned>(kBytes / 1024), kReps,
             mb * 1e6f / psSet, mb * 1e6f / isSet,
             static_cast<double>(psSet) / isSet,
             mb * 1e6f / ppCpy, mb * 1e6f / iiCpy, mb * 1e6f / piCpy,
             static_cast<double>(ppCpy) / iiCpy);

  heap_caps_free(ps); heap_caps_free(pd); heap_caps_free(is); heap_caps_free(id);
}
#endif

#if ATC_BENCH
/**
 * PSRAM memcpy throughput against working-set size.
 *
 * The point is Tier 1 of PERFORMANCE.md, specifically the half of it that
 * proposes doubling the data cache from 32 KB to 64 KB. That is only worth
 * anything if the working sets we actually stream are near the cache size --
 * if they are 400 KB, twice a 32 KB cache is still nowhere near enough and the
 * setting cannot help, whatever it costs to change.
 *
 * Sweeping the size maps where the cliff is and how steep it is, which answers
 * that for this workload without building anything. Sizes straddle the 32 KB
 * cache deliberately: the last size that fits, the first that does not, and the
 * scale the renderer actually works at.
 */
static void benchCacheCurve() {
  constexpr size_t kMax = 448u * 448u * 2u;
  uint8_t *a = static_cast<uint8_t *>(heap_caps_malloc(kMax, MALLOC_CAP_SPIRAM));
  uint8_t *b = static_cast<uint8_t *>(heap_caps_malloc(kMax, MALLOC_CAP_SPIRAM));
  if (a == nullptr || b == nullptr) {
    Log.println("[curve] alloc failed");
    heap_caps_free(a);
    heap_caps_free(b);
    return;
  }
  static const size_t kSizes[] = {4096,   8192,   16384,  24576,  32768,
                                  49152,  65536,  131072, 262144, kMax};
  Log.print("[curve] psram memcpy MB/s by working set:");
  for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i) {
    const size_t n = kSizes[i];
    // Same total bytes moved at every size, so timer resolution and loop
    // overhead do not masquerade as a throughput difference.
    const int reps = static_cast<int>(kMax * 4 / n);
    const uint32_t t = micros();
    for (int r = 0; r < reps; ++r) memcpy(b, a, n);
    const uint32_t dt = micros() - t;
    const float mb = static_cast<float>(n) * reps / (1024.0f * 1024.0f);
    Log.printf(" %uK=%.0f", static_cast<unsigned>(n / 1024), mb * 1e6f / dt);
  }
  Log.println("");
  heap_caps_free(a);
  heap_caps_free(b);
}
#endif

static bool initLvgl() {
  lv_init();
  lv_tick_set_cb(lvTickCb);
  // Styles are plain globals, but their property storage is lv_malloc'd, so
  // this has to run after lv_init() and before the first object is created.
  theme::init();

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
  lv_obj_set_style_bg_color(screen, theme::c(theme::kBgApp), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  infoPanel.create(screen);
  if (!radarView.create(screen, static_cast<float>(settings::get().radiusNm))) {
    return false;
  }

  return true;
}

void setup() {
  Log.begin(115200);
  delay(200);
  Log.println();
  Log.println("===== ESP32 ATC Home =====");

  gMutex = xSemaphoreCreateMutex();

  // Before anything reads the radius or the credentials.
  settings::load();
  {
    const AppSettings &cfg = settings::get();
    Log.printf("Home: %.6f, %.6f  radius %d nm\n", cfg.latitude, cfg.longitude,
                  cfg.radiusNm);
  }

  Log.println("Reset touch controller...");
  resetTouchViaPca9557();

  Log.println("Init display...");
  if (!lcd.init()) {
    Log.println("Display init failed");
    return;
  }
  lcd.setBrightness(255);

  Log.println("Init LVGL...");
  if (!initLvgl()) {
    Log.println("LVGL init failed");
    return;
  }

  // Before the first WiFi call. The driver otherwise runs in persistent mode
  // and rewrites nvs.net80211 on every WiFi.begin() -- including the one the
  // reconnect loop issues every 15 s while the AP is down.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  // The driver defaults to "01" (world safe mode, channels 1-11) and only widens
  // to the AP's advertised country once associated -- so an unassociated scan
  // silently misses channels 12/13, and the wizard's network list is short in
  // exactly the case where it matters. ieee80211d stays enabled, so this only
  // sets the *unassociated* baseline; a connected station still follows the AP.
  // Must come after mode() (which runs esp_wifi_init) and after persistent(false)
  // (which selects WIFI_STORAGE_RAM, so this doesn't write flash every boot).
  const esp_err_t countryErr = esp_wifi_set_country_code(settings::kWifiCountry, true);
  if (countryErr != ESP_OK) {
    Log.printf("[wifi] country %s rejected: %s\n", settings::kWifiCountry,
                  esp_err_to_name(countryErr));
  }
  {
    char domain[24];
    Log.printf("[wifi] %s\n", describeCountry(domain, sizeof(domain)));
  }

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

  Log.printf("[mem] free internal after init: %u B\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#if ATC_BENCH
  reportClocks();
  benchPsram();
  benchSram();
  benchCacheCurve();
#endif
  Log.println("Ready.");
}

void loop() {
  {
    probe::Scope _(probe::kLvgl);
    lv_timer_handler();
  }
  probe::report(millis());

  // ===== Settings wizard =====
  // While it is open, skip the rest of loop(): RadarView::redraw() rasterises
  // the full PSRAM canvas every 50 ms whether or not anything will blit it,
  // and the wizard's overlay hides it anyway.
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

    // New data grows the trails, which the per-frame dirty rects do not cover
    // -- those track only the symbols and labels, all that moves between
    // fetches. A full repaint here would be a ~200 ms hitch every 5 seconds,
    // so only the trail extents are invalidated.
    if (gDataDirty) {
      radarView.markDataChanged();
    }
    if (!everPainted) {
      radarView.markFullRepaint();
    }
    // Unconditionally: redraw() owns the decision now, and it has to, because
    // the sweep beam advances on wall-clock time whether or not any aircraft
    // moved. It still returns without painting when neither did -- which is
    // also why the old "is anything moving?" scan is gone rather than merely
    // ignored. It answered a coarser version of the same question.
    radarView.setSnapshot(arr, n, uiHasSel ? sel->hex : "");
    radarView.redraw();
    everPainted = true;
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
