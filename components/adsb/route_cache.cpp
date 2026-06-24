#include "route_cache.h"

#include "aircraft.h"
#include "cJSON.h"

#include <cstdio>

namespace adsb {

bool RouteCache::routeFor(const std::string& callsign, int64_t nowMs, std::string& route) const {
    const int index = find(callsign);
    if (index < 0 || entries_[index].route.empty() || nowMs >= entries_[index].nextLookupMs) {
        return false;
    }
    route = entries_[index].route;
    return true;
}

bool RouteCache::shouldLookup(const std::string& callsign, int64_t nowMs) const {
    if (callsign.empty()) {
        return false;
    }
    const int index = find(callsign);
    return index < 0 || nowMs >= entries_[index].nextLookupMs;
}

void RouteCache::markFound(const std::string& callsign, const std::string& route, int64_t nowMs) {
    if (callsign.empty()) {
        return;
    }
    int index = find(callsign);
    if (index < 0) {
        index = slot(nowMs);
    }
    if (index < 0) {
        return;
    }
    entries_[index].callsign = callsign;
    entries_[index].route = route;
    entries_[index].nextLookupMs = nowMs + kFoundTtlMs;
}

void RouteCache::markMissing(const std::string& callsign, int64_t nowMs) {
    if (callsign.empty()) {
        return;
    }
    int index = find(callsign);
    if (index < 0) {
        index = slot(nowMs);
    }
    if (index < 0) {
        return;
    }
    entries_[index].callsign = callsign;
    entries_[index].route.clear();
    entries_[index].nextLookupMs = nowMs + kMissingTtlMs;
}

int RouteCache::find(const std::string& callsign) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].callsign == callsign) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RouteCache::slot(int64_t nowMs) const {
    int oldest = -1;
    int64_t oldestNext = INT64_MAX;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].callsign.empty()) {
            return static_cast<int>(i);
        }
        if (nowMs >= entries_[i].nextLookupMs) {
            return static_cast<int>(i);
        }
        if (entries_[i].nextLookupMs < oldestNext) {
            oldestNext = entries_[i].nextLookupMs;
            oldest = static_cast<int>(i);
        }
    }
    return oldest;
}

std::string jsonEscape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char escaped[7];
                snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(ch));
                output += escaped;
            } else {
                output.push_back(ch);
            }
            break;
        }
    }
    return output;
}

std::string buildRouteRequestJson(const RouteRequest* requests, size_t count) {
    std::string json = "{\"planes\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            json += ",";
        }
        char lat[32];
        char lon[32];
        snprintf(lat, sizeof(lat), "%.6f", requests[i].latitude);
        snprintf(lon, sizeof(lon), "%.6f", requests[i].longitude);
        json += "{\"callsign\":\"";
        json += jsonEscape(requests[i].callsign);
        json += "\",\"lat\":";
        json += lat;
        json += ",\"lng\":";
        json += lon;
        json += "}";
    }
    json += "]}";
    return json;
}

size_t parseRouteResultsJson(const char* json, RouteResult* output, size_t capacity) {
    if (json == nullptr || output == nullptr || capacity == 0) {
        return 0;
    }

    cJSON* root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }

    size_t count = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (count >= capacity) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }

        const cJSON* callsign = cJSON_GetObjectItemCaseSensitive(item, "callsign");
        const cJSON* route = cJSON_GetObjectItemCaseSensitive(item, "_airport_codes_iata");
        if (!cJSON_IsString(callsign) || callsign->valuestring == nullptr ||
            !cJSON_IsString(route) || route->valuestring == nullptr) {
            continue;
        }

        output[count].callsign = normalizeCallsign(callsign->valuestring);
        output[count].route = trimCopy(route->valuestring);
        if (!output[count].callsign.empty() && !output[count].route.empty()) {
            ++count;
        }
    }

    cJSON_Delete(root);
    return count;
}

}  // namespace adsb
