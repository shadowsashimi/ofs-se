"""Generates src/UI/OFS_VulvaHeightMap.h, the height map carved into the face of
the 3D simulator's sleeve.

Hand traced from a head-on reference photograph of the face: the outlines of
the labia majora, the crease inside them, the labia minora, the slit and the
hood are measured as half widths at heights down the face, in face radii, and
the map is built from those outlines rather than from the photograph's
brightness, which is lighting, not height. No part of the photograph ships.

Pure standard library, so it runs anywhere:
    python tools/gen-vulva-heightmap.py [preview.png]
"""
import math, os, struct, sys, zlib

N = 128  # texels a side, covering u and v from -1 to 1

# (v, outer half width of the majora, half width of the crease inside them,
# half width of the slit). v runs +1 at the top (the hood) to -1 at the
# bottom, u across the slit; both in face radii.
OUTLINE = [
    ( 0.95, 0.000, 0.000, 0.000),
    ( 0.82, 0.070, 0.000, 0.000),
    ( 0.64, 0.160, 0.050, 0.000),
    ( 0.46, 0.240, 0.110, 0.000),
    ( 0.26, 0.310, 0.150, 0.012),
    ( 0.02, 0.360, 0.170, 0.032),
    (-0.20, 0.370, 0.170, 0.028),
    (-0.34, 0.355, 0.160, 0.000),
    (-0.48, 0.330, 0.140, 0.000),
    (-0.66, 0.260, 0.090, 0.000),
    (-0.83, 0.140, 0.020, 0.000),
    (-0.96, 0.000, 0.000, 0.000),
]

# Heights relative to the crest of the majora.
MINORA = 0.62
# The floor of the crease between minora and majora: high, so it reads as a
# soft fold rather than a channel, which came to points where it was widest
# and where the two sides met at the bottom.
CREASE = 0.40
SLIT = -0.40
# The slit as an ellipse, so its ends are round: centre, half length and
# half width, in face radii.
SLIT_CENTRE = -0.04
SLIT_LENGTH = 0.30
SLIT_WIDTH = 0.030
HOOD = 0.50
NUB = 0.45
LOW, HIGH = -0.45, 1.0


def outline_at(v):
    if v >= OUTLINE[0][0] or v <= OUTLINE[-1][0]:
        return 0.0, 0.0, 0.0
    for a, b in zip(OUTLINE, OUTLINE[1:]):
        if a[0] >= v >= b[0]:
            t = (a[0] - v) / (a[0] - b[0])
            # Smoothstep between samples, so the outline has no corners.
            t = t * t * (3 - 2 * t)
            return tuple(a[k] + (b[k] - a[k]) * t for k in (1, 2, 3))
    return 0.0, 0.0, 0.0


def height(u, v):
    wo, wi, _ = outline_at(v)
    e = (v - SLIT_CENTRE) / SLIT_LENGTH
    ws = SLIT_WIDTH * math.sqrt(max(0.0, 1.0 - e * e))
    au = abs(u)
    h = 0.0
    if wo > 0.0 and au < wo:
        if au >= wi:
            # The majora: a full pillow from the crease out to the outline,
            # steeper on the crease side.
            s = (au - wi) / max(1e-4, wo - wi)
            h = math.sin(math.pi * s) ** 0.55
            # Blended into the crease floor rather than dropping below it.
            h = max(h, CREASE * (1.0 - s) ** 2)
        else:
            # Inside the crease: the minora standing either side of the slit.
            h = CREASE
            if wi - ws > 0.02:
                centre = ws + 0.50 * (wi - ws)
                spread = 0.55 * (wi - ws)
                h = max(h, MINORA * math.exp(-((au - centre) / spread) ** 2))
            if ws > 0.0 and au < ws * 1.6:
                k = max(0.0, 1.0 - au / (ws * 1.6))
                h = h + (SLIT - h) * (k ** 0.7)
        # The ends taper away towards the rim.
        taper = min(1.0, max(0.0, (0.97 - abs(v)) / 0.22)) ** 0.5
        h *= taper
    # The hood, where the minora meet at the top, and the small nub under it.
    h = max(h, HOOD * math.exp(-((u / 0.075) ** 2) - (((v - 0.50) / 0.12) ** 2)))
    h = max(h, NUB * math.exp(-((u / 0.035) ** 2) - (((v - 0.36) / 0.035) ** 2)))
    return h


