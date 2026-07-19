#pragma once

#include "aircraft.hpp"

#include <stddef.h>

/** Fetch nearby aircraft from adsb.fi into out[0..*outCount). Returns false on network/parse failure. */
bool fetchNearbyAircraft(float homeLat, float homeLon, int radiusNm,
                         Aircraft *out, size_t maxCount, size_t *outCount);

/** Enrich manufacturer / type / registration from hexdb.io. Non-fatal. */
bool fetchAircraftDetails(Aircraft &aircraft);

/** Enrich origin / destination airport names from hexdb.io. Non-fatal. */
bool fetchRouteInfo(Aircraft &aircraft);
