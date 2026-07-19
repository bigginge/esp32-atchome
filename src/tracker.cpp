#include "tracker.hpp"

#include <math.h>
#include <string.h>

void Tracker::clear() {
  count_ = 0;
  selectedHex_[0] = '\0';
  autoSelected_ = true;
  memset(aircraft_, 0, sizeof(aircraft_));
}

Aircraft *Tracker::findByHex(const char *hex) {
  if (hex == nullptr || hex[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (strcasecmp(aircraft_[i].hex, hex) == 0) {
      return &aircraft_[i];
    }
  }
  return nullptr;
}

const Aircraft *Tracker::findByHex(const char *hex) const {
  if (hex == nullptr || hex[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (strcasecmp(aircraft_[i].hex, hex) == 0) {
      return &aircraft_[i];
    }
  }
  return nullptr;
}

void Tracker::pushTrail(Aircraft &ac, float eastNm, float northNm) {
  if (ac.trailCount > 0) {
    const uint8_t lastIdx =
        static_cast<uint8_t>((ac.trailHead + kTrailLen - 1) % kTrailLen);
    const TrailPoint &last = ac.trail[lastIdx];
    const float de = eastNm - last.eastNm;
    const float dn = northNm - last.northNm;
    if ((de * de + dn * dn) < (0.05f * 0.05f)) {
      return;
    }
  }

  ac.trail[ac.trailHead] = {eastNm, northNm};
  ac.trailHead = static_cast<uint8_t>((ac.trailHead + 1) % kTrailLen);
  if (ac.trailCount < kTrailLen) {
    ++ac.trailCount;
  }
}

void Tracker::retainEnrichment(Aircraft &dst, const Aircraft &prev) {
  if (prev.detailsLoaded) {
    strncpy(dst.manufacturer, prev.manufacturer, sizeof(dst.manufacturer) - 1);
    strncpy(dst.typeDescription, prev.typeDescription, sizeof(dst.typeDescription) - 1);
    strncpy(dst.registeredOwner, prev.registeredOwner, sizeof(dst.registeredOwner) - 1);
    if (dst.registration[0] == '\0') {
      strncpy(dst.registration, prev.registration, sizeof(dst.registration) - 1);
    }
    dst.detailsLoaded = true;
  }
  if (prev.routeLookupStep != 0 || prev.routeLoaded) {
    strncpy(dst.origin, prev.origin, sizeof(dst.origin) - 1);
    strncpy(dst.destination, prev.destination, sizeof(dst.destination) - 1);
    strncpy(dst.originIcao, prev.originIcao, sizeof(dst.originIcao) - 1);
    strncpy(dst.destinationIcao, prev.destinationIcao, sizeof(dst.destinationIcao) - 1);
    dst.routeLoaded = prev.routeLoaded;
    dst.routeLookupStep = prev.routeLookupStep;
  }

  memcpy(dst.trail, prev.trail, sizeof(dst.trail));
  dst.trailCount = prev.trailCount;
  dst.trailHead = prev.trailHead;
  dst.hue = prev.hue != 0 ? prev.hue : hueFromHex(dst.hex);
}

void Tracker::mergeSnapshot(const Aircraft *incoming, size_t count) {
  // This must not be a local array: one Aircraft is roughly 384 bytes, so a
  // 32-aircraft snapshot is about 12 KiB.  The Arduino loop task has an 8 KiB
  // stack by default, which caused a stack overflow on each successful poll.
  static Aircraft previous[kMaxAircraft];
  const size_t prevCount = count_;
  memcpy(previous, aircraft_, sizeof(Aircraft) * prevCount);

  count_ = 0;
  for (size_t i = 0; i < count && count_ < kMaxAircraft; ++i) {
    Aircraft &dst = aircraft_[count_];
    dst = incoming[i];
    dst.seen = true;

    const Aircraft *prev = nullptr;
    for (size_t j = 0; j < prevCount; ++j) {
      if (strcasecmp(previous[j].hex, dst.hex) == 0) {
        prev = &previous[j];
        break;
      }
    }

    if (prev != nullptr) {
      retainEnrichment(dst, *prev);
      pushTrail(dst, dst.eastNm, dst.northNm);
    } else {
      dst.hue = hueFromHex(dst.hex);
      dst.trailCount = 0;
      dst.trailHead = 0;
      pushTrail(dst, dst.eastNm, dst.northNm);
    }
    ++count_;
  }

  if (selectedHex_[0] != '\0' && findByHex(selectedHex_) == nullptr) {
    selectedHex_[0] = '\0';
    autoSelected_ = true;
  }
  if (autoSelected_) {
    selectNearest();
  }
}

Aircraft *Tracker::selected() {
  return findByHex(selectedHex_);
}

const Aircraft *Tracker::selected() const {
  return findByHex(selectedHex_);
}

void Tracker::selectNearest() {
  if (count_ == 0) {
    selectedHex_[0] = '\0';
    return;
  }

  size_t best = 0;
  float bestDist = aircraft_[0].distanceNm;
  for (size_t i = 1; i < count_; ++i) {
    if (aircraft_[i].distanceNm < bestDist) {
      bestDist = aircraft_[i].distanceNm;
      best = i;
    }
  }
  strncpy(selectedHex_, aircraft_[best].hex, sizeof(selectedHex_) - 1);
  selectedHex_[sizeof(selectedHex_) - 1] = '\0';
}

bool Tracker::selectByHex(const char *hex) {
  Aircraft *ac = findByHex(hex);
  if (ac == nullptr) {
    return false;
  }
  strncpy(selectedHex_, ac->hex, sizeof(selectedHex_) - 1);
  selectedHex_[sizeof(selectedHex_) - 1] = '\0';
  return true;
}

Aircraft *Tracker::selectNearestTo(float eastNm, float northNm, float hitRadiusNm) {
  Aircraft *best = nullptr;
  float bestDist2 = hitRadiusNm * hitRadiusNm;

  for (size_t i = 0; i < count_; ++i) {
    const float de = aircraft_[i].eastNm - eastNm;
    const float dn = aircraft_[i].northNm - northNm;
    const float d2 = de * de + dn * dn;
    if (d2 <= bestDist2) {
      bestDist2 = d2;
      best = &aircraft_[i];
    }
  }

  if (best != nullptr) {
    selectByHex(best->hex);
    autoSelected_ = false;
  } else {
    selectedHex_[0] = '\0';
    autoSelected_ = true;
    selectNearest();
  }
  return best;
}
