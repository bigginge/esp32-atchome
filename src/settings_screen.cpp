#include "settings_screen.hpp"

#include "net_control.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int32_t kScreenW = 800;
constexpr int32_t kScreenH = 480;
constexpr int32_t kKeyboardH = 220;

// Matches the radar / info panel palette.
constexpr uint32_t kBg = 0x0B1218;
constexpr uint32_t kPanel = 0x121A22;
constexpr uint32_t kRaised = 0x1E2A36;
constexpr uint32_t kText = 0xE8EEF4;
constexpr uint32_t kMuted = 0x7A8A9A;
constexpr uint32_t kAccent = 0x2D7FF9;
constexpr uint32_t kDanger = 0xE05B4B;

// Waiting for the worker to park costs at most one in-flight fetch plus one
// enrichment call, i.e. two 4 s HTTP timeouts back to back.
constexpr unsigned long kParkTimeoutMs = 12000;

enum class Action : int {
  None = 0,
  Rescan,
  ManualSsid,
  Cancel,
  Back,
  Connect,
  ToggleReveal,
  Retry,
  UseDetected,
  ChangeWifi,
  Save,
};

// The keyboard's character keys repeat on long press; only its control keys
// carry NO_REPEAT by default. A deliberate touchscreen press easily crosses the
// threshold, so every key gets NO_REPEAT except backspace, where repeating is
// what you actually want.
const char *const kNumberMap[] = {"1", "2", "3", LV_SYMBOL_KEYBOARD, "\n",
                                  "4", "5", "6", LV_SYMBOL_OK, "\n",
                                  "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
                                  "+/-", "0", ".", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, ""};

// One column wide. `kKey` suppresses auto-repeat; `kRepeatKey` keeps it, which
// is only wanted on backspace.
constexpr lv_buttonmatrix_ctrl_t kKey =
    static_cast<lv_buttonmatrix_ctrl_t>(1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
constexpr lv_buttonmatrix_ctrl_t kRepeatKey = static_cast<lv_buttonmatrix_ctrl_t>(1);

const lv_buttonmatrix_ctrl_t kNumberCtrl[] = {
    kKey, kKey, kKey, kKey,        // 1 2 3 [abc]
    kKey, kKey, kKey, kKey,        // 4 5 6 [ok]
    kKey, kKey, kKey, kRepeatKey,  // 7 8 9 [bksp]
    kKey, kKey, kKey, kKey, kKey,  // +/- 0 . [<] [>]
};

lv_obj_t *makePlain(lv_obj_t *parent, int32_t w, int32_t h) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text != nullptr ? text : "");
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, Action action, lv_event_cb_t cb,
                     void *self, int32_t w, bool primary = false) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, w, 52);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? kAccent : kRaised), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, self);

  lv_obj_t *label = makeLabel(btn, text, &lv_font_montserrat_16, kText);
  lv_obj_center(label);
  return btn;
}

lv_obj_t *makeField(lv_obj_t *parent, const char *caption, int32_t y, const char *value,
                    const char *acceptedChars, lv_event_cb_t focusCb, void *self) {
  lv_obj_t *cap = makeLabel(parent, caption, &lv_font_montserrat_16, kMuted);
  lv_obj_set_pos(cap, 40, y + 14);

  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, 190, y);
  lv_obj_set_size(ta, 260, 50);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_accepted_chars(ta, acceptedChars);
  lv_textarea_set_text(ta, value);
  lv_obj_set_style_bg_color(ta, lv_color_hex(kRaised), 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(kMuted), 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(kText), 0);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
  lv_obj_add_event_cb(ta, focusCb, LV_EVENT_FOCUSED, self);
  lv_obj_add_event_cb(ta, focusCb, LV_EVENT_CLICKED, self);
  return ta;
}

}  // namespace

// ===== lifecycle =====

