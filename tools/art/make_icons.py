#!/usr/bin/env python3
"""Авторинг иконки Enjoer: изометрический куб 2×2×2 в фирменных цветах мира.

Только стандартная библиотека (zlib + struct), без Pillow — как и
`tools/art/make_assets.py`, инструмент работает офлайн.

Иконка нужна двум адресатам:
  * Android — `android:icon` в `game/AndroidManifest.xml`;
  * RuStore — витрина требует PNG 512×512 (размер записывается в
    `game/res/mipmap-anydpi/ic_launcher_512.png`, чтобы его было удобно
    скачать и приложить к карточке).

Запуск: `python3 tools/art/make_icons.py`
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

# Палитра берётся из тайлов мира: травяная крыша, две боковые грани земли и
# тёмный контур. Значения согласованы с game/assets/textures/grass_*.png и
# dirt.png, чтобы иконка читалась как кусок того же мира.
GRASS = (106, 176, 76, 255)
GRASS_DARK = (84, 145, 60, 255)
DIRT = (140, 96, 60, 255)
DIRT_SHADOW = (104, 68, 40, 255)
OUTLINE = (38, 26, 18, 255)
BACKGROUND = (24, 28, 34, 0)

# Куда кладём результат: размеры Android-иконок плюс витринный квадрат RuStore.
MIPMAP_SIZES = {
    "mipmap-mdpi": 48,
    "mipmap-hdpi": 72,
    "mipmap-xhdpi": 96,
    "mipmap-xxhdpi": 144,
    "mipmap-xxxhdpi": 192,
    "mipmap-anydpi": 512,
}


def side(center_x: float, center_y: float, radius: float) -> float:
    """Половина ширины куба по горизонтали."""
    return radius


def top_face(px: float, py: float, cx: float, cy: float, r: float, h: float) -> bool:
    """Ромб верхней грани: |dx|/r + |dy|/h <= 1."""
    return abs(px - cx) / r + abs(py - cy) / h <= 1.0


def inside_cube(px: float, py: float, cx: float, cy: float, r: float, h: float) -> bool:
    """Силуэт куба: ромб-крыша плюс прямоугольник двух боковых граней."""
    dx = abs(px - cx)
    if dx > r:
        return False
    roof = h * (1.0 - dx / r)
    return cy - roof <= py <= cy + h


def cube_pixel(px: float, py: float, cx: float, cy: float, r: float) -> tuple[int, ...]:
    """Цвет пикселя иконки; BACKGROUND там, где куба нет."""
    h = r * math.sqrt(3.0) / 2.0
    pad = max(1.0, r * 0.02)
    if not inside_cube(px, py, cx, cy, r + pad, h + pad):
        return BACKGROUND
    if not inside_cube(px, py, cx, cy, r, h):
        return OUTLINE

    if top_face(px, py, cx, cy, r, h):
        return GRASS if _cell(px, py, cx, cy, r, h) == 0 else GRASS_DARK
    # Боковые грани: левая светлее, правая в тени. Раздел — вертикаль через центр.
    left = px <= cx
    if _edge(px, py, cx, cy, r, h):
        return OUTLINE
    return DIRT if left else DIRT_SHADOW


def _cell(px: float, py: float, cx: float, cy: float, r: float, h: float) -> int:
    """Номер клетки верхней грани 2×2 — для шахматного чередования оттенков.

    Ромб — это повёрнутый квадрат, поэтому (u, v) считаем по его диагоналям:
    u растёт к правой вершине, v — вниз. Сумма int(u)+int(v) даёт шахматную
    раскладку четырёх частей, из которых состоит один исходный блок.
    """
    u = (px - cx) / r + (py - cy) / h           # 0..2
    v = (cx - px) / r + (py - cy) / h + 1.0     # 0..2
    return (int(u) + int(v)) & 1


def _edge(px: float, py: float, cx: float, cy: float, r: float, h: float) -> bool:
    """Тонкий тёмный кант по рёбрам куба и по границе боковых граней."""
    line = max(1.0, r * 0.03)
    dx = abs(px - cx)
    if dx > r - line:
        return True
    roof = h * (1.0 - dx / r)
    if py < cy - roof + line:
        return True
    if py > cy + h - line:
        return True
    if abs(px - cx) < line:
        return True
    return False


def render(size: int) -> list[list[tuple[int, ...]]]:
    """Квадрат size×size с кубом по центру."""
    cx = cy = size / 2.0
    radius = size * 0.34
    rows = []
    for y in range(size):
        row = [cube_pixel(x + 0.5, y + 0.5, cx, cy, radius) for x in range(size)]
        rows.append(row)
    return rows


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_png(path: Path, rows: list[list[tuple[int, ...]]]) -> None:
    """Пишет RGBA PNG без зависимостей."""
    width, height = len(rows[0]), len(rows)
    raw = bytearray()
    for row in rows:
        raw.append(0)  # фильтр scanline: None
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))
    png = (b"\x89PNG\r\n\x1a\n"
           + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + _chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Сгенерировать иконки Enjoer.")
    parser.add_argument("--res", default="game/res",
                        help="каталог ресурсов Android (по умолчанию game/res)")
    args = parser.parse_args(argv)

    res = Path(args.res).resolve()
    for folder, size in sorted(MIPMAP_SIZES.items(), key=lambda item: item[1]):
        target = res / folder / "ic_launcher.png"
        write_png(target, render(size))
        print(f"{target.relative_to(res.parent)}: {size}×{size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
