#include "device_network.h"

#include "flights_config.h"
#include "setup_portal.h"
#include "storage.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace device_network {
namespace {

const char* TAG = "device_network";

httpd_handle_t s_server = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
Snapshot s_snapshot;
esp_netif_t* s_staNetif = nullptr;
esp_netif_t* s_apNetif = nullptr;
int64_t s_restartAtMs = 0;

int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

void lock() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void unlock() {
    xSemaphoreGive(s_mutex);
}

void copy_text(char* dest, size_t destSize, const std::string& value) {
    if (destSize == 0) {
        return;
    }
    strlcpy(dest, value.c_str(), destSize);
}

void copy_text(char* dest, size_t destSize, const char* value) {
    if (destSize == 0) {
        return;
    }
    strlcpy(dest, value ? value : "", destSize);
}

std::string json_escape(const char* input) {
    std::string output;
    if (!input) {
        return output;
    }
    for (const char* p = input; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            output.push_back('\\');
            output.push_back(*p);
        } else if (*p == '\n') {
            output += "\\n";
        } else {
            output.push_back(*p);
        }
    }
    return output;
}

void update_setup_ip_locked() {
    if (!s_apNetif) {
        return;
    }
    esp_netif_ip_info_t ipInfo = {};
    if (esp_netif_get_ip_info(s_apNetif, &ipInfo) == ESP_OK) {
        esp_ip4addr_ntoa(&ipInfo.ip, s_snapshot.setupIp, sizeof(s_snapshot.setupIp));
        snprintf(s_snapshot.setupUrl, sizeof(s_snapshot.setupUrl), "http://%s/", s_snapshot.setupIp);
        s_snapshot.setupApActive = true;
    }
}

void update_station_rssi_locked() {
    if (!s_snapshot.stationConnected) {
        s_snapshot.rssi = 0;
        return;
    }
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_snapshot.rssi = ap.rssi;
    }
}

void update_snapshot() {
    lock();
    update_setup_ip_locked();
    update_station_rssi_locked();
    unlock();
}

esp_err_t send_text(httpd_req_t* req, int status, const char* type, const std::string& body) {
    const char* statusLine = "500 Internal Server Error";
    switch (status) {
    case 200:
        statusLine = "200 OK";
        break;
    case 400:
        statusLine = "400 Bad Request";
        break;
    case 404:
        statusLine = "404 Not Found";
        break;
    case 500:
        statusLine = "500 Internal Server Error";
        break;
    default:
        break;
    }
    httpd_resp_set_status(req, statusLine);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t redirect_root(httpd_req_t* req) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "");
}

std::string read_body(httpd_req_t* req, size_t maxBytes) {
    const size_t contentLen = static_cast<size_t>(req->content_len);
    if (contentLen > maxBytes) {
        return "";
    }

    std::string body;
    body.resize(contentLen);
    size_t received = 0;
    while (received < contentLen) {
        const int ret = httpd_req_recv(req, body.data() + received, contentLen - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            body.clear();
            break;
        }
        received += static_cast<size_t>(ret);
    }
    return body;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            output.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            }
        } else {
            output.push_back(value[i]);
        }
    }
    return output;
}

std::string form_value(const std::string& body, const char* key) {
    const std::string prefix = std::string(key) + "=";
    size_t start = 0;
    while (start <= body.size()) {
        const size_t end = body.find('&', start);
        const std::string_view part(body.data() + start,
                                    (end == std::string::npos ? body.size() : end) - start);
        if (part.rfind(prefix, 0) == 0) {
            return url_decode(std::string(part.substr(prefix.size())));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return "";
}

void trim(std::string& value) {
    auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
}

void connect_station_from_settings() {
    std::string ssid;
    std::string pass;
    settings.getWifi(ssid, pass);
    if (ssid.empty()) {
        ESP_LOGI(TAG, "Wi-Fi station not configured");
        return;
    }

    wifi_config_t staConfig = {};
    copy_text(reinterpret_cast<char*>(staConfig.sta.ssid), sizeof(staConfig.sta.ssid), ssid);
    copy_text(reinterpret_cast<char*>(staConfig.sta.password), sizeof(staConfig.sta.password), pass);
    staConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (pass.empty()) {
        staConfig.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &staConfig));
    ESP_LOGI(TAG, "connecting station to %s", ssid.c_str());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

void wifi_event_handler(void*, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
        lock();
        s_snapshot.stationConnected = false;
        s_snapshot.stationIp[0] = '\0';
        s_snapshot.rssi = 0;
        unlock();
        ESP_LOGI(TAG, "station disconnected; retrying");
        esp_wifi_connect();
    } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_AP_START) {
        update_snapshot();
    } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(eventData);
        lock();
        s_snapshot.stationConnected = true;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_snapshot.stationIp, sizeof(s_snapshot.stationIp));
        std::string ssid;
        std::string pass;
        settings.getWifi(ssid, pass);
        copy_text(s_snapshot.stationSsid, sizeof(s_snapshot.stationSsid), ssid);
        update_station_rssi_locked();
        unlock();
        ESP_LOGI(TAG, "station connected at %s", s_snapshot.stationIp);
    }
}

