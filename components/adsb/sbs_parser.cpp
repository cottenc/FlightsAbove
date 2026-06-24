#include "sbs_parser.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <system_error>

namespace adsb {

bool SbsParser::parseLine(std::string_view line, AircraftUpdate& update) const {
    std::string messageType;
    if (!readField(line, 0, messageType) || messageType != "MSG") {
        return false;
    }

    std::string icao;
    if (!readField(line, 4, icao)) {
        return false;
    }
    trim(icao);
    uppercase(icao);
    if (icao.empty()) {
        return false;
    }

    update = AircraftUpdate{};
    update.icao = icao;

    std::string value;
    if (readField(line, 10, value)) {
        trim(value);
        if (!value.empty()) {
            update.callsign = value;
            update.hasCallsign = true;
        }
    }
    if (readField(line, 11, value) && parseInteger(value, update.altitudeFt)) {
        update.hasAltitude = true;
    }
    if (readField(line, 12, value) && parseInteger(value, update.groundSpeedKt)) {
        update.hasGroundSpeed = true;
    }
    if (readField(line, 13, value) && parseInteger(value, update.trackDeg)) {
        update.hasTrack = true;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (readField(line, 14, value) && parseDoubleValue(value, latitude) &&
        readField(line, 15, value) && parseDoubleValue(value, longitude)) {
        update.latitude = latitude;
        update.longitude = longitude;
        update.hasPosition = true;
    }

    return true;
}

bool SbsParser::readField(std::string_view line, size_t targetIndex, std::string& value) {
    size_t start = 0;
    size_t index = 0;

    while (start <= line.size()) {
        const size_t comma = line.find(',', start);
        const size_t end = comma == std::string_view::npos ? line.size() : comma;
        if (index == targetIndex) {
            value.assign(line.substr(start, end - start));
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
        ++index;
    }

    value.clear();
    return false;
}

bool SbsParser::parseInteger(const std::string& value, int& output) {
    std::string trimmed = value;
    trim(trimmed);
    if (trimmed.empty()) {
        return false;
    }

    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    auto [ptr, ec] = std::from_chars(begin, end, output);
    return ec == std::errc{} && ptr == end;
}

bool SbsParser::parseDoubleValue(const std::string& value, double& output) {
    std::string trimmed = value;
    trim(trimmed);
    if (trimmed.empty()) {
        return false;
    }

    char* end = nullptr;
    output = std::strtod(trimmed.c_str(), &end);
    return end != trimmed.c_str() && *end == '\0';
}

void SbsParser::trim(std::string& value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
}

void SbsParser::uppercase(std::string& value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
}

}  // namespace adsb
