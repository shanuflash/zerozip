"""Draws the ZeroZip icon into zerozip.ico and the package logos: a mint archive with
its zipper partly opened. Pure Python, needs nothing installed:
    python assets/make_icon.py"""
import math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def png(w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def box(x, y, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle centred at (cx, cy)."""
    qx, qy = abs(x - cx) - (hw - r), abs(y - cy) - (hh - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def seg(x, y, ax, ay, bx, by):
    """Distance to the segment a-b."""
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def mix(a, b, t):
    return [a[i] + (b[i] - a[i]) * t for i in range(3)]


TOP, BOTTOM = (94, 234, 212), (40, 200, 140)          # the mint/teal brand gradient
INSIDE_TOP, INSIDE_BOTTOM = (6, 46, 36), (4, 28, 22)
LIGHT = (240, 255, 250)


def sample(x, y, detail):
    """Colour and alpha of the icon at (x, y) in 0..1 space."""
    cx, cy, hw, hh, rad = 0.5, 0.5, 0.36, 0.41, 0.1
    out, a = [0.0, 0.0, 0.0], 0.0
    sh = box(x, y, cx, cy + 0.035, hw - 0.01, hh, rad)   # contact shadow under the body
    if sh < 0.06:
        out, a = [0, 0, 0], 0.28 * max(0.0, min(1.0, (0.06 - sh) / 0.06)) ** 2
    d = box(x, y, cx, cy, hw, hh, rad)
    if d > 0:
        return out, a
    t = (y - (cy - hh)) / (2 * hh)
    col = mix(TOP, BOTTOM, t)
    # the zipper runs down the middle; below the slider it has come apart
    slider_y = 0.56
    if y > slider_y:
        spread = 0.62
        if abs(x - cx) < (y - slider_y) * spread:
            col = mix(INSIDE_TOP, INSIDE_BOTTOM, t)
        reach = cy + hh - slider_y
        edge = min(seg(x, y, cx, slider_y, cx - spread * reach, cy + hh),
                   seg(x, y, cx, slider_y, cx + spread * reach, cy + hh))
        if edge < 0.022:
            col = mix(col, LIGHT, 0.92)
    elif detail:
        # teeth: small alternating blocks either side of the centre line
        pitch = 0.055
        k = math.floor((y - (cy - hh)) / pitch)
        side = -1 if k % 2 == 0 else 1
        if box(x, y, cx + side * 0.013, (cy - hh) + (k + 0.5) * pitch, 0.028, 0.024, 0.008) < 0:
            col = mix(col, LIGHT, 0.95)
    elif abs(x - cx) < 0.03:
        col = mix(col, LIGHT, 0.95)
    # slider and its pull tab
    sl = box(x, y, cx, slider_y - 0.01, 0.06, 0.05, 0.025)
    tab = box(x, y, cx, slider_y + 0.075, 0.032, 0.05, 0.03)
    hole = box(x, y, cx, slider_y + 0.085, 0.012, 0.022, 0.012)
    if sl < 0 or (tab < 0 and hole > 0):
        col = [255, 255, 255]
    elif min(sl, tab) < 0.012:
        col = mix(col, (4, 40, 30), 0.35)
    # top highlight and a thin darker rim at the bottom
    if d > -0.018:
        col = mix(col, (255, 255, 255) if y < cy else (6, 60, 44), 0.18)
    edge_a = max(0.0, min(1.0, -d * 220))
    out = [out[i] * (1 - edge_a) + col[i] * edge_a for i in range(3)]
    return out, a * (1 - edge_a) + edge_a


def render(size, ss):
    n = size * ss
    acc = [0.0] * (size * size * 4)
    detail = size >= 32
    for py in range(n):
        for px in range(n):
            col, a = sample((px + 0.5) / n, (py + 0.5) / n, detail)
            i = ((py // ss) * size + px // ss) * 4
            for k in range(3):
                acc[i + k] += col[k] * a
            acc[i + 3] += a
    out = bytearray(size * size * 4)
    m = ss * ss
    for i in range(size * size):
        a = acc[i * 4 + 3]
        out[i * 4 + 3] = int(round(a / m * 255))
        for k in range(3):
            out[i * 4 + k] = int(round(min(255.0, acc[i * 4 + k] / a))) if a else 0
    return bytes(out)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    images = [png(s, s, render(s, 6 if s <= 48 else 3 if s <= 64 else 2)) for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    dirs = b""
    for s, img in zip(sizes, images):
        dirs += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), off)
        off += len(img)
    with open(os.path.join(HERE, "zerozip.ico"), "wb") as f:
        f.write(head + dirs + b"".join(images))
    with open(os.path.join(HERE, "zerozip-256.png"), "wb") as f:
        f.write(images[-1])
    # logos the right-click menu package needs
    for name, s in (("StoreLogo.png", 50), ("Square44x44Logo.png", 44), ("Square150x150Logo.png", 150)):
        with open(os.path.join(HERE, "package", name), "wb") as f:
            f.write(png(s, s, render(s, 4)))


if __name__ == "__main__":
    main()
