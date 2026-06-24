#include "aircraft_store.h"
#include "basemap_default.h"
#include "device_network.h"
#include "flights_config.h"
#include "platform.h"
#include "readsb_json_parser.h"
#include "route_cache.h"
#include "sbs_parser.h"
#include "storage.h"
#include "ui_layout.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "rom/cache.h"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string>
#include <string.h>

using adsb::Aircraft;
using adsb::AircraftStore;
using adsb::AircraftUpdate;
using adsb::ReadsbJsonParser;
using adsb::RouteCache;
using adsb::RouteRequest;
using adsb::RouteResult;
using adsb::SbsParser;

namespace {

constexpr size_t kVisibleAircraft = 8;
constexpr size_t kPlaneIconPointCapacity = 16;
constexpr int kRadarWidth = 432;
constexpr int kRadarHeight = 318;
constexpr int kRadarRadius = 146;
constexpr size_t kHttpAircraftCapacity = 32;
constexpr size_t kRouteBatchCapacity = 4;
constexpr int kHttpTimeoutMs = 4000;
constexpr size_t kAircraftJsonMaxBytes = 32768;
constexpr size_t kRouteJsonMaxBytes = 4096;
constexpr int64_t kRouteLookupMinIntervalMs = 3500;
constexpr double kMilesPerNm = 1.150779448;
constexpr double kNmPerMile = 0.868976242;
constexpr const char* kRouteApiUrl = "http://adsb.im/api/0/routeset";
const char* TAG = "flightsabove";

AircraftStore g_aircraftStore(cfg::kReceiverLatitude, cfg::kReceiverLongitude, cfg::kAircraftStaleMs);
SbsParser g_parser;
ReadsbJsonParser g_readsbParser;
RouteCache g_routeCache;
SemaphoreHandle_t g_aircraftMutex = nullptr;
SemaphoreHandle_t g_routeCacheMutex = nullptr;

lv_obj_t* g_countLabel = nullptr;
lv_obj_t* g_statusLabel = nullptr;
lv_obj_t* g_nearestCallsign = nullptr;
lv_obj_t* g_nearestMeta = nullptr;
lv_obj_t* g_nearestType = nullptr;
lv_obj_t* g_rangeLabel = nullptr;
lv_obj_t* g_outerRangeLabel = nullptr;
lv_obj_t* g_radar = nullptr;
lv_obj_t* g_basemap = nullptr;
lv_obj_t* g_planeMarkers[kVisibleAircraft] = {};
lv_obj_t* g_trackLines[kVisibleAircraft] = {};
lv_point_t g_trackPoints[kVisibleAircraft][Aircraft::kTraceLength] = {};
lv_point_t g_planeIconPoints[kVisibleAircraft][kPlaneIconPointCapacity] = {};
Aircraft g_visibleAircraft[kVisibleAircraft] = {};
Aircraft g_routeVisibleAircraft[kVisibleAircraft] = {};
AircraftUpdate g_httpUpdates[kHttpAircraftCapacity] = {};
RouteRequest g_routeRequests[kRouteBatchCapacity] = {};
RouteResult g_routeResults[kRouteBatchCapacity] = {};
int64_t g_lastActivityMs = 0;
bool g_displaySleeping = false;

static lv_disp_drv_t* g_dispDrv = nullptr;

struct HttpResponse {
    std::string body;
    size_t maxBytes = 0;
};

int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == nullptr || evt->data == nullptr) {
        return ESP_OK;
    }

    auto* response = static_cast<HttpResponse*>(evt->user_data);
    if (response->body.size() + static_cast<size_t>(evt->data_len) > response->maxBytes) {
        return ESP_FAIL;
    }
    response->body.append(static_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

esp_err_t http_request(const char* url, const char* postBody, const char* contentType,
                       size_t maxBytes, std::string& responseBody, int* statusCode = nullptr) {
    HttpResponse response;
    response.maxBytes = maxBytes;
    response.body.reserve(std::min<size_t>(maxBytes, 4096));

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.max_redirection_count = 2;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (postBody != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_method(client, HTTP_METHOD_POST));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", contentType));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, postBody, strlen(postBody)));
    }

    const esp_err_t err = esp_http_client_perform(client);
    if (statusCode != nullptr) {
        *statusCode = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        responseBody.swap(response.body);
    }
    return err;
}

