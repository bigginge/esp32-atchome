#include "api_client.hpp"

#include "geo.hpp"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

static constexpr uint16_t kHttpTimeoutMs = 4000;

static void safeJsonCopy(char *dest, size_t destSize, JsonObject obj, const char *key) {
  dest[0] = '\0';
  if (!obj[key].is<const char *>()) {
    return;
  }
  const char *val = obj[key].as<const char *>();
  while (*val == ' ') {
    ++val;
  }
  strncpy(dest, val, destSize - 1);
  dest[destSize - 1] = '\0';
  size_t len = strlen(dest);
  while (len > 0 && dest[len - 1] == ' ') {
    dest[--len] = '\0';
  }
}

static void clearSnapshotFields(Aircraft &ac) {
  memset(&ac, 0, sizeof(ac));
}

bool fetchNearbyAircraft(float homeLat, float homeLon, int radiusNm,
                         Aircraft *out, size_t maxCount, size_t *outCount) {
  *outCount = 0;
  if (out == nullptr || maxCount == 0) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[160];
  snprintf(url, sizeof(url),
           "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d",
           homeLat, homeLon, radiusNm);

  Serial.printf("[adsb.fi] GET %s\n", url);

  if (!http.begin(client, url)) {
    Serial.println("[adsb.fi] begin failed");
    return false;
  }

  http.addHeader("Accept-Encoding", "identity");
  http.setTimeout(kHttpTimeoutMs);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[adsb.fi] HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["ac"][0]["hex"] = true;
  filter["ac"][0]["flight"] = true;
  filter["ac"][0]["r"] = true;
  filter["ac"][0]["t"] = true;
  filter["ac"][0]["lat"] = true;
  filter["ac"][0]["lon"] = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["gs"] = true;
  filter["ac"][0]["track"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("[adsb.fi] JSON %s\n", err.c_str());
    return false;
  }

  JsonArray aircraft = doc["ac"].as<JsonArray>();
  if (aircraft.isNull()) {
    Serial.println("[adsb.fi] No ac array");
    return true;
  }

  size_t count = 0;
  for (JsonObject ac : aircraft) {
    if (count >= maxCount) {
      break;
    }
    if (!ac["lat"].is<float>() || !ac["lon"].is<float>()) {
      continue;
    }

    Aircraft &dst = out[count];
    clearSnapshotFields(dst);

    safeJsonCopy(dst.hex, sizeof(dst.hex), ac, "hex");
    if (dst.hex[0] == '\0') {
      continue;
    }

    safeJsonCopy(dst.callsign, sizeof(dst.callsign), ac, "flight");
    safeJsonCopy(dst.registration, sizeof(dst.registration), ac, "r");
    safeJsonCopy(dst.typeCode, sizeof(dst.typeCode), ac, "t");

    dst.lat = ac["lat"].as<float>();
    dst.lon = ac["lon"].as<float>();
    latLonToNm(homeLat, homeLon, dst.lat, dst.lon, &dst.eastNm, &dst.northNm);
    dst.distanceNm = haversineNm(homeLat, homeLon, dst.lat, dst.lon);

    if (ac["alt_baro"].is<int>()) {
      dst.altitudeFt = ac["alt_baro"].as<int>();
    } else if (ac["alt_baro"].is<float>()) {
      dst.altitudeFt = static_cast<int>(ac["alt_baro"].as<float>());
    } else {
      dst.altitudeFt = 0;
    }

    if (ac["gs"].is<float>()) {
      dst.groundSpeedKts = ac["gs"].as<float>();
    } else if (ac["gs"].is<int>()) {
      dst.groundSpeedKts = static_cast<float>(ac["gs"].as<int>());
    }

    if (ac["track"].is<float>()) {
      dst.trackDeg = ac["track"].as<float>();
    } else if (ac["track"].is<int>()) {
      dst.trackDeg = static_cast<float>(ac["track"].as<int>());
    }

    dst.hue = hueFromHex(dst.hex);
    dst.seen = true;
    ++count;
  }

  *outCount = count;
  Serial.printf("[adsb.fi] Parsed %u aircraft\n", static_cast<unsigned>(count));
  return true;
}

