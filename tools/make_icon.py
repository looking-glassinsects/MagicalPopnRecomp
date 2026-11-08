"""Build the app icon and the README picture from the heroine sprite.

    python tools/make_icon.py [SOURCE_IMAGE]

SOURCE_IMAGE may be an upscaled copy of the sprite (any whole-pixel grid is detected and
reduced to the original pixels); without it, assets/heroine.png is used. Writes:
    assets/heroine.png      the sprite at its native size
    assets/icon.ico         16-256 px, used for MagicalPopn.exe, Builder.exe and the builder window
    assets/heroine_x5.png   5x nearest-neighbour, shown at the top of the README
Needs Pillow (pip install pillow).
"""
import pathlib
import sys

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
ASSETS = REPO / "assets"


def native(im):
    """Reduce an upscaled pixel-art image to its original pixels (finds the grid size)."""
    w, h = im.size
    px = im.load()
    best = None
    for nw in range(16, min(w, 160) + 1):
        sx = w / nw
        if sx < 1:
            break
        nh = round(h / sx)
        sy = h / nh
        err = 0
        for j in range(0, nh, 2):
            for i in range(0, nw, 2):
                c = px[int((i + .5) * sx), int((j + .5) * sy)]
                for fx, fy in ((.25, .25), (.75, .75)):
                    d = px[min(w - 1, int((i + fx) * sx)), min(h - 1, int((j + fy) * sy))]
                    err += sum(abs(a - b) for a, b in zip(c, d))
        if best is None or err < best[0]:
            best = (err, nw, nh)
        if err == 0:
            break
    _, nw, nh = best
    out = Image.new("RGBA", (nw, nh))
    for j in range(nh):
        for i in range(nw):
            out.putpixel((i, j), px[int((i + .5) * w / nw), int((j + .5) * h / nh)])
    return out


def main():
    ASSETS.mkdir(exist_ok=True)
    if len(sys.argv) > 1:
        sprite = native(Image.open(sys.argv[1]).convert("RGBA"))
        sprite.save(ASSETS / "heroine.png")
    sprite = Image.open(ASSETS / "heroine.png").convert("RGBA")
    w, h = sprite.size
    side = max(w, h)
    square = Image.new("RGBA", (side, side))
    square.paste(sprite, ((side - w) // 2, (side - h) // 2))
    big = square.resize((side * 3, side * 3), Image.NEAREST)          # crisp base for every size
    frames = []
    for s in (256, 128, 64, 48, 32, 24, 16):
        if s >= side * 3:
            f = Image.new("RGBA", (s, s))
            f.paste(big, ((s - side * 3) // 2, (s - side * 3) // 2))
        else:
            f = big.resize((s, s), Image.LANCZOS)
        frames.append(f)
    frames[0].save(ASSETS / "icon.ico", sizes=[f.size for f in frames], append_images=frames[1:])
    sprite.resize((w * 5, h * 5), Image.NEAREST).save(ASSETS / "heroine_x5.png")
    print(f"sprite {w}x{h} -> {ASSETS / 'icon.ico'}, {ASSETS / 'heroine_x5.png'}")


if __name__ == "__main__":
    main()
