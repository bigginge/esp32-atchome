#include "settings.hpp"
#include "log.hpp"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// config.h carries the first-boot seed values and is deliberately optional: a
// fresh clone compiles without it and boots straight into the setup wizard.
// This is the only translation unit that may include it.
#if defined(__has_include)
#if __has_include("config.h")
#include "config.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MY_LATITUDE
#define MY_LATITUDE 51.5074
#endif
#ifndef MY_LONGITUDE
#define MY_LONGITUDE -0.1278
#endif
#ifndef MY_LOCATION_NAME
#define MY_LOCATION_NAME ""
#endif
#ifndef SEARCH_RADIUS_NM
#define SEARCH_RADIUS_NM 25
#endif
#ifndef REFRESH_INTERVAL_MS
#define REFRESH_INTERVAL_MS 5000
#endif

namespace settings {

const unsigned long kRefreshIntervalMs = REFRESH_INTERVAL_MS;

namespace {

constexpr char kNamespace[] = "atchome";
constexpr uint8_t kSchemaVersion = 1;

// NVS keys are capped at 15 characters.
constexpr char kKeyVer[] = "ver";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyRadius[] = "radius";
constexpr char kKeyPlace[] = "place";
constexpr char kKeyLocOk[] = "locok";

AppSettings gCached;
volatile uint32_t gEpoch = 0;

// Guards gCached only. The critical section is a ~140 byte struct copy, never
// NVS I/O, so holding off the other core for it is cheap.
portMUX_TYPE gLock = portMUX_INITIALIZER_UNLOCKED;

void copyField(char *dest, size_t destSize, const char *src) {
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

/** Preferences::getString() returns 0 and leaves the buffer *untouched* when
 *  the key is missing, so every read has to start from a known-empty buffer. */
void readString(Preferences &prefs, const char *key, char *dest, size_t destSize) {
  memset(dest, 0, destSize);
  prefs.getString(key, dest, destSize);
  dest[destSize - 1] = '\0';
}

void writeAll(Preferences &prefs, const AppSettings &v) {
  prefs.putString(kKeySsid, v.ssid);
  prefs.putString(kKeyPass, v.password);
  prefs.putDouble(kKeyLat, v.latitude);
  prefs.putDouble(kKeyLon, v.longitude);
  prefs.putInt(kKeyRadius, v.radiusNm);
  prefs.putString(kKeyPlace, v.placeName);
  prefs.putBool(kKeyLocOk, v.locationConfirmed);
}

void publish(const AppSettings &v) {
  portENTER_CRITICAL(&gLock);
  gCached = v;
  portEXIT_CRITICAL(&gLock);
}

}  // namespace

double clampLatitude(double value) {
  if (value < -90.0) return -90.0;
  if (value > 90.0) return 90.0;
  return value;
}

double clampLongitude(double value) {
  if (value < -180.0) return -180.0;
  if (value > 180.0) return 180.0;
  return value;
}

int clampRadiusNm(int value) {
  if (value < 1) return 1;
  if (value > 250) return 250;  // adsb.fi's ceiling
  return value;
}

AppSettings defaults() {
  AppSettings v;
  copyField(v.ssid, sizeof(v.ssid), WIFI_SSID);
  copyField(v.password, sizeof(v.password), WIFI_PASSWORD);
  v.latitude = clampLatitude(MY_LATITUDE);
  v.longitude = clampLongitude(MY_LONGITUDE);
  v.radiusNm = clampRadiusNm(SEARCH_RADIUS_NM);
  copyField(v.placeName, sizeof(v.placeName), MY_LOCATION_NAME);
  // Only treat the seeded location as confirmed when config.h also supplied a
  // network: that combination is an existing working install being upgraded,
  // and it must not be dragged through the wizard. A build with no config.h
  // seeds a placeholder location nobody has agreed to, so it must prompt.
  v.locationConfirmed = v.ssid[0] != '\0';
  return v;
}

void load() {
  AppSettings v = defaults();

  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    Log.println("[settings] NVS open failed; running from compile-time defaults");
    publish(v);
    return;
  }

