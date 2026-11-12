"""Draws the PWA icons (a volume knob on a dark tile) into main/web/ with the standard library only (no Pillow needed):
    icon.svg                    vector version (browser tab, "any" purpose)
    icon-192.png, icon-512.png  "any" purpose, rounded tile
    icon-maskable-512.png       "maskable" purpose: full-bleed tile, artwork inside the central 80 % safe zone
    apple-touch-icon.png        180x180 for iOS (full-bleed: iOS rounds the corners itself)
Run by hand after changing the design:  python tools/make_icons.py
"""
import math, os, struct, zlib

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "main", "web")
BG, KNOB, RING, AMBER = (13, 14, 17), (34, 35, 41), (58, 60, 68), (225, 182, 106)


def ticks(scale):                       # 11 marks on a 270 degree arc, centred at the top (unit square, y down)
    out = []
    for i in range(11):
        a = math.radians(135 + 270 * i / 10)
        r0, r1 = 0.355 * scale, 0.405 * scale
        out.append((0.5 + r0 * math.cos(a), 0.5 + r0 * math.sin(a), 0.5 + r1 * math.cos(a), 0.5 + r1 * math.sin(a)))
    return out


def seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    t = max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - x0 - t * dx, py - y0 - t * dy)


def render(size, scale, rounded):
    ss = 2
    tk = ticks(scale)
    ang = math.radians(135 + 270 * 0.62)                 # the pointer
    p0 = (0.5 + 0.10 * scale * math.cos(ang), 0.5 + 0.10 * scale * math.sin(ang))
    p1 = (0.5 + 0.235 * scale * math.cos(ang), 0.5 + 0.235 * scale * math.sin(ang))
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    u, v = (x + (sx + .5) / ss) / size, (y + (sy + .5) / ss) / size
                    c, a = BG, 255
                    if rounded:                           # rounded square tile, radius 22 %
                        qx, qy = abs(u - .5) - (.5 - .22), abs(v - .5) - (.5 - .22)
                        d = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - .22
                        if d > 0: a = 0
                    if a:
                        r = math.hypot(u - .5, v - .5)
                        if r < .30 * scale: c = RING if r > .285 * scale else KNOB
                        for (x0, y0, x1, y1) in tk:
                            if seg_dist(u, v, x0, y0, x1, y1) < .011 * scale: c = AMBER
                        if seg_dist(u, v, *p0, *p1) < .020 * scale: c = AMBER
                    acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a
            n = ss * ss
            if acc[3]: row += bytes((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] // n))
            else: row += bytes((0, 0, 0, 0))
        rows.append(bytes(row))
    return rows


def png(path, size, scale, rounded):
    raw = b"".join(render(size, scale, rounded))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(os.path.join(OUT, path), "wb").write(data)
    print(path, len(data), "bytes")


def svg(scale=1.0):
    h = lambda c: "#%02x%02x%02x" % c
    s = 512
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">',
             '<rect width="512" height="512" rx="113" fill="%s"/>' % h(BG),
             '<circle cx="256" cy="256" r="%.1f" fill="%s" stroke="%s" stroke-width="%.1f"/>' % (.2925 * s * scale, h(KNOB), h(RING), .015 * s * scale)]
    for (x0, y0, x1, y1) in ticks(scale):
        parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (x0 * s, y0 * s, x1 * s, y1 * s, h(AMBER), .022 * s * scale))
    ang = math.radians(135 + 270 * 0.62)
    parts.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (
        (.5 + .10 * math.cos(ang)) * s, (.5 + .10 * math.sin(ang)) * s, (.5 + .235 * math.cos(ang)) * s, (.5 + .235 * math.sin(ang)) * s, h(AMBER), .04 * s))
    parts.append("</svg>\n")
    open(os.path.join(OUT, "icon.svg"), "w").write("\n".join(parts))
    print("icon.svg")


if __name__ == "__main__":
    svg()
    png("icon-192.png", 192, 1.0, True)
    png("icon-512.png", 512, 1.0, True)
    png("icon-maskable-512.png", 512, 0.88, False)
    png("apple-touch-icon.png", 180, 0.92, False)
