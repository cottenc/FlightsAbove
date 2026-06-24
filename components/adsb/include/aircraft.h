#pragma once

#include <stdint.h>
#include <string>

namespace adsb {

struct Aircraft {
    std::string icao;
    std::string callsign;
    int altitudeFt = 0;
    int groundSpeedKt = 0;
    int trackDeg = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double distanceNm = 0.0;
    bool hasAltitude = false;
    bool hasGroundSpeed = false;
    bool hasTrack = false;
    bool hasPosition = false;
    int64_t lastSeenMs = 0;
};

struct AircraftUpdate {
    std::string icao;
    std::string callsign;
    int altitudeFt = 0;
    int groundSpeedKt = 0;
    int trackDeg = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasCallsign = false;
    bool hasAltitude = false;
    bool hasGroundSpeed = false;
    bool hasTrack = false;
    bool hasPosition = false;
};

}  // namespace adsb
