"""65816 -> C static recompiler.

Each (function entry, M, X) state becomes one C function `f_<addr>_<m><x>(Cpu *c)`.
Inside a function every reachable (pc, M, X, PHP-stack) node becomes a label; branches
are gotos, so register widths are fixed at compile time and each instruction turns
into a few lines of plain C. Code shared between functions is duplicated.

Calls keep the real 65816 stack in emulated WRAM (JSR/JSL push the return address,
RTS/RTL pull it) so stack-inspecting game code still works. The caller then checks
that the callee returned to the expected address.

Output: <gen>/code_NNN.c, <gen>/gen.h, <gen>/dispatch.c   (call emit())
"""
import pathlib
from collections import deque
from snes import lorom_to_file, file_to_lorom
from disasm65816 import TABLE, SIZE, BRANCHES

PHP_MAX = 8
FUNCS_PER_FILE = 24          # many smaller files compile in parallel much faster
rom = b""
tracer = None
FLAG_HINTS = {}
OVERRIDES = {}               # pc -> C statement replacing that instruction (16-bit mode only)


def find_overrides(rom_bytes):
    """Game patches located by byte signature (so they apply to any version of the game).

    Sprite-piece draw helpers: the game culls each sprite piece to the 4:3 screen
    (objX + O - camera in [0, 0x100 + O)) and sets OAM X bit 9 only for pieces off the
    left edge. With widescreen, rt_ws_extra (E) widens the window by E on both sides and
    bit 9 is taken from bit 8 of the true screen X, so pieces in the extra columns are drawn
    on the correct side. With E == 0 every patched instruction behaves exactly like the
    original one."""
    import re
    ov = {}
    pat = re.compile(bytes.fromhex('69') + b'(..)' + bytes.fromhex('38E562B0034C') + b'..' + bytes.fromhex('C9') + b'(..)' + bytes.fromhex('90034C') + b'..' + bytes.fromhex('48C9') + b'(..)' + bytes.fromhex('B0'), re.S)
    for mt in pat.finditer(rom_bytes):
        o = int.from_bytes(mt.group(1), "little"); lim = int.from_bytes(mt.group(2), "little")
        if lim != 0x100 + o or int.from_bytes(mt.group(3), "little") != o:
            continue
        tail = rom_bytes.find(bytes.fromhex('6838E9') + mt.group(1), mt.end(), mt.end() + 160)
        if tail < 0:
            continue
        base = file_to_lorom(mt.start())
        at = lambda off: (base & 0xFF0000) | ((base + off) & 0xFFFF)
        ov[at(0)] = f"adc16(c, (uint16_t)(0x{o:04X} + rt_ws_extra));"
        ov[at(11)] = f"cmp16(c, c->a, (uint16_t)(0x{lim:04X} + 2 * rt_ws_extra));"
        ov[at(20)] = f"setc(c, !(((uint16_t)(c->a - 0x{o:04X} - rt_ws_extra)) & 0x100));"
        ov[at(tail - mt.start() + 2)] = f"sbc16(c, (uint16_t)(0x{o:04X} + rt_ws_extra));"
    # Object visibility checks with a narrow window (objX + O - camera in [0, L), e.g. doors,
    # which are only shown from 32 px left of the 4:3 screen to its right edge). With
    # widescreen each margin is raised to at least rt_ws_extra so the object shows in the extra
    # columns; wider windows (the usual enemy range) and E == 0 are unchanged.
    pat = re.compile(bytes.fromhex('A5001869') + b'(..)' + bytes.fromhex('38E562B0034C') + b'(..)' + bytes.fromhex('C9') + b'(..)' + bytes.fromhex('90034C') + b'(..)', re.S)
    for mt in pat.finditer(rom_bytes):
        o = int.from_bytes(mt.group(1), "little"); lim = int.from_bytes(mt.group(3), "little")
        r = lim - o - 0x100
        if mt.group(2) != mt.group(4) or not (0 <= o < 0x80 and 0 <= r < 0x80):
            continue
        base = file_to_lorom(mt.start())
        at = lambda off: (base & 0xFF0000) | ((base + off) & 0xFFFF)
        lo = f"(rt_ws_extra > 0x{o:X} ? rt_ws_extra : 0x{o:X})"
        hi = f"(rt_ws_extra > 0x{r:X} ? rt_ws_extra : 0x{r:X})"
        ov[at(3)] = f"adc16(c, (uint16_t){lo});"
        ov[at(14)] = f"cmp16(c, c->a, (uint16_t)({lo} + 0x100 + {hi}));"
    # The stone giant's flying head turns back at the 4:3 screen edges (X - 24 < camera, and
    # X - 216 >= camera). With widescreen it turns at the wide picture's edges instead, but
    # never beyond the arena's own ends.
    for m in re.finditer(re.escape(bytes.fromhex('A50038E91800C562B0')), rom_bytes):
        base = file_to_lorom(m.start())
        ov[(base & 0xFF0000) | ((base + 3) & 0xFFFF)] = "sbc16(c, (uint16_t)(0x0018 - rt_ws_reach_left()));"
    for m in re.finditer(re.escape(bytes.fromhex('A500186928FFC56290')), rom_bytes):
        base = file_to_lorom(m.start())
        ov[(base & 0xFF0000) | ((base + 3) & 0xFFFF)] = "adc16(c, (uint16_t)(0xFF28 - rt_ws_reach_right()));"
    return ov