void SettingsScreen::open(SettingsStep entry, const char *reason) {
  if (root_ != nullptr) {
    return;  // debounce: a second gear tap while open is a no-op
  }

  draft_ = settings::get();
  reason_ = reason;
  // Cancel is only offered when there is a working configuration to go back to;
  // on a fresh device the wizard is the only way forward.
  canCancel_ = settings::hasWifi() && settings::get().locationConfirmed;
  changed_ = false;
  pendingClose_ = false;
  hasNext_ = false;
  afterPrepare_ = entry;

  root_ = lv_obj_create(lv_screen_active());
  lv_obj_set_size(root_, kScreenW, kScreenH);
  lv_obj_set_pos(root_, 0, 0);
  // Opaque, square and border-free so LVGL's cover check picks this object and
  // skips rendering the radar canvas underneath. A radius here would quietly
  // cost a full 480x480 canvas rasterisation every frame.
  lv_obj_set_style_bg_color(root_, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(root_, 0, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_pad_all(root_, 0, 0);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

  timer_ = lv_timer_create(tickCb, 100, this);
  buildStep(SettingsStep::Preparing);
}

bool SettingsScreen::consumeSettingsChanged() {
  const bool v = changed_;
  changed_ = false;
  return v;
}

void SettingsScreen::teardown() {
  if (timer_ != nullptr) {
    lv_timer_delete(timer_);
    timer_ = nullptr;
  }
  if (root_ != nullptr) {
    lv_obj_delete(root_);
    root_ = nullptr;
  }
  content_ = nullptr;
  statusLabel_ = nullptr;
  detectedLabel_ = nullptr;
  entryTa_ = nullptr;
  latTa_ = nullptr;
  lonTa_ = nullptr;
  radiusTa_ = nullptr;
  keyboard_ = nullptr;
}

// ===== step machine =====

void SettingsScreen::requestStep(SettingsStep step) {
  next_ = step;
  hasNext_ = true;
}

void SettingsScreen::buildStep(SettingsStep step) {
  hasNext_ = false;
  step_ = step;
  stepStartMs_ = millis();
  commandPosted_ = false;
  bailoutShown_ = false;

  if (content_ != nullptr) {
    lv_obj_delete(content_);
  }
  statusLabel_ = nullptr;
  detectedLabel_ = nullptr;
  entryTa_ = nullptr;
  latTa_ = nullptr;
  lonTa_ = nullptr;
  radiusTa_ = nullptr;
  keyboard_ = nullptr;

  content_ = makePlain(root_, kScreenW, kScreenH);
  lv_obj_set_pos(content_, 0, 0);

  switch (step) {
    case SettingsStep::Preparing:
      buildPreparing();
      break;
    case SettingsStep::Network:
      buildNetwork();
      break;
    case SettingsStep::ManualSsid:
      buildTextEntry(false);
      break;
    case SettingsStep::Password:
      buildTextEntry(true);
      break;
    case SettingsStep::Connecting:
      buildBusy("Connecting...");
      break;
    case SettingsStep::ConnectFailed:
      buildConnectFailed();
      break;
    case SettingsStep::Location:
      buildLocation();
      break;
  }
}

void SettingsScreen::tickCb(lv_timer_t *timer) {
  static_cast<SettingsScreen *>(lv_timer_get_user_data(timer))->tick();
}

void SettingsScreen::tick() {
  if (pendingClose_) {
    teardown();
    return;
  }
  if (hasNext_) {
    buildStep(next_);
    return;
  }

  switch (step_) {
    case SettingsStep::Preparing:
      pollPreparing();
      break;
    case SettingsStep::Network:
      pollNetwork();
      break;
    case SettingsStep::Connecting:
      pollConnecting();
      break;
    case SettingsStep::Location:
      pollLocation();
      break;
    default:
      break;
  }
}

// ===== Preparing =====

void SettingsScreen::buildPreparing() {
  lv_obj_t *spinner = lv_spinner_create(content_);
  lv_obj_set_size(spinner, 64, 64);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(kRaised), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(kAccent), LV_PART_INDICATOR);

  statusLabel_ = makeLabel(content_, "Preparing...", &lv_font_montserrat_20, kText);
  lv_obj_align(statusLabel_, LV_ALIGN_CENTER, 0, 40);
}

void SettingsScreen::pollPreparing() {
  if (!netctl::isParked()) {
    if (!bailoutShown_ && millis() - stepStartMs_ > kParkTimeoutMs) {
      // The worker is wedged, most likely in a stuck TLS handshake. Rather than
      // spin forever, offer a way out. This must not touch commandPosted_: if
      // the worker recovers afterwards, that flag still has to gate the scan.
      bailoutShown_ = true;
      lv_label_set_text(statusLabel_, "Network is busy.");
      lv_obj_align(makeButton(content_, "Try again", Action::Retry, actionCb, this, 180,
                              /*primary=*/true),
                   LV_ALIGN_CENTER, canCancel_ ? -100 : 0, 110);
      if (canCancel_) {
        lv_obj_align(makeButton(content_, "Close", Action::Cancel, actionCb, this, 180),
                     LV_ALIGN_CENTER, 100, 110);
      }
    }
    return;
  }

  if (afterPrepare_ == SettingsStep::Network) {
    if (!commandPosted_) {
      if (netctl::post(SetupCmd::Scan)) {
        commandPosted_ = true;
        lv_label_set_text(statusLabel_, "Scanning for networks...");
      }
      return;
    }
    if (netctl::commandDone(&lastResult_)) {
      requestStep(SettingsStep::Network);
    }
    return;
  }

  requestStep(afterPrepare_);
}

// ===== Network =====

void SettingsScreen::buildNetwork() {
  lv_obj_set_pos(makeLabel(content_, "WiFi Setup", &lv_font_montserrat_20, kText), 24, 16);

  size_t count = 0;
  const ScanEntry *entries = netctl::scanResults(&count);

  const char *subtitle = reason_ != nullptr ? reason_ : "Choose a network";
  statusLabel_ = makeLabel(content_, count == 0 ? "No networks found" : subtitle,
                           &lv_font_montserrat_14, kMuted);
  lv_obj_set_pos(statusLabel_, 24, 46);

  lv_obj_t *list = lv_list_create(content_);
  lv_obj_set_size(list, 752, 286);
  lv_obj_set_pos(list, 24, 74);
  lv_obj_set_style_bg_color(list, lv_color_hex(kPanel), 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 8, 0);
  lv_obj_set_style_pad_all(list, 4, 0);

  for (size_t i = 0; i < count; ++i) {
    lv_obj_t *row = lv_list_add_button(list, LV_SYMBOL_WIFI, entries[i].ssid);
    lv_obj_set_style_bg_color(row, lv_color_hex(kPanel), 0);
    lv_obj_set_style_text_color(row, lv_color_hex(kText), 0);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_16, 0);
    lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row, networkPickedCb, LV_EVENT_CLICKED, this);

    // The compiled Montserrat fonts carry no padlock glyph (0xF023 is not in
    // the built-in FontAwesome subset), so security is spelled out.
    char meta[24];
    snprintf(meta, sizeof(meta), "%d dBm  %s", entries[i].rssi,
             entries[i].secure ? "WPA2" : "OPEN");
    lv_obj_t *metaLabel = makeLabel(row, meta, &lv_font_montserrat_14, kMuted);
    // The list row is a flex container; float this out of the layout so it
    // right-aligns instead of being packed after the SSID.
    lv_obj_add_flag(metaLabel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(metaLabel, LV_ALIGN_RIGHT_MID, -8, 0);
  }

  lv_obj_align(
      makeButton(content_, LV_SYMBOL_REFRESH " Rescan", Action::Rescan, actionCb, this, 180),
      LV_ALIGN_BOTTOM_LEFT, 24, -24);
  lv_obj_align(
      makeButton(content_, "Other network...", Action::ManualSsid, actionCb, this, 220),
      LV_ALIGN_BOTTOM_LEFT, 216, -24);

  if (canCancel_) {
    lv_obj_align(
        makeButton(content_, LV_SYMBOL_CLOSE " Cancel", Action::Cancel, actionCb, this, 180),
        LV_ALIGN_BOTTOM_RIGHT, -24, -24);
  }
}

