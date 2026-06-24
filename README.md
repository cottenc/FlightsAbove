# FlightsAbove

ESP32 firmware that displays nearby aircraft from decoded ADS-B messages.

FlightsAbove reads SBS/BaseStation-style ADS-B text over an ESP32 UART, tracks
the most recent aircraft state, and renders nearby traffic on the SenseCAP
Indicator D1Pro 480x480 touchscreen.

This project reuses the proven ESP-IDF display/platform structure from
[`cottenc/espresso`](https://github.com/cottenc/espresso), including the
SenseCAP board support component, LVGL framebuffer setup, layout profiles, and
LCD timing defaults.

## Hardware

- SenseCAP Indicator D1Pro or compatible ESP32-S3 display target
- ADS-B receiver/decoder that outputs SBS/BaseStation lines over serial

The ESP32 does not directly demodulate 1090 MHz RF. Use a decoder such as a
dump1090 bridge, Stratux-style receiver, or another ADS-B front end that emits
decoded messages.

## Default Wiring

| Signal | ESP32 pin |
| --- | --- |
| ADS-B decoder TX | GPIO 17 |
| ADS-B decoder GND | GND |

Only the ESP32 RX pin is required if the ADS-B decoder is transmit-only.
Change the UART pins in `main/flights_config.h` if your hardware uses a
different connector.

## Configure

Edit `main/flights_config.h` before uploading:

- `kReceiverLatitude` and `kReceiverLongitude`
- `kAdsbRxPin`, `kAdsbTxPin`, and `kAdsbBaudRate`
- stale-aircraft timeout and UI refresh timing

## Supported Input

The initial parser supports SBS/BaseStation `MSG` records, for example:

```text
MSG,3,1,1,A8B32F,1,2026/06/23,12:00:01.000,2026/06/23,12:00:01.000,,36000,420,273,37.62131,-122.37896,,,0,0,0,0
MSG,1,1,1,A8B32F,1,2026/06/23,12:00:02.000,2026/06/23,12:00:02.000,UAL123,,,,,,,,,,0
```

## Build And Upload

Install ESP-IDF 5.1 or newer, export the environment, then run:

```sh
. $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

## Project Layout

```text
components/adsb/             Aircraft state, SBS parser, fixed-size tracker
components/platform/         Reused display platform adapter from espresso
components/seeed_indicator/  Reused SenseCAP board support from espresso
components/ui_layout/        Reused layout profile structure
main/flights_config.h        Device and receiver configuration
main/main.cpp                UART ingest task and LVGL UI
```

## Roadmap

- Add optional Wi-Fi ingest from a local dump1090 endpoint
- Add directional radar plotting when aircraft bearing is available
- Persist receiver location and display options in NVS
- Support additional decoded formats such as Beast-to-text bridge output