def canon(a):
    fo = lorom_to_file(a)
    return a if fo is None else file_to_lorom(fo)


def fname(addr, m, x):
    return f"f_{addr:06X}_{m}{x}"


def decode(pc, m, x):
    fo = lorom_to_file(pc)
    if fo is None or fo >= len(rom):
        return None
    op = rom[fo]
    mn, mode = TABLE[op]
    size = (2 if m else 3) if mode == "immM" else (2 if x else 3) if mode == "immX" else SIZE[mode]
    raw = rom[fo + 1:fo + size]
    val = int.from_bytes(raw, "little")
    bank = pc & 0xFF0000
    nxt = bank | ((pc + size) & 0xFFFF)
    tgt = None
    if mode == "rel":
        tgt = bank | ((pc + size + (val - 0x100 if val & 0x80 else val)) & 0xFFFF)
    elif mode == "rell":
        tgt = bank | ((pc + size + (val - 0x10000 if val & 0x8000 else val)) & 0xFFFF)
    return dict(op=op, mn=mn, mode=mode, size=size, raw=raw, val=val, bank=bank, nxt=nxt, tgt=tgt)


def flags_after(pc, d, m, x, php):
    mn, val = d["mn"], d["val"]
    if mn == "REP":
        m, x = (0 if val & 0x20 else m), (0 if val & 0x10 else x)
    elif mn == "SEP":
        m, x = (1 if val & 0x20 else m), (1 if val & 0x10 else x)
    elif mn == "XCE":
        fo = lorom_to_file(pc)
        if fo and rom[fo - 1] == 0x18:
            m, x = 1, 1
    elif mn == "PHP":
        php = (php + ((m, x),))[-PHP_MAX:]
    elif mn == "PLP" and php:
        (m, x), php = php[-1], php[:-1]
    h = FLAG_HINTS.get(f"{pc:06X}")
    if h:
        m, x = h
    return m, x, php


