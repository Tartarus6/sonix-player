#!/usr/bin/env python3
"""
Draws the More-page tile for Crossy Road -- a chicken on the same pair of green
discs the other tiles stand on -- and writes it as
src/gui/crossyroad/crossy_icon.c (+ .h), ARGB8888, 112x112.

Needs nothing but Python: the picture is made by a small supersampling
rasteriser, so there is no cairosvg or pillow to install. The generated file is
checked in; run this only to change the drawing:

    python3 tools/gen_crossy_icon.py [preview.png]
"""
import math
import os
import struct
import sys
import zlib

W = 112
SS = 4  # samples per pixel along each axis

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(REPO, "src", "gui", "crossyroad", "crossy_icon.c")
OUT_H = os.path.join(REPO, "src", "gui", "crossyroad", "crossy_icon.h")

# The two discs, measured from the Flappy Bird tile: a solid one up and to the
# left, a translucent one down and to the right, each a green that darkens
# towards the bottom.
DISC_R = 36.0
SOLID = (41.5, 45.5)
GLASS = (69.5, 65.5)
GREEN_TOP = (141, 195, 111)
GREEN_BOTTOM = (119, 184, 83)
GLASS_ALPHA = 140 / 255.0
PALE = (205, 229, 191)
CUT = (112, 172, 82)

# The chicken, in units around (CX, CY), scaled by S.
CX, CY, S = 76.0, 58.0, 0.8


def disc_colour(cy, y):
    t = max(0.0, min(1.0, (y - (cy - DISC_R)) / (2 * DISC_R)))
    return tuple(GREEN_TOP[i] + (GREEN_BOTTOM[i] - GREEN_TOP[i]) * t for i in range(3))


def in_circle(x, y, cx, cy, r):
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_ellipse(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


def in_poly(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def in_bar(x, y, x1, y1, x2, y2, half):
    # A thick line segment.
    dx, dy = x2 - x1, y2 - y1
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / n))
    return (x - (x1 + t * dx)) ** 2 + (y - (y1 + t * dy)) ** 2 <= half * half


def p(dx, dy):
    return (CX + dx * S, CY + dy * S)


def chicken(x, y):
    """True where the pale silhouette is."""
    # Body, and the neck rising from it to the head.
    bx, by = p(-3, 7)
    if in_ellipse(x, y, bx, by, 19 * S, 14 * S):
        return True
    if in_poly(x, y, [p(0, -6), p(18, -8), p(18, 8), p(2, 10)]):
        return True
    hx, hy = p(10, -11)
    if in_circle(x, y, hx, hy, 9.5 * S):
        return True
    # Tail feathers, swept up and back.
    if in_poly(x, y, [p(-14, 6), p(-30, -4), p(-27, -17), p(-19, -9), p(-17, -19), p(-8, -4)]):
        return True
    # The comb: three bumps.
    for dx, dy, r in ((5.5, -20.5, 4.0), (11, -22.5, 4.5), (16.5, -20, 4.0)):
        cx, cy = p(dx, dy)
        if in_circle(x, y, cx, cy, r * S):
            return True
    # Beak and wattle.
    if in_poly(x, y, [p(18, -15), p(30, -10), p(18, -6)]):
        return True
    wx, wy = p(17, -2)
    if in_ellipse(x, y, wx, wy, 3.5 * S, 5 * S):
        return True
    # Legs and feet.
    for lx in (-7, 4):
        a, b = p(lx, 18)
        c, d = p(lx, 28)
        if in_bar(x, y, a, b, c, d, 1.8 * S):
            return True
        e, f = p(lx + 6, 28)
        g, h = p(lx - 1, 28)
        if in_bar(x, y, g, h, e, f, 1.8 * S):
            return True
    return False


def detail(x, y):
    """Where the silhouette is cut back to the green: eye and wing."""
    ex, ey = p(12.5, -12)
    if in_circle(x, y, ex, ey, 2.2 * S):
        return 1.0
    # The wing: the lower rim of an ellipse on the body, drawn as a stroke.
    wx, wy = p(-5, 5)
    rx, ry = 11 * S, 8 * S
    d = ((x - wx) / rx) ** 2 + ((y - wy) / ry) ** 2
    if 0.62 <= d <= 1.0 and y >= wy - 1.0 * S:
        return 0.7
    return 0.0


def render():
    pix = []
    for py in range(W):
        row = []
        for px in range(W):
            r = g = b = a = 0.0  # premultiplied accumulators
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    cr = cg = cb = ca = 0.0  # this sample, premultiplied
                    if in_circle(x, y, *SOLID, DISC_R):
                        c = disc_colour(SOLID[1], y)
                        cr, cg, cb, ca = c[0], c[1], c[2], 1.0
                    if in_circle(x, y, *GLASS, DISC_R):
                        c = disc_colour(GLASS[1], y)
                        k = GLASS_ALPHA
                        cr = c[0] * k + cr * (1 - k)
                        cg = c[1] * k + cg * (1 - k)
                        cb = c[2] * k + cb * (1 - k)
                        ca = k + ca * (1 - k)
                    if chicken(x, y):
                        cr, cg, cb, ca = PALE[0], PALE[1], PALE[2], 1.0
                        k = detail(x, y)
                        if k:
                            cr = CUT[0] * k + cr * (1 - k)
                            cg = CUT[1] * k + cg * (1 - k)
                            cb = CUT[2] * k + cb * (1 - k)
                    r += cr
                    g += cg
                    b += cb
                    a += ca
            n = SS * SS
            if a == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((round(r / a), round(g / a), round(b / a), round(255 * a / n)))
        pix.append(row)
    return pix


def write_png(path, pix):
    raw = b"".join(b"\x00" + bytes(v for q in row for v in q) for row in pix)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, W, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


def write_c(pix):
    data = bytearray()
    for row in pix:
        for r, g, b, a in row:
            # LVGL's ARGB8888 is stored blue, green, red, alpha in memory.
            data += bytes((b, g, r, a))
    lines = []
    for i in range(0, len(data), 16):
        lines.append("\t" + ", ".join("0x%02x" % v for v in data[i:i + 16]) + ",")
    with open(OUT_C, "w") as f:
        f.write('#include "crossy_icon.h"\n\n')
        f.write("// Generated by tools/gen_crossy_icon.py. Do not edit by hand.\n")
        f.write("static const uint8_t icon_menu_crossy_road_data[] = {\n")
        f.write("\n".join(lines) + "\n};\n\n")
        f.write("const lv_image_dsc_t icon_menu_crossy_road = {\n")
        f.write("\t.header = {\n\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n\t\t.cf = LV_COLOR_FORMAT_ARGB8888,\n")
        f.write("\t\t.w = %d,\n\t\t.h = %d,\n\t\t.stride = %d,\n\t},\n" % (W, W, W * 4))
        f.write("\t.data_size = sizeof(icon_menu_crossy_road_data),\n\t.data = icon_menu_crossy_road_data,\n};\n")
    with open(OUT_H, "w") as f:
        f.write("#ifndef GUI_CROSSY_ICON_H\n#define GUI_CROSSY_ICON_H\n\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write("// The More-page tile for Crossy Road, 112x112 ARGB8888.\n")
        f.write("extern const lv_image_dsc_t icon_menu_crossy_road;\n\n#endif /* GUI_CROSSY_ICON_H */\n")


if __name__ == "__main__":
    image = render()
    write_c(image)
    if len(sys.argv) > 1:
        write_png(sys.argv[1], image)
    print("wrote", OUT_C)
