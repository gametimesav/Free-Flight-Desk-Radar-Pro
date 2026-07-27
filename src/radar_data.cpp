#include "radar_data.h"

#include <HTTPClient.h>
#include <cmath>

#include "net_fetch.h"

namespace radar_data {
namespace {

struct RouteCacheEntry {
  char callsign[12];
  char route[16];
  uint8_t failCount;
  uint32_t retryAtMs;
  bool permanentMiss;
};

constexpr int ROUTE_CACHE_SIZE = 24;
constexpr int ROUTE_LOOKUP_BUDGET = 2;
constexpr uint32_t ROUTE_RETRY_BASE_MS = 30000;
constexpr uint32_t ROUTE_RETRY_MAX_MS = 600000;

RouteCacheEntry routeCache[ROUTE_CACHE_SIZE] = {};
int routeCacheNext = 0;

int findRouteCacheIndex(const String& callsign) {
  for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
    if (routeCache[i].callsign[0] && callsign.equals(routeCache[i].callsign)) {
      return i;
    }
  }
  return -1;
}

bool routeCacheRetryDue(const RouteCacheEntry& entry) {
  if (entry.retryAtMs == 0) {
    return true;
  }
  return (int32_t)(millis() - entry.retryAtMs) >= 0;
}

void cacheRouteFound(const String& callsign, const String& route) {
  if (callsign.length() == 0 || callsign == "UNK") {
    return;
  }
  int index = findRouteCacheIndex(callsign);
  if (index < 0) {
    index = routeCacheNext;
    routeCacheNext = (routeCacheNext + 1) % ROUTE_CACHE_SIZE;
  }
  strlcpy(routeCache[index].callsign, callsign.c_str(), sizeof(routeCache[index].callsign));
  strlcpy(routeCache[index].route, route.c_str(), sizeof(routeCache[index].route));
  routeCache[index].failCount = 0;
  routeCache[index].retryAtMs = 0;
  routeCache[index].permanentMiss = false;
}

void cacheRouteNotFound(const String& callsign) {
  if (callsign.length() == 0 || callsign == "UNK") {
    return;
  }
  int index = findRouteCacheIndex(callsign);
  if (index < 0) {
    index = routeCacheNext;
    routeCacheNext = (routeCacheNext + 1) % ROUTE_CACHE_SIZE;
  }
  strlcpy(routeCache[index].callsign, callsign.c_str(), sizeof(routeCache[index].callsign));
  routeCache[index].route[0] = '\0';
  routeCache[index].failCount = 0;
  routeCache[index].retryAtMs = 0;
  routeCache[index].permanentMiss = true;
}

void cacheRouteTransientFailure(const String& callsign) {
  if (callsign.length() == 0 || callsign == "UNK") {
    return;
  }
  int index = findRouteCacheIndex(callsign);
  if (index < 0) {
    index = routeCacheNext;
    routeCacheNext = (routeCacheNext + 1) % ROUTE_CACHE_SIZE;
    strlcpy(routeCache[index].callsign, callsign.c_str(), sizeof(routeCache[index].callsign));
    routeCache[index].route[0] = '\0';
    routeCache[index].failCount = 0;
  }

  if (routeCache[index].failCount < 7) {
    routeCache[index].failCount++;
  }
  uint32_t retryDelayMs = ROUTE_RETRY_BASE_MS << routeCache[index].failCount;
  if (retryDelayMs > ROUTE_RETRY_MAX_MS) {
    retryDelayMs = ROUTE_RETRY_MAX_MS;
  }
  routeCache[index].retryAtMs = millis() + retryDelayMs;
  routeCache[index].permanentMiss = false;
}

bool fetchRouteForCallsign(const String& callsign, String& routeOut) {
  routeOut = "";
  JsonDocument filter;
  filter["response"]["flightroute"]["origin"]["iata_code"] = true;
  filter["response"]["flightroute"]["destination"]["iata_code"] = true;

  JsonDocument doc;
  if (!net::http_get_json("https://api.adsbdb.com/v0/callsign/" + callsign, doc, &filter)) {
    return false;
  }

  const char* from = doc["response"]["flightroute"]["origin"]["iata_code"] | "";
  const char* to = doc["response"]["flightroute"]["destination"]["iata_code"] | "";
  if (from[0] && to[0]) {
    routeOut = String(from) + ">" + String(to);
  }
  return true;
}

String normalizeCountry(const String& rawCountry) {
  String country = rawCountry;
  if (country == "null" || country == "") country = "UNK";
  if (country == "United States") country = "USA";
  if (country == "United Kingdom") country = "UK";
  if (country == "Russian Federation") country = "Russia";
  if (country.length() > 8) country = country.substring(0, 8);
  return country;
}

}  // namespace