# ---------------------------------------------------------------- operand helpers
def ea(mode, v):
    """Return (kind, C expr). kind 'z' = bank-0 16-bit wrapping address, 'l' = 24-bit."""
    db = "((uint32_t)c->db << 16)"
    return {
        "dp": ("z", f"(uint16_t)(c->d + 0x{v:02X})"),
        "dpx": ("z", f"(uint16_t)(c->d + 0x{v:02X} + c->x)"),
        "dpy": ("z", f"(uint16_t)(c->d + 0x{v:02X} + c->y)"),
        "sr": ("z", f"(uint16_t)(c->s + 0x{v:02X})"),
        "dpi": ("l", f"({db} | rd16z((uint16_t)(c->d + 0x{v:02X})))"),
        "dpix": ("l", f"({db} | rd16z((uint16_t)(c->d + 0x{v:02X} + c->x)))"),
        "dpiy": ("l", f"((({db} | rd16z((uint16_t)(c->d + 0x{v:02X}))) + c->y) & 0xFFFFFF)"),
        "dpil": ("l", f"rd24z((uint16_t)(c->d + 0x{v:02X}))"),
        "dpily": ("l", f"((rd24z((uint16_t)(c->d + 0x{v:02X})) + c->y) & 0xFFFFFF)"),
        "sriy": ("l", f"((({db} | rd16z((uint16_t)(c->s + 0x{v:02X}))) + c->y) & 0xFFFFFF)"),
        "abs": ("l", f"({db} | 0x{v:04X})"),
        "absx": ("l", f"(({db} + 0x{v:04X} + c->x) & 0xFFFFFF)"),
        "absy": ("l", f"(({db} + 0x{v:04X} + c->y) & 0xFFFFFF)"),
        "absl": ("l", f"0x{v:06X}u"),
        "abslx": ("l", f"((0x{v:06X}u + c->x) & 0xFFFFFF)"),
    }[mode]


def rd(w, kind, e):
    return f"rd8({e})" if w == 8 else (f"rd16z({e})" if kind == "z" else f"rd16l({e})")


def wr(w, kind, e, v):
    return f"wr8({e}, {v})" if w == 8 else (f"wr16z({e}, {v})" if kind == "z" else f"wr16l({e}, {v})")


def operand(d, w):
    if d["mode"] in ("immM", "immX", "imm8"):
        return f"0x{d['val']:0{w // 4}X}"
    kind, e = ea(d["mode"], d["val"])
    return rd(w, kind, e)


REGS = {"A": "a", "X": "x", "Y": "y"}


def get_reg(r, w):
    return f"(uint8_t)c->{r}" if w == 8 else f"c->{r}"


def set_a(w, v):
    return f"c->a = (c->a & 0xFF00) | (uint8_t)({v});" if w == 8 else f"c->a = (uint16_t)({v});"


def set_xy(r, w, v):
    return f"c->{r} = (uint8_t)({v});" if w == 8 else f"c->{r} = (uint16_t)({v});"


