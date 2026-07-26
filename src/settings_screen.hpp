#pragma once

#include "settings.hpp"

#include <lvgl.h>

enum class SettingsStep : uint8_t {
  Preparing,      // waiting for the network task to park (and the first scan)
  Network,        // pick from the scan results
  ManualSsid,     // hidden network, typed by hand
  Password,
  Connecting,
  ConnectFailed,
  Location,
};

/** Full-screen setup wizard.
 *
 *  Runs entirely on the UI task. Anything needing the WiFi stack is posted to
 *  the parked network task through netctl and polled from tick(); nothing here
 *  ever blocks, and no LVGL call is made from a network context.
 *
 *  Button callbacks only ever record an intent -- every rebuild and the final
 *  teardown happen in tick(), because deleting the widget tree from inside one
 *  of its own event callbacks is a use-after-free, and calling
 *  lv_timer_handler() from a callback re-enters LVGL's timer handler. */
class SettingsScreen {
 public:
  /** `reason` is an optional line shown under the title (e.g. why the wizard
   *  opened). Not copied -- pass a literal or nullptr. */
  void open(SettingsStep entry, const char *reason);
  bool isOpen() const { return root_ != nullptr; }

  /** True once, after a save that changed the home location or radius and so
   *  invalidated every tracked aircraft. */
  bool consumeSettingsChanged();

 private:
  static void tickCb(lv_timer_t *timer);
  static void actionCb(lv_event_t *e);
  static void networkPickedCb(lv_event_t *e);
  static void focusCb(lv_event_t *e);

  void tick();
  void requestStep(SettingsStep step);
  void buildStep(SettingsStep step);
  void teardown();

  void buildPreparing();
  void buildNetwork();
  void buildTextEntry(bool password);
  void buildBusy(const char *message);
  void buildConnectFailed();
  void buildLocation();

  void pollPreparing();
  void pollNetwork();
  void pollConnecting();
  void pollLocation();

  void beginConnect();
  bool commitLocation();
  void refreshDetectedLabel();

  lv_obj_t *root_ = nullptr;
  lv_obj_t *content_ = nullptr;
  lv_timer_t *timer_ = nullptr;

  SettingsStep step_ = SettingsStep::Preparing;
  SettingsStep afterPrepare_ = SettingsStep::Network;
  SettingsStep next_ = SettingsStep::Preparing;
  bool hasNext_ = false;
  bool pendingClose_ = false;
  bool changed_ = false;
  bool canCancel_ = false;
  bool commandPosted_ = false;
  bool bailoutShown_ = false;
  bool geoPosted_ = false;
  unsigned long stepStartMs_ = 0;
  int lastResult_ = 0;
  const char *reason_ = nullptr;

  AppSettings draft_;
  char pickedSsid_[33] = {0};
  bool pickedSecure_ = true;

  lv_obj_t *statusLabel_ = nullptr;
  lv_obj_t *detectedLabel_ = nullptr;
  lv_obj_t *entryTa_ = nullptr;
  lv_obj_t *latTa_ = nullptr;
  lv_obj_t *lonTa_ = nullptr;
  lv_obj_t *radiusTa_ = nullptr;
  lv_obj_t *keyboard_ = nullptr;
};