  if (prefs.getUChar(kKeyVer, 0) != kSchemaVersion) {
    writeAll(prefs, v);
    prefs.putUChar(kKeyVer, kSchemaVersion);
    Log.println("[settings] first boot: seeded NVS from compile-time defaults");
  } else {
    readString(prefs, kKeySsid, v.ssid, sizeof(v.ssid));
    readString(prefs, kKeyPass, v.password, sizeof(v.password));
    v.latitude = clampLatitude(prefs.getDouble(kKeyLat, v.latitude));
    v.longitude = clampLongitude(prefs.getDouble(kKeyLon, v.longitude));
    v.radiusNm = clampRadiusNm(prefs.getInt(kKeyRadius, v.radiusNm));
    readString(prefs, kKeyPlace, v.placeName, sizeof(v.placeName));
    v.locationConfirmed = prefs.getBool(kKeyLocOk, false);
  }
  prefs.end();

  publish(v);
}

bool save(const AppSettings &in) {
  AppSettings v = in;
  v.ssid[sizeof(v.ssid) - 1] = '\0';
  v.password[sizeof(v.password) - 1] = '\0';
  v.placeName[sizeof(v.placeName) - 1] = '\0';
  v.latitude = clampLatitude(v.latitude);
  v.longitude = clampLongitude(v.longitude);
  v.radiusNm = clampRadiusNm(v.radiusNm);

  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    Log.println("[settings] NVS open failed; not saved");
    return false;
  }

  // Only the UI task calls save(), and it is the only writer of gCached, so
  // comparing against the cache without the lock is safe.
  bool changed = false;
  if (strcmp(v.ssid, gCached.ssid) != 0) {
    prefs.putString(kKeySsid, v.ssid);
    changed = true;
  }
  if (strcmp(v.password, gCached.password) != 0) {
    prefs.putString(kKeyPass, v.password);
    changed = true;
  }
  if (v.latitude != gCached.latitude) {
    prefs.putDouble(kKeyLat, v.latitude);
    changed = true;
  }
  if (v.longitude != gCached.longitude) {
    prefs.putDouble(kKeyLon, v.longitude);
    changed = true;
  }
  if (v.radiusNm != gCached.radiusNm) {
    prefs.putInt(kKeyRadius, v.radiusNm);
    changed = true;
  }
  if (strcmp(v.placeName, gCached.placeName) != 0) {
    prefs.putString(kKeyPlace, v.placeName);
    changed = true;
  }
  if (v.locationConfirmed != gCached.locationConfirmed) {
    prefs.putBool(kKeyLocOk, v.locationConfirmed);
    changed = true;
  }
  prefs.putUChar(kKeyVer, kSchemaVersion);
  prefs.end();

  publish(v);
  if (changed) {
    ++gEpoch;
    Log.printf("[settings] saved: %.6f, %.6f  radius %d nm  ssid \"%s\"\n", v.latitude,
                  v.longitude, v.radiusNm, v.ssid);
  }
  return changed;
}

const AppSettings &get() { return gCached; }

bool hasWifi() { return gCached.ssid[0] != '\0'; }

void snapshotForNetwork(double *lat, double *lon, int *radius, char *ssid, size_t ssidSize,
                        char *pass, size_t passSize) {
  portENTER_CRITICAL(&gLock);
  if (lat != nullptr) *lat = gCached.latitude;
  if (lon != nullptr) *lon = gCached.longitude;
  if (radius != nullptr) *radius = gCached.radiusNm;
  if (ssid != nullptr && ssidSize > 0) {
    strncpy(ssid, gCached.ssid, ssidSize - 1);
    ssid[ssidSize - 1] = '\0';
  }
  if (pass != nullptr && passSize > 0) {
    strncpy(pass, gCached.password, passSize - 1);
    pass[passSize - 1] = '\0';
  }
  portEXIT_CRITICAL(&gLock);
}

uint32_t epoch() { return gEpoch; }

}  // namespace settings