# ---------------------------------------------------------------- per-instruction codegen
def gen_insn(pc, d, m, x, label_of, out, calls):
    mn, mode, val = d["mn"], d["mode"], d["val"]
    wm, wx = (8 if m else 16), (8 if x else 16)
    T = "uint8_t" if wm == 8 else "uint16_t"
    TX = "uint8_t" if wx == 8 else "uint16_t"
    bank = d["bank"]
    pb = bank >> 16
    e = lambda s: out.append("    " + s)
    ret_addr = (pc + d["size"] - 1) & 0xFFFF   # what JSR/JSL push

    if mn in ("LDA", "LDX", "LDY"):
        r, w = ("a", wm) if mn == "LDA" else (REGS[mn[2]], wx)
        t = "uint8_t" if w == 8 else "uint16_t"
        e(f"{{ {t} v = {operand(d, w)}; {set_a(w, 'v') if r == 'a' else set_xy(r, w, 'v')} nz{w}(c, v); }}")
    elif mn in ("STA", "STX", "STY", "STZ"):
        r, w = {"STA": ("a", wm), "STX": ("x", wx), "STY": ("y", wx), "STZ": (None, wm)}[mn]
        kind, a = ea(mode, val)
        v = "0" if r is None else (f"(uint8_t)c->{r}" if w == 8 else f"c->{r}")
        e(wr(w, kind, a, v) + ";")
    elif mn in ("ADC", "SBC"):
        e(f"{mn.lower()}{wm}(c, {operand(d, wm)});")
    elif mn in ("CMP", "CPX", "CPY"):
        r, w = ("a", wm) if mn == "CMP" else (REGS[mn[2]], wx)
        e(f"cmp{w}(c, {get_reg(r, w)}, {operand(d, w)});")
    elif mn in ("AND", "ORA", "EOR"):
        opc = {"AND": "&", "ORA": "|", "EOR": "^"}[mn]
        e(f"{{ {T} r = ({T})({get_reg('a', wm)} {opc} {operand(d, wm)}); {set_a(wm, 'r')} nz{wm}(c, r); }}")
    elif mn == "BIT":
        if mode == "immM":
            e(f"setz(c, ({get_reg('a', wm)} & {operand(d, wm)}) == 0);")
        else:
            e(f"bit{wm}(c, {operand(d, wm)});")
    elif mn in ("INC", "DEC", "ASL", "LSR", "ROL", "ROR"):
        fn = {"INC": "inc", "DEC": "dec"}.get(mn, mn.lower())
        if mode == "acc":
            e(f"{set_a(wm, f'{fn}{wm}(c, {get_reg(chr(97), wm)})')}")
        else:
            kind, a = ea(mode, val)
            e(f"{{ uint32_t ea = {a}; {T} v = {rd(wm, kind, 'ea')}; v = {fn}{wm}(c, v); {wr(wm, kind, 'ea', 'v')}; }}")
    elif mn in ("TSB", "TRB"):
        kind, a = ea(mode, val)
        op = "v | A" if mn == "TSB" else "v & ~A"
        e(f"{{ uint32_t ea = {a}; {T} A = {get_reg('a', wm)}; {T} v = {rd(wm, kind, 'ea')}; "
          f"setz(c, (v & A) == 0); v = ({T})({op}); {wr(wm, kind, 'ea', 'v')}; }}")
    elif mn in ("INX", "INY", "DEX", "DEY"):
        r = mn[2].lower()
        op = "+ 1" if mn[0] == "I" else "- 1"
        e(f"{{ {TX} v = ({TX})(c->{r} {op}); c->{r} = v; nz{wx}(c, v); }}")
    elif mn in ("TAX", "TAY"):
        r = mn[2].lower()
        e(f"{{ {TX} v = ({TX})c->a; c->{r} = v; nz{wx}(c, v); }}")
    elif mn in ("TXA", "TYA"):
        r = mn[1].lower()
        e(f"{{ {T} v = ({T})c->{r}; {set_a(wm, 'v')} nz{wm}(c, v); }}")
    elif mn in ("TXY", "TYX"):
        s, t = mn[1].lower(), mn[2].lower()
        e(f"{{ {TX} v = ({TX})c->{s}; c->{t} = v; nz{wx}(c, v); }}")
    elif mn == "TSX":
        e(f"{{ {TX} v = ({TX})c->s; c->x = v; nz{wx}(c, v); }}")
    elif mn == "TXS":
        e("c->s = c->e ? (uint16_t)(0x0100 | (uint8_t)c->x) : c->x;")
    elif mn == "TCS":
        e("c->s = c->e ? (uint16_t)(0x0100 | (uint8_t)c->a) : c->a;")
    elif mn == "TSC":
        e("c->a = c->s; nz16(c, c->a);")
    elif mn == "TCD":
        e("c->d = c->a; nz16(c, c->d);")
    elif mn == "TDC":
        e("c->a = c->d; nz16(c, c->a);")
    elif mn == "XBA":
        e("c->a = (uint16_t)((c->a >> 8) | (c->a << 8)); nz8(c, (uint8_t)c->a);")
    elif mn in ("PHA", "PHX", "PHY"):
        r = mn[2].lower()
        w = wm if r == "a" else wx
        e(f"push{w}(c, {get_reg(r, w)});")
    elif mn in ("PLA", "PLX", "PLY"):
        r = mn[2].lower()
        w = wm if r == "a" else wx
        t = "uint8_t" if w == 8 else "uint16_t"
        e(f"{{ {t} v = ({t})pull{w}(c); {set_a(w, 'v') if r == 'a' else set_xy(r, w, 'v')} nz{w}(c, v); }}")
    elif mn == "PHP":
        e("push8(c, c->p);")
    elif mn == "PLP":
        e("set_p(c, pull8(c));")
    elif mn == "PHB":
        e("push8(c, c->db);")
    elif mn == "PLB":
        e("c->db = pull8(c); nz8(c, c->db);")
    elif mn == "PHD":
        e("push16(c, c->d);")
    elif mn == "PLD":
        e("c->d = pull16(c); nz16(c, c->d);")
    elif mn == "PHK":
        e(f"push8(c, 0x{pb:02X});")
    elif mn == "PEA":
        e(f"push16(c, 0x{val:04X});")
    elif mn == "PEI":
        e(f"push16(c, rd16z((uint16_t)(c->d + 0x{val:02X})));")
    elif mn == "PER":
        e(f"push16(c, 0x{(d['nxt'] + (val - 0x10000 if val & 0x8000 else val)) & 0xFFFF:04X});")
    elif mn == "REP":
        e(f"c->p &= (uint8_t)~0x{val:02X}; if (c->e) c->p |= 0x30;")
    elif mn == "SEP":
        e(f"set_p(c, c->p | 0x{val:02X});")
    elif mn == "XCE":
        e("xce(c);")
    elif mn in ("CLC", "SEC", "CLI", "SEI", "CLD", "SED", "CLV"):
        bit = {"C": "P_C", "I": "P_I", "D": "P_D", "V": "P_V"}[mn[2]]
        e(f"c->p {'|=' if mn[0] == 'S' else '&= (uint8_t)~'}{bit};" if mn[0] == "S"
          else f"c->p &= (uint8_t)~{bit};")
    elif mn in ("NOP", "WDM"):
        e(";")
    elif mn == "WAI":
        e("rt_wai(c);")
    elif mn == "STP":
        e(f"rt_stop(c, 0x{pc:06X}); return;")
    elif mn in ("BRK", "COP"):
        e(f"rt_soft_irq(c, 0x{pc:06X}, {1 if mn == 'COP' else 0});")
    elif mn in ("MVN", "MVP"):
        e(f"{mn.lower()}(c, 0x{d['raw'][1]:02X}, 0x{d['raw'][0]:02X});")
    elif mn in BRANCHES or mn in ("BRA", "BRL"):
        pass  # handled by caller (needs labels)
    elif mn == "JSR" and mode == "abs":
        pass
    else:
        pass
    return


