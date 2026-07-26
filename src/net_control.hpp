#pragma once

#include <stddef.h>
#include <stdint.h>

/** Command channel between the UI task (core 1) and the network task (core 0).
 *
 *  The project's hard invariant is that core 1 never calls WiFi or HTTP and
 *  core 0 never calls LVGL. The settings menu needs to scan, connect and
 *  geolocate, so instead of reaching across it asks the network task to do the
 *  work: the task parks at the top of its loop — where no TLS transaction can
 *  possibly be in flight and it holds no locks — and serves commands until the
 *  UI releases it. */

enum class SetupCmd : uint8_t { None, Scan, Connect, Geolocate };

struct ScanEntry {
  char ssid[33];
  int8_t rssi;
  bool secure;
};

constexpr size_t kMaxScanEntries = 24;

struct GeoResult {
  bool ok;
  float lat;
  float lon;
  char city[32];
  char country[4];
};

namespace netctl {

// ===== UI side =====

/** Ask the network task to park. Returns immediately; poll isParked(). */
void enterSetupMode();
void exitSetupMode();

/** True once the worker has acknowledged parking, i.e. it is idle and safe to
 *  touch the WiFi stack from a command. */
bool isParked();

/** Queue a command. False if one is still pending or the worker isn't parked. */
bool post(SetupCmd cmd);

/** Set the arguments for the next Connect. Call before post(). */
void setConnectArgs(const char *ssid, const char *pass);

/** Consumes a completion. `result` is the command's return value: the entry
 *  count for Scan, a wl_status_t for Connect, 1/0 for Geolocate. */
bool commandDone(int *result);

/** Valid until the next Scan completes. */
const ScanEntry *scanResults(size_t *count);
const GeoResult &geoResult();

/** Tell the worker the home location changed and it must refetch. */
void bumpEpoch();

// ===== Worker side =====

bool setupModeRequested();
void acknowledgeParked(bool parked);

/** Takes the queued command, if any, leaving it pending until
 *  completeCommand(). */
bool takeCommand(SetupCmd *out);
void completeCommand(int result);

/** Scratch space the worker fills before completing a Scan / Geolocate. */
ScanEntry *scanBuffer();
GeoResult *geoBuffer();
void setScanCount(size_t n);

/** The arguments posted for a Connect. */
void connectArgs(const char **ssid, const char **pass);

uint32_t epoch();

}  // namespace netctl
