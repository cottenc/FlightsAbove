/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified for SenseCAP Indicator D1Pro (FT6336U / FT5x06 touch only).
 */
#include <string.h>
#include "bsp_i2c.h"
#include "bsp_board.h"
#include "indev_tp.h"
#include "esp_log.h"
#include "esp_err.h"
#include "ft5x06.h"

static const char *TAG = "indev_tp";

static int tp_dev_id = -1;

esp_err_t indev_tp_init(void)
{
    // D1Pro always has the GX screen (FT6336U at 0x48) — skip I2C probe
    tp_dev_id = 0;
    esp_err_t ret_val = ft5x06_init();
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "ft5x06_init failed: %d", ret_val);
    }
    return ret_val;
}

esp_err_t indev_tp_get_dev(char **dev_name, uint8_t *dev_addr)
{
    if (tp_dev_id >= 0) {
        *dev_name = "FT6336U";
#if CONFIG_SENSECAP_INDICATOR_SCREEN_GX
        *dev_addr = 0x48;
#else
        *dev_addr = 0x38;
#endif
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t indev_tp_read(uint8_t *tp_num, uint16_t *x, uint16_t *y, uint8_t *btn_val)
{
    if (tp_dev_id < 0) return ESP_FAIL;

    *btn_val = 0;
    esp_err_t ret_val = ft5x06_read_pos(tp_num, x, y);

    const board_res_desc_t *brd = bsp_board_get_description();
    if (brd->TOUCH_PANEL_SWAP_XY) {
        uint16_t swap = *x;
        *x = *y;
        *y = swap;
    }
    if (brd->TOUCH_PANEL_INVERSE_X) *x = brd->LCD_WIDTH  - (*x + 1);
    if (brd->TOUCH_PANEL_INVERSE_Y) *y = brd->LCD_HEIGHT - (*y + 1);

    ESP_LOGV(TAG, "[%3u, %3u]", *x, *y);
    return ret_val;
}