COND = {"BPL": "!(c->p & P_N)", "BMI": "(c->p & P_N)", "BVC": "!(c->p & P_V)", "BVS": "(c->p & P_V)",
        "BCC": "!(c->p & P_C)", "BCS": "(c->p & P_C)", "BNE": "!(c->p & P_Z)", "BEQ": "(c->p & P_Z)"}


class FuncGen:
    def __init__(self, addr, m, x, queue):
        self.addr, self.m, self.x = addr, m, x
        self.queue = queue      # function-state worklist (shared)
        self.nodes = {}         # (pc,m,x,php) -> label
        self.order = []

    def label(self, node):
        if node not in self.nodes:
            self.nodes[node] = f"L{node[0]:06X}_{node[1]}{node[2]}_{len(self.nodes)}"
            self.work.append(node)
        return self.nodes[node]

    def call_target(self, addr, m, x):
        """C expression that calls addr: a recompiled function, or the dispatcher (which falls
        back to the interpreter) for targets outside ROM such as code copied to RAM."""
        addr = canon(addr)
        fo = lorom_to_file(addr)
        if fo is None or fo >= len(rom):
            return f"rt_dispatch(c, 0x{addr:06X})"
        self.queue.append((addr, m, x))
        return f"{fname(addr, m, x)}(c)"

    def generate(self):
        self.work = deque()
        entry = (self.addr, self.m, self.x, ())
        self.label(entry)
        bodies = {}
        while self.work:
            node = self.work.popleft()
            bodies[node] = self.gen_node(node)
        lines = [f"void {fname(self.addr, self.m, self.x)}(Cpu *c) {{",
                 f"    goto {self.nodes[entry]};"]
        ordered = sorted(bodies, key=lambda n: (n[0], n[1], n[2], len(n[3]), n[3]))
        for i, node in enumerate(ordered):
            body, fall = bodies[node]
            lines.append(f"{self.nodes[node]}:")
            lines.extend(body)
            if fall is not None:
                nxt = ordered[i + 1] if i + 1 < len(ordered) else None
                if nxt != fall:
                    lines.append(f"    goto {self.nodes[fall]};")
        lines.append("}")
        return "\n".join(lines) + "\n"

    def gen_node(self, node):
        pc, m, x, php = node
        d = decode(pc, m, x)
        out = []
        e = lambda s: out.append("    " + s)
        if d is None:
            e(f"rt_dispatch(c, 0x{pc:06X}); return; /* flow leaves ROM */")
            return out, None
        fo = lorom_to_file(pc)
        hexb = " ".join(f"{b:02X}" for b in rom[fo:fo + d["size"]])
        out.append(f"    /* ${pc:06X}  {hexb:<11}  {d['mn']} {d['mode']} m{m}x{x} */")
        out.append(f"    RT_FETCH(0x{pc >> 16:02X}, {d['size']}, {1 if d['mode'] in ('imp', 'acc') else 0});")
        if pc in OVERRIDES and not m:
            e(OVERRIDES[pc] + "  /* widescreen patch */")
        else:
            gen_insn(pc, d, m, x, None, out, None)
        nm, nx, nphp = flags_after(pc, d, m, x, php)
        mn, mode, val, bank = d["mn"], d["mode"], d["val"], d["bank"]
        pb = bank >> 16
        nxt_node = (canon(d["nxt"]), nm, nx, nphp)
        ret16 = (pc + d["size"] - 1) & 0xFFFF
        expect = (pb << 16) | ((ret16 + 1) & 0xFFFF)

        def jump_to(target):
            t = canon(target)
            if t <= pc:
                e(f"RT_TICK(c, 0x{t:06X});")
            e(f"goto {self.label((t, nm, nx, nphp))};")

        if mn in BRANCHES:
            t = canon(d["tgt"])
            tick = f" RT_TICK(c, 0x{t:06X});" if t <= pc else ""
            e(f"if ({COND[mn]}) {{{tick} goto {self.label((t, nm, nx, nphp))}; }}")
            return out, self.label(nxt_node) and nxt_node
        if mn in ("BRA", "BRL"):
            jump_to(d["tgt"])
            return out, None
        if mn == "JMP" and mode == "abs":
            jump_to(bank | val)
            return out, None
        if mn == "JML" and mode == "absl":
            jump_to(val)
            return out, None
        if mn == "JSR" and mode == "abs":
            f = self.call_target(bank | val, nm, nx)
            e(f"push16(c, 0x{ret16:04X}); {f}; RET_CHECK(c, 0x{expect:06X});")
            return out, self.label(nxt_node) and nxt_node
        if mn == "JSL":
            f = self.call_target(val, nm, nx)
            e(f"push8(c, 0x{pb:02X}); push16(c, 0x{ret16:04X}); {f}; RET_CHECK(c, 0x{expect:06X});")
            return out, self.label(nxt_node) and nxt_node
        if mn in ("JMP", "JSR", "JML") and mode in ("absi", "absix", "absil"):
            if mode == "absi":
                tgt = f"(0x{pb:02X}0000u | rd16l(0x{val:04X}))"
            elif mode == "absix":
                tgt = f"(0x{pb:02X}0000u | rd16l(0x{pb:02X}0000u | (uint16_t)(0x{val:04X} + c->x)))"
            else:
                tgt = f"rd24l(0x{val:04X})"
            targets = tracer.tables.get(pc, [])
            e(f"{{ uint32_t t = canon24({tgt});")
            if mn == "JSR":
                e(f"  push16(c, 0x{ret16:04X});")
                if targets:
                    e("  switch (t) {")
                    for t in sorted(set(targets)):
                        e(f"  case 0x{t:06X}: {self.call_target(t, nm, nx)}; break;")
                    e("  default: rt_dispatch(c, t); break; }")
                else:
                    e("  rt_dispatch(c, t);")
                e(f"  RET_CHECK(c, 0x{expect:06X}); }}")
                return out, self.label(nxt_node) and nxt_node
            if targets:
                e("  switch (t) {")
                for t in sorted(set(targets)):
                    e(f"  case 0x{t:06X}: goto {self.label((t, nm, nx, nphp))};")
                e("  } }")
            else:
                e("  }")
            e(f"rt_dispatch(c, canon24({tgt})); return;")
            return out, None
        if mn == "RTS":
            e(f"c->ret = 0x{pb:02X}0000u | (uint16_t)(pull16(c) + 1); return;")
            return out, None
        if mn == "RTL":
            e("{ uint16_t lo = pull16(c); uint8_t b = pull8(c); c->ret = ((uint32_t)b << 16) | (uint16_t)(lo + 1); } return;")
            return out, None
        if mn == "RTI":
            e("set_p(c, pull8(c)); { uint16_t lo = pull16(c); uint8_t b = c->e ? 0 : pull8(c); c->ret = ((uint32_t)b << 16) | lo; } return;")
            return out, None
        if mn == "STP":
            return out, None
        return out, self.label(nxt_node) and nxt_node


