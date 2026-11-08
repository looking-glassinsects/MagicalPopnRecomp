"""Recursive-descent 65816 tracer for LoROM.

Starts at the reset/NMI/IRQ vectors plus entry points discovered by the trace profiler
(runtime/trace_main.c) and by pointer harvesting, follows code while tracking the M/X
register-width flags (needed to decode immediate operand sizes and to emit fixed-width C).
"""
import json
from collections import defaultdict, deque
from snes import lorom_to_file, file_to_lorom

# fmt: off
OPS = """
BRK imm8|ORA dpix|COP imm8|ORA sr|TSB dp|ORA dp|ASL dp|ORA dpil|PHP imp|ORA immM|ASL acc|PHD imp|TSB abs|ORA abs|ASL abs|ORA absl
BPL rel|ORA dpiy|ORA dpi|ORA sriy|TRB dp|ORA dpx|ASL dpx|ORA dpily|CLC imp|ORA absy|INC acc|TCS imp|TRB abs|ORA absx|ASL absx|ORA abslx
JSR abs|AND dpix|JSL absl|AND sr|BIT dp|AND dp|ROL dp|AND dpil|PLP imp|AND immM|ROL acc|PLD imp|BIT abs|AND abs|ROL abs|AND absl
BMI rel|AND dpiy|AND dpi|AND sriy|BIT dpx|AND dpx|ROL dpx|AND dpily|SEC imp|AND absy|DEC acc|TSC imp|BIT absx|AND absx|ROL absx|AND abslx
RTI imp|EOR dpix|WDM imm8|EOR sr|MVP bm|EOR dp|LSR dp|EOR dpil|PHA imp|EOR immM|LSR acc|PHK imp|JMP abs|EOR abs|LSR abs|EOR absl
BVC rel|EOR dpiy|EOR dpi|EOR sriy|MVN bm|EOR dpx|LSR dpx|EOR dpily|CLI imp|EOR absy|PHY imp|TCD imp|JML absl|EOR absx|LSR absx|EOR abslx
RTS imp|ADC dpix|PER rell|ADC sr|STZ dp|ADC dp|ROR dp|ADC dpil|PLA imp|ADC immM|ROR acc|RTL imp|JMP absi|ADC abs|ROR abs|ADC absl
BVS rel|ADC dpiy|ADC dpi|ADC sriy|STZ dpx|ADC dpx|ROR dpx|ADC dpily|SEI imp|ADC absy|PLY imp|TDC imp|JMP absix|ADC absx|ROR absx|ADC abslx
BRA rel|STA dpix|BRL rell|STA sr|STY dp|STA dp|STX dp|STA dpil|DEY imp|BIT immM|TXA imp|PHB imp|STY abs|STA abs|STX abs|STA absl
BCC rel|STA dpiy|STA dpi|STA sriy|STY dpx|STA dpx|STX dpy|STA dpily|TYA imp|STA absy|TXS imp|TXY imp|STZ abs|STA absx|STZ absx|STA abslx
LDY immX|LDA dpix|LDX immX|LDA sr|LDY dp|LDA dp|LDX dp|LDA dpil|TAY imp|LDA immM|TAX imp|PLB imp|LDY abs|LDA abs|LDX abs|LDA absl
BCS rel|LDA dpiy|LDA dpi|LDA sriy|LDY dpx|LDA dpx|LDX dpy|LDA dpily|CLV imp|LDA absy|TSX imp|TYX imp|LDY absx|LDA absx|LDX absy|LDA abslx
CPY immX|CMP dpix|REP imm8|CMP sr|CPY dp|CMP dp|DEC dp|CMP dpil|INY imp|CMP immM|DEX imp|WAI imp|CPY abs|CMP abs|DEC abs|CMP absl
BNE rel|CMP dpiy|CMP dpi|CMP sriy|PEI dp|CMP dpx|DEC dpx|CMP dpily|CLD imp|CMP absy|PHX imp|STP imp|JML absil|CMP absx|DEC absx|CMP abslx
CPX immX|SBC dpix|SEP imm8|SBC sr|CPX dp|SBC dp|INC dp|SBC dpil|INX imp|SBC immM|NOP imp|XBA imp|CPX abs|SBC abs|INC abs|SBC absl
BEQ rel|SBC dpiy|SBC dpi|SBC sriy|PEA abs|SBC dpx|INC dpx|SBC dpily|SED imp|SBC absy|PLX imp|XCE imp|JSR absix|SBC absx|INC absx|SBC abslx
"""
# fmt: on
TABLE = [tuple(e.split()) for row in OPS.strip().splitlines() for e in row.split("|")]
assert len(TABLE) == 256

