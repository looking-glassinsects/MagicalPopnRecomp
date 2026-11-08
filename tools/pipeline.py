"""Magical Pop'n build pipeline: ROM in, native Windows game out.

  1. read + identify the ROM (any dump: .sfc/.smc/.zip, headered or not, original or patched)
  2. build the trace profiler and let an input bot play the game in parallel sessions,
     recording every routine and register-width state the game really uses
  3. trace all code statically from those entries (+ pointer harvesting)
  4. emit C for every routine
  5. compile + link with MSVC, embedding the ROM and icon as resources
  6. smoke-test the result headless

CLI:  python tools/pipeline.py ROM [--out DIR]
"""
import concurrent.futures as cf
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

FROZEN = getattr(sys, "frozen", False)          # running as the packaged Builder.exe
TOOLS = pathlib.Path(__file__).resolve().parent
REPO = pathlib.Path(sys._MEIPASS) if FROZEN else TOOLS.parent          # read-only sources
APP_DIR = pathlib.Path(sys.executable).resolve().parent if FROZEN else REPO
WORK_HOME = (pathlib.Path(os.environ.get("LOCALAPPDATA") or pathlib.Path.home()) / "MagicalPopnBuilder"
             if FROZEN else REPO / "build")                             # writable scratch
sys.path.insert(0, str(TOOLS))

import msvc                     # noqa: E402,F401
import toolchain                # noqa: E402
import snes                     # noqa: E402
import icon                     # noqa: E402
from disasm65816 import trace, read_entries, code_bytes   # noqa: E402
from emit_c import emit         # noqa: E402

LAKESNES = REPO / "third_party" / "LakeSnes" / "snes"
LAKESNES_SRC = ["snes.c", "snes_other.c", "cpu.c", "apu.c", "spc.c", "dsp.c", "dma.c", "ppu.c", "cart.c",
                "input.c", "statehandler.c"]
RUNTIME = REPO / "runtime"
RUNTIME_SRC = ["main.c", "rt.c", "hw.c", "platform_win32.c", "pad_win32.c", "menu.c", "settings.c", "audio_wasapi.c"]
PIPELINE_VERSION = 3

SESSIONS = [("attract", 1, 7200), ("menus", 1, 14400), ("menus", 2, 14400)] + \
           [("play", s, 43200) for s in range(1, 13)]
NOWIN = 0x08000000 if os.name == "nt" else 0
CODEMAP = REPO / "data" / "codemap.txt"


class NotWritable(Exception):
    """The install folder can't be written to."""


class Cancelled(Exception):
    pass


def _hash_files(paths, extra=""):
    h = hashlib.sha1(extra.encode())
    for p in paths:
        h.update(pathlib.Path(p).read_bytes())
    return h.hexdigest()[:16]


def build_tracer(log=print, tc=None):
    """Build (or reuse) the trace profiler executable."""
    tc = tc or toolchain.find()
    common = WORK_HOME / "common"
    common.mkdir(parents=True, exist_ok=True)
    tool_hash = _hash_files([RUNTIME / "trace_main.c", RUNTIME / "png_write.h"] + [LAKESNES / f for f in LAKESNES_SRC],
                            extra=tc.name)
    tracer_exe = common / f"popn_trace_{tool_hash}.exe"
    if not tracer_exe.exists():
        tc.exe([RUNTIME / "trace_main.c", *[LAKESNES / f for f in LAKESNES_SRC]], tracer_exe,
               includes=[LAKESNES, RUNTIME], defines=["_CRT_SECURE_NO_WARNINGS"], log=log)
    return tracer_exe


def codemap_entries(rom):
    """Shipped entry points whose code fingerprint matches this ROM, and how well the ROM
    matches the best-fitting known version of the game (0..1).

    Each row may name the versions it was found in (5th column, comma-separated); the ratio
    is taken per version, so adding more versions to the map never lowers the match of one."""
    import zlib
    if not CODEMAP.exists():
        return [], 0.0
    ok, total, hit = {}, {}, {}
    for line in CODEMAP.read_text().splitlines():
        parts = line.split()
        if line.startswith("#") or len(parts) < 4:
            continue
        addr, m, x, fp = int(parts[0], 16), int(parts[1]), int(parts[2]), parts[3]
        labels = parts[4].split(",") if len(parts) > 4 else ["fastrom"]
        fo = snes.lorom_to_file(addr)
        match = fo is not None and fo < len(rom) and f"{zlib.crc32(rom[fo:fo + 12]) & 0xFFFFFFFF:08x}" == fp
        if match:
            ok[(addr, m, x)] = 1
        for lb in labels:
            total[lb] = total.get(lb, 0) + 1
            hit[lb] = hit.get(lb, 0) + match
    ratio = max((hit[lb] / total[lb] for lb in total), default=0.0)
    return sorted(ok), ratio


