#include "aircraft_store.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adsb {

AircraftStore::AircraftStore(double receiverLatitude, double receiverLongitude, int64_t staleMs)
    : receiverLatitude_(receiverLatitude), receiverLongitude_(receiverLongitude), staleMs_(staleMs) {}

void AircraftStore::applyUpdate(const AircraftUpdate& update, int64_t nowMs) {
    if (update.icao.empty()) {
        return;
    }

    int index = findByIcao(update.icao);
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
        target.callsign = update.callsign;
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
    if (update.hasPosition) {
        target.latitude = update.latitude;
        target.longitude = update.longitude;
        target.distanceNm = distanceNm(receiverLatitude_, receiverLongitude_,
                                       update.latitude, update.longitude);
        target.hasPosition = true;
    }

    target.lastSeenMs = nowMs;
}

void AircraftStore::purgeStale(int64_t nowMs) {
    for (Aircraft& item : aircraft_) {
        if (!item.icao.empty() && nowMs - item.lastSeenMs > staleMs_) {
            item = Aircraft{};
        }
    }
}

size_t AircraftStore::snapshot(Aircraft* output, size_t capacity, int64_t nowMs) const {
    size_t count = 0;
    for (const Aircraft& item : aircraft_) {
        if (count >= capacity) {
            break;
        }
        if (!item.icao.empty() && nowMs - item.lastSeenMs <= staleMs_) {
            output[count++] = item;
        }
    }

    sortByDistance(output, count);
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

void AircraftStore::sortByDistance(Aircraft* aircraft, size_t count) {
    std::sort(aircraft, aircraft + count, [](const Aircraft& left, const Aircraft& right) {
        const double leftDistance = left.hasPosition ? left.distanceNm : 99999.0;
        const double rightDistance = right.hasPosition ? right.distanceNm : 99999.0;
        return leftDistance < rightDistance;
    });
}

}  // namespace adsb
