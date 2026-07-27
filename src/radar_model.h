#pragma once

#include <Arduino.h>

struct ScreenPlane {
  int x;
  int y;
  int lastX;
  int lastY;
  String callsign;
  String altStr;
  String country;
  float lat;
  float lon;
  float velocityMs;
  int gsKt;
  int vertRateFpm;
  bool onGround;
  bool emergency;
  float headingDeg;
  unsigned long positionTime;
  float brightness;
  bool soundTriggered;
};

struct RankedPlane {
  ScreenPlane plane;
  float distKm;
};

struct NetTelemetry {
  uint32_t openskyOk = 0;
  uint32_t openskyFail = 0;
  uint32_t routeCacheHits = 0;
  uint32_t routeResolved = 0;
  uint32_t routeNotFound = 0;
  uint32_t routeTransientFail = 0;
  unsigned long lastLogMs = 0;
};

constexpr int MAX_PLANES = 20;
