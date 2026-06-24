# FlightsAbove

ESP-IDF 6 firmware for a SenseCAP Indicator D1Pro that displays nearby aircraft
from a local ADS-B feeder.

FlightsAbove is HTTP-first: it polls a readsb/tar1090-compatible
`aircraft.json` endpoint, computes distance and bearing from the configured map
center, and renders a small radar view with a static basemap. UART
SBS/BaseStation ingest remains available as a fallback.

## Hardware

- SenseCAP Indicator D1Pro or compatible ESP32-S3 display target
- Local ADS-B feeder exposing readsb/tar1090 JSON, for example:

```text
http://adsb-feeder.local:8080/data/aircraft.json
```

Optional UART fallback expects SBS/BaseStation `MSG` lines on GPIO 17 at
9600 baud.

## Setup Portal

FlightsAbove starts a setup access point named `FlightsAbove-Setup` on boot.
Join that network and open the setup URL shown at the bottom of the device,
usually:

```text
http://192.168.4.1/
```

The setup portal configures:

- Wi-Fi credentials
- feeder `aircraft.json` URL
- map center latitude and longitude
- maximum radar range
- Logostream API key for airline logos/liveries
- display sleep timeout
- OTA firmware upload
- restart and factory reset

Settings are stored in the device NVS partition. The Logostream API key is not
stored in source files, is not returned in status JSON, and is not logged.

## Security Notes

The setup portal is intentionally simple and local-network oriented. It does not
implement authentication. Anyone who can reach the setup AP or the device on the
LAN can change settings, upload firmware, restart the device, factory reset it,
and read debug status endpoints. Use it only on a trusted local network.

Debug endpoints are available for development and field diagnosis:

- `/status`
- `/debug/adsb`
- `/debug/logo`
- `/debug/screenshot.bmp`

These endpoints do not expose the Logostream API key.

## Build, Test, And Flash

Install ESP-IDF 6.0 or newer, then:

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-21110 flash monitor
```

Run the host parser/cache regression test:

```sh
cc -std=c99 \
  -Imanaged_components/espressif__cjson/cJSON \
  -c managed_components/espressif__cjson/cJSON/cJSON.c \
  -o /tmp/flightsabove-cjson.o
g++ -std=c++17 \
  -Icomponents/adsb/include \
  -Imanaged_components/espressif__cjson/cJSON \
  tests/adsb_parser_test.cpp \
  components/adsb/aircraft.cpp \
  components/adsb/aircraft_store.cpp \
  components/adsb/readsb_json_parser.cpp \
  components/adsb/route_cache.cpp \
  /tmp/flightsabove-cjson.o \
  -o /tmp/flightsabove-adsb-parser-test
/tmp/flightsabove-adsb-parser-test
```

The host test uses the managed cJSON component. Run `idf.py build` once first
on a fresh clone so ESP-IDF downloads managed components.

The firmware is a native ESP-IDF project. It does not use Arduino,
`arduino-esp32`, or Arduino compatibility components.

## Runtime Behavior

- Default map center: `47.68571, -122.31595`
- Default maximum radar range: `10` miles
- Autoscale chooses the smallest useful range up to the configured maximum
- Static basemaps are embedded for 10, 25, 50, and 150 mile source ranges
- Tracked aircraft are purged after 120 seconds without updates
- Up to 32 aircraft are retained internally; up to 16 are rendered
- Route lookups use `http://adsb.im/api/0/routeset`
- Airline logo/livery lookups use Logostream when an API key is configured
- Common airline logos are cached in SPIFFS to reduce API calls

## Regenerating The Basemap

The basemap is generated as flash-resident LVGL RGB565 images in
`main/basemap_default.cpp`. Regenerate after changing the default map center or
source map ranges:

```sh
python3 -m venv .venv-basemap
. .venv-basemap/bin/activate
python -m pip install Pillow
python tools/generate_basemap.py
```

Preview images are written to:

- `docs/basemap_local_preview.png`
- `docs/basemap_close_preview.png`
- `docs/basemap_mid_preview.png`
- `docs/basemap_default_preview.png`

Map tiles are from OpenStreetMap contributors.

## Project Layout

```text
components/adsb/             Aircraft state, parsers, route cache
components/app_config/       Device defaults, pins, colors, ranges
components/device_network/   Wi-Fi AP/STA setup, portal, status, OTA
components/platform/         Display platform adapter
components/seeed_indicator/  SenseCAP board support
components/setup_portal/     Setup HTML rendering
components/storage/          NVS-backed settings
components/ui_layout/        Display profile structure
main/main.cpp                Tasks, HTTP ingest, logo cache, LVGL UI
main/aircraft_icons.cpp      LVGL aircraft marker assets
main/basemap_default.cpp     LVGL basemap assets
```

## Marker Attribution

Aircraft marker sprites in `main/aircraft_icons.cpp` include exact type-code
silhouettes derived from AircraftShapesSVG for common local aircraft, with
family fallback sprites cropped and repacked from tar1090's
`html/images/sprites.png` asset for LVGL:

- https://github.com/RexKramer1/AircraftShapesSVG
- https://github.com/wiedehopf/tar1090/blob/master/html/images/sprites.png

AircraftShapesSVG is licensed under GPL-3.0. tar1090 is licensed under
GPLv2-or-later.
