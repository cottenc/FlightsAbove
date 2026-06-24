#include "aircraft_store.h"
#include "aircraft_icons.h"
#include "basemap_default.h"
#include "device_network.h"
#include "flights_config.h"
#include "logo_fallbacks.h"
#include "platform.h"
#include "readsb_json_parser.h"
#include "route_cache.h"
#include "sbs_parser.h"
#include "storage.h"
#include "ui_layout.h"

#include "driver/uart.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "extra/libs/png/lv_png.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "rom/cache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <iterator>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/stat.h>

using adsb::Aircraft;
using adsb::AircraftStore;
using adsb::AircraftUpdate;
using adsb::ReadsbJsonParser;
using adsb::RouteCache;
using adsb::RouteRequest;
using adsb::RouteResult;
using adsb::SbsParser;

namespace {

constexpr size_t kVisibleAircraft = 16;
constexpr int kAircraftIconCellPx = 86;
constexpr uint16_t kPlaneIconZoom = 116;
constexpr uint16_t kPlaneIconStrokeZoom = 122;
constexpr uint16_t kExactPlaneIconZoom = 160;
constexpr uint16_t kExactPlaneIconStrokeZoom = 168;
constexpr uint16_t kNearestExactPlaneIconZoom = 196;
constexpr uint16_t kNearestExactPlaneIconStrokeZoom = 206;
constexpr int kRadarWidth = 432;
constexpr int kRadarHeight = 318;
constexpr int kRadarRadius = 146;
constexpr double kLocalBasemapRangeMiles = 10.0;
constexpr double kCloseBasemapRangeMiles = 25.0;
constexpr double kMidBasemapRangeMiles = 50.0;
constexpr double kLongBasemapRangeMiles = 150.0;
constexpr double kLocalBasemapMaxRangeMiles = 10.0;
constexpr double kCloseBasemapMaxRangeMiles = 25.0;
constexpr double kMidBasemapMaxRangeMiles = 50.0;
constexpr size_t kHttpAircraftCapacity = 32;
constexpr size_t kRouteBatchCapacity = 4;
constexpr int kHttpTimeoutMs = 10000;
constexpr size_t kAircraftJsonMaxBytes = 131072;
constexpr size_t kRouteJsonMaxBytes = 4096;
constexpr size_t kLogoPngMaxBytes = 65536;
constexpr int64_t kRouteLookupMinIntervalMs = 3500;
constexpr int64_t kLogoLookupMinIntervalMs = 60 * 1000;
constexpr int64_t kLogoMissingRetryMs = 24 * 60 * 60 * 1000;
constexpr int64_t kLogoSuccessRefreshMs = 30LL * 24 * 60 * 60 * 1000;
constexpr int64_t kLogoQuotaWindowMs = 24 * 60 * 60 * 1000;
constexpr uint16_t kLogoDailyLookupLimit = 100;
constexpr int64_t kLogoPrefetchIntervalMs = 10 * 1000;
constexpr int64_t kLogoPrefetchIdleMs = 60 * 60 * 1000;
constexpr size_t kLogoPersistentCacheMaxEntries = 32;
constexpr double kMilesPerNm = 1.150779448;
constexpr double kNmPerMile = 0.868976242;
constexpr const char* kRouteApiUrl = "http://adsb.im/api/0/routeset";
constexpr const char* kLogoApiBaseUrl = "https://api.logostream.dev/airlines/icao/";
constexpr const char* kLogoIataApiBaseUrl = "https://api.logostream.dev/airlines/iata/";
constexpr const char* kLiveryApiBaseUrl = "https://airlines-api.logostream.dev/livery/icao/";
constexpr const char* kLogoCacheBasePath = "/spiffs/logos";
const char* TAG = "flightsabove";

AircraftStore g_aircraftStore(cfg::kReceiverLatitude, cfg::kReceiverLongitude, cfg::kAircraftStaleMs);
SbsParser g_parser;
ReadsbJsonParser g_readsbParser;
RouteCache g_routeCache;
SemaphoreHandle_t g_aircraftMutex = nullptr;
SemaphoreHandle_t g_routeCacheMutex = nullptr;
SemaphoreHandle_t g_logoMutex = nullptr;
SemaphoreHandle_t g_lvglMutex = nullptr;

lv_obj_t* g_countLabel = nullptr;
lv_obj_t* g_statusLabel = nullptr;
lv_obj_t* g_nearestCallsign = nullptr;
lv_obj_t* g_nearestMeta = nullptr;
lv_obj_t* g_nearestType = nullptr;
lv_obj_t* g_nearestLogoFrame = nullptr;
lv_obj_t* g_nearestLogo = nullptr;
lv_obj_t* g_nearestPlaneShadow = nullptr;
lv_obj_t* g_nearestPlaneMarker = nullptr;
lv_obj_t* g_rangeLabel = nullptr;
lv_obj_t* g_outerRangeLabel = nullptr;
lv_obj_t* g_outerRangeShadow = nullptr;
lv_obj_t* g_radar = nullptr;
lv_obj_t* g_basemap = nullptr;
double g_basemapSourceRangeMiles = kLongBasemapRangeMiles;
lv_obj_t* g_planeShadows[kVisibleAircraft] = {};
lv_obj_t* g_planeMarkers[kVisibleAircraft] = {};
lv_obj_t* g_trackLines[kVisibleAircraft] = {};
lv_point_t g_trackPoints[kVisibleAircraft][Aircraft::kTraceLength] = {};
Aircraft g_visibleAircraft[kVisibleAircraft] = {};
Aircraft g_routeVisibleAircraft[kVisibleAircraft] = {};
AircraftUpdate g_httpUpdates[kHttpAircraftCapacity] = {};
RouteRequest g_routeRequests[kRouteBatchCapacity] = {};
RouteResult g_routeResults[kRouteBatchCapacity] = {};
SemaphoreHandle_t g_httpStatsMutex = nullptr;
size_t g_lastHttpAircraftBytes = 0;
size_t g_lastHttpParsedAircraft = 0;
int g_lastHttpStatus = 0;
esp_err_t g_lastHttpError = ESP_OK;
int64_t g_lastHttpFetchMs = 0;
std::string g_lastHttpFeederUrl;

struct ImageFetchTarget {
    std::string key;
    std::string airlineIcao;
    std::string iataType;
    std::string url;
    bool livery = false;
};

ImageFetchTarget image_target_for_aircraft(const Aircraft& aircraft, bool preferLivery);
void seed_builtin_logo_cache();

lv_img_dsc_t g_logoDescriptor = {};
std::string g_logoPngData;
std::string g_logoCode;
std::string g_pendingLogoPngData;
std::string g_pendingLogoCode;
std::string g_missingLogoCode;
std::string g_lastLogoPrefetchKey;
std::string g_lastLogoPrefetchEndpoint;
std::string g_lastLogoPrefetchSignature;
int64_t g_logoLastSuccessMs = 0;
int64_t g_logoRetryAfterMs = 0;
int64_t g_lastLogoPrefetchMs = 0;
esp_err_t g_lastLogoPrefetchError = ESP_OK;
int g_lastLogoPrefetchStatus = 0;
size_t g_lastLogoPrefetchBytes = 0;
bool g_lastLogoPrefetchPng = false;
bool g_lastLogoPrefetchStored = false;
bool g_pendingLogoReady = false;
uint16_t g_logoImageWidth = 0;
uint16_t g_logoImageHeight = 0;
bool g_logoCacheMounted = false;
bool g_logoCacheMountAttempted = false;
int64_t g_lastActivityMs = 0;
bool g_displaySleeping = false;

static lv_disp_drv_t* g_dispDrv = nullptr;

std::string response_signature(const std::string& body) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    const size_t count = std::min<size_t>(body.size(), 12);
    output.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t byte = static_cast<uint8_t>(body[i]);
        output.push_back(kHex[(byte >> 4) & 0x0F]);
        output.push_back(kHex[byte & 0x0F]);
    }
    return output;
}

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
                       size_t maxBytes, std::string& responseBody, int* statusCode = nullptr,
                       const char* headerName = nullptr, const char* headerValue = nullptr) {
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
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (postBody != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_method(client, HTTP_METHOD_POST));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", contentType));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, postBody, strlen(postBody)));
    }
    if (headerName != nullptr && headerValue != nullptr && headerValue[0] != '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, headerName, headerValue));
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

struct AltitudeColorPoint {
    double input;
    double value;
};

struct HslColor {
    double h;
    double s;
    double l;
};

double interpolate_value(const AltitudeColorPoint* points, size_t count, double input) {
    double value = points[0].value;
    for (size_t offset = count; offset > 0; --offset) {
        const size_t i = offset - 1;
        if (input > points[i].input) {
            if (i == count - 1) {
                value = points[i].value;
            } else {
                value = points[i].value +
                        (points[i + 1].value - points[i].value) *
                        (input - points[i].input) /
                        (points[i + 1].input - points[i].input);
            }
            break;
        }
    }
    return value;
}

uint8_t color_channel_from_unit(double value) {
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(value * 255.0)), 0, 255));
}