void SettingsScreen::pollNetwork() {
  if (commandPosted_ && netctl::commandDone(&lastResult_)) {
    requestStep(SettingsStep::Network);  // rebuild with the fresh scan
  }
}

void SettingsScreen::networkPickedCb(lv_event_t *e) {
  SettingsScreen *self = static_cast<SettingsScreen *>(lv_event_get_user_data(e));
  const size_t index = static_cast<size_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));

  size_t count = 0;
  const ScanEntry *entries = netctl::scanResults(&count);
  if (index >= count) {
    return;
  }

  strncpy(self->pickedSsid_, entries[index].ssid, sizeof(self->pickedSsid_) - 1);
  self->pickedSsid_[sizeof(self->pickedSsid_) - 1] = '\0';
  self->pickedSecure_ = entries[index].secure;

  if (self->pickedSecure_) {
    self->requestStep(SettingsStep::Password);
  } else {
    self->draft_.password[0] = '\0';
    self->beginConnect();
  }
}

// ===== ManualSsid / Password =====

void SettingsScreen::buildTextEntry(bool password) {
  const char *title = password ? "Password" : "Network name";
  lv_obj_set_pos(makeLabel(content_, title, &lv_font_montserrat_20, kText), 24, 16);

  if (password) {
    lv_obj_set_pos(makeLabel(content_, pickedSsid_, &lv_font_montserrat_16, kMuted), 24, 48);
  }

  entryTa_ = lv_textarea_create(content_);
  lv_obj_set_pos(entryTa_, 24, 84);
  lv_obj_set_size(entryTa_, password ? 660 : 752, 54);
  lv_textarea_set_one_line(entryTa_, true);
  lv_textarea_set_max_length(entryTa_, password ? 63 : 32);
  lv_obj_set_style_bg_color(entryTa_, lv_color_hex(kRaised), 0);
  lv_obj_set_style_border_width(entryTa_, 1, 0);
  lv_obj_set_style_border_color(entryTa_, lv_color_hex(kMuted), 0);
  lv_obj_set_style_text_color(entryTa_, lv_color_hex(kText), 0);
  lv_obj_set_style_text_font(entryTa_, &lv_font_montserrat_16, 0);

  if (password) {
    lv_textarea_set_password_mode(entryTa_, true);
    lv_textarea_set_password_show_time(entryTa_, 0);
    lv_textarea_set_placeholder_text(entryTa_, "Network password");
    lv_obj_set_pos(
        makeButton(content_, LV_SYMBOL_EYE_OPEN, Action::ToggleReveal, actionCb, this, 64),
        700, 85);
  } else {
    lv_textarea_set_placeholder_text(entryTa_, "Hidden network name");
  }

  lv_obj_set_pos(makeButton(content_, "Back", Action::Back, actionCb, this, 160), 24, 152);
  lv_obj_set_pos(makeButton(content_, password ? "Connect" : "Next", Action::Connect, actionCb,
                            this, 180, /*primary=*/true),
                 200, 152);

  keyboard_ = lv_keyboard_create(content_);
  lv_obj_set_size(keyboard_, kScreenW, kKeyboardH);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_popovers(keyboard_, false);  // popovers invalidate outside the kb rect
  lv_keyboard_set_textarea(keyboard_, entryTa_);
  lv_obj_add_event_cb(keyboard_, actionCb, LV_EVENT_READY, this);
  lv_obj_set_user_data(keyboard_,
                       reinterpret_cast<void *>(static_cast<intptr_t>(Action::Connect)));
}

