/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "bsp_board.h"
#include "indev.h"
#include "indev_tp.h"
#include "esp_err.h"

static indev_type_t g_major_dev = BSP_INDEV_NONE;

static esp_err_t indev_init(indev_type_t indev)
{
    switch (indev) {
    case BSP_INDEV_TP:
        return indev_tp_init();
    case BSP_INDEV_BTN:
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t indev_init_default(void)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    const board_res_desc_t *brd = bsp_board_get_description();

    if (brd->BSP_INDEV_IS_TP) {
        ret = indev_init(BSP_INDEV_TP);
        if (ESP_OK == ret) {
            g_major_dev = BSP_INDEV_TP;
        }
    } else {
        ret = indev_init(BSP_INDEV_BTN);
        if (ESP_OK == ret) {
            g_major_dev = BSP_INDEV_BTN;
        }
    }

    return ret;
}

void indev_reset(void)
{
#define EXPANDER_IO_TP_RESET    7
    const board_res_desc_t *brd = bsp_board_get_description();
    brd->io_expander_ops->set_direction(EXPANDER_IO_TP_RESET, 1);
    brd->io_expander_ops->set_level(EXPANDER_IO_TP_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    brd->io_expander_ops->set_level(EXPANDER_IO_TP_RESET, 1);
}

esp_err_t indev_get_major_value(indev_data_t *data)
{
    uint16_t x, y;
    uint8_t tp_num, btn_val;

    static uint32_t err_cnt = 0;

    switch (g_major_dev) {
    case BSP_INDEV_TP:
        if (ESP_OK == indev_tp_read(&tp_num, &x, &y, &btn_val)) {
            data->x = x;
            data->y = y;
            data->btn_val = btn_val;
            data->pressed = (tp_num > 0);
            err_cnt = 0;
        } else {
            err_cnt++;
            if (err_cnt > 100) {
                err_cnt = 0;
                ESP_LOGE("indev", "indev_tp_read err, reset...");
                indev_reset();
            }
        }
        break;
    default:
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}
