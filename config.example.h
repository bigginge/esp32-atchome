#ifndef CONFIG_H
#define CONFIG_H

// Copy this file to config.h and fill in your values.
// config.h is gitignored.

// ===== WiFi =====
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"

// ===== Home location (decimal degrees) =====
// Find yours at: https://www.latlong.net/
#define MY_LATITUDE   51.5074
#define MY_LONGITUDE  -0.1278

// ===== Search / refresh =====
#define SEARCH_RADIUS_NM       25     // Nautical miles (adsb.fi max 250)
#define REFRESH_INTERVAL_MS    15000  // ADS-B poll interval

#endif