// ===== Connecting / failed =====

void SettingsScreen::beginConnect() {
  netctl::setConnectArgs(pickedSsid_, draft_.password);
  requestStep(SettingsStep::Connecting);
}

void SettingsScreen::buildBusy(const char *message) {
  lv_obj_t *spinner = lv_spinner_create(content_);
  lv_obj_set_size(spinner, 64, 64);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(kRaised), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(kAccent), LV_PART_INDICATOR);

  statusLabel_ = makeLabel(content_, message, &lv_font_montserrat_20, kText);
  lv_obj_align(statusLabel_, LV_ALIGN_CENTER, 0, 40);

  if (step_ == SettingsStep::Connecting) {
    lv_obj_t *sub = makeLabel(content_, pickedSsid_, &lv_font_montserrat_16, kMuted);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 76);
  }
}

void SettingsScreen::pollConnecting() {
  if (!commandPosted_) {
    if (netctl::post(SetupCmd::Connect)) {
      commandPosted_ = true;
    }
    return;
  }
  if (!netctl::commandDone(&lastResult_)) {
    return;
  }

  if (lastResult_ == WL_CONNECTED) {
    strncpy(draft_.ssid, pickedSsid_, sizeof(draft_.ssid) - 1);
    draft_.ssid[sizeof(draft_.ssid) - 1] = '\0';
    requestStep(SettingsStep::Location);
  } else {
    requestStep(SettingsStep::ConnectFailed);
  }
}