esp_err_t http_get(const char* url, size_t maxBytes, std::string& responseBody, int* statusCode = nullptr) {
    return http_request(url, nullptr, nullptr, maxBytes, responseBody, statusCode);
}

esp_err_t http_post_json(const char* url, const std::string& body, size_t maxBytes,
                         std::string& responseBody, int* statusCode = nullptr) {
    return http_request(url, body.c_str(), "application/json", maxBytes, responseBody, statusCode);
}

void mark_activity() {
    g_lastActivityMs = now_ms();
    if (g_displaySleeping) {
        platform::setBacklight(true);
        g_displaySleeping = false;
    }
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
        mark_activity();
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

uint32_t color_for_aircraft(const Aircraft& aircraft) {
    if (aircraft.category == "A7" || aircraft.typeCode.rfind("H", 0) == 0 ||
        aircraft.typeDescription.find("HELICOPTER") != std::string::npos) {
        return cfg::kColorAmber;
    }
    if (aircraft.category == "A1" || aircraft.category == "A2") {
        return cfg::kColorGreen;
    }
    return cfg::kColorCyan;
}

std::string type_for_aircraft(const Aircraft& aircraft) {
    if (!aircraft.typeCode.empty()) {
        return aircraft.typeCode;
    }
    if (!aircraft.category.empty()) {
        return aircraft.category;
    }
    return "--";
}

double miles_to_nm(uint16_t miles) {
    return static_cast<double>(miles) * kNmPerMile;
}

double nm_to_miles(double nm) {
    return nm * kMilesPerNm;
}

double autoscaled_range_miles(const Aircraft* aircraft, size_t count, uint16_t maxRangeMiles) {
    double farthestMiles = 0.0;
    const double maxRangeNm = miles_to_nm(maxRangeMiles);
    for (size_t i = 0; i < count; ++i) {
        if (!(aircraft[i].hasDistance || aircraft[i].hasPosition) ||
            aircraft[i].distanceNm > maxRangeNm) {
            continue;
        }
        farthestMiles = std::max(farthestMiles, nm_to_miles(aircraft[i].distanceNm));
    }

    static constexpr double kScaleSteps[] = {5.0, 10.0, 25.0, 50.0, 100.0, 200.0, 500.0};
    for (double step : kScaleSteps) {
        if (step >= farthestMiles && step <= maxRangeMiles) {
            return step;
        }
    }
    return static_cast<double>(maxRangeMiles);
}

void format_miles(char* buffer, size_t bufferSize, double miles) {
    snprintf(buffer, bufferSize, "%.0f", miles);
}

void update_outer_range_label(double rangeMiles) {
    char buffer[8];
    format_miles(buffer, sizeof(buffer), rangeMiles);
    lv_label_set_text(g_outerRangeLabel, buffer);
}

void update_basemap_zoom(double rangeMiles) {
    if (g_basemap == nullptr || rangeMiles <= 0.0) {
        return;
    }

    const double maxRangeMiles = static_cast<double>(settings.getRadarRangeMiles());
    const double zoom = 256.0 * maxRangeMiles / rangeMiles;
    const uint16_t lvZoom = static_cast<uint16_t>(
        std::max(128.0, std::min(4096.0, std::round(zoom))));
    lv_img_set_zoom(g_basemap, lvZoom);
}

lv_point_t radar_point(double distanceNm, int bearingDeg, double rangeNm) {
    constexpr double kDegToRad = 0.017453292519943295;
    const double clamped = std::min(distanceNm, rangeNm);
    const double radius = (clamped / rangeNm) * kRadarRadius;
    const double angle = bearingDeg * kDegToRad;
    lv_point_t point = {};
    point.x = static_cast<lv_coord_t>(kRadarWidth / 2 + std::sin(angle) * radius);
    point.y = static_cast<lv_coord_t>(kRadarHeight / 2 - std::cos(angle) * radius);
    return point;
}

enum class AircraftIcon {
    Airliner,
    Heavy,
    Jet,
    Small,
    Glider,
    Helicopter,
};

AircraftIcon icon_for_aircraft(const Aircraft& aircraft) {
    const std::string& type = aircraft.typeCode;
    const std::string& desc = aircraft.typeDescription;
    if (aircraft.category == "A7" || type.rfind("H", 0) == 0 || type.rfind("EC", 0) == 0 ||
        desc.find("HELICOPTER") != std::string::npos) {
        return AircraftIcon::Helicopter;
    }
    if (desc.find("GLIDER") != std::string::npos || type == "GLID") {
        return AircraftIcon::Glider;
    }
    if (aircraft.category == "A5" || aircraft.category == "A6" ||
        type.rfind("B7", 0) == 0 || type.rfind("B8", 0) == 0 || type.rfind("A3", 0) == 0) {
        return AircraftIcon::Heavy;
    }
    if (aircraft.category == "A1" || aircraft.category == "A2" ||
        type.rfind("C", 0) == 0 || type.rfind("P", 0) == 0) {
        return AircraftIcon::Small;
    }
    if (type.rfind("F", 0) == 0 || desc.find("FIGHTER") != std::string::npos) {
        return AircraftIcon::Jet;
    }
    return AircraftIcon::Airliner;
}

const lv_point_t* icon_template(AircraftIcon icon, size_t& count) {
    // Compact LVGL line templates adapted from tar1090 marker families.
    static constexpr lv_point_t kAirliner[] = {
        {0, -13}, {-2, -5}, {-14, 2}, {-14, 4}, {-3, 2}, {-2, 9},
        {-8, 13}, {-8, 15}, {0, 12}, {8, 15}, {8, 13}, {2, 9},
        {3, 2}, {14, 4}, {14, 2}, {2, -5},
    };
    static constexpr lv_point_t kHeavy[] = {
        {0, -15}, {-3, -5}, {-16, 4}, {-15, 7}, {-4, 4}, {-3, 10},
        {-11, 15}, {-10, 17}, {0, 13}, {10, 17}, {11, 15}, {3, 10},
        {4, 4}, {15, 7}, {16, 4}, {3, -5},
    };
    static constexpr lv_point_t kJet[] = {
        {0, -14}, {-3, -3}, {-14, 5}, {-5, 5}, {-4, 10}, {-8, 15},
        {-2, 13}, {0, 10}, {2, 13}, {8, 15}, {4, 10}, {5, 5},
        {14, 5}, {3, -3}, {0, -14},
    };
    static constexpr lv_point_t kSmall[] = {
        {0, -12}, {-2, -2}, {-13, 1}, {-13, 4}, {-3, 4}, {-2, 10},
        {-6, 13}, {0, 11}, {6, 13}, {2, 10}, {3, 4}, {13, 4},
        {13, 1}, {2, -2}, {0, -12},
    };
    static constexpr lv_point_t kGlider[] = {
        {0, -11}, {-1, -2}, {-18, 0}, {-18, 2}, {-1, 3}, {-1, 11},
        {-6, 14}, {0, 12}, {6, 14}, {1, 11}, {1, 3}, {18, 2},
        {18, 0}, {1, -2}, {0, -11},
    };
    static constexpr lv_point_t kHelicopter[] = {
        {-15, -3}, {15, 3}, {3, 3}, {3, 12}, {-3, 12}, {-3, 3},
        {-15, 3}, {15, -3}, {5, -3}, {3, -10}, {-3, -10}, {-5, -3},
        {-15, -3},
    };

    switch (icon) {
    case AircraftIcon::Heavy:
        count = sizeof(kHeavy) / sizeof(kHeavy[0]);
        return kHeavy;
    case AircraftIcon::Jet:
        count = sizeof(kJet) / sizeof(kJet[0]);
        return kJet;
    case AircraftIcon::Small:
        count = sizeof(kSmall) / sizeof(kSmall[0]);
        return kSmall;
    case AircraftIcon::Glider:
        count = sizeof(kGlider) / sizeof(kGlider[0]);
        return kGlider;
    case AircraftIcon::Helicopter:
        count = sizeof(kHelicopter) / sizeof(kHelicopter[0]);
        return kHelicopter;
    case AircraftIcon::Airliner:
    default:
        count = sizeof(kAirliner) / sizeof(kAirliner[0]);
        return kAirliner;
    }
}

size_t plane_icon_points(const Aircraft& aircraft, const lv_point_t& center, int headingDeg,
                         lv_point_t* points, size_t capacity) {
    size_t templateCount = 0;
    const lv_point_t* shape = icon_template(icon_for_aircraft(aircraft), templateCount);
    constexpr double kDegToRad = 0.017453292519943295;
    const double angle = headingDeg * kDegToRad;
    const double sinA = std::sin(angle);
    const double cosA = std::cos(angle);
    const size_t shapeCount = std::min(capacity, templateCount);
    for (size_t i = 0; i < shapeCount; ++i) {
        const double x = shape[i].x;
        const double y = shape[i].y;
        points[i].x = static_cast<lv_coord_t>(center.x + x * cosA - y * sinA);
        points[i].y = static_cast<lv_coord_t>(center.y + x * sinA + y * cosA);
    }
    return shapeCount;
}

void build_ui() {
    lv_obj_t* scr = lv_scr_act();
    style_screen(scr, cfg::kColorBackground);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_set_size(header, cfg::kDisplayWidth, 54);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    style_screen(header, cfg::kColorPanel);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 14, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_label(header, &lv_font_montserrat_28, cfg::kColorText);
    lv_label_set_text(title, "FlightsAbove");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    g_countLabel = make_label(header, &lv_font_montserrat_20, cfg::kColorGreen);
    lv_label_set_text(g_countLabel, "0 aircraft");
    lv_obj_align(g_countLabel, LV_ALIGN_RIGHT_MID, 0, 0);

    g_radar = lv_obj_create(scr);
    lv_obj_set_size(g_radar, kRadarWidth, kRadarHeight);
    lv_obj_align(g_radar, LV_ALIGN_TOP_MID, 0, 64);
    style_screen(g_radar, cfg::kColorPanelAlt);
    lv_obj_set_style_radius(g_radar, 8, 0);
    lv_obj_set_style_border_width(g_radar, 1, 0);
    lv_obj_set_style_border_color(g_radar, lv_color_hex(0x265688), 0);
    lv_obj_set_style_pad_all(g_radar, 0, 0);
    lv_obj_clear_flag(g_radar, LV_OBJ_FLAG_SCROLLABLE);

    g_basemap = lv_img_create(g_radar);
    lv_img_set_src(g_basemap, &flightsabove_basemap_default);
    lv_img_set_pivot(g_basemap, kRadarWidth / 2, kRadarHeight / 2);
    lv_img_set_zoom(g_basemap, 256);
    lv_obj_set_pos(g_basemap, 0, 0);

    for (int i = 0; i < 4; ++i) {
        lv_obj_t* ring = lv_obj_create(g_radar);
        const int size = 76 + i * 72;
        lv_obj_set_size(ring, size, size);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, size / 2, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(i == 3 ? cfg::kColorCyan : 0x265688), 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_outerRangeLabel = make_label(g_radar, &lv_font_montserrat_20, cfg::kColorCyan);
    lv_obj_set_width(g_outerRangeLabel, 56);
    lv_obj_set_style_text_align(g_outerRangeLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_outerRangeLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(g_outerRangeLabel, "--");
    lv_obj_set_pos(g_outerRangeLabel, kRadarWidth / 2 - 28, kRadarHeight / 2 - kRadarRadius + 4);

    lv_obj_t* crossH = lv_obj_create(g_radar);
    lv_obj_set_size(crossH, kRadarWidth - 28, 1);
    lv_obj_align(crossH, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(crossH, lv_color_hex(0x1d3d6f), 0);
    lv_obj_set_style_border_width(crossH, 0, 0);
    lv_obj_t* crossV = lv_obj_create(g_radar);
    lv_obj_set_size(crossV, 1, kRadarHeight - 28);
    lv_obj_align(crossV, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(crossV, lv_color_hex(0x1d3d6f), 0);
    lv_obj_set_style_border_width(crossV, 0, 0);

    lv_obj_t* center = lv_obj_create(g_radar);
    lv_obj_set_size(center, 12, 12);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, 6, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(cfg::kColorAmber), 0);
    lv_obj_set_style_border_width(center, 0, 0);

    g_rangeLabel = make_label(g_radar, &lv_font_montserrat_14, cfg::kColorMuted);
    lv_label_set_text(g_rangeLabel, "AUTO -- MI");
    lv_obj_align(g_rangeLabel, LV_ALIGN_TOP_RIGHT, -10, 8);

    for (size_t i = 0; i < kVisibleAircraft; ++i) {
        g_trackLines[i] = lv_line_create(g_radar);
        lv_obj_set_style_line_width(g_trackLines[i], 2, 0);
        lv_obj_set_style_line_color(g_trackLines[i], lv_color_hex(0x5DE3F7), 0);
        lv_obj_set_style_line_opa(g_trackLines[i], LV_OPA_50, 0);
        lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);

        g_planeMarkers[i] = lv_line_create(g_radar);
        lv_obj_set_size(g_planeMarkers[i], kRadarWidth, kRadarHeight);
        lv_obj_set_style_line_width(g_planeMarkers[i], 2, 0);
        lv_obj_set_style_line_color(g_planeMarkers[i], lv_color_hex(cfg::kColorCyan), 0);
        lv_obj_set_style_line_rounded(g_planeMarkers[i], true, 0);
        lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* nearest = lv_obj_create(scr);
    lv_obj_set_size(nearest, 432, 64);
    lv_obj_align(nearest, LV_ALIGN_TOP_MID, 0, 390);
    style_screen(nearest, cfg::kColorPanel);
    lv_obj_set_style_radius(nearest, 8, 0);
    lv_obj_set_style_pad_all(nearest, 10, 0);
    lv_obj_clear_flag(nearest, LV_OBJ_FLAG_SCROLLABLE);

    g_nearestCallsign = make_label(nearest, &lv_font_montserrat_20, cfg::kColorText);
    lv_obj_set_width(g_nearestCallsign, 210);
    lv_label_set_long_mode(g_nearestCallsign, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestCallsign, "--");
    lv_obj_align(g_nearestCallsign, LV_ALIGN_TOP_LEFT, 0, 0);

    g_nearestType = make_label(nearest, &lv_font_montserrat_14, cfg::kColorAmber);
    lv_obj_set_width(g_nearestType, 190);
    lv_label_set_long_mode(g_nearestType, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestType, "--");
    lv_obj_align(g_nearestType, LV_ALIGN_TOP_RIGHT, 0, 4);

    g_nearestMeta = make_label(nearest, &lv_font_montserrat_14, cfg::kColorCyan);
    lv_obj_set_width(g_nearestMeta, 410);
    lv_label_set_long_mode(g_nearestMeta, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestMeta, "Waiting for ADS-B data");
    lv_obj_align(g_nearestMeta, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    g_statusLabel = make_label(scr, &lv_font_montserrat_14, cfg::kColorMuted);
    lv_label_set_text(g_statusLabel, "HTTP ADS-B | UART fallback");
    lv_obj_align(g_statusLabel, LV_ALIGN_BOTTOM_MID, 0, -4);
}

std::string label_for_aircraft(const Aircraft& aircraft) {
    if (!aircraft.callsign.empty()) {
        return aircraft.callsign;
    }
    return aircraft.icao;
}

void refresh_ui(lv_timer_t*) {
    size_t count = 0;
    size_t activeCount = 0;
    const int64_t now = now_ms();

    if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_aircraftStore.setReceiverLocation(settings.getReceiverLatitude(),
                                            settings.getReceiverLongitude());
        g_aircraftStore.purgeStale(now);
        count = g_aircraftStore.snapshot(g_visibleAircraft, kVisibleAircraft, now);
        activeCount = g_aircraftStore.activeCount(now);
        xSemaphoreGive(g_aircraftMutex);
    }

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%u aircraft", static_cast<unsigned>(activeCount));
    lv_label_set_text(g_countLabel, buffer);

    device_network::Snapshot network = device_network::snapshot();
    snprintf(buffer, sizeof(buffer), "%s | %s",
             network.stationConnected ? network.stationIp : network.setupUrl,
             "HTTP ADS-B | UART fallback");
    lv_label_set_text(g_statusLabel, buffer);

    const uint16_t sleepMin = settings.getDisplaySleepMin();
    if (!g_displaySleeping && sleepMin > 0 &&
        now - g_lastActivityMs >= static_cast<int64_t>(sleepMin) * 60 * 1000) {
        platform::setBacklight(false);
        g_displaySleeping = true;
    }

    if (count == 0) {
        lv_label_set_text(g_nearestCallsign, "--");
        lv_label_set_text(g_nearestType, "--");
        lv_label_set_text(g_nearestMeta, "Waiting for ADS-B data");
        snprintf(buffer, sizeof(buffer), "AUTO MAX %u MI",
                 static_cast<unsigned>(settings.getRadarRangeMiles()));
        lv_label_set_text(g_rangeLabel, buffer);
        update_outer_range_label(settings.getRadarRangeMiles());
        update_basemap_zoom(settings.getRadarRangeMiles());
        for (size_t i = 0; i < kVisibleAircraft; ++i) {
            lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const uint16_t maxRangeMiles = settings.getRadarRangeMiles();
    const double rangeMiles = autoscaled_range_miles(g_visibleAircraft, count, maxRangeMiles);
    const double rangeNm = rangeMiles * kNmPerMile;
    snprintf(buffer, sizeof(buffer), "AUTO %.0f/%u MI", rangeMiles, static_cast<unsigned>(maxRangeMiles));
    lv_label_set_text(g_rangeLabel, buffer);
    update_outer_range_label(rangeMiles);
    update_basemap_zoom(rangeMiles);

    for (size_t i = 0; i < kVisibleAircraft; ++i) {
        if (i >= count || !g_visibleAircraft[i].hasBearing ||
            !(g_visibleAircraft[i].hasDistance || g_visibleAircraft[i].hasPosition) ||
            g_visibleAircraft[i].distanceNm > rangeNm) {
            lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const Aircraft& item = g_visibleAircraft[i];
        const lv_point_t point = radar_point(item.distanceNm, item.bearingDeg, rangeNm);
        const size_t pointCount = plane_icon_points(item, point,
                                                    item.hasTrack ? item.trackDeg : item.bearingDeg,
                                                    g_planeIconPoints[i],
                                                    sizeof(g_planeIconPoints[i]) / sizeof(g_planeIconPoints[i][0]));
        lv_line_set_points(g_planeMarkers[i], g_planeIconPoints[i], pointCount);
        lv_obj_set_style_line_color(g_planeMarkers[i], lv_color_hex(color_for_aircraft(item)), 0);
        lv_obj_clear_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);

        size_t traceCount = 0;
        for (size_t p = 0; p < item.traceCount && p < Aircraft::kTraceLength; ++p) {
            if (item.traceDistanceNm[p] > rangeNm) {
                continue;
            }
            g_trackPoints[i][traceCount++] = radar_point(item.traceDistanceNm[p],
                                                         item.traceBearingDeg[p],
                                                         rangeNm);
        }
        if (traceCount >= 2) {
            lv_line_set_points(g_trackLines[i], g_trackPoints[i], traceCount);
            lv_obj_set_style_line_color(g_trackLines[i], lv_color_hex(color_for_aircraft(item)), 0);
            lv_obj_clear_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    const Aircraft& nearest = g_visibleAircraft[0];
    lv_label_set_text(g_nearestCallsign, label_for_aircraft(nearest).c_str());
    lv_label_set_text(g_nearestType, type_for_aircraft(nearest).c_str());
    const std::string metaPrefix = nearest.hasRoute && !nearest.route.empty()
        ? nearest.route + "  "
        : "";
    snprintf(buffer, sizeof(buffer), "%s%.1f mi  %s ft  %s kt  hdg %s",
             metaPrefix.c_str(),
             (nearest.hasDistance || nearest.hasPosition) ? nm_to_miles(nearest.distanceNm) : 0.0,
             nearest.hasAltitude ? std::to_string(nearest.altitudeFt).c_str() : "--",
             nearest.hasGroundSpeed ? std::to_string(nearest.groundSpeedKt).c_str() : "--",
             nearest.hasTrack ? std::to_string(nearest.trackDeg).c_str() : "--");
    lv_label_set_text(g_nearestMeta, buffer);
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
    mark_activity();
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

void adsb_http_task(void*) {
    ESP_LOGI(TAG, "ADS-B HTTP ingest enabled");

    for (;;) {
        const device_network::Snapshot network = device_network::snapshot();
        if (!network.stationConnected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::string body;
        int status = 0;
        const std::string feederUrl = settings.getFeederUrl();
        const esp_err_t err = http_get(feederUrl.c_str(), kAircraftJsonMaxBytes, body, &status);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "aircraft JSON fetch failed: %s status=%d url=%s",
                     esp_err_to_name(err), status, feederUrl.c_str());
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        const size_t count = g_readsbParser.parseAircraftJson(body.c_str(), g_httpUpdates,
                                                              kHttpAircraftCapacity);
        if (count > 0 && xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            const int64_t now = now_ms();
            for (size_t i = 0; i < count; ++i) {
                g_aircraftStore.applyUpdate(g_httpUpdates[i], now);
            }
            xSemaphoreGive(g_aircraftMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void route_lookup_task(void*) {
    int64_t lastLookupMs = 0;

    for (;;) {
        const device_network::Snapshot network = device_network::snapshot();
        if (!network.stationConnected) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const int64_t now = now_ms();
        if (now - lastLookupMs < kRouteLookupMinIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t visibleCount = 0;
        if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_aircraftStore.purgeStale(now);
            visibleCount = g_aircraftStore.snapshot(g_routeVisibleAircraft, kVisibleAircraft, now);
            xSemaphoreGive(g_aircraftMutex);
        }

        size_t requestCount = 0;
        for (size_t i = 0; i < visibleCount && requestCount < kRouteBatchCapacity; ++i) {
            const Aircraft& aircraft = g_routeVisibleAircraft[i];
            if (aircraft.normalizedCallsign.empty() || !aircraft.hasPosition) {
                continue;
            }

            std::string cachedRoute;
            bool hasCachedRoute = false;
            bool shouldLookup = false;
            if (xSemaphoreTake(g_routeCacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                hasCachedRoute = g_routeCache.routeFor(aircraft.normalizedCallsign, now, cachedRoute);
                shouldLookup = g_routeCache.shouldLookup(aircraft.normalizedCallsign, now);
                xSemaphoreGive(g_routeCacheMutex);
            }

            if (hasCachedRoute) {
                AircraftUpdate update;
                update.icao = aircraft.icao;
                update.route = cachedRoute;
                update.hasRoute = true;
                if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_aircraftStore.applyUpdate(update, now);
                    xSemaphoreGive(g_aircraftMutex);
                }
                continue;
            }

            if (!shouldLookup) {
                continue;
            }

            g_routeRequests[requestCount].callsign = aircraft.normalizedCallsign;
            g_routeRequests[requestCount].latitude = aircraft.latitude;
            g_routeRequests[requestCount].longitude = aircraft.longitude;
            ++requestCount;
        }

        if (requestCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const std::string requestBody = adsb::buildRouteRequestJson(g_routeRequests, requestCount);
        std::string responseBody;
        int status = 0;
        lastLookupMs = now_ms();
        const esp_err_t err = http_post_json(kRouteApiUrl, requestBody, kRouteJsonMaxBytes,
                                             responseBody, &status);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "route lookup failed: %s status=%d", esp_err_to_name(err), status);
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        const int64_t responseNow = now_ms();
        const size_t resultCount = adsb::parseRouteResultsJson(responseBody.c_str(), g_routeResults,
                                                               kRouteBatchCapacity);
        if (xSemaphoreTake(g_routeCacheMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (size_t i = 0; i < requestCount; ++i) {
                bool found = false;
                for (size_t r = 0; r < resultCount; ++r) {
                    if (g_routeResults[r].callsign == g_routeRequests[i].callsign) {
                        g_routeCache.markFound(g_routeResults[r].callsign, g_routeResults[r].route, responseNow);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_routeCache.markMissing(g_routeRequests[i].callsign, responseNow);
                }
            }
            xSemaphoreGive(g_routeCacheMutex);
        }

        if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (size_t r = 0; r < resultCount; ++r) {
                for (size_t i = 0; i < visibleCount; ++i) {
                    if (g_routeVisibleAircraft[i].normalizedCallsign == g_routeResults[r].callsign) {
                        AircraftUpdate update;
                        update.icao = g_routeVisibleAircraft[i].icao;
                        update.route = g_routeResults[r].route;
                        update.hasRoute = true;
                        g_aircraftStore.applyUpdate(update, responseNow);
                    }
                }
            }
            xSemaphoreGive(g_aircraftMutex);
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
    settings.load();

    g_aircraftMutex = xSemaphoreCreateMutex();
    configASSERT(g_aircraftMutex != nullptr);
    g_routeCacheMutex = xSemaphoreCreateMutex();
    configASSERT(g_routeCacheMutex != nullptr);

    device_network::begin();
    init_adsb_uart();
    xTaskCreatePinnedToCore(adsb_task, "adsb_uart", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(adsb_http_task, "adsb_http", 12288, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(route_lookup_task, "route_lookup", 12288, nullptr, 3, nullptr, 0);

    init_lvgl();
    for (;;) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
