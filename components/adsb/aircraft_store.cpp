#include "aircraft_store.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adsb {

AircraftStore::AircraftStore(double receiverLatitude, double receiverLongitude, int64_t staleMs)
    : receiverLatitude_(receiverLatitude), receiverLongitude_(receiverLongitude), staleMs_(staleMs) {}

void AircraftStore::setReceiverLocation(double latitude, double longitude) {
    const bool centerChanged = std::abs(receiverLatitude_ - latitude) > 0.000001 ||
                               std::abs(receiverLongitude_ - longitude) > 0.000001;
    receiverLatitude_ = latitude;
    receiverLongitude_ = longitude;
    for (Aircraft& item : aircraft_) {
        if (item.hasPosition) {
            item.distanceNm = distanceNm(receiverLatitude_, receiverLongitude_,
                                         item.latitude, item.longitude);
            item.bearingDeg = static_cast<int>(std::lround(bearingDeg(receiverLatitude_, receiverLongitude_,
                                                                      item.latitude, item.longitude)));
            item.hasBearing = true;
            item.hasDistance = true;
            if (centerChanged) {
                item.traceCount = 0;
                pushTrace(item);
            }
        }
    }
}

void AircraftStore::applyUpdate(const AircraftUpdate& update, int64_t nowMs) {
    if (update.icao.empty()) {
        return;
    }
    const bool hasObservation = update.hasCallsign || update.hasType || update.hasAltitude ||
                                update.hasGroundSpeed || update.hasTrack ||
                                update.hasPosition || update.hasDistance || update.hasBearing;

    int index = findByIcao(update.icao);
    if (index < 0 && !hasObservation) {
        return;
    }
    if (index < 0) {
        index = findSlot(nowMs);
    }
    if (index < 0) {
        return;
    }

    Aircraft& target = aircraft_[index];
    if (target.icao != update.icao) {
        target = Aircraft{};
        target.icao = update.icao;
    }

    if (update.hasCallsign) {
        const std::string previous = target.normalizedCallsign;
        target.callsign = trimCopy(update.callsign);
        target.normalizedCallsign = update.hasNormalizedCallsign
            ? update.normalizedCallsign
            : normalizeCallsign(target.callsign);
        if (previous != target.normalizedCallsign) {
            target.route.clear();
            target.hasRoute = false;
        }
    }
    if (update.hasRoute) {
        target.route = update.route;
        target.hasRoute = !target.route.empty();
    }
    if (update.hasType) {
        target.typeCode = trimCopy(update.typeCode);
        uppercaseInPlace(target.typeCode);
        target.typeDescription = trimCopy(update.typeDescription);
        target.category = trimCopy(update.category);
        uppercaseInPlace(target.category);
        target.hasType = !target.typeCode.empty() || !target.typeDescription.empty() ||
                         !target.category.empty();
    }
    if (update.hasAltitude) {
        target.altitudeFt = update.altitudeFt;
        target.hasAltitude = true;
    }
    if (update.hasGroundSpeed) {
        target.groundSpeedKt = update.groundSpeedKt;
        target.hasGroundSpeed = true;
    }
    if (update.hasTrack) {
        target.trackDeg = update.trackDeg;
        target.hasTrack = true;
    }
    if (update.hasBearing) {
        target.bearingDeg = update.bearingDeg;
        target.hasBearing = true;
    }
    if (update.hasPosition) {
        target.latitude = update.latitude;
        target.longitude = update.longitude;
        target.distanceNm = distanceNm(receiverLatitude_, receiverLongitude_,
                                       update.latitude, update.longitude);
        target.bearingDeg = static_cast<int>(std::lround(bearingDeg(receiverLatitude_, receiverLongitude_,
                                                                    update.latitude, update.longitude)));
        target.hasBearing = true;
        target.hasPosition = true;
        target.hasDistance = true;
        pushTrace(target);
    }
    if (!update.hasPosition && update.hasDistance) {
        target.distanceNm = update.distanceNm;
        target.hasDistance = true;
    }

    if (hasObservation) {
        target.lastSeenMs = nowMs;
    }
}

void AircraftStore::purgeStale(int64_t nowMs) {
    for (Aircraft& item : aircraft_) {
        if (!item.icao.empty() && nowMs - item.lastSeenMs > staleMs_) {
            item = Aircraft{};
        }
    }
}

size_t AircraftStore::snapshot(Aircraft* output, size_t capacity, int64_t nowMs) const {
    if (output == nullptr || capacity == 0) {
        return 0;
    }

    std::array<const Aircraft*, kMaxAircraft> active = {};
    size_t activeCount = 0;
    for (const Aircraft& item : aircraft_) {
        if (!item.icao.empty() && nowMs - item.lastSeenMs <= staleMs_) {
            active[activeCount++] = &item;
        }
    }

    std::sort(active.begin(), active.begin() + activeCount,
              [](const Aircraft* left, const Aircraft* right) {
                  const double leftDistance = (left->hasDistance || left->hasPosition)
                      ? left->distanceNm
                      : 99999.0;
                  const double rightDistance = (right->hasDistance || right->hasPosition)
                      ? right->distanceNm
                      : 99999.0;
                  if (leftDistance != rightDistance) {
                      return leftDistance < rightDistance;
                  }
                  return left->lastSeenMs > right->lastSeenMs;
              });

    const size_t count = std::min(capacity, activeCount);
    for (size_t i = 0; i < count; ++i) {
        output[i] = *active[i];
    }
    return count;
}

