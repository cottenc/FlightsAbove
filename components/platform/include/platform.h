#pragma once

#include "esp_err.h"

#include <stdint.h>

namespace platform {

enum class Target : uint8_t {
    SensecapIndicator,
    LilyGoTDisplayS3,
    Round360,
};

struct Capabilities {
    Target target;
    const char* id;
    uint16_t width;
    uint16_t height;
    bool touch;
    bool round;
    bool directMode;
    bool backlight;
    uint8_t defaultBacklightPercent;
};

const Capabilities& capabilities();
esp_err_t init();
void setBacklight(bool enabled);
void setBacklightLevel(uint8_t percent);
void getFrameBuffer(void** fb1, void** fb2);
esp_err_t flush(int x1, int y1, int x2, int y2, const void* data);
esp_err_t setFlushDoneCallback(bool (*callback)(void*), void* data);
void registerDirectModeCopy(void (*callback)(void));
void registerFlushIsLast(bool (*callback)(void));
esp_err_t resyncDisplay();
esp_err_t readTouch(uint8_t* pointCount, uint16_t* x, uint16_t* y, uint8_t* button);

}  // namespace platform
