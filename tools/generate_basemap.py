#!/usr/bin/env python3
"""Generate the embedded FlightsAbove radar basemap.

The generated LVGL image is intentionally static: it is centered on the
firmware's default receiver location and sized so the outer radar ring matches
the default maximum range.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import time
import urllib.request

from PIL import Image, ImageDraw, ImageEnhance


DEFAULT_LAT = 47.68571
DEFAULT_LON = -122.31595
DEFAULT_RANGE_MILES = 150.0
WIDTH = 432
HEIGHT = 318
RADAR_RADIUS = 146
TILE_SIZE = 256
USER_AGENT = "FlightsAbove static basemap generator"


def lon_to_tile_px(lon: float, zoom: int) -> float:
    return (lon + 180.0) / 360.0 * (2**zoom) * TILE_SIZE


def lat_to_tile_px(lat: float, zoom: int) -> float:
    lat_rad = math.radians(lat)
    return (
        (1.0 - math.log(math.tan(lat_rad) + (1.0 / math.cos(lat_rad))) / math.pi)
        / 2.0
        * (2**zoom)
        * TILE_SIZE
    )


def meters_per_tile_pixel(lat: float, zoom: int) -> float:
    return math.cos(math.radians(lat)) * 156543.03392804097 / (2**zoom)


def fetch_tile(zoom: int, x: int, y: int, cache_dir: pathlib.Path) -> Image.Image:
    cache_dir.mkdir(parents=True, exist_ok=True)
    tile_path = cache_dir / f"{zoom}_{x}_{y}.png"
    if not tile_path.exists():
        url = f"https://tile.openstreetmap.org/{zoom}/{x}/{y}.png"
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=15) as response:
            tile_path.write_bytes(response.read())
        time.sleep(0.15)
    return Image.open(tile_path).convert("RGB")


def choose_zoom(lat: float, range_miles: float) -> int:
    target_mpp = range_miles * 1609.344 / RADAR_RADIUS
    best_zoom = 8
    best_score = float("inf")
    for zoom in range(5, 12):
        source_scale = target_mpp / meters_per_tile_pixel(lat, zoom)
        score = abs(source_scale - 2.2)
        if score < best_score:
            best_score = score
            best_zoom = zoom
    return best_zoom


def render_basemap(
    lat: float,
    lon: float,
    range_miles: float,
    cache_dir: pathlib.Path,
) -> Image.Image:
    zoom = choose_zoom(lat, range_miles)
    target_mpp = range_miles * 1609.344 / RADAR_RADIUS
    tile_mpp = meters_per_tile_pixel(lat, zoom)
    source_w = max(WIDTH, int(round(WIDTH * target_mpp / tile_mpp)))
    source_h = max(HEIGHT, int(round(HEIGHT * target_mpp / tile_mpp)))
    center_x = lon_to_tile_px(lon, zoom)
    center_y = lat_to_tile_px(lat, zoom)
    left = int(round(center_x - source_w / 2.0))
    top = int(round(center_y - source_h / 2.0))
    right = left + source_w
    bottom = top + source_h

    tile_left = left // TILE_SIZE
    tile_top = top // TILE_SIZE
    tile_right = (right + TILE_SIZE - 1) // TILE_SIZE
    tile_bottom = (bottom + TILE_SIZE - 1) // TILE_SIZE

    mosaic = Image.new(
        "RGB",
        ((tile_right - tile_left) * TILE_SIZE, (tile_bottom - tile_top) * TILE_SIZE),
        (8, 18, 16),
    )
    for tx in range(tile_left, tile_right):
        for ty in range(tile_top, tile_bottom):
            tile = fetch_tile(zoom, tx, ty, cache_dir)
            mosaic.paste(tile, ((tx - tile_left) * TILE_SIZE, (ty - tile_top) * TILE_SIZE))

    crop = mosaic.crop(
        (
            left - tile_left * TILE_SIZE,
            top - tile_top * TILE_SIZE,
            right - tile_left * TILE_SIZE,
            bottom - tile_top * TILE_SIZE,
        )
    )
    image = crop.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).convert("RGB")

    image = ImageEnhance.Color(image).enhance(0.78)
    image = ImageEnhance.Contrast(image).enhance(1.08)
    image = ImageEnhance.Brightness(image).enhance(0.92)

    overlay = Image.new("RGB", image.size, (210, 230, 224))
    image = Image.blend(image, overlay, 0.08)

    draw = ImageDraw.Draw(image)
    draw.text((6, HEIGHT - 13), "(C) OpenStreetMap", fill=(72, 90, 92))
    return image


def rgb565_bytes(image: Image.Image) -> bytes:
    data = bytearray()
    for r, g, b in image.convert("RGB").get_flattened_data():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.append(value & 0xFF)
        data.append((value >> 8) & 0xFF)
    return bytes(data)


def write_cpp(image: Image.Image, header_path: pathlib.Path, source_path: pathlib.Path) -> None:
    data = rgb565_bytes(image)
    header_path.write_text(
        "#pragma once\n\n"
        "#include \"lvgl.h\"\n\n"
        "LV_IMG_DECLARE(flightsabove_basemap_default);\n",
        encoding="utf-8",
    )

    lines = [
        "#include \"basemap_default.h\"",
        "",
        "#include \"lvgl.h\"",
        "",
        f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t flightsabove_basemap_default_map[{len(data)}] = {{",
    ]
    for index in range(0, len(data), 16):
        chunk = data[index : index + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.extend(
        [
            "};",
            "",
            "const lv_img_dsc_t flightsabove_basemap_default = {",
            f"    {{LV_IMG_CF_TRUE_COLOR, 0, 0, {WIDTH}, {HEIGHT}}},",
            f"    {len(data)},",
            "    flightsabove_basemap_default_map,",
            "};",
            "",
        ]
    )
    source_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lat", type=float, default=DEFAULT_LAT)
    parser.add_argument("--lon", type=float, default=DEFAULT_LON)
    parser.add_argument("--range-miles", type=float, default=DEFAULT_RANGE_MILES)
    parser.add_argument("--cache-dir", type=pathlib.Path, default=pathlib.Path(".tile-cache"))
    parser.add_argument("--preview", type=pathlib.Path, default=pathlib.Path("docs/basemap_default_preview.png"))
    parser.add_argument("--header", type=pathlib.Path, default=pathlib.Path("main/basemap_default.h"))
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path("main/basemap_default.cpp"))
    args = parser.parse_args()

    image = render_basemap(args.lat, args.lon, args.range_miles, args.cache_dir)
    args.preview.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.preview)
    write_cpp(image, args.header, args.source)
    print(
        f"Generated {args.source} and {args.preview} "
        f"for {args.lat:.5f},{args.lon:.5f} at {args.range_miles:.0f} mi"
    )


if __name__ == "__main__":
    main()