bool fetchAircraftDetails(Aircraft &aircraft) {
  if (aircraft.hex[0] == '\0') {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[80];
  snprintf(url, sizeof(url), "https://hexdb.io/api/v1/aircraft/%s", aircraft.hex);
  Serial.printf("[hexdb] Aircraft %s\n", url);

  if (!http.begin(client, url)) {
    return false;
  }

  http.setTimeout(kHttpTimeoutMs);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[hexdb] Aircraft HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[hexdb] Aircraft JSON %s\n", err.c_str());
    return false;
  }

  JsonObject obj = doc.as<JsonObject>();
  safeJsonCopy(aircraft.manufacturer, sizeof(aircraft.manufacturer), obj, "Manufacturer");
  safeJsonCopy(aircraft.typeDescription, sizeof(aircraft.typeDescription), obj, "Type");
  safeJsonCopy(aircraft.registeredOwner, sizeof(aircraft.registeredOwner), obj,
               "RegisteredOwners");
  if (aircraft.registration[0] == '\0') {
    safeJsonCopy(aircraft.registration, sizeof(aircraft.registration), obj, "Registration");
  }

  aircraft.detailsLoaded = true;
  return true;
}

static bool fetchAirportInfo(const char *icao, char *buffer, size_t bufferSize) {
  if (icao == nullptr || icao[0] == '\0') {
    return false;
  }

  strncpy(buffer, icao, bufferSize - 1);
  buffer[bufferSize - 1] = '\0';

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[96];
  snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", icao);
  if (!http.begin(client, url)) {
    return false;
  }

  http.setTimeout(kHttpTimeoutMs);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    return false;
  }

  if (doc["airport"].is<const char *>() && doc["country_code"].is<const char *>()) {
    snprintf(buffer, bufferSize, "%s, %s", doc["airport"].as<const char *>(),
             doc["country_code"].as<const char *>());
    return true;
  }
  return false;
}

bool fetchRouteInfo(Aircraft &aircraft) {
  if (aircraft.callsign[0] == '\0' || aircraft.routeLoaded) {
    return false;
  }

  if (aircraft.routeLookupStep == 0) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char url[96];
    snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", aircraft.callsign);
    Serial.printf("[hexdb] Route %s\n", url);

    if (!http.begin(client, url)) {
      return false;
    }
    http.setTimeout(kHttpTimeoutMs);
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
      Serial.printf("[hexdb] Route HTTP %d\n", httpCode);
      http.end();
      return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err || !doc["route"].is<const char *>()) {
      return false;
    }

    const char *route = doc["route"].as<const char *>();
    const char *hyphen = strchr(route, '-');
    if (hyphen == nullptr) {
      return false;
    }

    const size_t originLen = static_cast<size_t>(hyphen - route);
    if (originLen == 0 || originLen >= sizeof(aircraft.originIcao)) {
      return false;
    }
    memcpy(aircraft.originIcao, route, originLen);
    aircraft.originIcao[originLen] = '\0';
    strncpy(aircraft.destinationIcao, hyphen + 1, sizeof(aircraft.destinationIcao) - 1);
    aircraft.destinationIcao[sizeof(aircraft.destinationIcao) - 1] = '\0';
    if (aircraft.destinationIcao[0] == '\0') {
      return false;
    }

    // Show the airport codes immediately; the following scheduled requests
    // replace them with human-friendly airport names.
    strncpy(aircraft.origin, aircraft.originIcao, sizeof(aircraft.origin) - 1);
    strncpy(aircraft.destination, aircraft.destinationIcao, sizeof(aircraft.destination) - 1);
    aircraft.routeLookupStep = 1;
    return true;
  }

  if (aircraft.routeLookupStep == 1) {
    // Advance even if the optional airport-name lookup fails: the ICAO code is
    // already displayed and retrying a failed request forever freezes the UI.
    fetchAirportInfo(aircraft.originIcao, aircraft.origin, sizeof(aircraft.origin));
    aircraft.routeLookupStep = 2;
    return true;
  }

  if (aircraft.routeLookupStep == 2) {
    fetchAirportInfo(aircraft.destinationIcao, aircraft.destination,
                     sizeof(aircraft.destination));
    aircraft.routeLookupStep = 3;
  }
  aircraft.routeLoaded = true;
  return true;
}
