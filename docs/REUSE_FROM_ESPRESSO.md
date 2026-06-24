# Reuse From espresso

FlightsAbove was started from the display architecture in
`https://github.com/cottenc/espresso`.

The linked `main/assets/status` directory contains coffee-machine SVG source
assets. Those assets are not used directly here because the domain is aircraft
traffic, but they led to the useful reusable pieces:

- ESP-IDF project structure for ESP32-S3 devices
- SenseCAP Indicator D1Pro board support under `components/seeed_indicator`
- display abstraction under `components/platform`
- LVGL framebuffer and flush timing pattern
- layout profile component under `components/ui_layout`

The ADS-B parsing and aircraft tracking logic is new in `components/adsb`.
