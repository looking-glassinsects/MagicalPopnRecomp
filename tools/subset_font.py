"""Cut the menu font down to the characters the menu can show (ASCII + every non-ASCII
character in runtime/menu.c). Run again after adding menu text:

    python tools/subset_font.py FULL_FONT.ttf
Needs fonttools (pip install fonttools).
"""
import pathlib
import sys

from fontTools import subset

REPO = pathlib.Path(__file__).resolve().parent.parent
src = REPO / "runtime" / "menu.c"
chars = {chr(c) for c in range(0x20, 0x7F)} | set("・ー「」、。")
chars |= {ch for ch in src.read_text(encoding="utf-8") if ord(ch) > 0x7F}
out = REPO / "runtime" / "fonts" / "menu_font.ttf"
opts = subset.Options()
opts.name_IDs = ["*"]
opts.name_languages = ["*"]
opts.layout_features = ["*"]
font = subset.load_font(sys.argv[1], opts)
s = subset.Subsetter(opts)
s.populate(text="".join(sorted(chars)))
s.subset(font)
subset.save_font(font, str(out), opts)
print(f"{len(chars)} characters -> {out} ({out.stat().st_size} bytes)")