SIZE = {"imp": 1, "acc": 1, "imm8": 2, "dp": 2, "dpx": 2, "dpy": 2, "dpi": 2, "dpix": 2,
        "dpiy": 2, "dpil": 2, "dpily": 2, "sr": 2, "sriy": 2, "rel": 2, "abs": 3, "absx": 3,
        "absy": 3, "absi": 3, "absix": 3, "absil": 3, "rell": 3, "bm": 3, "absl": 4, "abslx": 4}
FMT = {"imp": "", "acc": "A", "imm8": "#${:02X}", "immM": "#${v}", "immX": "#${v}",
       "dp": "${:02X}", "dpx": "${:02X},X", "dpy": "${:02X},Y", "dpi": "(${:02X})",
       "dpix": "(${:02X},X)", "dpiy": "(${:02X}),Y", "dpil": "[${:02X}]", "dpily": "[${:02X}],Y",
       "sr": "${:02X},S", "sriy": "(${:02X},S),Y", "abs": "${:04X}", "absx": "${:04X},X",
       "absy": "${:04X},Y", "absi": "(${:04X})", "absix": "(${:04X},X)", "absil": "[${:04X}]",
       "absl": "${:06X}", "abslx": "${:06X},X"}
BRANCHES = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ"}
STOPS = {"RTS", "RTL", "RTI", "STP", "BRA", "BRL", "JMP", "JML"}