void SettingsScreen::buildConnectFailed() {
  lv_obj_t *icon = makeLabel(content_, LV_SYMBOL_WARNING, &lv_font_montserrat_48, kDanger);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -100);

  const char *detail = nullptr;
  switch (lastResult_) {
    case WL_NO_SSID_AVAIL:
      detail = "Network not found";
      break;
    case WL_CONNECT_FAILED:
    case WL_CONNECTION_LOST:
      detail = "Wrong password?";
      break;
    default:
      detail = "Could not connect";
      break;
  }

  lv_obj_t *title = makeLabel(content_, detail, &lv_font_montserrat_20, kText);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *sub = makeLabel(content_, pickedSsid_, &lv_font_montserrat_16, kMuted);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 6);

  lv_obj_align(makeButton(content_, "Retry", Action::Retry, actionCb, this, 180,
                          /*primary=*/true),
               LV_ALIGN_CENTER, -100, 80);
  lv_obj_align(makeButton(content_, "Back to list", Action::Back, actionCb, this, 200),
               LV_ALIGN_CENTER, 110, 80);
}

// ===== Location =====

void SettingsScreen::buildLocation() {
  lv_obj_set_pos(makeLabel(content_, "Location", &lv_font_montserrat_20, kText), 24, 14);
  lv_obj_set_pos(
      makeLabel(content_, "Detected from your connection:", &lv_font_montserrat_14, kMuted), 24,
      46);

  detectedLabel_ = makeLabel(content_, "Looking up...", &lv_font_montserrat_16, kText);
  lv_obj_set_pos(detectedLabel_, 40, 68);
  refreshDetectedLabel();
  // Skip the lookup only if a previous visit already got an answer; otherwise
  // coming back here (via Change WiFi, say) must retry rather than sit on
  // "Looking up..." forever.
  geoPosted_ = netctl::geoResult().ok;

  char buf[32];
  snprintf(buf, sizeof(buf), "%.6f", draft_.latitude);
  latTa_ = makeField(content_, "Latitude", 108, buf, "0123456789.+-", focusCb, this);
  snprintf(buf, sizeof(buf), "%.6f", draft_.longitude);
  lonTa_ = makeField(content_, "Longitude", 166, buf, "0123456789.+-", focusCb, this);
  snprintf(buf, sizeof(buf), "%d", draft_.radiusNm);
  radiusTa_ = makeField(content_, "Radius (nm)", 224, buf, "0123456789", focusCb, this);

  statusLabel_ = makeLabel(content_, "", &lv_font_montserrat_14, kDanger);
  lv_obj_set_pos(statusLabel_, 40, 284);

  lv_obj_set_pos(
      makeButton(content_, "Use detected", Action::UseDetected, actionCb, this, 200), 24, 320);
  lv_obj_set_pos(makeButton(content_, LV_SYMBOL_WIFI " Change WiFi", Action::ChangeWifi,
                            actionCb, this, 220),
                 240, 320);
  if (canCancel_) {
    lv_obj_set_pos(makeButton(content_, "Cancel", Action::Cancel, actionCb, this, 140), 476,
                   320);
  }
  lv_obj_set_pos(makeButton(content_, LV_SYMBOL_OK " Save", Action::Save, actionCb, this, 160,
                            /*primary=*/true),
                 628, 320);

  // Hidden until a field is focused: it would otherwise cover the buttons.
  keyboard_ = lv_keyboard_create(content_);
  lv_obj_set_size(keyboard_, kScreenW, kKeyboardH);
  lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_popovers(keyboard_, false);
  lv_keyboard_set_mode(keyboard_, LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_NUMBER, kNumberMap, kNumberCtrl);
  lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard_, actionCb, LV_EVENT_READY, this);
  lv_obj_set_user_data(keyboard_,
                       reinterpret_cast<void *>(static_cast<intptr_t>(Action::None)));
}

