#pragma once

#include "aircraft.h"

#include <array>
#include <stddef.h>

namespace adsb {

class AircraftStore {
 public:
    static constexpr size_t kMaxAircraft = 32;

    AircraftStore(double receiverLatitude, double receiverLongitude, int64_t staleMs);

    void setReceiverLocation(double latitude, double longitude);
    void applyUpdate(const AircraftUpdate& update, int64_t nowMs);
    void purgeStale(int64_t nowMs);
    size_t snapshot(Aircraft* output, size_t capacity, int64_t nowMs) const;
    size_t activeCount(int64_t nowMs) const;

 private:
    std::array<Aircraft, kMaxAircraft> aircraft_{};
    double receiverLatitude_;
    double receiverLongitude_;
    int64_t staleMs_;

    int findByIcao(const std::string& icao) const;
    int findSlot(int64_t nowMs) const;
    double distanceNm(double lat1, double lon1, double lat2, double lon2) const;
    double bearingDeg(double lat1, double lon1, double lat2, double lon2) const;
    static void pushTrace(Aircraft& aircraft);
    static void sortByDistance(Aircraft* aircraft, size_t count);
};

}  // namespace adsb
