#pragma once

#include <stdint.h>

namespace ui_layout {

enum class Shape : uint8_t {
    Rectangular,
    Round,
};

enum class Density : uint8_t {
    Full,
    Compact,
};

enum Content : uint32_t {
    Header        = 1U << 0,
    TrafficList   = 1U << 1,
    NearestCard   = 1U << 2,
    RadarSummary  = 1U << 3,
    TouchControls = 1U << 4,
};

struct Insets {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
};

struct Profile {
    const char* id;
    uint16_t width;
    uint16_t height;
    Shape shape;
    Density density;
    Insets safe;
    uint16_t headerHeight;
    uint16_t keyboardHeight;
    uint16_t roundSafeRadius;
    uint32_t content;

    constexpr uint16_t contentTop() const {
        return headerHeight + 1;
    }

    constexpr uint16_t contentHeight() const {
        return height - contentTop();
    }

    constexpr uint16_t safeWidth() const {
        return width - safe.left - safe.right;
    }

    constexpr uint16_t safeHeight() const {
        return height - safe.top - safe.bottom;
    }

    constexpr bool shows(Content item) const {
        return (content & static_cast<uint32_t>(item)) != 0;
    }
};

inline constexpr uint32_t kFullContent =
    Header | TrafficList | NearestCard | RadarSummary | TouchControls;

inline constexpr Profile kSensecapSquare = {
    "sensecap-square-480",
    480, 480,
    Shape::Rectangular,
    Density::Full,
    {0, 0, 0, 0},
    56,
    260,
    0,
    kFullContent,
};

inline constexpr Profile kCompactWide = {
    "compact-wide-536x240",
    536, 240,
    Shape::Rectangular,
    Density::Compact,
    {8, 4, 8, 4},
    36,
    0,
    0,
    Header | TrafficList | NearestCard,
};

// Generic profile until the exact linked round board/controller is confirmed.
// The inset square stays comfortably inside a 360 px circular display.
inline constexpr Profile kRound360 = {
    "round-360",
    360, 360,
    Shape::Round,
    Density::Compact,
    {52, 52, 52, 52},
    0,
    0,
    174,
    Header | NearestCard,
};

const Profile& select(const char* platformId, uint16_t width, uint16_t height, bool round);
bool contains(const Profile& profile, int16_t x, int16_t y);

}  // namespace ui_layout
