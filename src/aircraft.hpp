#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t kMaxAircraft = 32;
static constexpr size_t kTrailLen = 12;
static constexpr int kMaxAltitudeFt = 40000;

struct TrailPoint {
  float eastNm;
  float northNm;
};

struct Aircraft {
  char hex[8];
  char callsign[10];
  char registration[10];
  char typeCode[6];

  float lat;
  float lon;
  float eastNm;
  float northNm;
  float distanceNm;
  float trackDeg;
  int altitudeFt;
  float groundSpeedKts;

  char manufacturer[24];
  char typeDescription[28];
  char registeredOwner[32];
  char origin[64];
  char destination[64];
  // Route enrichment is deliberately staged so a selected aircraft does not
  // hold the UI loop hostage while several HTTPS requests complete.
  char originIcao[10];
  char destinationIcao[10];

  bool detailsLoaded;
  bool routeLoaded;
  // 0: fetch route, 1: fetch origin, 2: fetch destination, 3: complete.
  uint8_t routeLookupStep;

  uint8_t hue;
  TrailPoint trail[kTrailLen];
  uint8_t trailCount;
  uint8_t trailHead;

  bool seen;
};

/** Map altitude to 0..1 (clamped). */
inline float altitudeNorm(int altitudeFt) {
  if (altitudeFt <= 0) return 0.0f;
  if (altitudeFt >= kMaxAltitudeFt) return 1.0f;
  return static_cast<float>(altitudeFt) / static_cast<float>(kMaxAltitudeFt);
}

/** Stable hue 0..255 from ICAO hex. */
inline uint8_t hueFromHex(const char *hex) {
  uint32_t hash = 2166136261u;
  for (const char *p = hex; p && *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  return static_cast<uint8_t>(hash & 0xFFu);
}
