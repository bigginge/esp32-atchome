#pragma once

#include "aircraft.hpp"

class Tracker {
 public:
  void clear();
  void mergeSnapshot(const Aircraft *incoming, size_t count);

  size_t count() const { return count_; }
  const Aircraft *aircraft() const { return aircraft_; }
  Aircraft *aircraft() { return aircraft_; }

  bool isAutoSelected() const { return autoSelected_; }

  Aircraft *selected();
  const Aircraft *selected() const;

  /** Select nearest by distance. Clears selection if none. */
  void selectNearest();

  /** Select by ICAO hex. Returns false if not found. */
  bool selectByHex(const char *hex);

  /** Hit-test in nm east/north from home. Returns selected aircraft or nullptr. */
  Aircraft *selectNearestTo(float eastNm, float northNm, float hitRadiusNm);

  bool hasSelection() const { return selectedHex_[0] != '\0'; }

 private:
  Aircraft *findByHex(const char *hex);
  const Aircraft *findByHex(const char *hex) const;
  void pushTrail(Aircraft &ac, float eastNm, float northNm);
  void retainEnrichment(Aircraft &dst, const Aircraft &prev);

  Aircraft aircraft_[kMaxAircraft];
  size_t count_ = 0;
  char selectedHex_[8] = {0};
  bool autoSelected_ = true;
};