def emit(rom_bytes, tracer_obj, gen_dir, rom_sha1, hints=None, log=print):
    global rom, tracer, FLAG_HINTS
    rom, tracer, FLAG_HINTS = rom_bytes, tracer_obj, (hints or {}).get("flags_after", {})
    global OVERRIDES
    OVERRIDES = find_overrides(rom)
    log(f"widescreen sprite patches: {len(OVERRIDES)} instructions")
    gen = pathlib.Path(gen_dir)
    gen.mkdir(parents=True, exist_ok=True)
    for old in gen.glob("*.c"):
        old.unlink()
    vec = lambda o: canon(int.from_bytes(rom[o:o + 2], "little"))
    roots = {"RESET": (vec(0x7FFC), 1, 1), "NMI": (vec(0x7FEA), 1, 1), "IRQ": (vec(0x7FEE), 1, 1)}
    in_rom = lambda a: (lorom_to_file(a) is not None and lorom_to_file(a) < len(rom))
    queue = deque(roots.values())
    for a, states in tracer.func_states.items():
        for (m, x) in states:
            queue.append((a, m, x))
    done = {}
    while queue:
        key = queue.popleft()
        if key in done or not in_rom(key[0]):
            continue
        done[key] = FuncGen(*key, queue).generate()

    NL = "\n"
    keys = sorted(done)
    for i in range(0, len(keys), FUNCS_PER_FILE):
        chunk = keys[i:i + FUNCS_PER_FILE]
        (gen / f"code_{i // FUNCS_PER_FILE:03d}.c").write_text(
            '#include "rt.h"' + NL + '#include "gen.h"' + NL + NL + NL.join(done[k] for k in chunk))

    protos = NL.join(f"void {fname(*k)}(Cpu *c);" for k in keys)
    aliases = NL.join(f"#define GEN_{n} {fname(*k)}" for n, k in roots.items() if k in done)
    (gen / "gen.h").write_text(NL.join([
        "#pragma once", '#include "cpu_state.h"',
        f'#define GEN_ROM_SHA1 "{rom_sha1}"', f"#define GEN_ROM_SIZE {len(rom)}u",
        f"#define GEN_FUNC_COUNT {len(keys)}", protos, "", aliases, ""]))

    cases = [f"    case 0x{a:06X}{m}{x}ull: return {fname(a, m, x)};" for (a, m, x) in keys]
    (gen / "dispatch.c").write_text(NL.join([
        '#include "rt.h"', '#include "gen.h"', "",
        "/* (addr<<8 | m<<4 | x) -> recompiled function, or NULL (then the interpreter runs it) */",
        "GenFn gen_lookup(uint32_t addr, int m, int x) {",
        "    switch (((unsigned long long)addr << 8) | (m << 4) | x) {",
        *cases,
        "    default: return 0;", "    }", "}", "",
        "GenFn gen_lookup_any(uint32_t addr, int *m, int *x) {",
        "    for (int mm = 0; mm < 2; mm++) for (int xx = 0; xx < 2; xx++) {",
        "        GenFn f = gen_lookup(addr, mm, xx);",
        "        if (f) { *m = mm; *x = xx; return f; }",
        "    }", "    return 0;", "}", ""]))
    lines = sum(v.count(NL) for v in done.values())
    nfiles = (len(keys) + FUNCS_PER_FILE - 1) // FUNCS_PER_FILE
    log(f"emitted {len(done)} functions, {lines} lines of C in {nfiles} files")
    return len(done), lines
