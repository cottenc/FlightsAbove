#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace adsb {

struct RouteRequest {
    std::string callsign;
    double latitude = 0.0;
    double longitude = 0.0;
};

struct RouteResult {
    std::string callsign;
    std::string route;
};

class RouteCache {
 public:
    static constexpr size_t kMaxEntries = 32;
    static constexpr int64_t kFoundTtlMs = 6 * 60 * 60 * 1000;
    static constexpr int64_t kMissingTtlMs = 5 * 60 * 1000;

    bool routeFor(const std::string& callsign, int64_t nowMs, std::string& route) const;
    bool shouldLookup(const std::string& callsign, int64_t nowMs) const;
    void markFound(const std::string& callsign, const std::string& route, int64_t nowMs);
    void markMissing(const std::string& callsign, int64_t nowMs);

 private:
    struct Entry {
        std::string callsign;
        std::string route;
        int64_t nextLookupMs = 0;
    };

    std::array<Entry, kMaxEntries> entries_{};

    int find(const std::string& callsign) const;
    int slot(int64_t nowMs) const;
};

std::string buildRouteRequestJson(const RouteRequest* requests, size_t count);
size_t parseRouteResultsJson(const char* json, RouteResult* output, size_t capacity);

}  // namespace adsb