double hue_to_rgb(double p, double q, double t) {
    if (t < 0.0) {
        t += 1.0;
    }
    if (t > 1.0) {
        t -= 1.0;
    }
    if (t < 1.0 / 6.0) {
        return p + (q - p) * 6.0 * t;
    }
    if (t < 1.0 / 2.0) {
        return q;
    }
    if (t < 2.0 / 3.0) {
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    }
    return p;
}

uint32_t hsl_to_rgb_hex(HslColor color) {
    double h = color.h / 360.0;
    double s = color.s * 0.01;
    double l = color.l * 0.01;
    double r = l;
    double g = l;
    double b = l;

    if (s > 0.0) {
        const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        const double p = 2.0 * l - q;
        r = hue_to_rgb(p, q, h + 1.0 / 3.0);
        g = hue_to_rgb(p, q, h);
        b = hue_to_rgb(p, q, h - 1.0 / 3.0);
    }

    return (static_cast<uint32_t>(color_channel_from_unit(r)) << 16) |
           (static_cast<uint32_t>(color_channel_from_unit(g)) << 8) |
           static_cast<uint32_t>(color_channel_from_unit(b));
}

HslColor tar1090_altitude_color(const Aircraft& aircraft) {
    static constexpr AltitudeColorPoint kHueByAltitude[] = {
        {0, 20},
        {2000, 32.5},
        {4000, 43},
        {6000, 54},
        {8000, 72},
        {9000, 85},
        {11000, 140},
        {40000, 300},
        {51000, 360},
    };
    static constexpr AltitudeColorPoint kLightnessByHue[] = {
        {0, 53},
        {20, 50},
        {32, 54},
        {40, 52},
        {46, 51},
        {50, 46},
        {60, 43},
        {80, 41},
        {100, 41},
        {120, 41},
        {140, 41},
        {160, 40},
        {180, 40},
        {190, 44},
        {198, 50},
        {200, 58},
        {220, 58},
        {240, 58},
        {255, 55},
        {266, 55},
        {270, 58},
        {280, 58},
        {290, 47},
        {300, 43},
        {310, 48},
        {320, 48},
        {340, 52},
        {360, 53},
    };

    if (aircraft.category == "C3" || aircraft.typeCode == "TWR") {
        return {220, 0, 45};
    }
    if (!aircraft.hasAltitude) {
        return {0, 0, 75};
    }

    const int roundTo = aircraft.altitudeFt < 8000 ? 50 : 500;
    const double altitude = roundTo * std::round(static_cast<double>(aircraft.altitudeFt) / roundTo);
    HslColor color = {
        interpolate_value(kHueByAltitude, std::size(kHueByAltitude), altitude),
        88,
        0,
    };
    color.l = interpolate_value(kLightnessByHue, std::size(kLightnessByHue), color.h);
    color.h = std::fmod(color.h, 360.0);
    if (color.h < 0) {
        color.h += 360.0;
    }
    color.s = std::clamp(color.s, 0.0, 95.0);
    color.l = std::clamp(color.l, 0.0, 95.0);
    return color;
}

uint32_t color_for_aircraft(const Aircraft& aircraft) {
    return hsl_to_rgb_hex(tar1090_altitude_color(aircraft));
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
    lv_label_set_text(g_outerRangeShadow, buffer);
    lv_label_set_text(g_outerRangeLabel, buffer);
}

void update_basemap_zoom(double rangeMiles) {
    if (g_basemap == nullptr || rangeMiles <= 0.0) {
        return;
    }

    const bool useLocalBasemap = rangeMiles <= kLocalBasemapMaxRangeMiles;
    const bool useCloseBasemap = !useLocalBasemap && rangeMiles <= kCloseBasemapMaxRangeMiles;
    const bool useMidBasemap = !useCloseBasemap && rangeMiles <= kMidBasemapMaxRangeMiles;
    const double sourceRangeMiles = useLocalBasemap
        ? kLocalBasemapRangeMiles
        : (useCloseBasemap
            ? kCloseBasemapRangeMiles
            : (useMidBasemap ? kMidBasemapRangeMiles : kLongBasemapRangeMiles));
    if (sourceRangeMiles != g_basemapSourceRangeMiles) {
        lv_img_set_src(g_basemap, useLocalBasemap
            ? &flightsabove_basemap_local
            : (useCloseBasemap
                ? &flightsabove_basemap_close
                : (useMidBasemap ? &flightsabove_basemap_mid : &flightsabove_basemap_long)));
        g_basemapSourceRangeMiles = sourceRangeMiles;
    }

    const double zoom = 256.0 * sourceRangeMiles / rangeMiles;
    const uint16_t lvZoom = static_cast<uint16_t>(
        std::max(128.0, std::min(4096.0, std::round(zoom))));
    lv_img_set_zoom(g_basemap, lvZoom);
}

