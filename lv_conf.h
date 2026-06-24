/**
 * lv_conf.h — LVGL 8.x configuration for the SenseCAP Indicator D1Pro
 * Place this file at the project root so LVGL's CMakeLists finds it via
 * -DLV_CONF_PATH=<project_root>/lv_conf.h
 */

#if 1  /* Set to "1" to enable */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Colour depth — 16-bit RGB565 matches the SenseCAP display */
#define LV_COLOR_DEPTH 16

/* Memory — 128 kB LVGL heap (drawn from PSRAM via CONFIG_SPIRAM_USE_MALLOC) */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (128U * 1024U)

/* Tick source — provided by lv_tick_inc() called from the UI task */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000))

/* Display */
#define LV_HOR_RES_MAX 480
#define LV_VER_RES_MAX 480

/* Fonts — enable what the UI uses */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Feature flags */
#define LV_USE_LABEL   1
#define LV_USE_BTN     1
#define LV_USE_ARC     1
#define LV_USE_SLIDER  1
#define LV_USE_SWITCH  1
#define LV_USE_SPINNER 1
#define LV_USE_MSGBOX  1
#define LV_USE_TABVIEW 1
#define LV_USE_KEYBOARD 1

/* Logging */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Remaining options left at LVGL defaults */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0

#define LV_SPRINTF_CUSTOM 0

#define LV_USE_USER_DATA 1
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

#define LV_COLOR_16_SWAP 0

#endif /* LV_CONF_H */
#endif /* Enable/Disable */