void parseOpenSkyStates(JsonArray states,
                        float radarLat,
                        float radarLon,
                        float maxRadarRangeKm,
                        int maxPlanesShown,
                        bool useMetric,
                        RankedPlane* rankedPlanes,
                        int& rankedCount) {
  rankedCount = 0;

  for (JsonArray plane : states) {
    if (plane[5].isNull() || plane[6].isNull()) continue;

    float lat = plane[6].as<float>();
    float lon = plane[5].as<float>();

    float dY = (lat - radarLat) * 111.1f;
    float dX = (lon - radarLon) * 111.1f * cos(radarLat * PI / 180.0f);
    float distKm = sqrt(dX * dX + dY * dY);
    if (distKm > maxRadarRangeKm) continue;

    int x = 120 + (distKm / maxRadarRangeKm * 100.0f) * sin(atan2(dX, dY));
    int y = 120 - (distKm / maxRadarRangeKm * 100.0f) * cos(atan2(dX, dY));

    String callsign = plane[1].as<String>();
    callsign.trim();
    if (callsign == "" || callsign == "null") callsign = "UNK";

    String country = normalizeCountry(plane[2].as<String>());

    float alt = plane[7].as<float>();
    String altStr = useMetric ? String((int)alt) + "m" : String((int)(alt * 3.28084f)) + "ft";

    float velocityMs = plane[9].isNull() ? 0.0f : plane[9].as<float>();
    int gsKt = (int)lround(velocityMs * 1.94384f);
    int vertRateFpm = plane[11].isNull() ? 0 : (int)lround(plane[11].as<float>() * 196.8504f);
    bool onGround = !plane[8].isNull() && plane[8].as<bool>();
    String squawk = plane[14].isNull() ? "" : plane[14].as<String>();
    bool emergency = (squawk == "7500" || squawk == "7600" || squawk == "7700");

    float headingDeg = plane[10].isNull() ? 0.0f : plane[10].as<float>();
    if (headingDeg < 0.0f) headingDeg = 0.0f;
    if (headingDeg >= 360.0f) headingDeg = fmod(headingDeg, 360.0f);

    ScreenPlane mapped = {x, y, 0, 0, callsign, altStr, country, lat, lon, velocityMs,
                          gsKt, vertRateFpm, onGround, emergency, headingDeg,
                          millis(), 1.0f, false};

    int insertPos = rankedCount;
    while (insertPos > 0 && rankedPlanes[insertPos - 1].distKm > distKm) {
      insertPos--;
    }

    if (insertPos >= maxPlanesShown) {
      continue;
    }

    int lastIndex = (rankedCount < maxPlanesShown) ? rankedCount : (maxPlanesShown - 1);
    for (int i = lastIndex; i > insertPos; i--) {
      rankedPlanes[i] = rankedPlanes[i - 1];
    }

    rankedPlanes[insertPos] = {mapped, distKm};
    if (rankedCount < maxPlanesShown) {
      rankedCount++;
    }
  }
}

void resolveRoutesForRankedPlanes(RankedPlane* rankedPlanes,
                                  int rankedCount,
                                  NetTelemetry& telemetry) {
  int budget = ROUTE_LOOKUP_BUDGET;
  for (int i = 0; i < rankedCount; i++) {
    String callsign = rankedPlanes[i].plane.callsign;
    if (callsign.length() == 0 || callsign == "UNK") {
      continue;
    }

    int cacheIndex = findRouteCacheIndex(callsign);
    if (cacheIndex >= 0) {
      const RouteCacheEntry& entry = routeCache[cacheIndex];
      if (entry.route[0]) {
        telemetry.routeCacheHits++;
        rankedPlanes[i].plane.country = String(entry.route);
        continue;
      }
      if (entry.permanentMiss || !routeCacheRetryDue(entry)) {
        continue;
      }
    }

    if (budget <= 0) {
      continue;
    }

    budget--;
    String route;
    if (!fetchRouteForCallsign(callsign, route)) {
      telemetry.routeTransientFail++;
      cacheRouteTransientFailure(callsign);
      continue;
    }

    if (route.length() == 0) {
      telemetry.routeNotFound++;
      cacheRouteNotFound(callsign);
      continue;
    }

    telemetry.routeResolved++;
    cacheRouteFound(callsign, route);
    rankedPlanes[i].plane.country = route;
  }
}

}  // namespace radar_data
