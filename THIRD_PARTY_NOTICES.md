# Third-Party Notices

FlightsAbove includes generated aircraft marker sprites and color behavior
derived from these projects:

## tar1090

- Source: https://github.com/wiedehopf/tar1090
- Asset: `html/images/sprites.png`
- License: GPLv2-or-later
- Use: family fallback aircraft marker sprites and altitude color ramp

## AircraftShapesSVG

- Source: https://github.com/RexKramer1/AircraftShapesSVG
- Assets: selected files from `Shapes SVG/`
- License: GPL-3.0
- Use: exact type-code aircraft marker sprites

The generated LVGL sprite arrays live in `main/aircraft_icons.cpp`. The
altitude-based marker color mapping is ported in `main/main.cpp`.
