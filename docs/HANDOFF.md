# FlightsAbove Handoff

## Current Target

- Device: SenseCAP Indicator D1Pro, ESP32-S3
- Serial port used during development: `/dev/cu.usbserial-21110`
- Device IP during development: `192.168.1.78`
- ESP-IDF path used during development: `/Users/cotten/esp/esp-idf`
- ESP-IDF version: 6.0.x

## Required Setup

Use native ESP-IDF only:

```sh
. /Users/cotten/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-21110 flash
```

Do not add Arduino or Arduino compatibility components.

## Device Configuration

Configure the device from the setup portal:

```text
http://192.168.4.1/
```

Important fields:

- Aircraft JSON URL: local readsb/tar1090 `aircraft.json`
- Map center latitude/longitude
- Maximum radar range
- Logostream API key, if logos/liveries are desired

The Logostream API key is stored only in NVS and should not be committed,
logged, copied into source files, or included in screenshots.

## Verification Checklist

Before handoff:

1. Run the host ADS-B parser/cache test from `README.md`.
2. Run `idf.py build` and confirm app partition headroom remains acceptable.
3. Flash the device.
4. Confirm `/status` reports the expected firmware hash.
5. Capture `/debug/screenshot.bmp` and visually confirm:
   - radar basemap draws fully
   - range label is legible
   - aircraft icons are visible over the map
   - nearest aircraft type and metadata are not clipped
   - logo/livery display is either visible or correctly omitted

## Known Operating Assumptions

- The setup portal and debug endpoints are unauthenticated and are intended only
  for trusted local networks.
- OTA upload is unauthenticated for the same reason.
- The feeder URL is HTTP-only because local readsb/tar1090 feeders commonly run
  without TLS.
- Route lookup uses `http://adsb.im/api/0/routeset`.
- Logostream calls are quota-limited locally and common airline logos are cached
  in SPIFFS; aircraft-specific liveries are best-effort and not persisted.
- Static basemaps are aligned to the configured default center. Changing the map
  center at runtime re-centers aircraft math but does not regenerate map imagery.

## Useful Endpoints

- `/status`
- `/debug/adsb`
- `/debug/logo`
- `/debug/screenshot.bmp`

These endpoints should not expose secrets.