void SettingsScreen::refreshDetectedLabel() {
  if (detectedLabel_ == nullptr) {
    return;
  }
  const GeoResult &geo = netctl::geoResult();
  if (!geo.ok) {
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "%s%s%s\n%.4f, %.4f  (city level, may be several km out)",
           geo.city, geo.country[0] != '\0' ? ", " : "", geo.country, geo.lat, geo.lon);
  lv_label_set_text(detectedLabel_, buf);
}

void SettingsScreen::pollLocation() {
  if (!geoPosted_) {
    if (netctl::post(SetupCmd::Geolocate)) {
      geoPosted_ = true;
    }
    return;
  }
  if (netctl::commandDone(&lastResult_)) {
    if (lastResult_ == 1) {
      refreshDetectedLabel();
    } else if (detectedLabel_ != nullptr) {
      lv_label_set_text(detectedLabel_, "Lookup unavailable. Enter your location below.");
    }
  }
}

void SettingsScreen::focusCb(lv_event_t *e) {
  SettingsScreen *self = static_cast<SettingsScreen *>(lv_event_get_user_data(e));
  if (self->keyboard_ == nullptr) {
    return;
  }
  lv_keyboard_set_textarea(self->keyboard_, lv_event_get_target_obj(e));
  lv_obj_clear_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}

bool SettingsScreen::commitLocation() {
  char *end = nullptr;
  const char *latText = lv_textarea_get_text(latTa_);
  const double lat = strtod(latText, &end);
  if (end == latText || lat < -90.0 || lat > 90.0) {
    lv_label_set_text(statusLabel_, "Latitude must be between -90 and 90");
    lv_obj_set_style_border_color(latTa_, lv_color_hex(kDanger), 0);
    return false;
  }
  lv_obj_set_style_border_color(latTa_, lv_color_hex(kMuted), 0);

  const char *lonText = lv_textarea_get_text(lonTa_);
  const double lon = strtod(lonText, &end);
  if (end == lonText || lon < -180.0 || lon > 180.0) {
    lv_label_set_text(statusLabel_, "Longitude must be between -180 and 180");
    lv_obj_set_style_border_color(lonTa_, lv_color_hex(kDanger), 0);
    return false;
  }
  lv_obj_set_style_border_color(lonTa_, lv_color_hex(kMuted), 0);

  const char *radiusText = lv_textarea_get_text(radiusTa_);
  const long radius = strtol(radiusText, &end, 10);
  if (end == radiusText || radius < 1 || radius > 250) {
    lv_label_set_text(statusLabel_, "Radius must be between 1 and 250 nm");
    lv_obj_set_style_border_color(radiusTa_, lv_color_hex(kDanger), 0);
    return false;
  }
  lv_obj_set_style_border_color(radiusTa_, lv_color_hex(kMuted), 0);

  draft_.latitude = lat;
  draft_.longitude = lon;
  draft_.radiusNm = static_cast<int>(radius);
  draft_.locationConfirmed = true;
  return true;
}

