#pragma once

#include "aircraft.h"

#include <string>
#include <string_view>

namespace adsb {

class SbsParser {
 public:
    bool parseLine(std::string_view line, AircraftUpdate& update) const;

 private:
    static bool readField(std::string_view line, size_t targetIndex, std::string& value);
    static bool parseInteger(const std::string& value, int& output);
    static bool parseDoubleValue(const std::string& value, double& output);
    static void trim(std::string& value);
    static void uppercase(std::string& value);
};

}  // namespace adsb