size_t AircraftStore::activeCount(int64_t nowMs) const {
    size_t count = 0;
    for (const Aircraft& item : aircraft_) {
        if (!item.icao.empty() && nowMs - item.lastSeenMs <= staleMs_) {
            count++;
        }
    }
    return count;
}

size_t AircraftStore::rangeCount(double rangeNm, int64_t nowMs) const {
    size_t count = 0;
    for (const Aircraft& item : aircraft_) {
        if (item.icao.empty() || nowMs - item.lastSeenMs > staleMs_ ||
            !(item.hasDistance || item.hasPosition)) {
            continue;
        }
        if (item.distanceNm <= rangeNm) {
            count++;
        }
    }
    return count;
}

int AircraftStore::findByIcao(const std::string& icao) const {
    for (size_t i = 0; i < aircraft_.size(); ++i) {
        if (aircraft_[i].icao == icao) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int AircraftStore::findSlot(int64_t nowMs) const {
    int oldestIndex = -1;
    int64_t oldestSeen = std::numeric_limits<int64_t>::max();

    for (size_t i = 0; i < aircraft_.size(); ++i) {
        if (aircraft_[i].icao.empty()) {
            return static_cast<int>(i);
        }
        if (nowMs - aircraft_[i].lastSeenMs > staleMs_) {
            return static_cast<int>(i);
        }
        if (aircraft_[i].lastSeenMs < oldestSeen) {
            oldestSeen = aircraft_[i].lastSeenMs;
            oldestIndex = static_cast<int>(i);
        }
    }

    return oldestIndex;
}

double AircraftStore::distanceNm(double lat1, double lon1, double lat2, double lon2) const {
    constexpr double kEarthRadiusNm = 3440.065;
    constexpr double kDegToRad = 0.017453292519943295;

    const double phi1 = lat1 * kDegToRad;
    const double phi2 = lat2 * kDegToRad;
    const double dPhi = (lat2 - lat1) * kDegToRad;
    const double dLambda = (lon2 - lon1) * kDegToRad;
    const double sinDPhi = std::sin(dPhi / 2.0);
    const double sinDLambda = std::sin(dLambda / 2.0);
    const double a = sinDPhi * sinDPhi +
                     std::cos(phi1) * std::cos(phi2) * sinDLambda * sinDLambda;
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthRadiusNm * c;
}

double AircraftStore::bearingDeg(double lat1, double lon1, double lat2, double lon2) const {
    constexpr double kRadToDeg = 57.29577951308232;
    constexpr double kDegToRad = 0.017453292519943295;

    const double phi1 = lat1 * kDegToRad;
    const double phi2 = lat2 * kDegToRad;
    const double dLambda = (lon2 - lon1) * kDegToRad;
    const double y = std::sin(dLambda) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) -
                     std::sin(phi1) * std::cos(phi2) * std::cos(dLambda);
    double bearing = std::atan2(y, x) * kRadToDeg;
    if (bearing < 0.0) {
        bearing += 360.0;
    }
    return bearing;
}

void AircraftStore::pushTrace(Aircraft& aircraft) {
    if (!aircraft.hasDistance || !aircraft.hasBearing) {
        return;
    }
    if (aircraft.traceCount > 0 &&
        std::abs(aircraft.traceDistanceNm[0] - aircraft.distanceNm) < 0.05 &&
        std::abs(aircraft.traceBearingDeg[0] - aircraft.bearingDeg) < 2) {
        return;
    }
    const size_t limit = std::min<size_t>(aircraft.traceCount, Aircraft::kTraceLength - 1);
    for (size_t i = limit; i > 0; --i) {
        aircraft.traceDistanceNm[i] = aircraft.traceDistanceNm[i - 1];
        aircraft.traceBearingDeg[i] = aircraft.traceBearingDeg[i - 1];
    }
    aircraft.traceDistanceNm[0] = aircraft.distanceNm;
    aircraft.traceBearingDeg[0] = aircraft.bearingDeg;
    if (aircraft.traceCount < Aircraft::kTraceLength) {
        ++aircraft.traceCount;
    }
}

void AircraftStore::sortByDistance(Aircraft* aircraft, size_t count) {
    std::sort(aircraft, aircraft + count, [](const Aircraft& left, const Aircraft& right) {
        const double leftDistance = (left.hasDistance || left.hasPosition) ? left.distanceNm : 99999.0;
        const double rightDistance = (right.hasDistance || right.hasPosition) ? right.distanceNm : 99999.0;
        return leftDistance < rightDistance;
    });
}

}  // namespace adsb
