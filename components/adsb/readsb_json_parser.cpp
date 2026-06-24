#include "readsb_json_parser.h"

#include "cJSON.h"

#include <cmath>

namespace adsb {

namespace {

bool read_number(const cJSON* object, const char* name, double& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(value)) {
        return false;
    }
    output = value->valuedouble;
    return true;
}

bool read_int(const cJSON* object, const char* name, int& output) {
    double value = 0.0;
    if (!read_number(object, name, value)) {
        return false;
    }
    output = static_cast<int>(std::lround(value));
    return true;
}

bool read_string(const cJSON* object, const char* name, std::string& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }
    output = value->valuestring;
    return true;
}

}  // namespace

size_t ReadsbJsonParser::parseAircraftJson(const char* json, AircraftUpdate* output,
                                           size_t capacity) const {
    if (json == nullptr || output == nullptr || capacity == 0) {
        return 0;
    }

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return 0;
    }

    const cJSON* aircraft = cJSON_GetObjectItemCaseSensitive(root, "aircraft");
    if (!cJSON_IsArray(aircraft)) {
        cJSON_Delete(root);
        return 0;
    }

    size_t count = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, aircraft) {
        if (count >= capacity) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }

        std::string hex;
        if (!read_string(item, "hex", hex)) {
            continue;
        }
        hex = trimCopy(hex);
        uppercaseInPlace(hex);
        if (hex.empty()) {
            continue;
        }

        AircraftUpdate update;
        update.icao = hex;

        std::string flight;
        if (read_string(item, "flight", flight)) {
            flight = trimCopy(flight);
            if (!flight.empty()) {
                update.callsign = flight;
                update.normalizedCallsign = normalizeCallsign(flight);
                update.hasCallsign = true;
                update.hasNormalizedCallsign = true;
            }
        }

        std::string typeCode;
        std::string typeDescription;
        std::string category;
        if (read_string(item, "t", typeCode)) {
            update.typeCode = trimCopy(typeCode);
            update.hasType = !update.typeCode.empty();
        }
        if (read_string(item, "desc", typeDescription)) {
            update.typeDescription = trimCopy(typeDescription);
            update.hasType = update.hasType || !update.typeDescription.empty();
        }
        if (read_string(item, "category", category)) {
            update.category = trimCopy(category);
            update.hasType = update.hasType || !update.category.empty();
        }

        if (read_int(item, "alt_baro", update.altitudeFt) ||
            read_int(item, "alt_geom", update.altitudeFt)) {
            update.hasAltitude = true;
        }
        if (read_int(item, "gs", update.groundSpeedKt)) {
            update.hasGroundSpeed = true;
        }
        if (read_int(item, "track", update.trackDeg)) {
            update.hasTrack = true;
        }

        double lat = 0.0;
        double lon = 0.0;
        if (read_number(item, "lat", lat) && read_number(item, "lon", lon)) {
            update.latitude = lat;
            update.longitude = lon;
            update.hasPosition = true;
        }

        double distance = 0.0;
        if (read_number(item, "r_dst", distance)) {
            update.distanceNm = distance;
            update.hasDistance = true;
        }

        double bearing = 0.0;
        if (read_number(item, "r_dir", bearing)) {
            update.bearingDeg = static_cast<int>(std::lround(bearing));
            update.hasBearing = true;
        }

        output[count++] = update;
    }

    cJSON_Delete(root);
    return count;
}

}  // namespace adsb
