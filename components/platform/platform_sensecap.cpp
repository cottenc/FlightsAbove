#include "platform.h"

#include "bsp_board.h"
#include "bsp_lcd.h"
#include "indev_tp.h"
#include "sdkconfig.h"

namespace platform {

const Capabilities& capabilities()
{
    static const Capabilities caps = {
        Target::SensecapIndicator,
        "sensecap-indicator-d1pro",
        480,
        480,
        true,
        false,
#if CONFIG_LCD_LVGL_DIRECT_MODE
        true,
#else
        false,
#endif
        true,
        100,
    };
    return caps;
}

esp_err_t init()
{
    return bsp_board_init();
}

void setBacklight(bool enabled)
{
    bsp_lcd_set_backlight(enabled);
}

void setBacklightLevel(uint8_t percent)
{
    bsp_lcd_set_backlight(percent > 0);
}

void getFrameBuffer(void** fb1, void** fb2)
{
    bsp_lcd_get_frame_buffer(fb1, fb2);
}

esp_err_t flush(int x1, int y1, int x2, int y2, const void* data)
{
    return bsp_lcd_flush(x1, y1, x2, y2, data);
}

esp_err_t setFlushDoneCallback(bool (*callback)(void*), void* data)
{
    return bsp_lcd_set_cb(callback, data);
}

void registerDirectModeCopy(void (*callback)(void))
{
#if CONFIG_LCD_LVGL_DIRECT_MODE
    bsp_lcd_direct_mode_register(callback);
#else
    (void)callback;
#endif
}

void registerFlushIsLast(bool (*callback)(void))
{
#if CONFIG_LCD_LVGL_DIRECT_MODE
    bsp_lcd_flush_is_last_register(callback);
#else
    (void)callback;
#endif
}

esp_err_t resyncDisplay()
{
    return bsp_lcd_resync();
}

esp_err_t readTouch(uint8_t* pointCount, uint16_t* x, uint16_t* y, uint8_t* button)
{
    return indev_tp_read(pointCount, x, y, button);
}

}  // namespace platform
