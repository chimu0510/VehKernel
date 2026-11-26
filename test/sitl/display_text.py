# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the text on SITL's display image (ADR-0022).

SITL's --display writes what the simulated SSD1306 shows as a binary PBM
file. The local display draws 21 columns by 8 lines in its font's 6 x 8
cells, one pixel in from the left edge, so each cell is matched against the
glyphs of the firmware's own font table in modules/local_display/src/font.cpp.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FONT = REPO / "modules" / "local_display" / "src" / "font.cpp"
COLUMNS, LINES = 21, 8
CELL_W, CELL_H, GLYPH_W, GLYPH_H = 6, 8, 5, 7
LEFT = 1


def load_glyphs(font: Path = FONT) -> dict[tuple[str, ...], str]:
    """Each glyph's seven rows, mapped to its character, from the font table."""
    rows = re.findall(r'\{("[.#]{5}"(?:, "[.#]{5}"){6})\},', font.read_text(encoding="utf-8"))
    if len(rows) != 95:
        raise ValueError(f"expected 95 glyphs in {font}, found {len(rows)}")
    return {tuple(r.strip('"') for r in row.split(", ")): chr(0x20 + i) for i, row in enumerate(rows)}


def read_pbm(path: Path) -> list[list[bool]]:
    data = path.read_bytes()
    match = re.match(rb"P4\s+(\d+)\s+(\d+)\s", data)
    if match is None:
        raise ValueError(f"{path} is not a binary PBM file")
    width, height = int(match.group(1)), int(match.group(2))
    pixels = data[match.end():]
    stride = (width + 7) // 8
    return [[bool(pixels[y * stride + x // 8] & (0x80 >> (x % 8))) for x in range(width)] for y in range(height)]


def screen_text(path: Path) -> list[str]:
    """The display's eight lines, trailing spaces removed; '\\x00' for a cell no glyph matches."""
    image = read_pbm(path)
    glyphs = load_glyphs()
    lines = []
    for line in range(LINES):
        text = ""
        for column in range(COLUMNS):
            x0, y0 = LEFT + column * CELL_W, line * CELL_H
            rows = tuple("".join("#" if image[y0 + y][x0 + x] else "." for x in range(GLYPH_W))
                         for y in range(GLYPH_H))
            text += glyphs.get(rows, "\x00")
        lines.append(text.rstrip())
    return lines


def read_contrast(path: Path) -> int:
    """The contrast the simulated SSD1306 is set to, which --display records next to the image."""
    return int(Path(str(path) + ".contrast").read_text(encoding="utf-8").strip())


def is_dark(path: Path) -> bool:
    """Whether the image shows no lit pixel, as while the display is off."""
    return not any(any(row) for row in read_pbm(path))


def has_text(path: Path, text: str, glyphs: dict[tuple[str, ...], str] | None = None, scale: int = 1) -> bool:
    """Whether `text` is drawn anywhere on the image, in the 5 x 7 font on a dark ground or dark on a
    lit one, at any pixel position. The menu screens of spec 0016 draw text at pixel positions, not
    in the cells of screen_text(). With `scale` 2 each pixel of a glyph is two wide, as the overview's
    mode and arming state are drawn (the cell is then 12 pixels wide)."""
    table = glyphs if glyphs is not None else load_glyphs()
    rows_of = {char: rows for rows, char in table.items()}
    image = read_pbm(path)
    height, width = len(image), len(image[0])
    # Each character's glyph in its cell: the pixels that are lit and those that are dark in it.
    lit: list[tuple[int, int]] = []
    dark: list[tuple[int, int]] = []
    for index, char in enumerate(text):
        for y, row in enumerate(rows_of[char]):
            for x, pixel in enumerate(row):
                for sub in range(scale):
                    (lit if pixel == "#" else dark).append((index * CELL_W * scale + x * scale + sub, y))
    span = (len(text) * CELL_W - (CELL_W - GLYPH_W)) * scale
    for inverted in (False, True):
        on, off = (dark, lit) if inverted else (lit, dark)
        for y0 in range(height - GLYPH_H + 1):
            for x0 in range(width - span + 1):
                if all(image[y0 + y][x0 + x] for x, y in on) and not any(image[y0 + y][x0 + x] for x, y in off):
                    return True
    return False


# The status bar's page dots (modules/local_display/src/status_bar.cpp): eight, four pixels apart,
# the current page a filled 3 x 3 square and the others one pixel.
PAGE_COUNT = 8
DOTS_FIRST_X = 96


def page_index(path: Path) -> int | None:
    """The page whose dot is filled in the status bar, or None when no dot matches. The image may be
    shifted by a pixel (display protection), so each offset is tried."""
    image = read_pbm(path)
    for dy in (0, 1):
        for dx in (0, 1):
            found = []
            for page in range(PAGE_COUNT):
                x = DOTS_FIRST_X + 4 * page + dx
                filled = all(image[2 + dy + j][x - 1 + i] for i in range(3) for j in range(3))
                single = image[3 + dy][x] and not any(image[2 + dy][x + i] for i in (-1, 1)) \
                    and not image[2 + dy][x] and not image[4 + dy][x]
                if filled:
                    found.append(page)
                elif not single:
                    found = []
                    break
            if len(found) == 1:
                return found[0]
    return None