// ===== actions =====

void SettingsScreen::actionCb(lv_event_t *e) {
  SettingsScreen *self = static_cast<SettingsScreen *>(lv_event_get_user_data(e));
  lv_obj_t *target = lv_event_get_target_obj(e);
  const Action action =
      static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));

  switch (action) {
    case Action::Rescan:
      if (netctl::post(SetupCmd::Scan)) {
        self->commandPosted_ = true;
        lv_label_set_text(self->statusLabel_, "Scanning...");
      }
      break;

    case Action::ManualSsid:
      self->requestStep(SettingsStep::ManualSsid);
      break;

    case Action::Cancel:
      self->pendingClose_ = true;
      break;

    case Action::Back:
      self->afterPrepare_ = SettingsStep::Network;
      self->requestStep(SettingsStep::Preparing);
      break;

    case Action::Retry:
      // Same button on two steps: restart the park wait, or redial.
      if (self->step_ == SettingsStep::Preparing) {
        self->requestStep(SettingsStep::Preparing);
      } else {
        self->beginConnect();
      }
      break;

    case Action::ToggleReveal: {
      lv_obj_t *label = lv_obj_get_child(target, 0);
      const bool hidden = lv_textarea_get_password_mode(self->entryTa_);
      lv_textarea_set_password_mode(self->entryTa_, !hidden);
      lv_label_set_text(label, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
      break;
    }

    case Action::Connect:
      if (self->step_ == SettingsStep::ManualSsid) {
        const char *text = lv_textarea_get_text(self->entryTa_);
        if (text[0] == '\0') {
          break;
        }
        strncpy(self->pickedSsid_, text, sizeof(self->pickedSsid_) - 1);
        self->pickedSsid_[sizeof(self->pickedSsid_) - 1] = '\0';
        self->pickedSecure_ = true;
        self->requestStep(SettingsStep::Password);
      } else if (self->step_ == SettingsStep::Password) {
        strncpy(self->draft_.password, lv_textarea_get_text(self->entryTa_),
                sizeof(self->draft_.password) - 1);
        self->draft_.password[sizeof(self->draft_.password) - 1] = '\0';
        self->beginConnect();
      }
      break;

    case Action::UseDetected: {
      const GeoResult &geo = netctl::geoResult();
      if (!geo.ok) {
        break;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "%.6f", geo.lat);
      lv_textarea_set_text(self->latTa_, buf);
      snprintf(buf, sizeof(buf), "%.6f", geo.lon);
      lv_textarea_set_text(self->lonTa_, buf);
      strncpy(self->draft_.placeName, geo.city, sizeof(self->draft_.placeName) - 1);
      self->draft_.placeName[sizeof(self->draft_.placeName) - 1] = '\0';
      break;
    }

    case Action::ChangeWifi:
      self->afterPrepare_ = SettingsStep::Network;
      self->requestStep(SettingsStep::Preparing);
      break;

    case Action::Save:
      if (!self->commitLocation()) {
        break;
      }
      self->changed_ = settings::save(self->draft_);
      self->pendingClose_ = true;
      break;

    case Action::None:
      // Keyboard "OK" on the numeric pad: just dismiss it.
      if (self->keyboard_ != nullptr) {
        lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
      }
      break;
  }
}
