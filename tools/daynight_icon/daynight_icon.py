"""Day/night icon for Endless Keeper: pixel art in the game's palette (data/palette.dat).

Writes:
  - src/world_daynight_art.h of this repository (or the path given with --header): the disc art (day half with
    the sun on top, night half with the moon below) as a 128x128 palette-index image, 4x the size it is shown at,
    and the fixed rim (36x36), both run-length encoded;
  - daynight_icon_preview.png next to this script: the rim and disc at several times of day, rotated the way the
    engine does it (each shown pixel is the most common colour of 4x4 samples), scaled up 6x.

The engine rotates the disc a half turn over the day (sunrise left, noon top, sunset right) and a half turn over
the night (moon on top at midnight).
Usage: python daynight_icon.py [--header PATH] [--palette PATH]
"""
import argparse
import math
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
DISC = 32            # disc diameter shown, in game pixels (640-wide GUI units)
RIM = 36             # rim size
SRC = DISC * 4       # source art size (4x: the engine takes 4x4 samples per shown pixel)
ART = SRC * 4        # drawing size before the source is made (majority of 4x4 again)

# palette indices (data/palette.dat): checked against the palette in main()
C = {
    "sky_hi": 95, "sky": 94, "sky_lo": 93, "sky_horizon": 92,
    "night": 88, "night_lo": 87, "night_rim": 254,
    "sun_core": 141, "sun": 140, "sun_edge": 76, "ray": 75,
    "cloud": 31, "cloud_shade": 205,
    "moon": 143, "moon_shade": 235,
    "star": 199, "star_dim": 90,
    "rim_dark": 254, "rim_shadow": 81, "rim": 8, "rim_mid": 9, "rim_hi": 123, "rim_glint": 14,
    "gold": 232, "gold_hi": 141, "gold_dark": 231,
}


def load_palette(path):
    raw = open(path, "rb").read()
    if len(raw) < 768:
        sys.exit(f"{path}: not a 768-byte palette")
    return [tuple((raw[i * 3 + k] << 2) | (raw[i * 3 + k] >> 4) for k in range(3)) for i in range(256)]


def draw_disc_art():
    """The disc at ART x ART, in palette indices (0 outside). y grows downwards; the day half is on top."""
    img = [[0] * ART for _ in range(ART)]
    R = ART / 2.0

    def inside(px, py):  # disc coordinates: (0,0) centre, radius 1, y up
        return px * px + py * py <= 1.0

    stars = [(-0.56, -0.30, 1.0), (0.58, -0.40, 0.8), (-0.26, -0.74, 0.7), (0.30, -0.16, 0.55),
             (-0.72, -0.08, 0.45), (0.12, -0.84, 0.45), (0.74, -0.18, 0.4)]
    clouds = [(-0.56, 0.24, 0.20), (-0.40, 0.30, 0.17), (-0.70, 0.30, 0.13),
              (0.48, 0.66, 0.13), (0.62, 0.62, 0.11), (0.36, 0.62, 0.10)]
    for y in range(ART):
        for x in range(ART):
            px = (x + 0.5 - R) / R
            py = (R - (y + 0.5)) / R
            d = math.hypot(px, py)
            if d > 1.0:
                continue
            if py >= 0:                                   # day
                col = C["sky_hi"] if py > 0.62 else C["sky"] if py > 0.24 else C["sky_lo"]
                if py < 0.07:
                    col = C["sky_horizon"]
                # sun at (0, 0.48)
                sx, sy = px, py - 0.48
                sd = math.hypot(sx, sy)
                ang = math.atan2(sy, sx)
                ray = abs(((ang / (2 * math.pi) * 8) % 1.0) - 0.5) * 2  # 1 at a ray's centre line
                if sd < 0.135:
                    col = C["sun_core"]
                elif sd < 0.19:
                    col = C["sun"]
                elif sd < 0.215:
                    col = C["sun_edge"]
                elif 0.24 < sd < 0.33 - 0.11 * (1 - ray) and ray > 0.55:
                    col = C["ray"]
                for (cx, cy, cr) in clouds:
                    if math.hypot(px - cx, (py - cy) * 1.25) < cr:
                        col = C["cloud_shade"] if (py - cy) < -0.45 * cr else C["cloud"]
            else:                                         # night
                col = C["night"] if d < 0.80 else C["night_lo"]
                if py > -0.06:
                    col = C["night_lo"]
                # crescent moon at (0, -0.50): a disc minus a disc moved up-right
                mx, my = px, py + 0.50
                if math.hypot(mx, my) < 0.26 and math.hypot(mx - 0.11, my - 0.10) > 0.21:
                    col = C["moon_shade"] if math.hypot(mx, my) > 0.225 else C["moon"]
                for (sx0, sy0, s) in stars:
                    ax, ay = abs(px - sx0), abs(py - sy0)
                    arm = 0.085 * s
                    if (ax < 0.018 and ay < arm) or (ay < 0.018 and ax < arm):
                        col = C["star"] if (ax < 0.018 and ay < 0.018) or s > 0.6 else C["star_dim"]
            if d > 0.955:
                col = C["night_rim"] if py < 0 else C["sky_horizon"]
            img[y][x] = col
    return img


