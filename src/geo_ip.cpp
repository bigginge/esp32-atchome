#include "geo_ip.hpp"

#include "text_util.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <math.h>
#include <string.h>

// ip-api.com's free tier is plain HTTP only -- it has no TLS endpoint without
// a paid key. That is acceptable here precisely because the result is a
// suggestion the user visually confirms before it is ever used: the worst an
// on-path attacker achieves is a wrong number in an editable field. The
// coordinates are still range-checked below. Everything else in this project
// goes over TLS (see api_client.cpp); this is the one exception, which is why
// it lives in its own file.
static constexpr char kUrl[] =
    "http://ip-api.com/json/?fields=status,message,lat,lon,city,countryCode";
static constexpr uint16_t kHttpTimeoutMs = 4000;

bool fetchIpLocation(GeoResult *out) {
  if (out == nullptr) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, kUrl)) {
    Serial.println("[geoip] begin failed");
    return false;
  }

  http.setTimeout(kHttpTimeoutMs);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[geoip] HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[geoip] JSON %s\n", err.c_str());
    return false;
  }

  // The free tier reports rate limiting (45 req/min) in the body with HTTP 200,
  // not as an error status.
  if (strcmp(doc["status"] | "", "success") != 0) {
    Serial.printf("[geoip] lookup failed: %s\n", doc["message"] | "unknown");
    return false;
  }

  const double lat = doc["lat"] | NAN;
  const double lon = doc["lon"] | NAN;
  if (isnan(lat) || isnan(lon) || fabs(lat) > 90.0 || fabs(lon) > 180.0 ||
      (lat == 0.0 && lon == 0.0)) {
    Serial.println("[geoip] implausible coordinates, ignoring");
    return false;
  }

  out->lat = static_cast<float>(lat);
  out->lon = static_cast<float>(lon);
  strncpy(out->city, doc["city"] | "", sizeof(out->city) - 1);
  strncpy(out->country, doc["countryCode"] | "", sizeof(out->country) - 1);
  sanitizeAscii(out->city);
  sanitizeAscii(out->country);
  out->ok = true;

  Serial.printf("[geoip] %s, %s  %.4f, %.4f\n", out->city, out->country, out->lat, out->lon);
  return true;
}
