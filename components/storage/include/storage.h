#pragma once

#include <stdint.h>
#include <string>

struct Settings {
    std::string wifiSsid;
    std::string wifiPass;
    std::string feederUrl;
    std::string logostreamApiKey;
    double receiverLatitude = 0.0;
    double receiverLongitude = 0.0;
    uint16_t displaySleepMin = 0;
    uint16_t radarRangeMiles = 0;

    void load();
    void save();

    void getWifi(std::string& ssid, std::string& pass);
    void setWifi(const std::string& ssid, const std::string& pass);

    std::string getFeederUrl();
    void setFeederUrl(const std::string& url);

    bool hasLogostreamApiKey();
    std::string getLogostreamApiKey();
    void setLogostreamApiKey(const std::string& apiKey);

    void setReceiverLocation(double latitude, double longitude);
    double getReceiverLatitude();
    double getReceiverLongitude();

    uint16_t getDisplaySleepMin();
    void setDisplaySleepMin(uint16_t minutes);

    uint16_t getRadarRangeMiles();
    void setRadarRangeMiles(uint16_t miles);

    void factoryReset();

};

extern Settings settings;