def build(rom_path, out_dir=None, thorough=None, force_unknown=False,
          progress=lambda frac, msg: None, log=print, cancel=lambda: False):
    t_start = time.time()

    def step(frac, msg):
        if cancel():
            raise Cancelled()
        progress(frac, msg)
        log(msg)

    # ---------------------------------------------------------------- 1. ROM
    step(0.01, "Reading ROM")
    rom, info = snes.load(rom_path)
    if not info["is_popn"] and not force_unknown:
        raise snes.RomError("This doesn't look like Magical Pop'n (the internal title doesn't match).")
    log(f"  {info['title']}  {info['size'] // 1024} KB  {info['variant']}  SHA-1 {info['sha1']}")
    sha = info["sha1"]
    out = pathlib.Path(out_dir) if out_dir else APP_DIR / "MagicalPopn"
    try:
        out.mkdir(parents=True, exist_ok=True)
        probe = out / ".write_test"
        probe.write_bytes(b"")
        probe.unlink()
    except OSError as e:
        raise NotWritable(str(out)) from e
    try:
        (out / "MagicalPopn.old.exe").unlink()        # left by a rebuild while the game was running
    except OSError:
        pass
    work = WORK_HOME / f"rom-{sha[:12]}"
    work.mkdir(parents=True, exist_ok=True)
    (work / "rom.sfc").write_bytes(rom)
    (work / "rom_info.json").write_text(json.dumps(info, indent=1, ensure_ascii=False), encoding="utf-8")

    step(0.03, "Setting up the compiler")
    tc = toolchain.find()
    if tc is None:
        raise toolchain.ToolchainMissing("No C compiler found: the toolchain folder next to the builder is missing.")
    log(f"  compiler: {tc.name}")
    common = WORK_HOME / "common"
    common.mkdir(parents=True, exist_ok=True)

    # ---------------------------------------------------------------- 2. code map / profile
    known, ratio = codemap_entries(rom)
    known_file = work / "codemap_matched.txt"
    known_file.write_text("".join(f"{a:06X} {m} {x} codemap\n" for a, m, x in known))
    outputs = [known_file]
    if ratio >= 0.9:
        step(0.08, f"Recognised this version of the game: using the code map ({len(known)} entry points)")
    else:
        log(f"  code map matches {ratio * 100:.0f}% of this ROM: finding its code by playing it automatically")
        step(0.05, "Building the trace profiler")
        tracer_exe = build_tracer(log, tc)
        prof = work / "profile"
        prof.mkdir(exist_ok=True)
        total_frames = sum(f for _, _, f in SESSIONS)
        step(0.08, f"Playing the game automatically to find all code ({len(SESSIONS)} sessions)")
        done_frames = 0
        workers = max(1, min(len(SESSIONS), (os.cpu_count() or 4) - 1))

        def run_session(sess):
            mode, seed, frames = sess
            o = prof / f"{mode}_{seed}_{frames}.txt"
            if not o.exists():
                r = subprocess.run([str(tracer_exe), str(work / "rom.sfc"), str(o) + ".tmp", mode, str(seed), str(frames)],
                                   capture_output=True, text=True, creationflags=NOWIN)
                if r.returncode:
                    raise RuntimeError(f"trace session {mode}/{seed} failed: {r.stderr[-500:]}")
                os.replace(str(o) + ".tmp", o)
                if os.path.exists(str(o) + ".tmp.exec"):
                    os.replace(str(o) + ".tmp.exec", str(o) + ".exec")
            return sess, o

        with cf.ThreadPoolExecutor(workers) as ex:
            futs = [ex.submit(run_session, s) for s in SESSIONS]
            for fut in cf.as_completed(futs):
                sess, o = fut.result()
                outputs.append(o)
                done_frames += sess[2]
                step(0.08 + 0.42 * done_frames / total_frames,
                     f"  session {sess[0]} #{sess[1]} done ({done_frames * 100 // total_frames}%)")

    # ---------------------------------------------------------------- 3. analyze
    lib_hash = _hash_files([LAKESNES / f for f in LAKESNES_SRC], extra=tc.name)
    lib_dir = common / f"lakesnes_{lib_hash}"
    if not lib_dir.exists():
        step(0.52, "Compiling the SNES hardware library")
        tmp = common / f"lakesnes_{lib_hash}.tmp"
        if tmp.exists():
            shutil.rmtree(tmp)
        tmp.mkdir()
        tc.compile([LAKESNES / f for f in LAKESNES_SRC], tmp, includes=[LAKESNES],
                   defines=["_CRT_SECURE_NO_WARNINGS"], log=log)
        os.replace(tmp, lib_dir)

    (work / "popn.ico").write_bytes(icon.make_ico())
    shutil.copy2(RUNTIME / "fonts" / "menu_font.ttf", work / "menu_font.ttf")   # menu font (SIL OFL)
    rc = work / "res.rc"
    rc.write_text(
        '1 ICON "popn.ico"\n'
        'ROMDATA RCDATA "rom.sfc"\n'
        'MENUFONT RCDATA "menu_font.ttf"\n'
        '1 VERSIONINFO\nFILEVERSION 1,0,0,0\nBEGIN\n BLOCK "StringFileInfo"\n BEGIN\n  BLOCK "040904B0"\n  BEGIN\n'
        '   VALUE "FileDescription", "Magical Pop\'n (recompiled)"\n'
        '   VALUE "ProductName", "Magical Pop\'n"\n  END\n END\n'
        ' BLOCK "VarFileInfo"\n BEGIN\n  VALUE "Translation", 0x409, 1200\n END\nEND\n', encoding="ascii")
    res = tc.resource(rc, work, log=log)

    exe = out / "MagicalPopn.exe"
    exe_tmp = work / "MagicalPopn.exe"
    selftest_found = work / "discovered.txt"          # written by the self-test runs of exe_tmp
    if selftest_found.exists():
        selftest_found.unlink()
    extra_files = [out / "discovered.txt"]            # code the installed game ever had to interpret
    MAX_PASSES = 3
    for build_pass in range(1, MAX_PASSES + 1):
        lo, hi = (0.52, 0.97) if build_pass == 1 else (0.90, 0.97)
        span = lambda f: lo + (hi - lo) * f

        # ------------------------------------------------------------ 3. analyse
        step(span(0.0), "Analysing code" + (f" (pass {build_pass})" if build_pass > 1 else ""))
        entries = read_entries(outputs + extra_files)
        t = trace(rom, entries, log=log)
        starts = {snes.lorom_to_file(a) for a in t.insns}
        executed = bytearray(len(rom))
        for o in outputs:
            e = pathlib.Path(str(o) + ".exec")
            if e.exists():
                for i, v in enumerate(e.read_bytes()[:len(rom)]):
                    if v:
                        executed[i] = 1
        n_exec = sum(executed)
        n_cov = sum(1 for i, v in enumerate(executed) if v and i in starts)
        cov_pct = 100.0 * n_cov / max(1, n_exec)
        log(f"  {code_bytes(t)} bytes of code in {len(t.funcs)} routines"
            + (f"; {cov_pct:.2f}% of instructions seen while playing are recompiled" if n_exec else ""))

        # ------------------------------------------------------------ 4. emit
        step(span(0.05), "Generating C code")
        gen = work / "gen"
        nfuncs, nlines = emit(rom, t, gen, sha, log=log)

        # ------------------------------------------------------------ 5. compile
        obj = work / "obj"
        if obj.exists():
            shutil.rmtree(obj)
        obj.mkdir()
        step(span(0.10), f"Compiling the game ({nlines // 1000}k lines of C, this is the slow part)")
        gen_c = sorted(gen.glob("*.c"))
        inc = [RUNTIME, gen, LAKESNES]
        objs = tc.compile(gen_c, obj, includes=inc, defines=["_CRT_SECURE_NO_WARNINGS"], log=log,
                          progress=lambda d, n: step(span(0.10 + 0.55 * d / n), f"  compiled {d}/{n} code files"))
        objs += tc.compile([RUNTIME / f for f in RUNTIME_SRC], obj, includes=inc,
                           defines=["_CRT_SECURE_NO_WARNINGS"], warnings=False, log=log)
        step(span(0.70), "Linking")
        tc.link(objs + sorted(lib_dir.glob("*" + tc.obj)) + [res], exe_tmp, gui=True, log=log, cwd=work)

        # ------------------------------------------------------------ 6. self-test the real exe
        # Attract mode and a bot session run on the recompiled code itself; anything they had to
        # interpret (timing differs slightly from the profiler) goes into one more pass.
        step(span(0.75), "Testing the build")
        before = set(read_entries([selftest_found]))
        tests = [["--headless", "7200"], ["--headless", "14400", "--bot", str(10 + build_pass)],
                 ["--headless", "4000", "--bot", "5", "--state-test", "3000"]]
        procs = [subprocess.Popen([str(exe_tmp), *a], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  creationflags=NOWIN, cwd=work) for a in tests]
        results = []
        for p in procs:
            outp, _ = p.communicate(timeout=1200)
            results.append((p.returncode, outp.strip()))
        for code, outp in results:
            log("  " + " | ".join(l for l in outp.splitlines() if not l.startswith(("Loaded", '"', "32K"))))
            if code != 0:
                raise RuntimeError("The built game failed its self-test:\n" + outp)
        found = set(read_entries([selftest_found])) - before
        if not found or build_pass == MAX_PASSES:
            break
        log(f"  self-test reached {len(found)} routine(s) the profiler missed; recompiling with them")
        if selftest_found not in extra_files:
            extra_files.append(selftest_found)

    try:
        shutil.copy2(exe_tmp, exe)
    except OSError:
        # the game is probably running: Windows can't overwrite a running exe, but it can rename it
        old = out / "MagicalPopn.old.exe"
        try:
            old.unlink()
        except OSError:
            pass
        os.replace(exe, old)
        shutil.copy2(exe_tmp, exe)
        log("  the game was running: the new version is installed and starts next time")
    (out / "README.txt").write_text(README_TXT, encoding="utf-8")
    summary = {
        "rom": info, "routines": nfuncs, "lines_of_c": nlines, "coverage_percent": round(cov_pct, 2),
        "passes": build_pass, "seconds": round(time.time() - t_start), "exe": str(exe),
        "selftest": [r[1] for r in results], "pipeline_version": PIPELINE_VERSION,
    }
    (out / "build_info.json").write_text(json.dumps(summary, indent=1, ensure_ascii=False), encoding="utf-8")
    if FROZEN:
        # the packaged builder keeps only what makes a rebuild faster (the play profiles),
        # not ~140 MB of generated code and objects, nor a copy of the ROM
        for d in ("gen", "obj"):
            shutil.rmtree(work / d, ignore_errors=True)
        for f in ("MagicalPopn.exe", "rom.sfc", "res.o", "res.res", "res.rc", "menu_font.ttf", "popn.ico"):
            try:
                (work / f).unlink()
            except OSError:
                pass
    step(1.0, f"Done in {summary['seconds'] // 60}m {summary['seconds'] % 60}s: {exe}")
    return summary


