#pragma once

#include <stddef.h>
#include <stdint.h>

/** Runtime configuration, persisted in NVS.
 *
 *  config.h — if it exists — only *seeds* these values on the very first boot.
 *  After that NVS is authoritative and editing config.h has no effect; the
 *  on-device settings menu is the way to change them. */
struct AppSettings {
  char ssid[33] = {0};
  char password[64] = {0};
  double latitude = 0.0;
  double longitude = 0.0;
  int radiusNm = 25;
  char placeName[32] = {0};
  bool locationConfirmed = false;
};

namespace settings {

/** ADS-B poll interval. Not exposed in the settings menu, so it stays
 *  compile-time (seeded from config.h like the rest). */
extern const unsigned long kRefreshIntervalMs;

/** Regulatory domain, as an ISO country code. Decides which channels a scan
 *  visits, so it has to be set before the wizard's first scan — which rules out
 *  NVS, since a fresh device reaches the wizard with nothing stored. Unlike the
 *  rest of config.h this is a plain compile-time constant, not a seed. */
extern const char *const kWifiCountry;

/** Reads NVS into the cached copy, seeding from config.h on first boot.
 *  Call once from setup(), before anything reads the radius. */
void load();

/** Normalises `in`, writes the keys whose value actually changed, and
 *  republishes the cache. Returns true if anything changed (in which case
 *  epoch() has been bumped). UI task only. */
bool save(const AppSettings &in);

/** The cached copy. Safe to read from the UI task, which is the only writer;
 *  the network task must use snapshotForNetwork() instead. */
const AppSettings &get();

bool hasWifi();

/** Copies the fields the network task needs out of the cache under a lock, so
 *  a save() on the UI core can't tear the doubles. Any out-param may be null. */
void snapshotForNetwork(double *lat, double *lon, int *radius, char *ssid, size_t ssidSize,
                        char *pass, size_t passSize);

/** Bumped on every save that changed something. The network task watches this
 *  to learn the home location moved and it must refetch. */
uint32_t epoch();

/** Restores the compile-time defaults (the wizard's escape hatch, and what a
 *  device with no config.h starts from). Does not write NVS; pass the result
 *  to save(). */
AppSettings defaults();

// Validation shared by the settings menu and the persistence layer, so a typed
// value and a stored value can never disagree about what is in range.
double clampLatitude(double value);
double clampLongitude(double value);
int clampRadiusNm(int value);

}  // namespace settings
