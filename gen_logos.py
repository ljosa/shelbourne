#!/usr/bin/env python3
"""Generate 128x100 8-bit grayscale station logos as C header.

Radio/podcast logos use only the top 50 rows, leaving the bottom half
for text drawn by the player at runtime.  AUX and USB logos are
full-height since they don't need dynamic text below.
"""

from PIL import Image, ImageDraw, ImageFont
import math, os

W, H = 128, 100
HALF = 50  # radio logos use top half only

def find_font(size):
    """Find a suitable font."""
    paths = [
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica.ttc",
    ]
    for p in paths:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except:
                continue
    return ImageFont.load_default()

def find_bold_font(size):
    """Find a bold font."""
    paths = [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/Library/Fonts/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]
    for p in paths:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except:
                continue
    return find_font(size)

def center_text(draw, text, font, y, fill=255):
    """Draw centered text at given y position."""
    bbox = draw.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    x = (W - tw) // 2
    draw.text((x, y), text, fill=fill, font=font)

# ── Half-height radio logos (top 50 rows) ──────────────────────

def make_logo_vpr():
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(30)
    center_text(draw, "VPR", big, 2)
    # Mountain silhouette scaled to top half
    pts = [(0, 45), (30, 30), (50, 38), (64, 24), (78, 38), (98, 30), (128, 45)]
    for i in range(len(pts) - 1):
        draw.line([pts[i], pts[i+1]], fill=180, width=2)
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i+1]
        for x in range(x0, x1 + 1):
            if x1 != x0:
                y = y0 + (y1 - y0) * (x - x0) // (x1 - x0)
            else:
                y = y0
            for yy in range(y, min(47, HALF)):
                if 0 <= x < W:
                    img.putpixel((x, yy), max(img.getpixel((x, yy)), 60))
    return img

def make_logo_wdev():
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(32)
    center_text(draw, "WDEV", big, 2)
    cx, cy = 64, 36
    for r in [8, 14, 20]:
        draw.arc([cx-r, cy-r, cx+r, cy+r], 220, 320, fill=140, width=2)
    return img

def make_logo_bbc_ws():
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(26)
    small = find_font(11)
    draw.rectangle([14, 4, 114, 32], outline=255, width=2)
    center_text(draw, "BBC", big, 5)
    cx, cy = 64, 40
    r = 7
    draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline=160, width=1)
    draw.line([(cx-r, cy), (cx+r, cy)], fill=120, width=1)
    return img

def make_logo_nrk(channel):
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    med = find_bold_font(20)
    huge = find_bold_font(36)
    center_text(draw, "NRK", med, 1)
    center_text(draw, f"P{channel}", huge, 16)
    return img

def make_logo_station(call, freq, location):
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(30)
    center_text(draw, call, big, 2)
    cx = 64
    draw.line([(cx, 34), (cx, 46)], fill=200, width=2)
    draw.line([(cx-3, 34), (cx, 30), (cx+3, 34)], fill=200, width=1)
    for r in [6, 10, 14]:
        draw.arc([cx-r, 28-r, cx+r, 28+r], 220, 320, fill=120, width=1)
    return img

def make_logo_somafm(name, subtitle):
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    tiny = find_font(10)
    center_text(draw, "SomaFM", tiny, 1, fill=120)
    draw.line([(10, 12), (118, 12)], fill=60, width=1)
    name_len = len(name)
    if name_len <= 6:
        font_size = 30
    elif name_len <= 8:
        font_size = 24
    elif name_len <= 12:
        font_size = 18
    else:
        font_size = 15
    big = find_bold_font(font_size)
    center_text(draw, name, big, 16)
    # Wave visualization
    for x in range(W):
        amp = 4 + 2 * math.sin(x * 0.15) + 1 * math.sin(x * 0.3)
        y_base = 44
        for dy in range(-int(amp), int(amp)):
            brightness = int(80 * (1 - abs(dy) / amp))
            if 0 <= y_base + dy < HALF:
                img.putpixel((x, y_base + dy), max(img.getpixel((x, y_base + dy)), brightness))
    return img

