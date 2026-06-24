#include "ui_layout.h"

#include <string.h>

namespace ui_layout {

const Profile& select(const char* platformId, uint16_t width, uint16_t height, bool round) {
    if (platformId && strcmp(platformId, "sensecap-indicator-d1pro") == 0) {
        return kSensecapSquare;
    }
    if (platformId && strcmp(platformId, "lilygo-t-display-s3") == 0) {
        return kCompactWide;
    }
    if (platformId && strcmp(platformId, "round-360") == 0) {
        return kRound360;
    }

    if (round) {
        return kRound360;
    }
    if (width <= 536 && height <= 260) {
        return kCompactWide;
    }
    return kSensecapSquare;
}

bool contains(const Profile& profile, int16_t x, int16_t y) {
    if (x < 0 || y < 0 || x >= profile.width || y >= profile.height) {
        return false;
    }
    if (profile.shape != Shape::Round) {
        return true;
    }

    const int32_t centerX = profile.width / 2;
    const int32_t centerY = profile.height / 2;
    const int32_t dx = x - centerX;
    const int32_t dy = y - centerY;
    const int32_t radius = profile.roundSafeRadius;
    return dx * dx + dy * dy <= radius * radius;
}

}  // namespace ui_layout
