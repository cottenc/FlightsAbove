#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

namespace adsb {

struct Aircraft {
    static constexpr size_t kTraceLength = 6;

    std::string icao;
    std::string callsign;
    std::string normalizedCallsign;
    std::string route;
    std::string typeCode;
    std::string typeDescription;
    std::string category;
    int altitudeFt = 0;
    int groundSpeedKt = 0;
    int trackDeg = 0;
    int bearingDeg = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double distanceNm = 0.0;
    double traceDistanceNm[kTraceLength] = {};
    int traceBearingDeg[kTraceLength] = {};
    uint8_t traceCount = 0;
    bool hasAltitude = false;
    bool hasGroundSpeed = false;
    bool hasTrack = false;
    bool hasBearing = false;
    bool hasPosition = false;
    bool hasDistance = false;
    bool hasRoute = false;
    bool hasType = false;
    int64_t lastSeenMs = 0;
};

struct AircraftUpdate {
    std::string icao;
    std::string callsign;
    std::string normalizedCallsign;
    std::string route;
    std::string typeCode;
    std::string typeDescription;
    std::string category;
    int altitudeFt = 0;
    int groundSpeedKt = 0;
    int trackDeg = 0;
    int bearingDeg = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double distanceNm = 0.0;
    bool hasCallsign = false;
    bool hasNormalizedCallsign = false;
    bool hasRoute = false;
    bool hasAltitude = false;
    bool hasGroundSpeed = false;
    bool hasTrack = false;
    bool hasBearing = false;
    bool hasPosition = false;
    bool hasDistance = false;
    bool hasType = false;
};

std::string normalizeCallsign(const std::string& callsign);
std::string airlineIcaoFromCallsign(const std::string& callsign);
std::string trimCopy(const std::string& value);
void uppercaseInPlace(std::string& value);

}  // namespace adsb
