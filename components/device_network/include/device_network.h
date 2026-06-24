#pragma once

#include <stdint.h>

namespace device_network {

struct Snapshot {
    bool stationConnected = false;
    bool setupApActive = false;
    bool otaRunning = false;
    char stationSsid[33] = {};
    char stationIp[16] = {};
    char setupIp[16] = {};
    char setupUrl[40] = {};
    int rssi = 0;
};

void begin();
Snapshot snapshot();
void requestRestart(uint32_t delayMs = 500);

}  // namespace device_network