README_TXT = """Magical Pop'n (recompiled)
==========================

Run MagicalPopn.exe.

Keyboard            Xbox        PlayStation        Switch
  Arrows  D-pad       D-pad / left stick on every controller
  Z       B           A           Cross              B
  X       A           B           Circle             A
  A       Y           X           Square             Y
  S       X           Y           Triangle           X
  Q / W   L / R       LB / RB     L1 / R1            L / R
  Enter   Start       Start       Options            +
  RShift  Select      Back        Share              -

Any controller works as soon as it is plugged in or paired (buttons go by position:
bottom = B, right = A, left = Y, top = X). If one is mixed up: menu > Controls > Set up controller.

F1 / Esc   menu (controller: click a stick, Home / PS, or hold Select + Start)
F5 / F9    save / load state        F6 / F7   previous / next slot
P          pause                    F11 / Alt+Enter   fullscreen
F12        screenshot               Tab / right trigger (hold)   fast-forward

In the menu: up/down select, left/right change, Enter opens a page or runs an action,
Esc goes back. The mouse works too (left click changes or activates, right click goes back
one value).
Your place is saved when you quit and resumed next time ("Continue on launch").
Settings: settings.ini   Save states: the states folder   Screenshots: the screenshots folder
"""


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--out")
    ap.add_argument("--force", action="store_true", help="build even if the ROM isn't recognised")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    build(a.rom, a.out, force_unknown=a.force)
