#!/usr/bin/env python3
"""Generate 128x100 8-bit grayscale station logos as C header."""

from PIL import Image, ImageDraw, ImageFont
import struct, sys, os

W, H = 128, 100

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

def make_logo_vpr():
    """Vermont Public Radio - green mountain state theme."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(36)
    small = find_font(14)
    tiny = find_font(11)
    center_text(draw, "VPR", big, 8)
    # Draw a simple mountain silhouette
    pts = [(0, 70), (30, 45), (50, 60), (64, 35), (78, 60), (98, 45), (128, 70)]
    for i in range(len(pts) - 1):
        draw.line([pts[i], pts[i+1]], fill=180, width=2)
    # Fill below the mountain line
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i+1]
        for x in range(x0, x1 + 1):
            if x1 != x0:
                y = y0 + (y1 - y0) * (x - x0) // (x1 - x0)
            else:
                y = y0
            for yy in range(y, 72):
                if 0 <= x < W and 0 <= yy < H:
                    img.putpixel((x, yy), max(img.getpixel((x, yy)), 60))
    center_text(draw, "VERMONT PUBLIC", tiny, 75, fill=160)
    center_text(draw, "RADIO", tiny, 87, fill=160)
    return img

def make_logo_wdev():
    """WDEV - Waterbury VT."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(38)
    med = find_font(16)
    small = find_font(12)
    center_text(draw, "WDEV", big, 5)
    # Radio wave arcs
    cx, cy = 64, 55
    for r in [12, 20, 28]:
        draw.arc([cx-r, cy-r, cx+r, cy+r], 220, 320, fill=140, width=2)
    center_text(draw, "96.1 FM", med, 72, fill=200)
    center_text(draw, "Waterbury, VT", small, 87, fill=120)
    return img

def make_logo_bbc_ws():
    """BBC World Service."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(28)
    med = find_bold_font(18)
    small = find_font(12)
    # Draw BBC text in a box
    draw.rectangle([14, 8, 114, 42], outline=255, width=2)
    center_text(draw, "BBC", big, 10)
    # Globe hint: simple circle with lines
    cx, cy = 64, 62
    r = 14
    draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline=160, width=1)
    draw.arc([cx-r+4, cy-r, cx+r-4, cy+r], 0, 360, fill=120, width=1)
    draw.line([(cx-r, cy), (cx+r, cy)], fill=120, width=1)
    center_text(draw, "WORLD SERVICE", small, 82, fill=180)
    return img

def make_logo_nrk(channel):
    """NRK P1/P2/P3."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(32)
    huge = find_bold_font(52)
    small = find_font(12)
    center_text(draw, "NRK", big, 4)
    # Big P number
    center_text(draw, f"P{channel}", huge, 35)
    # Underline
    draw.line([(20, 88), (108, 88)], fill=140, width=1)
    return img

def make_logo_station(call, freq, location):
    """Generic radio station with call sign."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(34)
    med = find_font(16)
    small = find_font(11)
    center_text(draw, call, big, 10)
    # Radio tower icon (simple)
    cx = 64
    draw.line([(cx, 50), (cx, 70)], fill=200, width=2)
    draw.line([(cx-3, 50), (cx, 45), (cx+3, 50)], fill=200, width=1)
    # Waves
    for r in [8, 14, 20]:
        draw.arc([cx-r, 42-r, cx+r, 42+r], 220, 320, fill=120, width=1)
    if freq:
        center_text(draw, freq, med, 73, fill=200)
    if location:
        center_text(draw, location, small, 89, fill=120)
    return img

def make_logo_usb():
    """USB / local files."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    big = find_bold_font(28)
    med = find_font(14)
    # USB icon (simplified)
    cx, cy = 64, 40
    # Main body rectangle
    draw.rounded_rectangle([cx-18, cy-15, cx+18, cy+15], radius=3, outline=200, width=2)
    # Connector tab
    draw.rectangle([cx-8, cy+15, cx+8, cy+22], outline=200, width=2)
    # USB symbol inside
    draw.line([(cx, cy-8), (cx, cy+8)], fill=200, width=2)
    draw.line([(cx-5, cy-3), (cx, cy-8), (cx+5, cy-3)], fill=200, width=1)
    center_text(draw, "USB", big, 65)
    center_text(draw, "Music", med, 88, fill=160)
    return img

def make_logo_aux():
    """AUX input."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    huge = find_bold_font(44)
    med = find_font(14)
    # 3.5mm jack icon
    cx, cy = 64, 35
    # Plug tip
    draw.ellipse([cx-8, cy-8, cx+8, cy+8], outline=180, width=2)
    draw.ellipse([cx-3, cy-3, cx+3, cy+3], fill=180)
    # Cable
    draw.line([(cx, cy+8), (cx, cy+30)], fill=180, width=3)
    # Rings
    draw.arc([cx-12, cy-2, cx+12, cy+14], 0, 180, fill=140, width=1)
    center_text(draw, "AUX", huge, 55)
    return img

def make_logo_somafm(name, subtitle):
    """SomaFM channel."""
    img = Image.new('L', (W, H), 0)
    draw = ImageDraw.Draw(img)
    tiny = find_font(11)
    small = find_font(12)
    # SomaFM branding at top
    center_text(draw, "SomaFM", tiny, 2, fill=120)
    draw.line([(10, 14), (118, 14)], fill=60, width=1)
    # Channel name - fit to width
    name_len = len(name)
    if name_len <= 6:
        font_size = 36
    elif name_len <= 8:
        font_size = 28
    elif name_len <= 12:
        font_size = 22
    else:
        font_size = 18
    big = find_bold_font(font_size)
    center_text(draw, name, big, 20)
    # Sound wave visualization at bottom
    import math
    for x in range(W):
        amp = 8 + 4 * math.sin(x * 0.15) + 2 * math.sin(x * 0.3)
        y_base = 68
        for dy in range(-int(amp), int(amp)):
            brightness = int(100 * (1 - abs(dy) / amp))
            if 0 <= y_base + dy < H:
                img.putpixel((x, y_base + dy), max(img.getpixel((x, y_base + dy)), brightness))
    if subtitle:
        center_text(draw, subtitle, small, 84, fill=160)
    return img

def img_to_c_array(img, name):
    """Convert image to C array declaration."""
    data = list(img.tobytes())
    lines = []
    lines.append(f"static const uint8_t logo_{name}[{W * H}] = {{")
    for row in range(H):
        row_data = data[row * W:(row + 1) * W]
        hex_vals = ", ".join(f"0x{b:02x}" for b in row_data)
        lines.append(f"    {hex_vals},")
    lines.append("};")
    return "\n".join(lines)

# Generate all logos
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
    "usb": make_logo_usb(),
    "aux": make_logo_aux(),
}

# Save preview PNGs
os.makedirs("/Users/ljosa/src/shelbourne/logos", exist_ok=True)
for name, img in logos.items():
    img.save(f"/Users/ljosa/src/shelbourne/logos/{name}.png")

# Generate C header
header = [
    "/* Auto-generated station logos — 128x100 8-bit grayscale */",
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
