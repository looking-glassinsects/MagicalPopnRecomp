"""Developer shortcut: recompile only runtime/*.c (and LakeSnes if it changed) and relink an
existing build, reusing its generated-code objects. Much faster than a full pipeline run.

    python tools/dev_relink.py build/rom-XXXXXXXXXXXX OUT_DIR
"""
import pathlib
import shutil
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import pipeline as P                           # noqa: E402
import toolchain                               # noqa: E402

tc = toolchain.find()
work = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
obj, gen = work / "obj", work / "gen"
lib_hash = P._hash_files([P.LAKESNES / f for f in P.LAKESNES_SRC], extra=tc.name)
lib_dir = P.WORK_HOME / "common" / f"lakesnes_{lib_hash}"
if not lib_dir.exists():
    lib_dir.mkdir(parents=True)
    tc.compile([P.LAKESNES / f for f in P.LAKESNES_SRC], lib_dir, includes=[P.LAKESNES],
               defines=["_CRT_SECURE_NO_WARNINGS"], log=print)
tc.compile([P.RUNTIME / f for f in P.RUNTIME_SRC], obj, includes=[P.RUNTIME, gen, P.LAKESNES],
           defines=["_CRT_SECURE_NO_WARNINGS"], log=print)
res = work / ("res.o" if tc.obj == ".o" else "res.res")
exe = work / "MagicalPopn.exe"
tc.link(sorted(obj.glob("*" + tc.obj)) + sorted(lib_dir.glob("*" + tc.obj)) + [res], exe, gui=True, log=print, cwd=work)
out.mkdir(parents=True, exist_ok=True)
shutil.copy2(exe, out / "MagicalPopn.exe")
print("relinked ->", out / "MagicalPopn.exe")
