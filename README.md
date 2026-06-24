# FlightsAbove

ESP-IDF 6 firmware that displays nearby aircraft from decoded ADS-B messages.

FlightsAbove reads SBS/BaseStation-style ADS-B text over an ESP32 UART, tracks
the most recent aircraft state, and renders nearby traffic on the SenseCAP
Indicator D1Pro 480x480 touchscreen.

This project reuses the proven native ESP-IDF display/platform structure from
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
Change the UART pins in `components/app_config/include/flights_config.h` if
your hardware uses a different connector.

## Configure

Compile-time defaults live in `components/app_config/include/flights_config.h`:

- `kAdsbRxPin`, `kAdsbTxPin`, and `kAdsbBaudRate`
- setup AP name and hostname
- stale-aircraft timeout and UI refresh timing

Receiver location and Wi-Fi credentials can be configured from the device setup
portal and are stored in NVS.

## Setup Portal

FlightsAbove starts a setup access point named `FlightsAbove-Setup` on boot.
Join that network and open the setup URL shown at the bottom of the device
display, usually:

```text
http://192.168.4.1/
```

The portal provides:

- Wi-Fi credential setup
- receiver latitude/longitude
- Logostream API key entry for airline logos
- display sleep preference
- status JSON
- restart and factory reset actions
- OTA app firmware upload

For OTA updates, build the project and upload `build/flightsabove.bin` from the
portal. The device writes the app to the inactive OTA partition and reboots
after a successful upload.

## Airline Logos

FlightsAbove can show a Logostream airline logo for the nearest aircraft when a
callsign starts with a three-letter airline ICAO code, such as `ASA328`. Enter
the Logostream API key in the setup portal. The key is stored only in the
device's NVS settings, is never written to source files, is not returned by the
status JSON, and is not logged. Common local secret file patterns are ignored in
`.gitignore`.

Airline logo PNGs are cached on the device in the SPIFFS data partition. The
firmware prefers cached airline logos before calling Logostream, stores newly
fetched airline logos, and slowly prefetches the most common local airline
logos to protect the API quota. Aircraft-specific liveries remain best-effort
and are not persisted.

## Supported Input

The initial parser supports SBS/BaseStation `MSG` records, for example:

```text
MSG,3,1,1,A8B32F,1,2026/06/23,12:00:01.000,2026/06/23,12:00:01.000,,36000,420,273,37.62131,-122.37896,,,0,0,0,0
MSG,1,1,1,A8B32F,1,2026/06/23,12:00:02.000,2026/06/23,12:00:02.000,UAL123,,,,,,,,,,0
```

## Build And Upload

Install ESP-IDF 6.0 or newer, export the environment, then run:

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

The firmware is intentionally kept as a clean ESP-IDF project. It does not use
Arduino, `arduino-esp32`, or Arduino compatibility components.

## Marker Attribution

Aircraft marker sprites in `main/aircraft_icons.cpp` include exact type-code
silhouettes derived from AircraftShapesSVG for common local aircraft, with
family fallback sprites cropped and repacked from tar1090's
`html/images/sprites.png` asset for LVGL:
https://github.com/RexKramer1/AircraftShapesSVG
https://github.com/wiedehopf/tar1090/blob/master/html/images/sprites.png

AircraftShapesSVG is licensed under GPL-3.0. tar1090 is licensed under
GPLv2-or-later:
https://github.com/wiedehopf/tar1090

## Static Basemap

The radar basemap is generated as four flash-resident LVGL RGB565 images in
`main/basemap_default.cpp`: 10-mile local, 25-mile close-range, 50-mile
mid-range, and 150-mile long-range maps. Runtime autoscale uses the closest
matching source to avoid heavily zooming a wider map. All maps are aligned to
the default map center `47.68571, -122.31595`. Regenerate them after changing
those defaults:

```sh
python3 -m venv .venv-basemap
. .venv-basemap/bin/activate
python -m pip install Pillow
python tools/generate_basemap.py
```

The generator writes visual check images to `docs/basemap_local_preview.png`,
`docs/basemap_close_preview.png`, `docs/basemap_mid_preview.png`, and
`docs/basemap_default_preview.png`. Map tiles are from OpenStreetMap
contributors.

## Project Layout

```text
components/adsb/             Aircraft state, SBS parser, fixed-size tracker
components/app_config/       Shared device defaults and pins
components/device_network/   Wi-Fi setup AP, setup portal server, OTA upload
components/platform/         Reused display platform adapter from espresso
components/seeed_indicator/  Reused SenseCAP board support from espresso
components/setup_portal/     Generic FlightsAbove setup HTML
components/storage/          NVS-backed Wi-Fi and receiver settings
components/ui_layout/        Reused layout profile structure
main/main.cpp                UART ingest task and LVGL UI
```

## Roadmap

- Add optional Wi-Fi ingest from a local dump1090 endpoint
- Add directional radar plotting when aircraft bearing is available
- Support additional decoded formats such as Beast-to-text bridge output
