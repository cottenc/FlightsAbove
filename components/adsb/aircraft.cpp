#include "aircraft.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace adsb {

std::string trimCopy(const std::string& value) {
    std::string output = value;
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    output.erase(output.begin(), std::find_if_not(output.begin(), output.end(), isSpace));
    output.erase(std::find_if_not(output.rbegin(), output.rend(), isSpace).base(), output.end());
    return output;
}

void uppercaseInPlace(std::string& value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
}

std::string normalizeCallsign(const std::string& callsign) {
    std::string normalized = trimCopy(callsign);
    uppercaseInPlace(normalized);

    size_t alphaEnd = 0;
    while (alphaEnd < normalized.size() &&
           normalized[alphaEnd] >= 'A' && normalized[alphaEnd] <= 'Z') {
        ++alphaEnd;
    }

    size_t digitEnd = alphaEnd;
    while (digitEnd < normalized.size() &&
           normalized[digitEnd] >= '0' && normalized[digitEnd] <= '9') {
        ++digitEnd;
    }

    if (alphaEnd == 0 || digitEnd == alphaEnd) {
        return normalized;
    }

    size_t firstDigit = alphaEnd;
    while (firstDigit + 1 < digitEnd && normalized[firstDigit] == '0') {
        ++firstDigit;
    }

    return normalized.substr(0, alphaEnd) +
           normalized.substr(firstDigit, digitEnd - firstDigit) +
           normalized.substr(digitEnd);
}

std::string airlineIcaoFromCallsign(const std::string& callsign) {
    std::string normalized = trimCopy(callsign);
    uppercaseInPlace(normalized);

    if (normalized.size() < 4) {
        return "";
    }
    for (size_t i = 0; i < 3; ++i) {
        if (normalized[i] < 'A' || normalized[i] > 'Z') {
            return "";
        }
    }
    if (normalized[3] < '0' || normalized[3] > '9') {
        return "";
    }
    return normalized.substr(0, 3);
}

std::string iataTypeFromIcaoType(const std::string& typeCode) {
    struct TypeMap {
        const char* icao;
        const char* iata;
    };
    static constexpr std::array<TypeMap, 58> kTypes = {{
        {"A318", "318"}, {"A319", "319"}, {"A320", "320"}, {"A321", "321"},
        {"A20N", "32N"}, {"A21N", "32Q"}, {"A306", "AB6"}, {"A310", "310"},
        {"A332", "332"}, {"A333", "333"}, {"A338", "338"}, {"A339", "339"},
        {"A342", "342"}, {"A343", "343"}, {"A345", "345"}, {"A346", "346"},
        {"A359", "359"}, {"A35K", "351"}, {"A388", "388"},
        {"B712", "717"}, {"B722", "722"}, {"B732", "732"}, {"B733", "733"},
        {"B734", "734"}, {"B735", "735"}, {"B736", "736"}, {"B737", "737"},
        {"B738", "738"}, {"B739", "739"}, {"B37M", "7M7"}, {"B38M", "7M8"},
        {"B39M", "7M9"}, {"B3XM", "7MJ"}, {"B752", "752"}, {"B753", "753"},
        {"B762", "762"}, {"B763", "763"}, {"B764", "764"}, {"B772", "772"},
        {"B77L", "77L"}, {"B77W", "77W"}, {"B788", "788"}, {"B789", "789"},
        {"B78X", "781"}, {"B744", "744"}, {"B748", "748"},
        {"C25A", "CNJ"}, {"C25B", "CNJ"}, {"C25C", "CNJ"}, {"C680", "CNJ"},
        {"CRJ2", "CR2"}, {"CRJ7", "CR7"}, {"CRJ9", "CR9"}, {"E170", "E70"},
        {"E190", "E90"}, {"E75L", "E75"}, {"E75S", "E75"}, {"DH8D", "DH4"},
    }};

    std::string normalized = trimCopy(typeCode);
    uppercaseInPlace(normalized);
    for (const TypeMap& item : kTypes) {
        if (normalized == item.icao) {
            return item.iata;
        }
    }
    if (normalized.size() == 3) {
        bool alphaNumeric = true;
        for (char ch : normalized) {
            alphaNumeric = alphaNumeric &&
                           ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
        }
        if (alphaNumeric) {
            return normalized;
        }
    }
    return "";
}

}  // namespace adsb
