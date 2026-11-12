"""Tile every snapshot in out/seq/ (from --dump-every) into one labelled PNG (contact sheet).

    python tools/contact_sheet.py [seqdir] [out.png] [columns]
"""
import pathlib
import struct
import sys
import zlib

seq = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out/seq")
out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else seq / "sheet.png")
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
here = pathlib.Path(__file__).parent


def read_png(p):
    data = p.read_bytes()
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        n, t = struct.unpack(">I4s", data[i:i + 8])
        if t == b"IHDR":
            w, h = struct.unpack(">II", data[i + 8:i + 16])
        elif t == b"IDAT":
            idat += data[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


# 3x5 digit font for frame labels
FONT = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
        "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
        "8": "111101111101111", "9": "111101111001111"}

frames = sorted(p for p in seq.iterdir() if p.is_dir())
tiles = []
for f in frames:
    png = f / "frame.png"   # written by: magicalpopn.exe --headless N --dump-every K
    if not png.exists():
        continue
    tiles.append((f.name.lstrip("f").lstrip("0") or "0", read_png(png)))

W, H, PAD = max(t[1][0] for t in tiles), max(t[1][1] for t in tiles), 4
rows = (len(tiles) + cols - 1) // cols
SW, SH = cols * (W + PAD) + PAD, rows * (H + PAD + 12) + PAD
canvas = [bytearray(b"\x20" * (SW * 3)) for _ in range(SH)]
for k, (label, (w, h, px)) in enumerate(tiles):
    ox, oy = PAD + (k % cols) * (W + PAD), PAD + (k // cols) * (H + PAD + 12)
    for ci, ch in enumerate(label):
        for b, bit in enumerate(FONT[ch]):
            if bit == "1":
                for dy in range(2):
                    for dx in range(2):
                        x, y = ox + ci * 8 + (b % 3) * 2 + dx, oy + (b // 3) * 2 + dy
                        canvas[y][x * 3:x * 3 + 3] = b"\xff\xff\xff"
    for y in range(h):
        canvas[oy + 12 + y][ox * 3:(ox + w) * 3] = px[y]
raw = b"".join(b"\x00" + bytes(r) for r in canvas)
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
out.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SW, SH, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
print(f"{len(tiles)} frames -> {out}")