def make_logo_eots():
    """Eye on the Sky — Vermont weather podcast."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    # Sun icon
    cx, cy = 64, 18
    r = 10
    draw.ellipse([cx-r, cy-r, cx+r, cy+r], fill=200)
    for angle in range(0, 360, 45):
        import math as m
        x0 = cx + int((r+3) * m.cos(m.radians(angle)))
        y0 = cy + int((r+3) * m.sin(m.radians(angle)))
        x1 = cx + int((r+7) * m.cos(m.radians(angle)))
        y1 = cy + int((r+7) * m.sin(m.radians(angle)))
        draw.line([(x0, y0), (x1, y1)], fill=180, width=1)
    # Cloud
    draw.ellipse([30, 30, 60, 44], fill=140)
    draw.ellipse([45, 28, 80, 44], fill=160)
    draw.ellipse([65, 32, 90, 46], fill=130)
    draw.rectangle([35, 38, 85, 46], fill=140)
    return img

# ── Full-height logos (AUX, USB) ───────────────────────────────

def make_logo_usb():
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(28)
    med = find_font(14)
    cx, cy = 64, 40
    draw.rounded_rectangle([cx-18, cy-15, cx+18, cy+15], radius=3, outline=200, width=2)
    draw.rectangle([cx-8, cy+15, cx+8, cy+22], outline=200, width=2)
    draw.line([(cx, cy-8), (cx, cy+8)], fill=200, width=2)
    draw.line([(cx-5, cy-3), (cx, cy-8), (cx+5, cy-3)], fill=200, width=1)
    center_text(draw, "USB", big, 65)
    center_text(draw, "Music", med, 88, fill=160)
    return img

def make_logo_aux():
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    huge = find_bold_font(44)
    cx, cy = 64, 35
    draw.ellipse([cx-8, cy-8, cx+8, cy+8], outline=180, width=2)
    draw.ellipse([cx-3, cy-3, cx+3, cy+3], fill=180)
    draw.line([(cx, cy+8), (cx, cy+30)], fill=180, width=3)
    draw.arc([cx-12, cy-2, cx+12, cy+14], 0, 180, fill=140, width=1)
    center_text(draw, "AUX", huge, 55)
    return img

# ── Generate ───────────────────────────────────────────────────

def img_to_c_array(img, name):
    data = list(img.tobytes())
    lines = []
    lines.append(f"static const uint8_t logo_{name}[{W * H}] = {{")
    for row in range(H):
        row_data = data[row * W:(row + 1) * W]
        hex_vals = ", ".join(f"0x{b:02x}" for b in row_data)
        lines.append(f"    {hex_vals},")
    lines.append("};")
    return "\n".join(lines)

logos = {
    "vpr": make_logo_vpr(),
    "wdev": make_logo_wdev(),
    "bbc_ws": make_logo_bbc_ws(),
    "nrk_p1": make_logo_nrk(1),
    "nrk_p2": make_logo_nrk(2),
    "nrk_p3": make_logo_nrk(3),
    "wbtz": make_logo_station("WBTZ", "99.9 FM", "Burlington, VT"),
    "kroq": make_logo_station("KROQ", "106.7 FM", "Los Angeles"),
    "groovesalad": make_logo_somafm("Groove Salad", "Ambient/Downtempo"),
    "indiepop": make_logo_somafm("Indie Pop", "Indie/Alternative"),
    "defcon": make_logo_somafm("DEF CON", "Hacker Music"),
    "eots": make_logo_eots(),
    "usb": make_logo_usb(),
    "aux": make_logo_aux(),
}

os.makedirs("/Users/ljosa/src/shelbourne/logos", exist_ok=True)
for name, img in logos.items():
    img.save(f"/Users/ljosa/src/shelbourne/logos/{name}.png")

header = [
    "/* Auto-generated station logos — 128x100 8-bit grayscale */",
    "/* Radio logos use top 50 rows only; bottom half for runtime text */",
    "/* Generated by gen_logos.py */",
    "",
    "#ifndef STATION_LOGOS_H",
    "#define STATION_LOGOS_H",
    "",
    "#include <stdint.h>",
    "",
]

for name, img in logos.items():
    header.append(img_to_c_array(img, name))
    header.append("")

header.append("#endif /* STATION_LOGOS_H */")
header.append("")

with open("/Users/ljosa/src/shelbourne/station_logos.h", "w") as f:
    f.write("\n".join(header))

print(f"Generated {len(logos)} logos")
print(f"Header: station_logos.h ({os.path.getsize('/Users/ljosa/src/shelbourne/station_logos.h')} bytes)")
print(f"PNGs saved to logos/")
