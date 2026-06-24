#pragma once

#include "aircraft.h"

#include <stddef.h>

namespace adsb {

class ReadsbJsonParser {
 public:
    size_t parseAircraftJson(const char* json, AircraftUpdate* output, size_t capacity) const;
};

}  // namespace adsb