def blur(grid, passes):
    for _ in range(passes):
        out = [[0.0] * N for _ in range(N)]
        for y in range(N):
            for x in range(N):
                acc = 0.0
                wsum = 0.0
                for dy, wy in ((-1, 1), (0, 2), (1, 1)):
                    yy = min(N - 1, max(0, y + dy))
                    for dx, wx in ((-1, 1), (0, 2), (1, 1)):
                        xx = min(N - 1, max(0, x + dx))
                        acc += grid[yy][xx] * wy * wx
                        wsum += wy * wx
                out[y][x] = acc / wsum
        grid = out
    return grid


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Row 0 is v = +1, the top; column 0 is u = -1.
    grid = [[height(-1 + (2 * x + 1) / N, 1 - (2 * y + 1) / N) for x in range(N)] for y in range(N)]
    grid = blur(grid, 3)
    # Cavity: how far each texel sits below its surroundings, for shading the
    # creases and the slit deeper.
    wide = blur(grid, 10)
    cavity = [[min(1.0, max(0.0, (wide[y][x] - grid[y][x]) * 1.6)) for x in range(N)] for y in range(N)]

    def q(val, lo, hi):
        return max(0, min(255, int(round((val - lo) / (hi - lo) * 255))))

    hbytes = [q(grid[y][x], LOW, HIGH) for y in range(N) for x in range(N)]
    cbytes = [q(cavity[y][x], 0.0, 1.0) for y in range(N) for x in range(N)]

    def table(name, data):
        lines = ['static constexpr uint8_t %s[VulvaMapSize * VulvaMapSize] = {' % name]
        for y in range(N):
            lines.append('    ' + ','.join(str(b) for b in data[y * N:(y + 1) * N]) + ',')
        lines.append('};')
        return '\n'.join(lines)

    header = '\n'.join([
        '// Generated by tools/gen-vulva-heightmap.py; edit that and rerun rather',
        '// than editing this. The height of the face of the sleeve over the face,',
        '// hand traced from a head-on reference: row 0 is the top, the hood; column',
        '// 0 is the left edge. u and v both run -1 to 1 across the face.',
        '#pragma once',
        '#include <cstdint>',
        '',
        'constexpr int32_t VulvaMapSize = %d;' % N,
        '// The heights the bytes span, relative to the crest of the majora.',
        'constexpr float VulvaMapLow = %.3ff;' % LOW,
        'constexpr float VulvaMapHigh = %.3ff;' % HIGH,
        '',
        table('VulvaHeightMap', hbytes),
        '',
        '// How far below its surroundings each texel sits, 0 to 1.',
        table('VulvaCavityMap', cbytes),
        '',
    ])
    with open(os.path.join(root, 'src', 'UI', 'OFS_VulvaHeightMap.h'), 'wb') as f:
        f.write(header.replace('\n', '\r\n').encode('ascii'))

    if len(sys.argv) > 1:
        # A preview: height as grey, the cavity tinted red, four times the size.
        S = 4
        raw = bytearray()
        for y in range(N * S):
            raw.append(0)
            for x in range(N * S):
                hb = hbytes[(y // S) * N + x // S]
                cb = cbytes[(y // S) * N + x // S]
                raw += bytes((hb, max(0, hb - cb), max(0, hb - cb)))
        def chunk(tag, data):
            return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N * S, N * S, 8, 2, 0, 0, 0)) \
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
        with open(sys.argv[1], 'wb') as f:
            f.write(png)
    print('ok')


if __name__ == '__main__':
    main()
