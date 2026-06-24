#pragma once

#include <stdint.h>
#include <string>

struct Settings {
    std::string wifiSsid;
    std::string wifiPass;
    double receiverLatitude = 0.0;
    double receiverLongitude = 0.0;
    uint16_t displaySleepMin = 0;

    void load();
    void save();

    void getWifi(std::string& ssid, std::string& pass);
    void setWifi(const std::string& ssid, const std::string& pass);

    void setReceiverLocation(double latitude, double longitude);
    double getReceiverLatitude();
    double getReceiverLongitude();

    uint16_t getDisplaySleepMin();
    void setDisplaySleepMin(uint16_t minutes);

    void factoryReset();

};

extern Settings settings;
