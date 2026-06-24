#include "storage.h"

#include "flights_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cmath>

Settings settings;

namespace {

SemaphoreHandle_t s_mutex = nullptr;
constexpr double kLegacyDefaultLatitude = 37.62131;
constexpr double kLegacyDefaultLongitude = -122.37896;

void take() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void give() {
    xSemaphoreGive(s_mutex);
}

std::string get_string(nvs_handle_t nvs, const char* key, const char* fallback) {
    size_t length = 0;
    esp_err_t err = nvs_get_str(nvs, key, nullptr, &length);
    if (err != ESP_OK || length == 0) {
        return fallback ? fallback : "";
    }

    std::string value(length, '\0');
    err = nvs_get_str(nvs, key, value.data(), &length);
    if (err != ESP_OK) {
        return fallback ? fallback : "";
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

double get_double(nvs_handle_t nvs, const char* key, double fallback) {
    int64_t scaled = 0;
    if (nvs_get_i64(nvs, key, &scaled) != ESP_OK) {
        return fallback;
    }
    return static_cast<double>(scaled) / 1000000.0;
}

void set_double(nvs_handle_t nvs, const char* key, double value) {
    nvs_set_i64(nvs, key, static_cast<int64_t>(value * 1000000.0));
}

}  // namespace

void Settings::load() {
    take();
    nvs_handle_t nvs = 0;
    if (nvs_open("flights", NVS_READWRITE, &nvs) == ESP_OK) {
        wifiSsid = get_string(nvs, "wifi_ssid", "");
        wifiPass = get_string(nvs, "wifi_pass", "");
        feederUrl = get_string(nvs, "feeder_url", cfg::kDefaultFeederUrl);
        receiverLatitude = get_double(nvs, "rx_lat", cfg::kDefaultReceiverLatitude);
        receiverLongitude = get_double(nvs, "rx_lon", cfg::kDefaultReceiverLongitude);
        if (std::abs(receiverLatitude - kLegacyDefaultLatitude) < 0.000001 &&
            std::abs(receiverLongitude - kLegacyDefaultLongitude) < 0.000001) {
            receiverLatitude = cfg::kDefaultReceiverLatitude;
            receiverLongitude = cfg::kDefaultReceiverLongitude;
        }
        uint16_t sleep = 0;
        displaySleepMin = nvs_get_u16(nvs, "sleep_min", &sleep) == ESP_OK ? sleep : 0;
        uint16_t range = 0;
        radarRangeMiles = nvs_get_u16(nvs, "radar_miles", &range) == ESP_OK
            ? std::clamp<uint16_t>(range, cfg::kMinRadarRangeMiles, cfg::kMaxRadarRangeMiles)
            : cfg::kDefaultRadarRangeMiles;
        nvs_close(nvs);
    } else {
        wifiSsid = "";
        wifiPass = "";
        feederUrl = cfg::kDefaultFeederUrl;
        receiverLatitude = cfg::kDefaultReceiverLatitude;
        receiverLongitude = cfg::kDefaultReceiverLongitude;
        displaySleepMin = 0;
        radarRangeMiles = cfg::kDefaultRadarRangeMiles;
    }
    give();
}

void Settings::save() {
    take();
    nvs_handle_t nvs = 0;
    if (nvs_open("flights", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "wifi_ssid", wifiSsid.c_str());
        nvs_set_str(nvs, "wifi_pass", wifiPass.c_str());
        nvs_set_str(nvs, "feeder_url", feederUrl.c_str());
        set_double(nvs, "rx_lat", receiverLatitude);
        set_double(nvs, "rx_lon", receiverLongitude);
        nvs_set_u16(nvs, "sleep_min", displaySleepMin);
        nvs_set_u16(nvs, "radar_miles", radarRangeMiles);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    give();
}

void Settings::getWifi(std::string& ssid, std::string& pass) {
    take();
    ssid = wifiSsid;
    pass = wifiPass;
    give();
}

void Settings::setWifi(const std::string& ssid, const std::string& pass) {
    take();
    wifiSsid = ssid;
    wifiPass = pass;
    give();
    save();
}

std::string Settings::getFeederUrl() {
    take();
    std::string value = feederUrl.empty() ? cfg::kDefaultFeederUrl : feederUrl;
    give();
    return value;
}

void Settings::setFeederUrl(const std::string& url) {
    take();
    feederUrl = url.empty() ? cfg::kDefaultFeederUrl : url;
    give();
    save();
}

void Settings::setReceiverLocation(double latitude, double longitude) {
    take();
    receiverLatitude = latitude;
    receiverLongitude = longitude;
    give();
    save();
}

double Settings::getReceiverLatitude() {
    take();
    double value = receiverLatitude;
    give();
    return value;
}

double Settings::getReceiverLongitude() {
    take();
    double value = receiverLongitude;
    give();
    return value;
}

uint16_t Settings::getDisplaySleepMin() {
    take();
    uint16_t value = displaySleepMin;
    give();
    return value;
}

void Settings::setDisplaySleepMin(uint16_t minutes) {
    take();
    displaySleepMin = std::min<uint16_t>(minutes, 120);
    give();
    save();
}

uint16_t Settings::getRadarRangeMiles() {
    take();
    uint16_t value = radarRangeMiles == 0 ? cfg::kDefaultRadarRangeMiles : radarRangeMiles;
    give();
    return value;
}

void Settings::setRadarRangeMiles(uint16_t miles) {
    take();
    radarRangeMiles = std::clamp<uint16_t>(miles,
                                           cfg::kMinRadarRangeMiles,
                                           cfg::kMaxRadarRangeMiles);
    give();
    save();
}

void Settings::factoryReset() {
    take();
    nvs_handle_t nvs = 0;
    if (nvs_open("flights", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    wifiSsid = "";
    wifiPass = "";
    feederUrl = cfg::kDefaultFeederUrl;
    receiverLatitude = cfg::kDefaultReceiverLatitude;
    receiverLongitude = cfg::kDefaultReceiverLongitude;
    displaySleepMin = 0;
    radarRangeMiles = cfg::kDefaultRadarRangeMiles;
    give();
}