void set_nearest_logo_visible(bool visible) {
    if (g_nearestLogoFrame == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(g_nearestLogoFrame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_nearestCallsign, 76, 0);
        lv_obj_set_width(g_nearestCallsign, 180);
        lv_obj_set_pos(g_nearestMeta, 76, 36);
        lv_obj_set_width(g_nearestMeta, 276);
    } else {
        lv_obj_add_flag(g_nearestLogoFrame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(g_nearestCallsign, 0, 0);
        lv_obj_set_width(g_nearestCallsign, 210);
        lv_obj_set_pos(g_nearestMeta, 0, 36);
        lv_obj_set_width(g_nearestMeta, 328);
    }
}

bool png_dimensions(const std::string& data, uint16_t& width, uint16_t& height) {
    static constexpr uint8_t kPngMagic[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (data.size() < 24 || memcmp(data.data(), kPngMagic, sizeof(kPngMagic)) != 0) {
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const uint32_t pngWidth = (static_cast<uint32_t>(bytes[16]) << 24) |
                              (static_cast<uint32_t>(bytes[17]) << 16) |
                              (static_cast<uint32_t>(bytes[18]) << 8) |
                              static_cast<uint32_t>(bytes[19]);
    const uint32_t pngHeight = (static_cast<uint32_t>(bytes[20]) << 24) |
                               (static_cast<uint32_t>(bytes[21]) << 16) |
                               (static_cast<uint32_t>(bytes[22]) << 8) |
                               static_cast<uint32_t>(bytes[23]);
    if (pngWidth == 0 || pngHeight == 0 || pngWidth > UINT16_MAX || pngHeight > UINT16_MAX) {
        return false;
    }

    width = static_cast<uint16_t>(pngWidth);
    height = static_cast<uint16_t>(pngHeight);
    return true;
}

void apply_pending_logo() {
    if (g_logoMutex == nullptr || g_nearestLogo == nullptr) {
        return;
    }

    bool changed = false;
    if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (g_pendingLogoReady) {
            g_logoPngData.swap(g_pendingLogoPngData);
            g_logoCode = g_pendingLogoCode;
            g_pendingLogoCode.clear();
            g_pendingLogoPngData.clear();
            g_pendingLogoReady = false;

            uint16_t width = 0;
            uint16_t height = 0;
            const bool hasDimensions = png_dimensions(g_logoPngData, width, height);

            g_logoDescriptor.header.cf = LV_IMG_CF_RAW_ALPHA;
            g_logoDescriptor.header.always_zero = 0;
            g_logoDescriptor.header.reserved = 0;
            g_logoDescriptor.header.w = hasDimensions ? width : 0;
            g_logoDescriptor.header.h = hasDimensions ? height : 0;
            g_logoDescriptor.data_size = static_cast<uint32_t>(g_logoPngData.size());
            g_logoDescriptor.data = reinterpret_cast<const uint8_t*>(g_logoPngData.data());
            g_logoImageWidth = hasDimensions ? width : 0;
            g_logoImageHeight = hasDimensions ? height : 0;
            changed = true;
        }
        xSemaphoreGive(g_logoMutex);
    }

    if (changed && g_logoDescriptor.data_size > 0) {
        lv_img_set_src(g_nearestLogo, &g_logoDescriptor);
        uint16_t zoom = 256;
        if (g_logoImageWidth > 0 && g_logoImageHeight > 0) {
            const double fitX = 58.0 / static_cast<double>(g_logoImageWidth);
            const double fitY = 36.0 / static_cast<double>(g_logoImageHeight);
            zoom = static_cast<uint16_t>(
                std::max(64.0, std::min(256.0, std::floor(256.0 * std::min(fitX, fitY)))));
        }
        lv_img_set_zoom(g_nearestLogo, zoom);
        lv_obj_center(g_nearestLogo);
    }
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
    Unknown,
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
    if (!aircraft.hasType && aircraft.category.empty() && type.empty() && desc.empty()) {
        return AircraftIcon::Unknown;
    }
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

const lv_img_dsc_t* icon_descriptor(AircraftIcon icon) {
    switch (icon) {
    case AircraftIcon::Heavy:
        return &flightsabove_icon_heavy_2e;
    case AircraftIcon::Jet:
        return &flightsabove_icon_jet_swept;
    case AircraftIcon::Small:
        return &flightsabove_icon_cessna;
    case AircraftIcon::Glider:
        return &flightsabove_icon_glider;
    case AircraftIcon::Helicopter:
        return &flightsabove_icon_helicopter;
    case AircraftIcon::Unknown:
        return &flightsabove_icon_unknown;
    case AircraftIcon::Airliner:
    default:
        return &flightsabove_icon_airliner;
    }
}

const lv_img_dsc_t* icon_descriptor_for_type(const std::string& typeCode) {
    struct TypeIcon {
        const char* typeCode;
        const lv_img_dsc_t* descriptor;
    };
    static constexpr TypeIcon kTypeIcons[] = {
        {"A21N", &flightsabove_icon_type_a21n},
        {"A321", &flightsabove_icon_type_a321},
        {"A359", &flightsabove_icon_type_a359},
        {"B38M", &flightsabove_icon_type_b38m},
        {"B39M", &flightsabove_icon_type_b39m},
        {"B737", &flightsabove_icon_type_b737},
        {"B738", &flightsabove_icon_type_b738},
        {"B739", &flightsabove_icon_type_b739},
        {"B763", &flightsabove_icon_type_b763},
        {"B77L", &flightsabove_icon_type_b77l},
        {"B77W", &flightsabove_icon_type_b77w},
        {"B789", &flightsabove_icon_type_b789},
        {"C172", &flightsabove_icon_type_c172},
        {"C68A", &flightsabove_icon_type_c68a},
        {"E75L", &flightsabove_icon_type_e75l},
    };

    for (const TypeIcon& icon : kTypeIcons) {
        if (typeCode == icon.typeCode) {
            return icon.descriptor;
        }
    }
    return nullptr;
}

const lv_img_dsc_t* icon_descriptor_for_aircraft(const Aircraft& aircraft) {
    if (const lv_img_dsc_t* typed = icon_descriptor_for_type(aircraft.typeCode)) {
        return typed;
    }
    return icon_descriptor(icon_for_aircraft(aircraft));
}

bool has_exact_type_icon(const Aircraft& aircraft) {
    return icon_descriptor_for_type(aircraft.typeCode) != nullptr;
}

void position_plane_icon(lv_obj_t* image, const lv_point_t& center, int headingDeg,
                         const lv_img_dsc_t* descriptor, uint16_t zoom) {
    lv_img_set_src(image, descriptor);
    lv_img_set_pivot(image, kAircraftIconCellPx / 2, kAircraftIconCellPx / 2);
    lv_img_set_angle(image, static_cast<int16_t>((headingDeg % 360) * 10));
    lv_img_set_zoom(image, zoom);
    lv_obj_set_pos(image,
                   center.x - kAircraftIconCellPx / 2,
                   center.y - kAircraftIconCellPx / 2);
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
    lv_img_set_src(g_basemap, &flightsabove_basemap_long);
    lv_img_set_pivot(g_basemap, kRadarWidth / 2, kRadarHeight / 2);
    lv_img_set_zoom(g_basemap, 256);
    lv_obj_set_pos(g_basemap, 0, 0);

    lv_obj_t* basemapScrim = lv_obj_create(g_radar);
    lv_obj_set_size(basemapScrim, kRadarWidth, kRadarHeight);
    lv_obj_set_pos(basemapScrim, 0, 0);
    lv_obj_set_style_bg_color(basemapScrim, lv_color_hex(0x04100D), 0);
    lv_obj_set_style_bg_opa(basemapScrim, LV_OPA_30, 0);
    lv_obj_set_style_border_width(basemapScrim, 0, 0);
    lv_obj_set_style_radius(basemapScrim, 0, 0);
    lv_obj_clear_flag(basemapScrim, LV_OBJ_FLAG_SCROLLABLE);

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
        lv_obj_set_style_line_width(g_trackLines[i], 3, 0);
        lv_obj_set_style_line_color(g_trackLines[i], lv_color_hex(cfg::kColorCyan), 0);
        lv_obj_set_style_line_opa(g_trackLines[i], LV_OPA_70, 0);
        lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);

        g_planeShadows[i] = lv_img_create(g_radar);
        lv_img_set_src(g_planeShadows[i], &flightsabove_icon_unknown);
        lv_img_set_pivot(g_planeShadows[i], kAircraftIconCellPx / 2, kAircraftIconCellPx / 2);
        lv_img_set_zoom(g_planeShadows[i], kPlaneIconStrokeZoom);
        lv_obj_set_style_img_recolor(g_planeShadows[i], lv_color_hex(0x020403), 0);
        lv_obj_set_style_img_recolor_opa(g_planeShadows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_img_opa(g_planeShadows[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(g_planeShadows[i], LV_OBJ_FLAG_HIDDEN);

        g_planeMarkers[i] = lv_img_create(g_radar);
        lv_img_set_src(g_planeMarkers[i], &flightsabove_icon_unknown);
        lv_img_set_pivot(g_planeMarkers[i], kAircraftIconCellPx / 2, kAircraftIconCellPx / 2);
        lv_img_set_zoom(g_planeMarkers[i], kPlaneIconZoom);
        lv_obj_set_style_img_recolor(g_planeMarkers[i], lv_color_hex(cfg::kColorCyan), 0);
        lv_obj_set_style_img_recolor_opa(g_planeMarkers[i], LV_OPA_COVER, 0);
        lv_obj_set_style_img_opa(g_planeMarkers[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < kVisibleAircraft; ++i) {
        lv_obj_move_foreground(g_planeMarkers[i]);
    }

    const int rangeLabelX = kRadarWidth / 2 + kRadarRadius + 4;
    const int rangeLabelY = kRadarHeight / 2 - 12;
    g_outerRangeShadow = make_label(g_radar, &lv_font_montserrat_20, 0x020403);
    lv_obj_set_size(g_outerRangeShadow, 44, 24);
    lv_obj_set_style_text_align(g_outerRangeShadow, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_outerRangeShadow, LV_LABEL_LONG_CLIP);
    lv_label_set_text(g_outerRangeShadow, "--");
    lv_obj_set_pos(g_outerRangeShadow, rangeLabelX + 1, rangeLabelY + 1);
    g_outerRangeLabel = make_label(g_radar, &lv_font_montserrat_20, cfg::kColorCyan);
    lv_obj_set_size(g_outerRangeLabel, 44, 24);
    lv_obj_set_style_text_align(g_outerRangeLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_outerRangeLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(g_outerRangeLabel, "--");
    lv_obj_set_pos(g_outerRangeLabel, rangeLabelX, rangeLabelY);

    lv_obj_t* nearest = lv_obj_create(scr);
    lv_obj_set_size(nearest, 432, 64);
    lv_obj_align(nearest, LV_ALIGN_TOP_MID, 0, 390);
    style_screen(nearest, cfg::kColorPanel);
    lv_obj_set_style_radius(nearest, 8, 0);
    lv_obj_set_style_pad_all(nearest, 10, 0);
    lv_obj_clear_flag(nearest, LV_OBJ_FLAG_SCROLLABLE);

    g_nearestLogoFrame = lv_obj_create(nearest);
    lv_obj_set_size(g_nearestLogoFrame, 64, 42);
    lv_obj_set_pos(g_nearestLogoFrame, 0, 1);
    lv_obj_set_style_radius(g_nearestLogoFrame, 6, 0);
    lv_obj_set_style_pad_all(g_nearestLogoFrame, 3, 0);
    lv_obj_set_style_bg_color(g_nearestLogoFrame, lv_color_hex(0xF4F7F2), 0);
    lv_obj_set_style_border_width(g_nearestLogoFrame, 0, 0);
    lv_obj_set_style_clip_corner(g_nearestLogoFrame, true, 0);
    lv_obj_clear_flag(g_nearestLogoFrame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_nearestLogoFrame, LV_OBJ_FLAG_HIDDEN);

    g_nearestLogo = lv_img_create(g_nearestLogoFrame);
    lv_obj_center(g_nearestLogo);

    g_nearestCallsign = make_label(nearest, &lv_font_montserrat_20, cfg::kColorText);
    lv_obj_set_width(g_nearestCallsign, 210);
    lv_label_set_long_mode(g_nearestCallsign, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestCallsign, "--");
    lv_obj_set_pos(g_nearestCallsign, 0, 0);

    g_nearestType = make_label(nearest, &lv_font_montserrat_14, cfg::kColorAmber);
    lv_obj_set_width(g_nearestType, 58);
    lv_obj_set_style_text_align(g_nearestType, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_nearestType, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestType, "--");
    lv_obj_set_pos(g_nearestType, 366, 36);

    g_nearestMeta = make_label(nearest, &lv_font_montserrat_14, cfg::kColorCyan);
    lv_obj_set_width(g_nearestMeta, 328);
    lv_label_set_long_mode(g_nearestMeta, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_nearestMeta, "Waiting for ADS-B data");
    lv_obj_set_pos(g_nearestMeta, 0, 36);

    g_nearestPlaneShadow = lv_img_create(nearest);
    lv_img_set_src(g_nearestPlaneShadow, &flightsabove_icon_unknown);
    lv_img_set_pivot(g_nearestPlaneShadow, kAircraftIconCellPx / 2, kAircraftIconCellPx / 2);
    lv_img_set_zoom(g_nearestPlaneShadow, kPlaneIconStrokeZoom);
    lv_obj_set_style_img_recolor(g_nearestPlaneShadow, lv_color_hex(0x020403), 0);
    lv_obj_set_style_img_recolor_opa(g_nearestPlaneShadow, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(g_nearestPlaneShadow, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_nearestPlaneShadow, LV_OBJ_FLAG_HIDDEN);

    g_nearestPlaneMarker = lv_img_create(nearest);
    lv_img_set_src(g_nearestPlaneMarker, &flightsabove_icon_unknown);
    lv_img_set_pivot(g_nearestPlaneMarker, kAircraftIconCellPx / 2, kAircraftIconCellPx / 2);
    lv_img_set_zoom(g_nearestPlaneMarker, kPlaneIconZoom);
    lv_obj_set_style_img_recolor(g_nearestPlaneMarker, lv_color_hex(cfg::kColorCyan), 0);
    lv_obj_set_style_img_recolor_opa(g_nearestPlaneMarker, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(g_nearestPlaneMarker, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_nearestPlaneMarker, LV_OBJ_FLAG_HIDDEN);

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
    apply_pending_logo();

    size_t count = 0;
    size_t activeCount = 0;
    size_t maxRangeCount = 0;
    const int64_t now = now_ms();
    const uint16_t maxRangeMiles = settings.getRadarRangeMiles();

    if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_aircraftStore.setReceiverLocation(settings.getReceiverLatitude(),
                                            settings.getReceiverLongitude());
        g_aircraftStore.purgeStale(now);
        count = g_aircraftStore.snapshot(g_visibleAircraft, kVisibleAircraft, now);
        activeCount = g_aircraftStore.activeCount(now);
        maxRangeCount = g_aircraftStore.rangeCount(miles_to_nm(maxRangeMiles), now);
        xSemaphoreGive(g_aircraftMutex);
    }

    char buffer[128];
    if (maxRangeCount < activeCount) {
        snprintf(buffer, sizeof(buffer), "%u/%u aircraft",
                 static_cast<unsigned>(maxRangeCount),
                 static_cast<unsigned>(activeCount));
    } else {
        snprintf(buffer, sizeof(buffer), "%u aircraft", static_cast<unsigned>(activeCount));
    }
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
        set_nearest_logo_visible(false);
        lv_label_set_text(g_nearestCallsign, "--");
        lv_label_set_text(g_nearestType, "--");
        lv_label_set_text(g_nearestMeta, "Waiting for ADS-B data");
        lv_obj_add_flag(g_nearestPlaneShadow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_nearestPlaneMarker, LV_OBJ_FLAG_HIDDEN);
        snprintf(buffer, sizeof(buffer), "AUTO MAX %u MI",
                 static_cast<unsigned>(settings.getRadarRangeMiles()));
        lv_label_set_text(g_rangeLabel, buffer);
        update_outer_range_label(settings.getRadarRangeMiles());
        update_basemap_zoom(settings.getRadarRangeMiles());
        for (size_t i = 0; i < kVisibleAircraft; ++i) {
            lv_obj_add_flag(g_planeShadows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

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
            lv_obj_add_flag(g_planeShadows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_planeMarkers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_trackLines[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const Aircraft& item = g_visibleAircraft[i];
        const lv_point_t point = radar_point(item.distanceNm, item.bearingDeg, rangeNm);
        const int heading = item.hasTrack ? item.trackDeg : item.bearingDeg;
        const lv_img_dsc_t* descriptor = icon_descriptor_for_aircraft(item);
        const bool exactIcon = has_exact_type_icon(item);
        position_plane_icon(g_planeShadows[i], point, heading, descriptor,
                            exactIcon ? kExactPlaneIconStrokeZoom : kPlaneIconStrokeZoom);
        lv_obj_clear_flag(g_planeShadows[i], LV_OBJ_FLAG_HIDDEN);
        position_plane_icon(g_planeMarkers[i], point, heading, descriptor,
                            exactIcon ? kExactPlaneIconZoom : kPlaneIconZoom);
        lv_obj_set_style_img_recolor(g_planeMarkers[i], lv_color_hex(color_for_aircraft(item)), 0);
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
    const ImageFetchTarget preferredImage = image_target_for_aircraft(nearest, true);
    const ImageFetchTarget fallbackImage = image_target_for_aircraft(nearest, false);
    const bool showLogo = settings.hasLogostreamApiKey() &&
                          (!preferredImage.key.empty() || !fallbackImage.key.empty()) &&
                          (preferredImage.key == g_logoCode || fallbackImage.key == g_logoCode) &&
                          g_logoDescriptor.data_size > 0;
    set_nearest_logo_visible(showLogo);
    lv_label_set_text(g_nearestCallsign, label_for_aircraft(nearest).c_str());
    lv_label_set_text(g_nearestType, type_for_aircraft(nearest).c_str());
    const lv_img_dsc_t* nearestDescriptor = icon_descriptor_for_aircraft(nearest);
    const int nearestHeading = nearest.hasTrack ? nearest.trackDeg : nearest.bearingDeg;
    const lv_point_t nearestIconCenter = {394, 15};
    const bool nearestExactIcon = has_exact_type_icon(nearest);
    position_plane_icon(g_nearestPlaneShadow, nearestIconCenter, nearestHeading,
                        nearestDescriptor,
                        nearestExactIcon ? kNearestExactPlaneIconStrokeZoom : kPlaneIconStrokeZoom);
    lv_obj_clear_flag(g_nearestPlaneShadow, LV_OBJ_FLAG_HIDDEN);
    position_plane_icon(g_nearestPlaneMarker, nearestIconCenter, nearestHeading,
                        nearestDescriptor,
                        nearestExactIcon ? kNearestExactPlaneIconZoom : kPlaneIconZoom);
    lv_obj_set_style_img_recolor(g_nearestPlaneMarker, lv_color_hex(color_for_aircraft(nearest)), 0);
    lv_obj_clear_flag(g_nearestPlaneMarker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_nearestPlaneShadow);
    lv_obj_move_foreground(g_nearestPlaneMarker);
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
    lv_png_init();

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
        if (g_httpStatsMutex != nullptr &&
            xSemaphoreTake(g_httpStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_lastHttpStatus = status;
            g_lastHttpError = err;
            g_lastHttpFetchMs = now_ms();
            g_lastHttpAircraftBytes = body.size();
            g_lastHttpFeederUrl = feederUrl;
            if (err != ESP_OK || status != 200) {
                g_lastHttpParsedAircraft = 0;
            }
            xSemaphoreGive(g_httpStatsMutex);
        }
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "aircraft JSON fetch failed: %s status=%d url=%s",
                     esp_err_to_name(err), status, feederUrl.c_str());
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        const size_t count = g_readsbParser.parseAircraftJson(body.c_str(), g_httpUpdates,
                                                              kHttpAircraftCapacity);
        if (g_httpStatsMutex != nullptr &&
            xSemaphoreTake(g_httpStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_lastHttpParsedAircraft = count;
            xSemaphoreGive(g_httpStatsMutex);
        }
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

bool looks_like_png(const std::string& data) {
    static constexpr uint8_t kPngMagic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return data.size() >= sizeof(kPngMagic) &&
           memcmp(data.data(), kPngMagic, sizeof(kPngMagic)) == 0;
}

const char* const kCommonAirlineIcao[] = {
    "SKW", "ASA", "DAL", "QXE", "UAL", "SWA", "AAL", "JZA", "ACA", "WJA",
    "ROU", "ATN", "UPS", "ICE", "FDX", "BAW", "ANA", "AAY", "FFT", "CFS",
};

bool ensure_logo_cache_mounted() {
    if (g_logoCacheMounted) {
        return true;
    }
    if (g_logoCacheMountAttempted) {
        return false;
    }
    g_logoCacheMountAttempted = true;

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = nullptr;
    conf.max_files = 8;
    conf.format_if_mount_failed = true;

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "logo cache mount failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_spiffs_info(nullptr, &total, &used));
    ESP_LOGI(TAG, "logo cache mounted: %u/%u bytes used",
             static_cast<unsigned>(used), static_cast<unsigned>(total));
    mkdir(kLogoCacheBasePath, 0775);
    g_logoCacheMounted = true;
    seed_builtin_logo_cache();
    return true;
}

std::string logo_cache_path_for_key(const std::string& key) {
    std::string filename;
    filename.reserve(key.size() + 4);
    for (char ch : key) {
        const bool safe = (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') ||
                          ch == '_' || ch == '-';
        filename.push_back(safe ? ch : '_');
    }
    filename += ".png";
    return std::string(kLogoCacheBasePath) + "/" + filename;
}

bool logo_cache_file_exists(const std::string& key) {
    if (key.empty() || !ensure_logo_cache_mounted()) {
        return false;
    }
    struct stat st = {};
    return stat(logo_cache_path_for_key(key).c_str(), &st) == 0 &&
           st.st_size > 0 &&
           st.st_size <= static_cast<off_t>(kLogoPngMaxBytes);
}

size_t logo_cache_entry_count() {
    if (!ensure_logo_cache_mounted()) {
        return 0;
    }
    DIR* dir = opendir(kLogoCacheBasePath);
    if (dir == nullptr) {
        return 0;
    }
    size_t count = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

bool logo_cache_key_is_common_file(const char* filename) {
    if (filename == nullptr) {
        return false;
    }
    for (const char* code : kCommonAirlineIcao) {
        std::string expected = "A_";
        expected += code;
        expected += ".png";
        if (expected == filename) {
            return true;
        }
    }
    return false;
}

void evict_logo_cache_entry_if_needed(const std::string& keepKey) {
    if (logo_cache_entry_count() < kLogoPersistentCacheMaxEntries) {
        return;
    }

    const std::string keepPath = logo_cache_path_for_key(keepKey);
    DIR* dir = opendir(kLogoCacheBasePath);
    if (dir == nullptr) {
        return;
    }

    std::string candidate;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || logo_cache_key_is_common_file(entry->d_name)) {
            continue;
        }
        std::string path = std::string(kLogoCacheBasePath) + "/" + entry->d_name;
        if (path != keepPath) {
            candidate = path;
            break;
        }
    }
    closedir(dir);

    if (!candidate.empty()) {
        remove(candidate.c_str());
    }
}

bool read_logo_from_cache(const ImageFetchTarget& target, std::string& output) {
    if (target.livery || target.key.empty() || !ensure_logo_cache_mounted()) {
        return false;
    }

    const std::string path = logo_cache_path_for_key(target.key);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > static_cast<long>(kLogoPngMaxBytes)) {
        fclose(file);
        remove(path.c_str());
        return false;
    }

    output.assign(static_cast<size_t>(size), '\0');
    const size_t read = fread(output.data(), 1, output.size(), file);
    fclose(file);
    if (read != output.size() || !looks_like_png(output)) {
        remove(path.c_str());
        output.clear();
        return false;
    }
    return true;
}

void write_logo_to_cache(const ImageFetchTarget& target, const std::string& pngData) {
    if (target.livery || target.key.empty() || !looks_like_png(pngData) ||
        !ensure_logo_cache_mounted()) {
        return;
    }

    evict_logo_cache_entry_if_needed(target.key);
    const std::string path = logo_cache_path_for_key(target.key);
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "logo cache write failed for %s", target.key.c_str());
        return;
    }
    const size_t written = fwrite(pngData.data(), 1, pngData.size(), file);
    fclose(file);
    if (written != pngData.size()) {
        remove(path.c_str());
        ESP_LOGW(TAG, "logo cache short write for %s", target.key.c_str());
        return;
    }
    ESP_LOGI(TAG, "logo stored in persistent cache for %s", target.key.c_str());
}

void seed_builtin_logo_cache() {
    static bool attempted = false;
    if (attempted) {
        return;
    }

    size_t count = 0;
    const BuiltinLogoFallback* fallbacks = builtin_logo_fallbacks(&count);
    for (size_t i = 0; i < count; ++i) {
        if (fallbacks[i].key == nullptr || fallbacks[i].data == nullptr ||
            fallbacks[i].size == 0 || logo_cache_file_exists(fallbacks[i].key)) {
            continue;
        }

        ImageFetchTarget target;
        target.key = fallbacks[i].key;
        std::string png(reinterpret_cast<const char*>(fallbacks[i].data), fallbacks[i].size);
        write_logo_to_cache(target, png);
    }
    attempted = true;
}

bool queue_logo_for_display(const ImageFetchTarget& target, std::string& pngData, int64_t now) {
    if (target.key.empty() || !looks_like_png(pngData)) {
        return false;
    }
    if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_pendingLogoCode = target.key;
        g_pendingLogoPngData.swap(pngData);
        g_pendingLogoReady = true;
        g_logoLastSuccessMs = now;
        if (g_missingLogoCode == target.key) {
            g_missingLogoCode.clear();
            g_logoRetryAfterMs = 0;
        }
        xSemaphoreGive(g_logoMutex);
        return true;
    }
    return false;
}

bool load_logo_cache_for_display(const ImageFetchTarget& target, int64_t now) {
    bool alreadyLoaded = false;
    if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        alreadyLoaded = target.key == g_logoCode || target.key == g_pendingLogoCode;
        xSemaphoreGive(g_logoMutex);
    }
    if (alreadyLoaded) {
        return true;
    }

    std::string cachedPng;
    if (!read_logo_from_cache(target, cachedPng)) {
        return false;
    }
    const bool queued = queue_logo_for_display(target, cachedPng, now);
    if (queued) {
        ESP_LOGI(TAG, "logo loaded from persistent cache for %s", target.key.c_str());
    }
    return queued;
}

std::string url_query_escape(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') ||
                                (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') ||
                                ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(kHex[(ch >> 4) & 0x0F]);
            output.push_back(kHex[ch & 0x0F]);
        }
    }
    return output;
}

std::string image_url_with_key(const ImageFetchTarget& target, const std::string& apiKey) {
    if (target.url.empty()) {
        return "";
    }
    return target.url + (target.url.find('?') == std::string::npos ? "?key=" : "&key=") +
           url_query_escape(apiKey);
}

std::string iata_code_for_airline_icao(const std::string& code) {
    if (code == "SKW") {
        return "OO";
    }
    return "";
}

esp_err_t fetch_image_target(const ImageFetchTarget& target, const std::string& apiKey,
                             std::string& body, int* status, std::string* endpointUsed) {
    body.clear();
    if (status != nullptr) {
        *status = 0;
    }
    if (endpointUsed != nullptr) {
        *endpointUsed = target.livery ? "livery-query-key" : "logo-query-key";
    }

    return http_get(image_url_with_key(target, apiKey).c_str(),
                    kLogoPngMaxBytes, body, status);
}

std::string logo_url_for_airline_icao(const std::string& code) {
    const std::string iata = iata_code_for_airline_icao(code);
    if (!iata.empty()) {
        return std::string(kLogoIataApiBaseUrl) + iata +
               "?variant=logo-bg-white&format=png&size=84";
    }
    return std::string(kLogoApiBaseUrl) + code +
           "?variant=logo-bg-white&format=png&size=84";
}

std::string livery_url_for_airline_icao(const std::string& code, const std::string& iataType) {
    return std::string(kLiveryApiBaseUrl) + code + "?type=" + iataType + "&size=128";
}

ImageFetchTarget image_target_for_aircraft(const Aircraft& aircraft, bool preferLivery) {
    ImageFetchTarget target;
    target.airlineIcao = adsb::airlineIcaoFromCallsign(aircraft.normalizedCallsign.empty()
        ? aircraft.callsign
        : aircraft.normalizedCallsign);
    if (target.airlineIcao.empty()) {
        return target;
    }

    target.iataType = adsb::iataTypeFromIcaoType(aircraft.typeCode);
    if (preferLivery && !target.iataType.empty()) {
        target.livery = true;
        target.key = "L:" + target.airlineIcao + ":" + target.iataType;
        target.url = livery_url_for_airline_icao(target.airlineIcao, target.iataType);
    } else {
        target.key = "A:" + target.airlineIcao;
        target.url = logo_url_for_airline_icao(target.airlineIcao);
    }
    return target;
}

ImageFetchTarget airline_logo_target_for_icao(const char* code) {
    ImageFetchTarget target;
    if (code == nullptr || strlen(code) != 3) {
        return target;
    }
    target.airlineIcao = code;
    target.key = "A:" + target.airlineIcao;
    target.url = logo_url_for_airline_icao(target.airlineIcao);
    target.livery = false;
    return target;
}

bool logo_cache_should_skip(const ImageFetchTarget& target, int64_t now, bool* missingOut = nullptr) {
    const bool cached = target.key == g_logoCode &&
                        now - g_logoLastSuccessMs < kLogoSuccessRefreshMs;
    const bool pending = g_pendingLogoReady && target.key == g_pendingLogoCode;
    const bool missing = target.key == g_missingLogoCode && now < g_logoRetryAfterMs;
    if (missingOut != nullptr) {
        *missingOut = missing;
    }
    return cached || pending || missing;
}

void airline_logo_task(void*) {
    int64_t lastLookupMs = -kLogoLookupMinIntervalMs;
    int64_t quotaWindowStartMs = now_ms();
    uint16_t lookupsThisWindow = 0;

    for (;;) {
        const device_network::Snapshot network = device_network::snapshot();
        if (!network.stationConnected) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const std::string apiKey = settings.getLogostreamApiKey();
        if (apiKey.empty()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        Aircraft nearest;
        bool hasNearest = false;
        const int64_t now = now_ms();
        if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_aircraftStore.purgeStale(now);
            hasNearest = g_aircraftStore.snapshot(&nearest, 1, now) == 1;
            xSemaphoreGive(g_aircraftMutex);
        }
        if (!hasNearest) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const ImageFetchTarget preferredTarget = image_target_for_aircraft(nearest, true);
        const ImageFetchTarget fallbackTarget = image_target_for_aircraft(nearest, false);
        if (preferredTarget.key.empty() && fallbackTarget.key.empty()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!fallbackTarget.key.empty() && load_logo_cache_for_display(fallbackTarget, now)) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ImageFetchTarget target = preferredTarget.key.empty() ? fallbackTarget : preferredTarget;
        bool skip = false;
        bool missing = false;
        if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            skip = logo_cache_should_skip(target, now, &missing);
            xSemaphoreGive(g_logoMutex);
        }
        if (skip && missing && target.livery) {
            target = fallbackTarget;
            if (target.key.empty()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                skip = logo_cache_should_skip(target, now, nullptr);
                xSemaphoreGive(g_logoMutex);
            }
        }
        if (skip) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (now - lastLookupMs < kLogoLookupMinIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (now - quotaWindowStartMs >= kLogoQuotaWindowMs) {
            quotaWindowStartMs = now;
            lookupsThisWindow = 0;
        }
        if (lookupsThisWindow >= kLogoDailyLookupLimit) {
            vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
            continue;
        }

        std::string body;
        int status = 0;
        lastLookupMs = now_ms();
        ++lookupsThisWindow;
        std::string endpointUsed;
        esp_err_t err = fetch_image_target(target, apiKey, body, &status, &endpointUsed);
        if (target.livery && !(err == ESP_OK && status == 200 && looks_like_png(body))) {
            if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_missingLogoCode = target.key;
                g_logoRetryAfterMs = now_ms() + kLogoMissingRetryMs;
                xSemaphoreGive(g_logoMutex);
            }
            ESP_LOGW(TAG, "livery lookup failed for %s/%s: %s status=%d; trying logo fallback",
                     target.airlineIcao.c_str(), target.iataType.c_str(), esp_err_to_name(err), status);
            target = image_target_for_aircraft(nearest, false);
            body.clear();
            status = 0;
            if (target.key.empty()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            bool fallbackSkip = false;
            if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                fallbackSkip = logo_cache_should_skip(target, now_ms(), nullptr);
                xSemaphoreGive(g_logoMutex);
            }
            if (!fallbackSkip && load_logo_cache_for_display(target, now_ms())) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (fallbackSkip) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (lookupsThisWindow >= kLogoDailyLookupLimit) {
                vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            {
                ++lookupsThisWindow;
                endpointUsed.clear();
                err = fetch_image_target(target, apiKey, body, &status, &endpointUsed);
            }
        }
        const int64_t responseNow = now_ms();
        if (err == ESP_OK && status == 200 && looks_like_png(body)) {
            if (!target.livery) {
                write_logo_to_cache(target, body);
            }
            queue_logo_for_display(target, body, responseNow);
            ESP_LOGI(TAG, "%s image cached for %s",
                     target.livery ? "livery" : "airline logo",
                     target.key.c_str());
        } else {
            if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_missingLogoCode = target.key;
                g_logoRetryAfterMs = responseNow + kLogoMissingRetryMs;
                xSemaphoreGive(g_logoMutex);
            }
            ESP_LOGW(TAG, "image lookup failed for %s: %s status=%d",
                     target.key.c_str(), esp_err_to_name(err), status);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void logo_prefetch_task(void*) {
    size_t nextIndex = 0;
    int64_t quotaWindowStartMs = now_ms();
    uint16_t lookupsThisWindow = 0;

    for (;;) {
        const device_network::Snapshot network = device_network::snapshot();
        const std::string apiKey = settings.getLogostreamApiKey();
        if (!network.stationConnected || apiKey.empty() || !ensure_logo_cache_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const int64_t now = now_ms();
        if (now - quotaWindowStartMs >= kLogoQuotaWindowMs) {
            quotaWindowStartMs = now;
            lookupsThisWindow = 0;
        }
        if (lookupsThisWindow >= kLogoDailyLookupLimit) {
            vTaskDelay(pdMS_TO_TICKS(kLogoPrefetchIdleMs));
            continue;
        }

        bool fetched = false;
        for (size_t attempt = 0; attempt < sizeof(kCommonAirlineIcao) / sizeof(kCommonAirlineIcao[0]); ++attempt) {
            const char* code = kCommonAirlineIcao[nextIndex];
            nextIndex = (nextIndex + 1) % (sizeof(kCommonAirlineIcao) / sizeof(kCommonAirlineIcao[0]));

            ImageFetchTarget target = airline_logo_target_for_icao(code);
            if (target.key.empty() || logo_cache_file_exists(target.key)) {
                continue;
            }

            std::string body;
            int status = 0;
            ++lookupsThisWindow;
            std::string endpointUsed;
            const esp_err_t err = fetch_image_target(target, apiKey, body, &status, &endpointUsed);
            const bool png = looks_like_png(body);
            const bool stored = err == ESP_OK && status == 200 && png;
            if (stored) {
                write_logo_to_cache(target, body);
                ESP_LOGI(TAG, "prefetched common airline logo for %s", target.key.c_str());
            } else {
                ESP_LOGW(TAG, "common logo prefetch failed for %s: %s status=%d",
                         target.key.c_str(), esp_err_to_name(err), status);
            }
            if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_lastLogoPrefetchKey = target.key;
                g_lastLogoPrefetchEndpoint = endpointUsed;
                g_lastLogoPrefetchSignature = response_signature(body);
                g_lastLogoPrefetchMs = now_ms();
                g_lastLogoPrefetchError = err;
                g_lastLogoPrefetchStatus = status;
                g_lastLogoPrefetchBytes = body.size();
                g_lastLogoPrefetchPng = png;
                g_lastLogoPrefetchStored = stored;
                xSemaphoreGive(g_logoMutex);
            }
            fetched = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(fetched ? kLogoPrefetchIntervalMs : kLogoPrefetchIdleMs));
    }
}

std::string json_escape_text(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char ch : input) {
        if (ch == '"' || ch == '\\') {
            output.push_back('\\');
            output.push_back(ch);
        } else if (ch == '\n') {
            output += "\\n";
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

void write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

void write_i32_le(uint8_t* dst, int32_t value) {
    write_u32_le(dst, static_cast<uint32_t>(value));
}

esp_err_t handle_debug_logo(httpd_req_t* req) {
    Aircraft nearest;
    bool hasNearest = false;
    const int64_t now = now_ms();
    if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_aircraftStore.purgeStale(now);
        hasNearest = g_aircraftStore.snapshot(&nearest, 1, now) == 1;
        xSemaphoreGive(g_aircraftMutex);
    }

    const ImageFetchTarget preferred = hasNearest ? image_target_for_aircraft(nearest, true) : ImageFetchTarget{};
    const ImageFetchTarget fallback = hasNearest ? image_target_for_aircraft(nearest, false) : ImageFetchTarget{};
    std::string cachedKey;
    std::string pendingKey;
    std::string missingKey;
    std::string prefetchKey;
    std::string prefetchEndpoint;
    std::string prefetchSignature;
    size_t cachedBytes = 0;
    size_t prefetchBytes = 0;
    uint16_t cachedWidth = 0;
    uint16_t cachedHeight = 0;
    lv_coord_t objectWidth = 0;
    lv_coord_t objectHeight = 0;
    uint16_t objectZoom = 0;
    bool objectHidden = true;
    const bool persistentPreferred = logo_cache_file_exists(preferred.key);
    const bool persistentFallback = logo_cache_file_exists(fallback.key);
    const bool persistentSkywestCached = logo_cache_file_exists("A:SKW");
    size_t persistentCommonCached = 0;
    for (const char* code : kCommonAirlineIcao) {
        std::string key = "A:";
        key += code;
        if (logo_cache_file_exists(key)) {
            ++persistentCommonCached;
        }
    }
    const size_t persistentCommonTotal = sizeof(kCommonAirlineIcao) / sizeof(kCommonAirlineIcao[0]);
    const size_t persistentCount = logo_cache_entry_count();
    bool pending = false;
    bool prefetchPng = false;
    bool prefetchStored = false;
    int64_t retryAfterMs = 0;
    int64_t lastSuccessAgeMs = -1;
    int64_t lastPrefetchAgeMs = -1;
    esp_err_t prefetchErr = ESP_OK;
    int prefetchStatus = 0;
    if (xSemaphoreTake(g_logoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        cachedKey = g_logoCode;
        pendingKey = g_pendingLogoCode;
        missingKey = g_missingLogoCode;
        prefetchKey = g_lastLogoPrefetchKey;
        prefetchEndpoint = g_lastLogoPrefetchEndpoint;
        prefetchSignature = g_lastLogoPrefetchSignature;
        cachedBytes = g_logoDescriptor.data_size;
        prefetchBytes = g_lastLogoPrefetchBytes;
        cachedWidth = g_logoImageWidth;
        cachedHeight = g_logoImageHeight;
        pending = g_pendingLogoReady;
        prefetchPng = g_lastLogoPrefetchPng;
        prefetchStored = g_lastLogoPrefetchStored;
        retryAfterMs = g_logoRetryAfterMs;
        prefetchErr = g_lastLogoPrefetchError;
        prefetchStatus = g_lastLogoPrefetchStatus;
        if (g_logoLastSuccessMs > 0) {
            lastSuccessAgeMs = now - g_logoLastSuccessMs;
        }
        if (g_lastLogoPrefetchMs > 0) {
            lastPrefetchAgeMs = now - g_lastLogoPrefetchMs;
        }
        xSemaphoreGive(g_logoMutex);
    }
    if (g_nearestLogo != nullptr && g_nearestLogoFrame != nullptr) {
        objectWidth = lv_obj_get_width(g_nearestLogo);
        objectHeight = lv_obj_get_height(g_nearestLogo);
        objectZoom = lv_img_get_zoom(g_nearestLogo);
        objectHidden = lv_obj_has_flag(g_nearestLogoFrame, LV_OBJ_FLAG_HIDDEN);
    }

    std::string body = "{";
    body += "\"api_key_configured\":" + std::string(settings.hasLogostreamApiKey() ? "true" : "false") + ",";
    body += "\"has_nearest\":" + std::string(hasNearest ? "true" : "false") + ",";
    body += "\"nearest_callsign\":\"" + json_escape_text(hasNearest ? label_for_aircraft(nearest) : "") + "\",";
    body += "\"nearest_type_code\":\"" + json_escape_text(hasNearest ? nearest.typeCode : "") + "\",";
    body += "\"preferred_key\":\"" + json_escape_text(preferred.key) + "\",";
    body += "\"preferred_livery\":" + std::string(preferred.livery ? "true" : "false") + ",";
    body += "\"fallback_key\":\"" + json_escape_text(fallback.key) + "\",";
    body += "\"cached_key\":\"" + json_escape_text(cachedKey) + "\",";
    body += "\"cached_bytes\":" + std::to_string(cachedBytes) + ",";
    body += "\"cached_width\":" + std::to_string(cachedWidth) + ",";
    body += "\"cached_height\":" + std::to_string(cachedHeight) + ",";
    body += "\"object_width\":" + std::to_string(objectWidth) + ",";
    body += "\"object_height\":" + std::to_string(objectHeight) + ",";
    body += "\"object_zoom\":" + std::to_string(objectZoom) + ",";
    body += "\"object_hidden\":" + std::string(objectHidden ? "true" : "false") + ",";
    body += "\"persistent_cache_mounted\":" + std::string(g_logoCacheMounted ? "true" : "false") + ",";
    body += "\"persistent_cache_entries\":" + std::to_string(persistentCount) + ",";
    body += "\"persistent_preferred_cached\":" + std::string(persistentPreferred ? "true" : "false") + ",";
    body += "\"persistent_fallback_cached\":" + std::string(persistentFallback ? "true" : "false") + ",";
    body += "\"persistent_skywest_cached\":" + std::string(persistentSkywestCached ? "true" : "false") + ",";
    body += "\"persistent_common_cached\":" + std::to_string(persistentCommonCached) + ",";
    body += "\"persistent_common_total\":" + std::to_string(persistentCommonTotal) + ",";
    body += "\"pending\":" + std::string(pending ? "true" : "false") + ",";
    body += "\"pending_key\":\"" + json_escape_text(pendingKey) + "\",";
    body += "\"missing_key\":\"" + json_escape_text(missingKey) + "\",";
    body += "\"retry_after_ms\":" + std::to_string(retryAfterMs) + ",";
    body += "\"last_success_age_ms\":" + std::to_string(lastSuccessAgeMs) + ",";
    body += "\"last_prefetch_key\":\"" + json_escape_text(prefetchKey) + "\",";
    body += "\"last_prefetch_endpoint\":\"" + json_escape_text(prefetchEndpoint) + "\",";
    body += "\"last_prefetch_signature\":\"" + json_escape_text(prefetchSignature) + "\",";
    body += "\"last_prefetch_error\":\"" + json_escape_text(esp_err_to_name(prefetchErr)) + "\",";
    body += "\"last_prefetch_status\":" + std::to_string(prefetchStatus) + ",";
    body += "\"last_prefetch_bytes\":" + std::to_string(prefetchBytes) + ",";
    body += "\"last_prefetch_png\":" + std::string(prefetchPng ? "true" : "false") + ",";
    body += "\"last_prefetch_stored\":" + std::string(prefetchStored ? "true" : "false") + ",";
    body += "\"last_prefetch_age_ms\":" + std::to_string(lastPrefetchAgeMs) + ",";
    const bool displayable = settings.hasLogostreamApiKey() &&
                             cachedBytes > 0 &&
                             (cachedKey == preferred.key || cachedKey == fallback.key);
    body += "\"displayable\":" + std::string(displayable ? "true" : "false");
    body += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t handle_debug_adsb(httpd_req_t* req) {
    const int64_t now = now_ms();
    size_t activeCount = 0;
    size_t maxRangeCount = 0;
    if (xSemaphoreTake(g_aircraftMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_aircraftStore.purgeStale(now);
        activeCount = g_aircraftStore.activeCount(now);
        maxRangeCount = g_aircraftStore.rangeCount(miles_to_nm(settings.getRadarRangeMiles()), now);
        xSemaphoreGive(g_aircraftMutex);
    }

    size_t bytes = 0;
    size_t parsed = 0;
    int status = 0;
    esp_err_t error = ESP_OK;
    int64_t ageMs = -1;
    std::string feederUrl;
    if (g_httpStatsMutex != nullptr &&
        xSemaphoreTake(g_httpStatsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bytes = g_lastHttpAircraftBytes;
        parsed = g_lastHttpParsedAircraft;
        status = g_lastHttpStatus;
        error = g_lastHttpError;
        if (g_lastHttpFetchMs > 0) {
            ageMs = now - g_lastHttpFetchMs;
        }
        feederUrl = g_lastHttpFeederUrl;
        xSemaphoreGive(g_httpStatsMutex);
    }

    std::string body = "{";
    body += "\"feeder_url\":\"" + json_escape_text(feederUrl) + "\",";
    body += "\"last_fetch_age_ms\":" + std::to_string(ageMs) + ",";
    body += "\"last_http_status\":" + std::to_string(status) + ",";
    body += "\"last_http_error\":\"" + json_escape_text(esp_err_to_name(error)) + "\",";
    body += "\"last_response_bytes\":" + std::to_string(bytes) + ",";
    body += "\"last_parsed_aircraft\":" + std::to_string(parsed) + ",";
    body += "\"tracked_aircraft\":" + std::to_string(activeCount) + ",";
    body += "\"configured_range_miles\":" + std::to_string(settings.getRadarRangeMiles()) + ",";
    body += "\"tracked_in_configured_range\":" + std::to_string(maxRangeCount);
    body += "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t handle_debug_screenshot(httpd_req_t* req) {
    enum {
        BMP_FILE_HEADER_SIZE = 14,
        BMP_INFO_HEADER_SIZE = 40,
        BMP_HEADER_SIZE = BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE,
    };

    void* fb1Raw = nullptr;
    void* fb2Raw = nullptr;
    platform::getFrameBuffer(&fb1Raw, &fb2Raw);
    const lv_color_t* pixels = static_cast<const lv_color_t*>(fb1Raw);
    if (pixels == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Framebuffer is unavailable");
        return ESP_FAIL;
    }

    const uint16_t width = cfg::kDisplayWidth;
    const uint16_t height = cfg::kDisplayHeight;
    const size_t rowStride = static_cast<size_t>(width) * 3U;
    const size_t rowStridePadded = (rowStride + 3U) & ~static_cast<size_t>(3U);
    const uint32_t pixelDataSize = static_cast<uint32_t>(rowStridePadded * static_cast<size_t>(height));
    const uint32_t fileSize = BMP_HEADER_SIZE + pixelDataSize;
    uint8_t* bmp = static_cast<uint8_t*>(heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bmp == nullptr) {
        bmp = static_cast<uint8_t*>(malloc(fileSize));
    }
    if (bmp == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    memset(bmp, 0, fileSize);
    uint8_t* header = bmp;
    header[0] = 'B';
    header[1] = 'M';
    write_u32_le(&header[2], fileSize);
    write_u32_le(&header[10], BMP_HEADER_SIZE);
    write_u32_le(&header[14], BMP_INFO_HEADER_SIZE);
    write_i32_le(&header[18], width);
    write_i32_le(&header[22], height);
    write_u16_le(&header[26], 1);
    write_u16_le(&header[28], 24);
    write_u32_le(&header[34], pixelDataSize);

    for (int32_t y = height - 1; y >= 0; --y) {
        uint8_t* row = bmp + BMP_HEADER_SIZE +
                       (static_cast<size_t>(height - 1 - y) * rowStridePadded);
        for (int32_t x = 0; x < width; ++x) {
            lv_color32_t color32 = {};
            color32.full = lv_color_to32(pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)]);
            const size_t offset = static_cast<size_t>(x) * 3U;
            row[offset + 0] = color32.ch.blue;
            row[offset + 1] = color32.ch.green;
            row[offset + 2] = color32.ch.red;
        }
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"flightsabove-screenshot.bmp\"");
    esp_err_t ret = httpd_resp_send(req, reinterpret_cast<const char*>(bmp), fileSize);
    free(bmp);
    return ret;
}

void register_debug_routes() {
    xTaskCreatePinnedToCore([](void*) {
        const esp_err_t logoErr = device_network::registerGetHandler("/debug/logo", handle_debug_logo);
        const esp_err_t adsbErr = device_network::registerGetHandler("/debug/adsb", handle_debug_adsb);
        const esp_err_t screenshotErr = device_network::registerGetHandler("/debug/screenshot.bmp", handle_debug_screenshot);
        for (;;) {
            const bool logoReady = logoErr == ESP_OK || logoErr == ESP_ERR_HTTPD_HANDLER_EXISTS;
            const bool adsbReady = adsbErr == ESP_OK || adsbErr == ESP_ERR_HTTPD_HANDLER_EXISTS;
            const bool screenshotReady = screenshotErr == ESP_OK ||
                                         screenshotErr == ESP_ERR_HTTPD_HANDLER_EXISTS;
            if (logoReady && adsbReady && screenshotReady) {
                ESP_LOGI(TAG, "debug routes registered");
                vTaskDelete(nullptr);
                return;
            }
            const esp_err_t retryLogoErr = device_network::registerGetHandler("/debug/logo", handle_debug_logo);
            const esp_err_t retryAdsbErr = device_network::registerGetHandler("/debug/adsb", handle_debug_adsb);
            const esp_err_t retryScreenshotErr = device_network::registerGetHandler("/debug/screenshot.bmp", handle_debug_screenshot);
            if ((retryLogoErr == ESP_OK || retryLogoErr == ESP_ERR_HTTPD_HANDLER_EXISTS) &&
                (retryAdsbErr == ESP_OK || retryAdsbErr == ESP_ERR_HTTPD_HANDLER_EXISTS) &&
                (retryScreenshotErr == ESP_OK || retryScreenshotErr == ESP_ERR_HTTPD_HANDLER_EXISTS)) {
                ESP_LOGI(TAG, "debug routes registered");
                vTaskDelete(nullptr);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }, "debug_routes", 4096, nullptr, 2, nullptr, 0);
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
    ensure_logo_cache_mounted();

    g_aircraftMutex = xSemaphoreCreateMutex();
    configASSERT(g_aircraftMutex != nullptr);
    g_routeCacheMutex = xSemaphoreCreateMutex();
    configASSERT(g_routeCacheMutex != nullptr);
    g_logoMutex = xSemaphoreCreateMutex();
    configASSERT(g_logoMutex != nullptr);
    g_httpStatsMutex = xSemaphoreCreateMutex();
    configASSERT(g_httpStatsMutex != nullptr);
    g_lvglMutex = xSemaphoreCreateMutex();
    configASSERT(g_lvglMutex != nullptr);

    device_network::begin();
    init_adsb_uart();
    xTaskCreatePinnedToCore(adsb_task, "adsb_uart", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(adsb_http_task, "adsb_http", 12288, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(route_lookup_task, "route_lookup", 12288, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(airline_logo_task, "airline_logo", 12288, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(logo_prefetch_task, "logo_prefetch", 12288, nullptr, 1, nullptr, 1);

    init_lvgl();
    register_debug_routes();
    for (;;) {
        if (xSemaphoreTake(g_lvglMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(g_lvglMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
