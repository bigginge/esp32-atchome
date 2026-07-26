#include "net_control.hpp"

#include <string.h>

namespace netctl {
namespace {

volatile bool gSetupRequested = false;
volatile bool gParked = false;

// The pending/done handshake is the whole synchronisation story: the UI only
// writes the payloads while gCmd == None, and the worker only writes results
// before setting gDone, so neither ever reads a half-written buffer.
volatile SetupCmd gCmd = SetupCmd::None;
volatile bool gDone = false;
volatile int gResult = 0;

char gConnectSsid[33] = {0};
char gConnectPass[64] = {0};

ScanEntry gScan[kMaxScanEntries];
volatile size_t gScanCount = 0;
GeoResult gGeo = {};

volatile uint32_t gEpoch = 0;

}  // namespace

void enterSetupMode() { gSetupRequested = true; }

void exitSetupMode() {
  gSetupRequested = false;
  // Drop any unconsumed completion so the next session can't observe a stale
  // result from the last one.
  gCmd = SetupCmd::None;
  gDone = false;
}

bool isParked() { return gParked; }

bool post(SetupCmd cmd) {
  if (!gParked || gCmd != SetupCmd::None) {
    return false;
  }
  gDone = false;
  gCmd = cmd;
  return true;
}

void setConnectArgs(const char *ssid, const char *pass) {
  strncpy(gConnectSsid, ssid != nullptr ? ssid : "", sizeof(gConnectSsid) - 1);
  gConnectSsid[sizeof(gConnectSsid) - 1] = '\0';
  strncpy(gConnectPass, pass != nullptr ? pass : "", sizeof(gConnectPass) - 1);
  gConnectPass[sizeof(gConnectPass) - 1] = '\0';
}

bool commandDone(int *result) {
  if (!gDone) {
    return false;
  }
  if (result != nullptr) {
    *result = gResult;
  }
  gDone = false;
  gCmd = SetupCmd::None;
  return true;
}

const ScanEntry *scanResults(size_t *count) {
  if (count != nullptr) {
    *count = gScanCount;
  }
  return gScan;
}

const GeoResult &geoResult() { return gGeo; }

void bumpEpoch() { ++gEpoch; }

bool setupModeRequested() { return gSetupRequested; }

void acknowledgeParked(bool parked) { gParked = parked; }

bool takeCommand(SetupCmd *out) {
  if (gCmd == SetupCmd::None || gDone) {
    return false;
  }
  *out = gCmd;
  return true;
}

void completeCommand(int result) {
  gResult = result;
  gDone = true;
}

ScanEntry *scanBuffer() { return gScan; }

GeoResult *geoBuffer() { return &gGeo; }

void connectArgs(const char **ssid, const char **pass) {
  *ssid = gConnectSsid;
  *pass = gConnectPass;
}

void setScanCount(size_t n) { gScanCount = n; }

uint32_t epoch() { return gEpoch; }

}  // namespace netctl
