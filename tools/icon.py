"""The app icon: assets/icon.ico (made from the heroine sprite by tools/make_icon.py)."""
import pathlib
import sys


def _repo():
    if getattr(sys, "frozen", False):
        return pathlib.Path(sys._MEIPASS)
    return pathlib.Path(__file__).resolve().parent.parent


def make_ico(sizes=None):
    """Bytes of the multi-size .ico (all sizes; `sizes` is accepted for compatibility)."""
    return (_repo() / "assets" / "icon.ico").read_bytes()
