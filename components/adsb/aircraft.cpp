#include "aircraft.h"

#include <algorithm>
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

}  // namespace adsb
