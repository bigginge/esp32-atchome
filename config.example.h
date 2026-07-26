#ifndef CONFIG_H
#define CONFIG_H

// OPTIONAL first-boot seed values.
//
// This file is no longer required: without a config.h the sketch still
// compiles and boots straight into the on-device setup wizard (tap the gear on
// the info panel to reopen it later).
//
// If you do provide a config.h, its values are written to NVS once, on the very
// first boot. After that NVS is authoritative and *editing config.h has no
// effect* -- change things from the wizard instead. To start over, erase the
// flash (esptool erase_flash) and reflash.
//
// Copy this file to config.h and fill in your values. config.h is gitignored.

// ===== WiFi =====
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"

// ===== Home location (decimal degrees) =====
// Find yours at: https://www.latlong.net/
// The wizard can also suggest these from your IP address.
#define MY_LATITUDE   51.5074
#define MY_LONGITUDE  -0.1278
#define MY_LOCATION_NAME "London"  // Friendly label for the detected place

// ===== Search / refresh =====
#define SEARCH_RADIUS_NM       25     // Nautical miles (adsb.fi max 250)
#define REFRESH_INTERVAL_MS    5000   // ADS-B poll interval (fetch is off the UI thread)

#endif
