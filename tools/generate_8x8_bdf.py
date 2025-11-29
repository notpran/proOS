#!/usr/bin/env python3
import pathlib
import re
from textwrap import dedent

root = pathlib.Path(__file__).resolve().parents[1]
header_path = root / "kernel" / "fb_font.h"
output_path = root / "assets" / "font.bdf"

hex_pattern = re.compile(r"0x([0-9A-Fa-f]{2})")
rows = []
with header_path.open('r', encoding='ascii') as header:
    collecting = False
    for line in header:
        if "font8x8_basic" in line:
            collecting = True
        elif collecting:
            if "};" in line:
                break
            for match in hex_pattern.finditer(line):
                rows.append(int(match.group(1), 16))

if len(rows) != 96 * 8:
    raise SystemExit(f"Expected 768 bytes, found {len(rows)}")

header = dedent("""
STARTFONT 2.1
FONT -proOS-proFont-Medium-R-Normal--8-80-75-75-C-80-ISO10646-1
SIZE 8 75 75
FONTBOUNDINGBOX 8 8 0 0
STARTPROPERTIES 6
FAMILY_NAME "proFont"
FOUNDRY "proOS"
PIXEL_SIZE 8
FONT_ASCENT 8
FONT_DESCENT 0
SPACING "C"
ENDPROPERTIES
CHARS 96
""").lstrip()

def glyph_name(code: int) -> str:
    if code == 32:
        return "space"
    if 33 <= code <= 126:
        return f"U+{code:04X}"
    if code == 127:
        return "DEL"
    return f"GLYPH{code}"

def glyph_block(code: int, bitmap: list[int]) -> str:
    lines = [f"{row:02X}" for row in bitmap]
    bitmap_str = "\n".join(lines)
    return dedent(f"""
    STARTCHAR {glyph_name(code)}
    ENCODING {code}
    SWIDTH 800 0
    DWIDTH 8 0
    BBX 8 8 0 0
    BITMAP
    {bitmap_str}
    ENDCHAR
    """).strip()+"\n"

content = [header]
for idx in range(96):
    code = 32 + idx
    start = idx * 8
    glyph_rows = rows[start:start + 8]
    content.append(glyph_block(code, glyph_rows))

output_path.write_text("".join(content), encoding='ascii')
print(f"Wrote {output_path}")
