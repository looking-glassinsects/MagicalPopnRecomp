"""ROM loading and identification. Works with any dump of Magical Pop'n:
.sfc/.smc/.swc/.fig, with or without a 512-byte copier header, optionally inside a .zip,
original Japanese release or a patched (translated / expanded / FastROM) build.
Nothing here is specific to one dump: no hashes or addresses are hard-coded."""
import hashlib
import io
import pathlib
import zipfile

ROM_EXTS = (".sfc", ".smc", ".swc", ".fig", ".bin")
# Internal header name of Magical Pop'n, half-width katakana in JIS X 0201: "ﾏｼﾞｶﾙﾎﾟｯﾌﾟﾝ"
TITLE_BYTES = bytes.fromhex("cfbcdeb6d9cedfafccdfdd")


class RomError(Exception):
    pass


def read_rom_file(path):
    """Return raw bytes of the ROM, unpacking a .zip if needed."""
    path = pathlib.Path(path)
    data = path.read_bytes()
    if data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.lower().endswith(ROM_EXTS)]
            if not names:
                raise RomError("The .zip file doesn't contain a SNES ROM (.sfc/.smc).")
            names.sort(key=lambda n: -z.getinfo(n).file_size)
            data = z.read(names[0])
    return data


def lorom_to_file(addr):
    """24-bit SNES address -> file offset (LoROM), or None if it isn't ROM."""
    bank, off = (addr >> 16) & 0xFF, addr & 0xFFFF
    if bank in (0x7E, 0x7F) or off < 0x8000:
        return None
    return (bank & 0x7F) * 0x8000 + (off - 0x8000)


def file_to_lorom(fo):
    return (((fo // 0x8000) | 0x80) << 16) | (0x8000 + fo % 0x8000)


def _header_score(rom, base):
    if base + 0x40 > len(rom):
        return -100
    h = rom[base:base + 0x40]
    score = 0
    cc = int.from_bytes(h[0x1C:0x1E], "little")
    cs = int.from_bytes(h[0x1E:0x20], "little")
    if cc ^ cs == 0xFFFF:
        score += 8
    if h[0x15] & 0xEF in (0x20, 0x21, 0x23, 0x25, 0x30, 0x31, 0x35):
        score += 4
    reset = int.from_bytes(h[0x3C:0x3E], "little")
    if reset >= 0x8000:
        score += 4
    if all(0x20 <= b < 0x7F or b >= 0xA1 for b in h[:21]):
        score += 2
    return score


def normalize(data):
    """Strip copier header, check mapping. Returns plain LoROM image bytes."""
    if len(data) < 0x8000:
        raise RomError("File is too small to be a SNES ROM.")
    if len(data) % 0x400 == 0x200:
        data = data[0x200:]
    lo, hi = _header_score(data, 0x7FC0), _header_score(data, 0xFFC0)
    if hi > lo + 4:
        raise RomError("This ROM uses HiROM mapping; Magical Pop'n is LoROM, so this is a different game "
                       "or an interleaved dump.")
    return data


def identify(data):
    """Describe a normalized ROM image."""
    h = data[0x7FC0:0x8000]
    cc = int.from_bytes(h[0x1C:0x1E], "little")
    cs = int.from_bytes(h[0x1E:0x20], "little")
    computed = sum(data) & 0xFFFF
    is_popn = h[:len(TITLE_BYTES)] == TITLE_BYTES
    # mirror-fill like a real cart before checksumming a non-power-of-two image
    size = len(data)
    original = is_popn and size == 0x200000 and computed == cs and (cc ^ cs) == 0xFFFF
    if not is_popn:
        variant = "unknown"
    elif original:
        variant = "original"
    else:
        variant = "modified"
    return {
        "is_popn": is_popn,
        "variant": variant,                 # original | modified (translation/hack) | unknown
        "title": h[:21].decode("shift_jis", errors="replace").strip(),
        "size": size,
        "sha1": hashlib.sha1(data).hexdigest(),
        "header_checksum": f"{cs:04X}",
        "computed_checksum": f"{computed:04X}",
        "region": "Japan" if h[0x19] == 0 else f"region {h[0x19]:02X}",
        "fastrom": bool(h[0x15] & 0x10),
        "vectors": {
            "reset": int.from_bytes(data[0x7FFC:0x7FFE], "little"),
            "nmi": int.from_bytes(data[0x7FEA:0x7FEC], "little"),
            "irq": int.from_bytes(data[0x7FEE:0x7FF0], "little"),
        },
    }


def load(path):
    data = normalize(read_rom_file(path))
    return data, identify(data)