class Tracer:
    def __init__(self, rom, hints):
        self.rom = rom
        self.hints = hints
        self.insns = {}          # addr -> (mn, mode, size, operand_text, m, x)
        self.owner = {}          # file offset -> addr of instruction covering it
        self.labels = {}         # addr -> name
        self.funcs = {}          # addr -> {"m","x","callers"}
        self.unresolved = []
        self.conflicts = []
        self.work = deque()
        self.seen = set()        # (addr, m, x, php)
        self.tables = {}         # indirect-jump insn addr -> list of canonical targets
        self.func_states = {}    # func addr -> set of (m, x) it is entered with

    def byte(self, addr):
        fo = lorom_to_file(addr)
        if fo is None or fo >= len(self.rom):
            return None
        return self.rom[fo]

    @staticmethod
    def canon(addr):
        fo = lorom_to_file(addr)
        return addr if fo is None else file_to_lorom(fo)

    def add_func(self, addr, m, x, caller=None, name=None, php=()):
        addr = self.canon(addr)
        f = self.funcs.setdefault(addr, {"m": m, "x": x, "callers": []})
        if (f["m"], f["x"]) != (m, x):
            f.setdefault("alt_states", []).append([m, x])
        if caller is not None:
            f["callers"].append(f"${caller:06X}")
        self.labels.setdefault(addr, name or f"func_{addr:06X}")
        self.func_states.setdefault(addr, set()).add((m, x))
        self.work.append((addr, m, x, php))

    def run(self):
        while self.work:
            self.trace_block(*self.work.popleft())

    def trace_block(self, pc, m, x, php=()):
        pc = self.canon(pc)
        while True:
            key = (pc, m, x, php[-4:])
            if key in self.seen:
                return
            self.seen.add(key)
            fo = lorom_to_file(pc)
            if fo is None or fo >= len(self.rom):
                self.unresolved.append(f"${pc:06X}  flow leaves ROM")
                return
            op = self.rom[fo]
            mn, mode = TABLE[op]
            if mode == "immM":
                size = 2 if m else 3
            elif mode == "immX":
                size = 2 if x else 3
            else:
                size = SIZE[mode]
            raw = self.rom[fo + 1:fo + size]
            val = int.from_bytes(raw, "little")
            bank = pc & 0xFF0000
            nxt = bank | ((pc + size) & 0xFFFF)

            # operand text
            if mode in ("rel", "rell"):
                disp = val - (0x100 if mode == "rel" and val & 0x80 else 0) \
                    - (0x10000 if mode == "rell" and val & 0x8000 else 0)
                tgt = bank | ((pc + size + disp) & 0xFFFF)
                txt = None
            elif mode == "bm":
                txt = f"${raw[1]:02X},${raw[0]:02X}"
            elif mode in ("immM", "immX"):
                txt = f"#${val:0{(size-1)*2}X}"
            else:
                txt = FMT[mode].format(val) if FMT[mode] else ""

            for i in range(size):
                prev = self.owner.get(fo + i)
                if prev is not None and prev != pc and prev != -1:
                    self.conflicts.append(f"${pc:06X} (m{m}x{x}) overlaps insn at ${prev:06X}")
                    break
                self.owner[fo + i] = pc
            self.insns[pc] = [mn, mode, size, txt, m, x, None]

            # flag tracking
            hint = self.hints.get("flags_after", {}).get(f"{pc:06X}")
            if mn == "REP":
                m = 0 if val & 0x20 else m
                x = 0 if val & 0x10 else x
            elif mn == "SEP":
                m = 1 if val & 0x20 else m
                x = 1 if val & 0x10 else x
            elif mn == "XCE":
                prev_op = self.byte(bank | ((pc - 1) & 0xFFFF))
                if prev_op == 0x18:      # CLC; XCE -> native, regs come up 8-bit
                    m, x = 1, 1
            elif mn == "PHP":
                php = php + ((m, x),)
            elif mn == "PLP" and not hint:
                if php:
                    (m, x), php = php[-1], php[:-1]
                else:
                    self.unresolved.append(f"${pc:06X}  PLP with no matching PHP in this function: flags assumed unchanged (m={m} x={x})")
            if hint:
                m, x = hint

            # control flow
            if mn in BRANCHES:
                self.insns[pc][3] = self.label_for(tgt)
                self.work.append((tgt, m, x, php))
            elif mn in ("BRA", "BRL"):
                self.insns[pc][3] = self.label_for(tgt)
                self.work.append((tgt, m, x, php))
                return
            elif mn == "JSR" and mode == "abs":
                t = bank | val
                self.add_func(t, m, x, pc)
                self.insns[pc][3] = self.labels[self.canon(t)]
            elif mn == "JSL":
                self.add_func(val, m, x, pc)
                self.insns[pc][3] = self.labels[self.canon(val)]
            elif mn == "JMP" and mode == "abs":
                t = bank | val
                self.insns[pc][3] = self.label_for(t)
                self.work.append((t, m, x, php))
                return
            elif mn == "JML" and mode == "absl":
                self.insns[pc][3] = self.label_for(val)
                self.work.append((val, m, x, php))
                return
            elif mode in ("absi", "absix", "absil"):
                self.unresolved.append(f"${pc:06X}  indirect {mn} {txt}  (jump table? add targets to hints.json)")
                targets = list(self.hints.get("jump_tables", {}).get(f"{pc:06X}", []))
                if mode == "absix" and not targets and val == (nxt & 0xFFFF):
                    targets = self.inline_table(bank, val)
                    self.unresolved[-1] += f"  -> auto-resolved {len(targets)} inline entries"
                targets = [self.canon(int(t, 16) if isinstance(t, str) else t) for t in targets]
                if targets:
                    self.tables[pc] = targets
                for t in targets:
                    if mn == "JSR":
                        self.add_func(t, m, x, pc)
                    else:
                        self.label_for(t)
                        self.work.append((t, m, x, php))
                if mn != "JSR":
                    return
            elif mn in ("RTS", "RTL", "RTI", "STP"):
                return
            elif mn in ("BRK", "COP"):
                self.unresolved.append(f"${pc:06X}  {mn} executed (data or software interrupt?)")
            pc = nxt

    def inline_table(self, bank, start):
        """Read 16-bit pointers from start until one points back into the table or below $8000."""
        out, a, lowest = [], start, 0x10000
        while a < lowest and a < 0xFFFE:
            fo = lorom_to_file(bank | a)
            w = int.from_bytes(self.rom[fo:fo + 2], "little")
            if w < 0x8000 or (start <= w < a + 2):
                break
            out.append(bank | w)
            lowest = min(lowest, w)
            for i in range(2):
                self.owner.setdefault(fo + i, -1)  # mark as data
            a += 2
        return out

    def label_for(self, addr):
        addr = self.canon(addr)
        return self.labels.setdefault(addr, f"loc_{addr:06X}")


BAD_OPS = {0x00, 0x02, 0x42, 0xDB}          # BRK COP WDM STP never start real routines here