def majority_downsample(img, f):
    n = len(img) // f
    out = [[0] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            cnt = Counter(img[y * f + j][x * f + i] for j in range(f) for i in range(f))
            col, k = cnt.most_common(1)[0]
            if col == 0 and k < f * f:  # an edge pixel: keep it if most samples are inside... handled below
                inside = [c for c in cnt.elements() if c]
                col = Counter(inside).most_common(1)[0][0] if len(inside) * 2 >= f * f else 0
            out[y][x] = col
    return out


def rotate_disc(src, theta):
    """What the engine shows: DISC x DISC, each pixel the most common colour of 4x4 samples of the source,
    turned by theta (radians, clockwise on screen)."""
    out = [[0] * DISC for _ in range(DISC)]
    c, s = math.cos(theta), math.sin(theta)
    half = DISC / 2.0
    k = SRC / DISC
    for y in range(DISC):
        for x in range(DISC):
            if (x + 0.5 - half) ** 2 + (y + 0.5 - half) ** 2 > half * half:
                continue
            cnt = Counter()
            for j in range(4):
                for i in range(4):
                    dx = x + (i + 0.5) / 4 - half
                    dy = y + (j + 0.5) / 4 - half
                    # inverse rotation: screen -> source (clockwise on screen is counter-clockwise back)
                    sx = c * dx + s * dy
                    sy = -s * dx + c * dy
                    ix, iy = int((sx + half) * k), int((sy + half) * k)
                    if 0 <= ix < SRC and 0 <= iy < SRC and src[iy][ix]:
                        cnt[src[iy][ix]] += 1
            if cnt:
                out[y][x] = cnt.most_common(1)[0][0]
    return out


def draw_rim():
    """RIM x RIM fixed frame: a bevelled bronze ring with a gold marker at the top (where 'now' is)."""
    img = [[0] * RIM for _ in range(RIM)]
    c = RIM / 2.0
    r_in = DISC / 2.0
    for y in range(RIM):
        for x in range(RIM):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            d = math.hypot(dx, dy)
            if r_in - 0.6 <= d < c:
                light = (-dx - dy) / (d * math.sqrt(2))      # +1 towards the top left
                if d < r_in + 0.4:
                    col = C["rim_dark"]                      # dark line round the disc
                elif d > c - 0.9:
                    col = C["rim_shadow"] if light < 0.3 else C["rim"]
                else:
                    col = C["rim_glint"] if light > 0.75 else C["rim_hi"] if light > 0.25 else \
                        C["rim_mid"] if light > -0.35 else C["rim"]
                img[y][x] = col
    # marker: a small gold wedge over the top of the ring, pointing at the disc
    mid = RIM // 2
    wedge = [(-3, 0), (-2, 0), (-1, 0), (0, 0), (1, 0), (2, 0), (-2, 1), (-1, 1), (0, 1), (1, 1), (-1, 2), (0, 2),
             (-1, 3), (0, 3)]
    for (wx, wy) in wedge:
        x, y = mid + wx, wy
        img[y][x] = C["gold_hi"] if (wy == 0 and wx in (-1, 0)) else C["gold"] if wx < 0 else C["gold_dark"]
    for (wx, wy) in ((-4, 0), (3, 0), (-3, 1), (2, 1), (-2, 2), (1, 2), (-2, 3), (1, 3), (-1, 4), (0, 4)):
        x, y = mid + wx, wy
        if img[y][x] not in (C["gold"], C["gold_hi"], C["gold_dark"]):
            img[y][x] = C["rim_dark"]
    return img


def rle_rows(img):
    """Per row: (run length, colour) pairs, colour 0 transparent; flattened bytes."""
    out = []
    for row in img:
        x = 0
        while x < len(row):
            col = row[x]
            n = 1
            while x + n < len(row) and row[x + n] == col and n < 255:
                n += 1
            out += [n, col]
            x += n
    return out


def c_array(name, data, width):
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(str(v) for v in data[i:i + 24]) + ",")
    return f"static const unsigned char {name}[{len(data)}] = {{ /* {width}x{width}: (count, colour) runs per row */\n" + \
        "\n".join(lines) + "\n};\n"