esp_err_t handle_root(httpd_req_t* req) {
    const std::string page = setup_portal::renderPage("");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), page.size());
}

esp_err_t handle_status(httpd_req_t* req) {
    update_snapshot();
    const esp_app_desc_t* app = esp_app_get_description();
    const Snapshot snap = snapshot();

    std::string json = "{";
    json += "\"device\":\"" + std::string(cfg::kDeviceName) + "\",";
    json += "\"firmware\":\"" + json_escape(app ? app->version : "unknown") + "\",";
    json += "\"wifi\":\"" + json_escape(snap.stationConnected ? snap.stationSsid : "Disconnected") + "\",";
    json += "\"station_ip\":\"" + json_escape(snap.stationIp) + "\",";
    json += "\"setup_ip\":\"" + json_escape(snap.setupIp) + "\",";
    json += "\"setup_url\":\"" + json_escape(snap.setupUrl) + "\",";
    json += "\"signal_dbm\":" + std::to_string(snap.rssi) + ",";
    const double mapLat = settings.getReceiverLatitude();
    const double mapLon = settings.getReceiverLongitude();
    json += "\"map_center_latitude\":" + std::to_string(mapLat) + ",";
    json += "\"map_center_longitude\":" + std::to_string(mapLon) + ",";
    json += "\"receiver_latitude\":" + std::to_string(mapLat) + ",";
    json += "\"receiver_longitude\":" + std::to_string(mapLon) + ",";
    json += "\"display_sleep_min\":" + std::to_string(settings.getDisplaySleepMin()) + ",";
    json += "\"radar_range_miles\":" + std::to_string(settings.getRadarRangeMiles()) + ",";
    const std::string feederUrl = settings.getFeederUrl();
    json += "\"feeder_url\":\"" + json_escape(feederUrl.c_str()) + "\",";
    json += "\"logostream_api_key_configured\":" + std::string(settings.hasLogostreamApiKey() ? "true" : "false") + ",";
    json += "\"free_heap\":" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)) + ",";
    json += "\"ota_running\":" + std::string(snap.otaRunning ? "true" : "false");
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t handle_save_wifi(httpd_req_t* req) {
    const std::string body = read_body(req, 2048);
    std::string ssid = form_value(body, "ssid");
    std::string pass = form_value(body, "pass");
    trim(ssid);
    if (ssid.empty()) {
        return send_text(req, 400, "text/plain", "Network name is required.");
    }

    if (pass.empty()) {
        std::string oldSsid;
        std::string oldPass;
        settings.getWifi(oldSsid, oldPass);
        if (oldSsid == ssid) {
            pass = oldPass;
        }
    }

    settings.setWifi(ssid, pass);
    esp_wifi_disconnect();
    connect_station_from_settings();
    return send_text(req, 200, "text/html",
                     "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                     "<body style='font:16px system-ui;background:#07100d;color:#f4f7f2;padding:20px'>"
                     "<h2>Wi-Fi saved</h2><p>The device is reconnecting. Return to <a href='/'>setup</a>.</p></body>");
}

esp_err_t handle_save_receiver(httpd_req_t* req) {
    const std::string body = read_body(req, 1024);
    const double lat = std::strtod(form_value(body, "lat").c_str(), nullptr);
    const double lon = std::strtod(form_value(body, "lon").c_str(), nullptr);
    const int sleep = std::atoi(form_value(body, "sleep").c_str());
    const int range = std::atoi(form_value(body, "range").c_str());
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return send_text(req, 400, "text/plain", "Receiver latitude or longitude is out of range.");
    }
    if (range < cfg::kMinRadarRangeMiles || range > cfg::kMaxRadarRangeMiles) {
        return send_text(req, 400, "text/plain", "Radar range is out of range.");
    }

    settings.setReceiverLocation(lat, lon);
    settings.setDisplaySleepMin(static_cast<uint16_t>(sleep < 0 ? 0 : sleep));
    settings.setRadarRangeMiles(static_cast<uint16_t>(range));
    return redirect_root(req);
}

esp_err_t handle_save_feeder(httpd_req_t* req) {
    const std::string body = read_body(req, 2048);
    std::string url = form_value(body, "url");
    trim(url);
    if (url.rfind("http://", 0) != 0) {
        return send_text(req, 400, "text/plain", "Feeder URL must start with http://.");
    }
    settings.setFeederUrl(url);
    return redirect_root(req);
}

