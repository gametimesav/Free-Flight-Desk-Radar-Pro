#pragma once

#include <ArduinoJson.h>

#include "radar_model.h"

namespace radar_data {

void parseOpenSkyStates(JsonArray states,
                        float radarLat,
                        float radarLon,
                        float maxRadarRangeKm,
                        int maxPlanesShown,
                        bool useMetric,
                        RankedPlane* rankedPlanes,
                        int& rankedCount);

void resolveRoutesForRankedPlanes(RankedPlane* rankedPlanes,
                                  int rankedCount,
                                  NetTelemetry& telemetry);

}  // namespace radar_data
