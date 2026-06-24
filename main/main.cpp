#include "aircraft_store.h"
#include "flights_config.h"
#include "platform.h"
#include "sbs_parser.h"
#include "ui_layout.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "rom/cache.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <string.h>

using adsb::Aircraft;
using adsb::AircraftStore;
using adsb::AircraftUpdate;
using adsb::SbsParser;

namespace {

constexpr size_t kVisibleAircraft = 8;
const char* TAG = "flightsabove";

AircraftStore g_aircraftStore(cfg::kReceiverLatitude, cfg::kReceiverLongitude, cfg::kAircraftStaleMs);
SbsParser g_parser;
SemaphoreHandle_t g_aircraftMutex = nullptr;

lv_obj_t* g_countLabel = nullptr;
lv_obj_t* g_statusLabel = nullptr;
lv_obj_t* g_nearestCallsign = nullptr;
lv_obj_t* g_nearestMeta = nullptr;
lv_obj_t* g_listLabels[kVisibleAircraft] = {};

static lv_disp_drv_t* g_dispDrv = nullptr;

int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

bool flush_done_cb(void*) {
    if (g_dispDrv) {
        lv_disp_flush_ready(g_dispDrv);
    }
    return false;
}

void display_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px_map) {
    g_dispDrv = drv;
    platform::flush(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

#if CONFIG_LCD_LVGL_DIRECT_MODE
bool display_flush_is_last() {
    return g_dispDrv && lv_disp_flush_is_last(g_dispDrv);
}

void display_direct_mode_copy() {
    lv_disp_t* disp = _lv_refr_get_disp_refreshing();
    auto* from = reinterpret_cast<uint8_t*>(disp->driver->draw_buf->buf_act);
    auto* buf1 = reinterpret_cast<uint8_t*>(disp->driver->draw_buf->buf1);
    auto* buf2 = reinterpret_cast<uint8_t*>(disp->driver->draw_buf->buf2);
    auto* to = from == buf1 ? buf2 : buf1;
    const uint32_t bytesPerLine = disp->driver->hor_res * sizeof(lv_color_t);

    for (int32_t i = 0; i < disp->inv_p; ++i) {
        if (disp->inv_area_joined[i]) {
            continue;
        }
        const lv_area_t& area = disp->inv_areas[i];
        const uint32_t copyBytes = lv_area_get_width(&area) * sizeof(lv_color_t);
        uint8_t* src = from + (area.y1 * disp->driver->hor_res + area.x1) * sizeof(lv_color_t);
        uint8_t* dst = to + (area.y1 * disp->driver->hor_res + area.x1) * sizeof(lv_color_t);
        uint8_t* flushStart = dst;
        for (lv_coord_t y = area.y1; y <= area.y2; ++y) {
            memcpy(dst, src, copyBytes);
            src += bytesPerLine;
            dst += bytesPerLine;
        }
        const uint32_t flushBytes = (area.y2 - area.y1) * bytesPerLine + copyBytes;
        Cache_WriteBack_Addr((uint32_t)(uintptr_t)flushStart, flushBytes);
    }
}
#endif

void touch_read(lv_indev_drv_t*, lv_indev_data_t* data) {
    static uint16_t lastX = 0;
    static uint16_t lastY = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t pointCount = 0;
    uint8_t button = 0;

    const auto& caps = platform::capabilities();
    esp_err_t err = platform::readTouch(&pointCount, &x, &y, &button);
    if (err == ESP_OK && pointCount > 0 && x < caps.width && y < caps.height) {
        lastX = x;
        lastY = y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    data->point.x = static_cast<lv_coord_t>(lastX);
    data->point.y = static_cast<lv_coord_t>(lastY);
}

void style_screen(lv_obj_t* obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(cfg::kColorText), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    return label;
}

void build_ui() {
    lv_obj_t* scr = lv_scr_act();
    style_screen(scr, cfg::kColorBackground);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_set_size(header, cfg::kDisplayWidth, 66);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    style_screen(header, cfg::kColorPanel);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 16, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_label(header, &lv_font_montserrat_28, cfg::kColorText);
    lv_label_set_text(title, "FlightsAbove");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    g_countLabel = make_label(header, &lv_font_montserrat_20, cfg::kColorGreen);
    lv_label_set_text(g_countLabel, "0 aircraft");
    lv_obj_align(g_countLabel, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* radar = lv_obj_create(scr);
    lv_obj_set_size(radar, 184, 184);
    lv_obj_align(radar, LV_ALIGN_TOP_LEFT, 24, 94);
    style_screen(radar, cfg::kColorPanelAlt);
    lv_obj_set_style_radius(radar, 92, 0);
    lv_obj_set_style_border_width(radar, 2, 0);
    lv_obj_set_style_border_color(radar, lv_color_hex(cfg::kColorGreen), 0);
    lv_obj_clear_flag(radar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t* ring = lv_obj_create(radar);
        const int size = 54 + i * 44;
        lv_obj_set_size(ring, size, size);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, size / 2, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x34554A), 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t* center = lv_obj_create(radar);
    lv_obj_set_size(center, 12, 12);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, 6, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(cfg::kColorAmber), 0);
    lv_obj_set_style_border_width(center, 0, 0);

    lv_obj_t* nearest = lv_obj_create(scr);
    lv_obj_set_size(nearest, 232, 184);
    lv_obj_align(nearest, LV_ALIGN_TOP_RIGHT, -24, 94);
    style_screen(nearest, cfg::kColorPanel);
    lv_obj_set_style_radius(nearest, 14, 0);
    lv_obj_set_style_pad_all(nearest, 18, 0);
    lv_obj_clear_flag(nearest, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* nearestTitle = make_label(nearest, &lv_font_montserrat_14, cfg::kColorMuted);
    lv_label_set_text(nearestTitle, "NEAREST AIRCRAFT");
    lv_obj_align(nearestTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    g_nearestCallsign = make_label(nearest, &lv_font_montserrat_48, cfg::kColorText);
    lv_label_set_text(g_nearestCallsign, "--");
    lv_obj_align(g_nearestCallsign, LV_ALIGN_TOP_LEFT, 0, 38);

    g_nearestMeta = make_label(nearest, &lv_font_montserrat_20, cfg::kColorCyan);
    lv_label_set_text(g_nearestMeta, "Waiting for SBS data");
    lv_obj_align(g_nearestMeta, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_set_size(list, 432, 154);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -24);
    style_screen(list, cfg::kColorPanel);
    lv_obj_set_style_radius(list, 14, 0);
    lv_obj_set_style_pad_all(list, 14, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < kVisibleAircraft; ++i) {
        g_listLabels[i] = make_label(list, &lv_font_montserrat_14, cfg::kColorText);
        lv_obj_set_width(g_listLabels[i], 400);
        lv_label_set_text(g_listLabels[i], "");
        lv_obj_align(g_listLabels[i], LV_ALIGN_TOP_LEFT, 0, static_cast<int>(i) * 17);
    }

    g_statusLabel = make_label(scr, &lv_font_montserrat_14, cfg::kColorMuted);
    lv_label_set_text(g_statusLabel, "UART1 RX GPIO17 | SBS/BaseStation");
    lv_obj_align(g_statusLabel, LV_ALIGN_BOTTOM_MID, 0, -4);
}

std::string label_for_aircraft(const Aircraft& aircraft) {
    if (!aircraft.callsign.empty()) {
        return aircraft.callsign;
    }
    return aircraft.icao;
}

void refresh_ui(lv_timer_t*) {
    Aircraft aircraft[kVisibleAircraft];
    size_t count = 0;
    size_t activeCount = 0;
    const int64_t now = now_ms();

    if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_aircraftStore.purgeStale(now);
        count = g_aircraftStore.snapshot(aircraft, kVisibleAircraft, now);
        activeCount = g_aircraftStore.activeCount(now);
        xSemaphoreGive(g_aircraftMutex);
    }

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%u aircraft", static_cast<unsigned>(activeCount));
    lv_label_set_text(g_countLabel, buffer);

    if (count == 0) {
        lv_label_set_text(g_nearestCallsign, "--");
        lv_label_set_text(g_nearestMeta, "Waiting for SBS data");
        for (lv_obj_t* label : g_listLabels) {
            lv_label_set_text(label, "");
        }
        return;
    }

    const Aircraft& nearest = aircraft[0];
    lv_label_set_text(g_nearestCallsign, label_for_aircraft(nearest).c_str());
    snprintf(buffer, sizeof(buffer), "%.1f nm  %s ft",
             nearest.hasPosition ? nearest.distanceNm : 0.0,
             nearest.hasAltitude ? std::to_string(nearest.altitudeFt).c_str() : "--");
    lv_label_set_text(g_nearestMeta, buffer);

    for (size_t i = 0; i < kVisibleAircraft; ++i) {
        if (i >= count) {
            lv_label_set_text(g_listLabels[i], "");
            continue;
        }
        const Aircraft& item = aircraft[i];
        const std::string label = label_for_aircraft(item);
        snprintf(buffer, sizeof(buffer), "%-8s %5s ft %5s kt %5.1f nm",
                 label.c_str(),
                 item.hasAltitude ? std::to_string(item.altitudeFt).c_str() : "--",
                 item.hasGroundSpeed ? std::to_string(item.groundSpeedKt).c_str() : "--",
                 item.hasPosition ? item.distanceNm : 0.0);
        lv_label_set_text(g_listLabels[i], buffer);
    }
}

void init_lvgl() {
    ESP_ERROR_CHECK(platform::init());
    const auto& caps = platform::capabilities();
    const auto& profile = ui_layout::select(caps.id, caps.width, caps.height, caps.round);
    ESP_LOGI(TAG, "display profile %s on %s (%ux%u)", profile.id, caps.id,
             static_cast<unsigned>(caps.width), static_cast<unsigned>(caps.height));

    platform::setBacklight(false);
    lv_init();

    void* fb1Raw = nullptr;
    void* fb2Raw = nullptr;
    platform::getFrameBuffer(&fb1Raw, &fb2Raw);

    static lv_disp_draw_buf_t drawBuf;
    lv_disp_draw_buf_init(&drawBuf, static_cast<lv_color_t*>(fb1Raw),
                          static_cast<lv_color_t*>(fb2Raw),
                          cfg::kDisplayWidth * cfg::kDisplayHeight);

    static lv_disp_drv_t dispDrv;
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = cfg::kDisplayWidth;
    dispDrv.ver_res = cfg::kDisplayHeight;
    dispDrv.flush_cb = display_flush;
    dispDrv.draw_buf = &drawBuf;
#if CONFIG_LCD_LVGL_FULL_REFRESH
    dispDrv.full_refresh = 1;
#elif CONFIG_LCD_LVGL_DIRECT_MODE
    dispDrv.direct_mode = 1;
#endif
    lv_disp_drv_register(&dispDrv);

    ESP_ERROR_CHECK(platform::setFlushDoneCallback(flush_done_cb, nullptr));
#if CONFIG_LCD_LVGL_DIRECT_MODE
    platform::registerFlushIsLast(display_flush_is_last);
    platform::registerDirectModeCopy(display_direct_mode_copy);
#endif

    static const uint32_t tickPeriodMs = 2;
    esp_timer_create_args_t tickTimerArgs = {};
    tickTimerArgs.callback = [](void* arg) { lv_tick_inc(*static_cast<uint32_t*>(arg)); };
    tickTimerArgs.arg = (void*)&tickPeriodMs;
    tickTimerArgs.dispatch_method = ESP_TIMER_TASK;
    tickTimerArgs.name = "lvgl_tick";
    tickTimerArgs.skip_unhandled_events = true;
    esp_timer_handle_t tickTimer;
    ESP_ERROR_CHECK(esp_timer_create(&tickTimerArgs, &tickTimer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tickTimer, tickPeriodMs * 1000));

    if (caps.touch) {
        static lv_indev_drv_t indevDrv;
        lv_indev_drv_init(&indevDrv);
        indevDrv.type = LV_INDEV_TYPE_POINTER;
        indevDrv.read_cb = touch_read;
        lv_indev_t* touchIndev = lv_indev_drv_register(&indevDrv);
        lv_timer_set_period(touchIndev->driver->read_timer, 4);
    }

    build_ui();
    lv_timer_create(refresh_ui, cfg::kUiRefreshMs, nullptr);
    refresh_ui(nullptr);
    lv_obj_invalidate(lv_scr_act());
    platform::resyncDisplay();
    platform::setBacklight(true);
}

void adsb_task(void*) {
    ESP_LOGI(TAG, "ADS-B UART%d RX GPIO%d at %d baud", cfg::kAdsbUart,
             cfg::kAdsbRxPin, cfg::kAdsbBaudRate);

    std::string line;
    line.reserve(cfg::kAdsbLineMax);
    uint8_t byte = 0;

    for (;;) {
        const int read = uart_read_bytes(cfg::kAdsbUart, &byte, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }

        const char c = static_cast<char>(byte);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            AircraftUpdate update;
            if (g_parser.parseLine(line, update)) {
                if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_aircraftStore.applyUpdate(update, now_ms());
                    xSemaphoreGive(g_aircraftMutex);
                }
            }
            line.clear();
            continue;
        }

        if (line.size() < cfg::kAdsbLineMax) {
            line.push_back(c);
        } else {
            line.clear();
        }
    }
}

void init_adsb_uart() {
    uart_config_t uartConfig = {};
    uartConfig.baud_rate = cfg::kAdsbBaudRate;
    uartConfig.data_bits = UART_DATA_8_BITS;
    uartConfig.parity = UART_PARITY_DISABLE;
    uartConfig.stop_bits = UART_STOP_BITS_1;
    uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartConfig.rx_flow_ctrl_thresh = 0;
    uartConfig.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(cfg::kAdsbUart, cfg::kAdsbRxBufferBytes, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg::kAdsbUart, &uartConfig));
    ESP_ERROR_CHECK(uart_set_pin(cfg::kAdsbUart, cfg::kAdsbTxPin, cfg::kAdsbRxPin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

}  // namespace

extern "C" void app_main() {
    esp_err_t nvsResult = nvs_flash_init();
    if (nvsResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsResult = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsResult);

    g_aircraftMutex = xSemaphoreCreateMutex();
    configASSERT(g_aircraftMutex != nullptr);

    init_adsb_uart();
    xTaskCreatePinnedToCore(adsb_task, "adsb_uart", 4096, nullptr, 5, nullptr, 0);

    init_lvgl();
    for (;;) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