esp_err_t handle_save_logostream(httpd_req_t* req) {
    const std::string body = read_body(req, 2048);
    std::string key = form_value(body, "key");
    const bool clear = form_value(body, "clear") == "1";
    trim(key);

    if (clear) {
        settings.setLogostreamApiKey("");
        return redirect_root(req);
    }

    if (!key.empty()) {
        if (key.size() > 512) {
            return send_text(req, 400, "text/plain", "Logostream API key is too long.");
        }
        settings.setLogostreamApiKey(key);
    }

    return redirect_root(req);
}

esp_err_t handle_ota(httpd_req_t* req) {
    if (req->content_len <= 0) {
        return send_text(req, 400, "text/plain", "Firmware body is empty.");
    }

    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (!updatePartition) {
        return send_text(req, 500, "text/plain", "No OTA update partition is available.");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(updatePartition, req->content_len, &ota);
    if (err != ESP_OK) {
        return send_text(req, 500, "text/plain", std::string("OTA begin failed: ") + esp_err_to_name(err));
    }

    lock();
    s_snapshot.otaRunning = true;
    unlock();

    char buffer[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        const int received = httpd_req_recv(req, buffer, std::min<int>(remaining, sizeof(buffer)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota);
            lock();
            s_snapshot.otaRunning = false;
            unlock();
            return send_text(req, 500, "text/plain", "Firmware upload failed.");
        }
        err = esp_ota_write(ota, buffer, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            lock();
            s_snapshot.otaRunning = false;
            unlock();
            return send_text(req, 500, "text/plain", std::string("OTA write failed: ") + esp_err_to_name(err));
        }
        remaining -= received;
    }

    err = esp_ota_end(ota);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(updatePartition);
    }

    lock();
    s_snapshot.otaRunning = false;
    unlock();

    if (err != ESP_OK) {
        return send_text(req, 500, "text/plain", std::string("OTA finalize failed: ") + esp_err_to_name(err));
    }

    requestRestart(800);
    return send_text(req, 200, "text/plain", "Firmware updated. FlightsAbove is rebooting.");
}

esp_err_t handle_restart(httpd_req_t* req) {
    requestRestart(500);
    return send_text(req, 200, "text/html", "<h2>Restarting FlightsAbove...</h2>");
}

esp_err_t handle_factory_reset(httpd_req_t* req) {
    settings.factoryReset();
    requestRestart(700);
    return send_text(req, 200, "text/html", "<h2>Factory reset complete. Restarting...</h2>");
}

esp_err_t handle_not_found(httpd_req_t* req, httpd_err_code_t) {
    return send_text(req, 404, "text/plain", "Not found");
}

void register_uri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t cfg = {};
    cfg.uri = uri;
    cfg.method = method;
    cfg.handler = handler;
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s_server, &cfg));
}

void start_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = cfg::kSetupPortalPort;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));
    register_uri("/", HTTP_GET, handle_root);
    register_uri("/status", HTTP_GET, handle_status);
    register_uri("/save-wifi", HTTP_POST, handle_save_wifi);
    register_uri("/save-receiver", HTTP_POST, handle_save_receiver);
    register_uri("/save-feeder", HTTP_POST, handle_save_feeder);
    register_uri("/save-logostream", HTTP_POST, handle_save_logostream);
    register_uri("/ota", HTTP_POST, handle_ota);
    register_uri("/restart", HTTP_POST, handle_restart);
    register_uri("/factory-reset", HTTP_POST, handle_factory_reset);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);
    ESP_LOGI(TAG, "setup portal listening on port %u", cfg::kSetupPortalPort);
}

void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    s_staNetif = esp_netif_create_default_wifi_sta();
    s_apNetif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_staNetif, cfg::kSetupHostname);

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t apConfig = {};
    copy_text(reinterpret_cast<char*>(apConfig.ap.ssid), sizeof(apConfig.ap.ssid), cfg::kSetupApSsid);
    apConfig.ap.ssid_len = strlen(cfg::kSetupApSsid);
    apConfig.ap.max_connection = 4;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    update_snapshot();
    ESP_LOGI(TAG, "setup AP %s at %s", cfg::kSetupApSsid, s_snapshot.setupIp);
    connect_station_from_settings();
}

void task(void*) {
    init_wifi();
    start_server();

    for (;;) {
        update_snapshot();
        if (s_restartAtMs != 0 && now_ms() >= s_restartAtMs) {
            ESP_LOGI(TAG, "restarting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

void begin() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreatePinnedToCore(task, "device_network", 8192, nullptr, 4, nullptr, 0);
}

Snapshot snapshot() {
    if (!s_mutex) {
        return Snapshot{};
    }
    lock();
    Snapshot copy = s_snapshot;
    unlock();
    return copy;
}

void requestRestart(uint32_t delayMs) {
    s_restartAtMs = now_ms() + delayMs;
}

}  // namespace device_network
