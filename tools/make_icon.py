#!/usr/bin/env python3
import math
import os
import struct
import sys
import zlib

W = H = 512


def lerp(a, b, t):
    return a + (b - a) * t


def pixel(x, y):
    t = y / (H - 1)
    r, g, b = lerp(30, 4, t), lerp(8, 0, t), lerp(60, 12, t)
    d = math.hypot(x - 256, y - 256)
    glow = math.exp(-((d / 210.0) ** 2))
    r += 110 * glow
    g += 30 * glow
    b += 170 * glow
    if 176 <= d <= 192:  # anel roxo
        r, g, b = 168, 85, 247
    if 215 <= x <= 350 and abs(y - 256) <= (350 - x) * 86 / 135:  # triângulo "play"
        r, g, b = 240, 225, 255
    return (min(255, int(r)), min(255, int(g)), min(255, int(b)))


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "data/sce_sys/icon0.png"
    if os.path.exists(out):
        print("icone ja existe, mantendo:", out)
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw.extend(pixel(x, y))
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(out, "wb") as f:
        f.write(png)
    print("icone gerado:", out)


main()