def _trial(t, rom, entry, m, x, limit=3000):
    """Decode a candidate routine on its own (branches + fall-through, not calls).
    Rejects it if it executes BRK/COP/WDM/STP, leaves ROM, or any instruction collides
    with already-confirmed code at a different alignment."""
    seen, work, n = set(), [(entry, m, x)], 0
    while work:
        pc, m, x = work.pop()
        while (pc, m, x) not in seen:
            seen.add((pc, m, x))
            n += 1
            if n > limit:
                return False
            fo = lorom_to_file(pc)
            if fo is None or fo + 4 > len(rom):
                return False
            op = rom[fo]
            if op in BAD_OPS:
                return False
            mn, mode = TABLE[op]
            size = (2 if m else 3) if mode == "immM" else (2 if x else 3) if mode == "immX" else SIZE[mode]
            for i in range(size):
                o = t.owner.get(fo + i)
                if o is not None and (o == -1 or (o != pc and i == 0) or (i > 0 and o != pc)):
                    if not (i == 0 and o == pc):
                        return False
            val = int.from_bytes(rom[fo + 1:fo + size], "little")
            if mn == "REP":
                m, x = (0 if val & 0x20 else m), (0 if val & 0x10 else x)
            elif mn == "SEP":
                m, x = (1 if val & 0x20 else m), (1 if val & 0x10 else x)
            bank = pc & 0xFF0000
            nxt = bank | ((pc + size) & 0xFFFF)
            if mode in ("rel", "rell"):
                disp = (val - 0x100 if val & 0x80 else val) if mode == "rel" else (val - 0x10000 if val & 0x8000 else val)
                tgt = bank | ((nxt + disp) & 0xFFFF)
                if mn in ("BRA", "BRL"):
                    pc = tgt
                    continue
                work.append((tgt, m, x))
            elif mn in ("RTS", "RTL", "RTI") or mode in ("absi", "absix", "absil"):
                break
            if mn in ("JSR", "JMP") and mode == "abs" and val < 0x8000:
                return False                      # real code doesn't call I/O space
            if mn in ("JSL", "JML") and mode == "absl":
                tfo = lorom_to_file(val)
                if tfo is None or tfo >= len(rom):
                    return False
            elif mn == "JMP" and mode == "abs":
                pc = bank | val
                continue
            elif mn == "JML":
                pc = t.canon(val)
                continue
            pc = nxt
    return True


def harvest(t, rom):
    """Pointer harvesting: 16-bit immediates loaded in traced code that point at plausible code
    in the same bank are likely routine addresses (object/task handlers stored to RAM and
    later dispatched through JMP (abs,X)). Each candidate is trial-decoded and only kept if it
    decodes cleanly. Misses are still safe: the runtime interpreter covers them."""
    found = {}
    for pc, ins in list(t.insns.items()):
        mn, mode, size, txt, m, x, _ = ins
        if size != 3 or not (mode in ("immM", "immX") or mn == "PEA"):
            continue
        fo = lorom_to_file(pc)
        val = int.from_bytes(rom[fo + 1:fo + 3], "little")
        if val < 0x8000:
            continue
        cands = [t.canon((pc & 0xFF0000) | val)]
        if mn == "PEA":
            cands.append(t.canon((pc & 0xFF0000) | ((val + 1) & 0xFFFF)))  # PEA target-1 / RTS idiom
        for cand in cands:
            cfo = lorom_to_file(cand)
            if cand in t.funcs or cand in found or cfo is None or cfo >= len(rom):
                continue
            if cfo in t.owner and t.owner[cfo] != cand:
                continue
            for state in ((m, x), (0, 0), (1, 1), (0, 1), (1, 0)):
                if _trial(t, rom, cand, *state):
                    found[cand] = state
                    break
    for cand, (m, x) in found.items():
        t.add_func(cand, m, x, name=f"hv_{cand:06X}")
    return len(found)