def theta_for(phase):
    """phase 0..1 over the cycle with the day as the first 12/20: -90 deg at sunrise, 0 at noon, 180 at midnight."""
    day = 12.0 / 20.0
    if phase < day:
        return math.radians(-90 + 180 * phase / day)
    return math.radians(90 + 180 * (phase - day) / (1 - day))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--palette", default=os.path.normpath(os.path.join(HERE, "..", "..", "data", "palette.dat")))
    ap.add_argument("--header", default=None)
    a = ap.parse_args()
    pal = load_palette(a.palette)
    art = draw_disc_art()
    src = majority_downsample(art, ART // SRC)
    rim = draw_rim()

    from PIL import Image
    scale = 6
    labels = [("sunrise", 0.0), ("morning", 0.15), ("noon", 0.30), ("afternoon", 0.45), ("sunset", 0.60),
              ("evening", 0.70), ("midnight", 0.80), ("late night", 0.90)]
    pad = 6
    sheet = Image.new("RGB", (len(labels) * (RIM + pad) * scale + pad * scale, (RIM + 2 * pad) * scale), (24, 18, 10))
    for n, (_, ph) in enumerate(labels):
        frame = [row[:] for row in rim]
        disc = rotate_disc(src, theta_for(ph))
        off = (RIM - DISC) // 2
        for y in range(DISC):
            for x in range(DISC):
                if disc[y][x] and not frame[y + off][x + off]:
                    frame[y + off][x + off] = disc[y][x]
        # the marker overlaps the disc's top edge
        for y in range(RIM):
            for x in range(RIM):
                if rim[y][x]:
                    frame[y][x] = rim[y][x]
        im = Image.new("RGB", (RIM, RIM), (24, 18, 10))
        for y in range(RIM):
            for x in range(RIM):
                if frame[y][x]:
                    im.putpixel((x, y), pal[frame[y][x]])
        im = im.resize((RIM * scale, RIM * scale), Image.NEAREST)
        sheet.paste(im, ((pad + n * (RIM + pad)) * scale, pad * scale))
    prev = os.path.join(HERE, "daynight_icon_preview.png")
    sheet.save(prev)
    print("preview:", prev, "(", ", ".join(l for l, _ in labels), ")")

    if a.header:
        disc_rle, rim_rle = rle_rows(src), rle_rows(rim)
        text = ("/* Generated by tools/daynight_icon/daynight_icon.py: do not edit by hand. */\n"
                "#ifndef DK_WORLD_DAYNIGHT_ART_H\n#define DK_WORLD_DAYNIGHT_ART_H\n\n"
                f"#define DAYNIGHT_DISC {DISC}\n#define DAYNIGHT_RIM {RIM}\n#define DAYNIGHT_SRC {SRC}\n\n"
                "/* the disc, 4x: the day half (sun) on top, the night half (moon) below */\n" +
                c_array("daynight_disc_rle", disc_rle, SRC) +
                "\n/* the fixed rim with the gold marker at the top */\n" + c_array("daynight_rim_rle", rim_rle, RIM) +
                "\n#endif\n")
        with open(a.header, "w", newline="\n") as f:
            f.write(text)
        print("header:", a.header, f"(disc {len(disc_rle)} bytes, rim {len(rim_rle)} bytes)")


if __name__ == "__main__":
    main()
