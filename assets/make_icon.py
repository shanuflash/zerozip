"""Draws the ZeroZip icon (a glowing zero on a dark tile) into zerozip.ico.
Pure Python so it needs nothing installed:  python assets/make_icon.py"""
import math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def png(w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rounded_rect_dist(x, y, s, r):
    # signed distance to a rounded square of side s centred at s/2
    qx, qy = abs(x - s / 2) - (s / 2 - r), abs(y - s / 2) - (s / 2 - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def render(size, ss=4):
    S = size * ss
    acc = [0.0] * (size * size * 4)
    for py in range(S):
        for px in range(S):
            x, y = (px + 0.5) / S, (py + 0.5) / S
            d = rounded_rect_dist(x, y, 1.0, 0.23)
            if d > 0:
                continue
            # tile: near-black with a faint top highlight
            col = [10 + 8 * (1 - y), 12 + 8 * (1 - y), 15 + 9 * (1 - y)]
            # zero: an ellipse ring
            ex, ey = (x - 0.5) / 0.205, (y - 0.5) / 0.285
            rr = math.hypot(ex, ey)
            ring = abs(rr - 1.0) * 0.24
            width = 0.052
            glow = max(0.0, 1 - ring / 0.16) ** 3 * 0.55
            g = (0.16 + 0.84 * (1 - y))           # gradient top -> bottom
            mint = (52 + 60 * g, 211 + 30 * g, 153 + 60 * g)
            if ring < width:
                a = min(1.0, (width - ring) / 0.012)
                col = [c * (1 - a) + m * a for c, m in zip(col, mint)]
            else:
                col = [min(255, c + m * glow * 0.35) for c, m in zip(col, mint)]
            # hairline edge on the tile
            if d > -0.012:
                col = [c + 18 for c in col]
            i = ((py // ss) * size + (px // ss)) * 4
            for k in range(3):
                acc[i + k] += col[k]
            acc[i + 3] += 255
    n = ss * ss
    out = bytearray(size * size * 4)
    for i in range(size * size):
        a = acc[i * 4 + 3] / n
        out[i * 4 + 3] = int(round(a))
        for k in range(3):
            out[i * 4 + k] = int(round(acc[i * 4 + k] / n / (a / 255))) if a else 0
    return bytes(out)


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [png(s, s, render(s, 4 if s <= 64 else 2)) for s in sizes]
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


if __name__ == "__main__":
    main()
