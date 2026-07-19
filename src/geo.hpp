#pragma once

#include <math.h>

/** East/north offsets in nautical miles from home (approx. flat-earth). */
inline void latLonToNm(float homeLat, float homeLon, float lat, float lon,
                       float *eastNm, float *northNm) {
  const float cosLat = cosf(homeLat * static_cast<float>(DEG_TO_RAD));
  *eastNm = (lon - homeLon) * 60.0f * cosLat;
  *northNm = (lat - homeLat) * 60.0f;
}

inline float haversineNm(float lat1, float lon1, float lat2, float lon2) {
  constexpr float kEarthNm = 3440.065f;
  const float dLat = (lat2 - lat1) * static_cast<float>(DEG_TO_RAD);
  const float dLon = (lon2 - lon1) * static_cast<float>(DEG_TO_RAD);
  const float a =
      sinf(dLat * 0.5f) * sinf(dLat * 0.5f) +
      cosf(lat1 * static_cast<float>(DEG_TO_RAD)) *
          cosf(lat2 * static_cast<float>(DEG_TO_RAD)) *
          sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
  return kEarthNm * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}
