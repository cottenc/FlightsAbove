#pragma once

#include "driver/uart.h"

#include <stdint.h>

namespace cfg {

constexpr const char* kDeviceName = "FlightsAbove";
constexpr const char* kSetupApSsid = "FlightsAbove-Setup";
constexpr const char* kSetupHostname = "flightsabove";
constexpr uint16_t kSetupPortalPort = 80;

constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

constexpr uart_port_t kAdsbUart = UART_NUM_1;
constexpr int kAdsbRxPin = 17;
constexpr int kAdsbTxPin = UART_PIN_NO_CHANGE;
constexpr int kAdsbBaudRate = 9600;
constexpr int kAdsbRxBufferBytes = 4096;
constexpr int kAdsbLineMax = 240;

constexpr const char* kDefaultFeederUrl = "http://cotten-2.l.adsb.im:8080/data/aircraft.json";

constexpr double kDefaultReceiverLatitude = 47.68571;
constexpr double kDefaultReceiverLongitude = -122.31595;
constexpr double kReceiverLatitude = kDefaultReceiverLatitude;
constexpr double kReceiverLongitude = kDefaultReceiverLongitude;

constexpr int64_t kAircraftStaleMs = 120000;
constexpr uint32_t kUiRefreshMs = 500;
constexpr uint16_t kDefaultRadarRangeMiles = 150;
constexpr uint16_t kMinRadarRangeMiles = 5;
constexpr uint16_t kMaxRadarRangeMiles = 500;

constexpr uint32_t kColorBackground = 0x07100D;
constexpr uint32_t kColorPanel = 0x10211C;
constexpr uint32_t kColorPanelAlt = 0x162B24;
constexpr uint32_t kColorText = 0xF4F7F2;
constexpr uint32_t kColorMuted = 0x92A098;
constexpr uint32_t kColorGreen = 0x47D16C;
constexpr uint32_t kColorCyan = 0x48D6D2;
constexpr uint32_t kColorAmber = 0xF2B84B;
constexpr uint32_t kColorRunway = 0xDDE7DF;

}  // namespace cfg