def harvest_tables(t, rom, max_entries=128):
    """Pointer-table harvesting, restricted to tables that traced code references: an absolute
    (indexed) operand or 16-bit immediate pointing at non-code bytes in ROM is taken as a
    table base, and consecutive 16-bit pointers into the same bank are read from it until an
    entry stops looking like a pointer to clean code."""
    bases = set()
    for pc, ins in list(t.insns.items()):
        mn, mode, size, txt, m, x, _ = ins
        fo = lorom_to_file(pc)
        if mode in ("abs", "absx", "absy", "absix", "absi") and size == 3:
            val = rom[fo + 1] | (rom[fo + 2] << 8)
            if val >= 0x8000:
                bases.add(t.canon((pc & 0xFF0000) | val))
        elif mode in ("absl", "abslx") and size == 4 and mn not in ("JSL", "JML"):
            val = rom[fo + 1] | (rom[fo + 2] << 8) | (rom[fo + 3] << 16)
            if (val & 0xFFFF) >= 0x8000:
                bases.add(t.canon(val))
    found = {}
    code_banks = sorted({a & 0xFF0000 for a in t.insns})

    def clean(cand):
        cand = t.canon(cand)
        if cand in t.funcs or cand in found:
            return (cand, None)
        cfo = lorom_to_file(cand)
        if cfo is None or cfo >= len(rom) or (cfo in t.owner and t.owner[cfo] != cand):
            return None
        st = next((s_ for s_ in ((0, 0), (1, 1), (0, 1), (1, 0)) if _trial(t, rom, cand, *s_, limit=1500)), None)
        return (cand, st) if st else None

    for base in sorted(bases):
        bfo = lorom_to_file(base)
        if bfo is None or bfo + 4 > len(rom) or (bfo in t.owner and t.owner[bfo] != -1):
            continue
        best = []
        # entry size 2 (packed pointers) in the table's own bank, or entry size 2/4 (pointer +
        # parameter) pointing into any code bank: keep the reading with the longest clean run
        for stride in (2, 4):
            for bank24 in ([base & 0xFF0000] + [b_ for b_ in code_banks if b_ != base & 0xFF0000]):
                run = []
                for i in range(max_entries if stride == 2 else 512):
                    k = bfo + i * stride
                    if k + 2 > len(rom) or (k in t.owner and t.owner[k] != -1) or (k // 0x8000) != (bfo // 0x8000):
                        break
                    w = rom[k] | (rom[k + 1] << 8)
                    if w < 0x8000:
                        break
                    r = clean(bank24 | w)
                    if r is None:
                        break
                    run.append(r)
                other = stride != 2 or bank24 != base & 0xFF0000
                if len(run) > len(best) and (not other or len(run) >= 8):
                    best = run
        for cand, st in best:
            if st:
                found[cand] = st
    for cand, (m, x) in found.items():
        if cand not in t.funcs:
            t.add_func(cand, m, x, name=f"tb_{cand:06X}")
    return len(found)


def read_entries(paths):
    """Entry lists: lines of 'ADDR M X [kind]' (trace profiler, runtime discoveries)."""
    out = []
    for p in paths:
        p = __import__("pathlib").Path(p)
        if not p.exists():
            continue
        for line in p.read_text().split("\n"):
            parts = line.split()
            if len(parts) >= 3:
                try:
                    out.append((int(parts[0], 16), int(parts[1]), int(parts[2])))
                except ValueError:
                    pass
    return out


def trace(rom, entries=(), hints=None, harvest_passes=2, tables=True, log=print):
    hints = hints or {}
    t = Tracer(rom, hints)
    vec = lambda o: int.from_bytes(rom[o:o + 2], "little")
    t.add_func(vec(0x7FFC), 1, 1, name="RESET")       # emulation mode at reset
    t.add_func(vec(0x7FEA), 1, 1, name="NMI")
    if vec(0x7FEE) != vec(0x7FEA):
        t.add_func(vec(0x7FEE), 1, 1, name="IRQ")
    for e in hints.get("entry_points", []):
        t.add_func(int(e["addr"], 16), e.get("m", 1), e.get("x", 1), name=e.get("name"))
    for a, m, x in entries:
        t.add_func(a, m, x)
    t.run()
    log(f"traced {len(t.insns)} instructions, {len(t.funcs)} functions from vectors + {len(entries)} profiled entries")
    for i in range(harvest_passes):
        n = harvest(t, rom)
        t.run()
        log(f"pointer harvest pass {i + 1}: +{n} candidate routines -> {len(t.funcs)} functions")
        if not n:
            break
    for i in range(3 if tables else 0):
        n = harvest_tables(t, rom) + harvest(t, rom)
        t.run()
        log(f"table + pointer harvest pass {i + 1}: +{n} candidate routines -> {len(t.funcs)} functions")
        if not n:
            break
    return t


def write_listing(t, rom, path):
    with open(path, "w", encoding="utf-8") as f:
        last = None
        for a in sorted(t.insns):
            mn, mode, size, txt, m, x, _ = t.insns[a]
            if last is not None and a != last:
                f.write("\n")
            if a in t.labels:
                f.write(f"{t.labels[a]}:\n")
            fo = lorom_to_file(a)
            hexb = " ".join(f"{b:02X}" for b in rom[fo:fo + size])
            f.write(f"  ${a:06X}  {hexb:<12} {mn} {txt or ''}".rstrip() + f"{'':4}; m{m}x{x}\n")
            last = (a & 0xFF0000) | ((a + size) & 0xFFFF)


def code_bytes(t):
    return sum(1 for o in t.owner.values() if o != -1)


if __name__ == "__main__":
    import sys
    from snes import load
    rom, info = load(sys.argv[1])
    t = trace(rom, read_entries(sys.argv[2:]))
    write_listing(t, rom, "disasm.asm")
    print(f"{code_bytes(t)} code bytes; listing in disasm.asm")
