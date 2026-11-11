"""C toolchains the builder can use.

  * Bundled clang (llvm-mingw) in a `toolchain` folder next to Builder.exe or in the repo:
    nothing to install, used whenever present.
  * Microsoft C++ Build Tools (cl/link/rc), if installed.
"""
import concurrent.futures as cf
import os
import pathlib
import subprocess
import sys

import msvc

NOWIN = 0x08000000 if os.name == "nt" else 0
LIBS = ["user32", "gdi32", "winmm", "xinput", "opengl32", "ole32", "hid", "winhttp"]


def _app_dir():
    if getattr(sys, "frozen", False):
        return pathlib.Path(sys.executable).resolve().parent
    return pathlib.Path(__file__).resolve().parent.parent


def bundled_clang_root():
    for base in (_app_dir(), pathlib.Path(__file__).resolve().parent.parent):
        t = base / "toolchain"
        for cand in [t] + (sorted(t.glob("llvm-mingw*")) if t.exists() else []):
            if (cand / "bin" / "x86_64-w64-mingw32-clang.exe").exists():
                return cand
    return None


class ToolchainMissing(Exception):
    pass


class Clang:
    name = "clang (bundled)"
    obj = ".o"

    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.bin = self.root / "bin"
        self.cc = str(self.bin / "x86_64-w64-mingw32-clang.exe")
        self.windres = str(self.bin / "x86_64-w64-mingw32-windres.exe")
        self.env = dict(os.environ)
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env.get("PATH", "")

    def _run(self, args, cwd, log=None):
        r = subprocess.run(args, cwd=cwd, env=self.env, capture_output=True, text=True, errors="replace",
                           creationflags=NOWIN)
        out = (r.stdout + r.stderr).strip()
        if r.returncode:
            raise RuntimeError(f"{pathlib.Path(args[0]).name} failed:\n" + out[-4000:])
        return out

    def compile(self, sources, out_dir, includes=(), defines=(), opt=2, warnings=False, log=None,
                progress=None, jobs=None):
        out_dir = pathlib.Path(out_dir)
        jobs = jobs or max(1, (os.cpu_count() or 4))
        flags = [f"-O{opt}", "-fno-strict-aliasing", "-fwrapv"] + ([] if warnings else ["-w"])
        flags += [f"-I{i}" for i in includes] + [f"-D{d}" for d in defines]

        def one(src):
            src = pathlib.Path(src)
            o = out_dir / (src.stem + self.obj)
            self._run([self.cc, "-c", *flags, str(src), "-o", str(o)], cwd=out_dir)
            return o

        objs = []
        sources = sorted(sources, key=lambda f: -pathlib.Path(f).stat().st_size)   # biggest first
        with cf.ThreadPoolExecutor(jobs) as ex:
            for i, o in enumerate(ex.map(one, sources)):
                objs.append(o)
                if progress:
                    progress(i + 1, len(sources))
        return objs

    def resource(self, rc_file, out_dir, log=None):
        out = pathlib.Path(out_dir) / "res.o"
        self._run([self.windres, "-i", str(rc_file), "-o", str(out), "-O", "coff"], cwd=pathlib.Path(rc_file).parent)
        return out

    def link(self, objs, out_exe, gui=True, log=None, cwd=None):
        args = [self.cc, "-o", str(out_exe), *[str(o) for o in objs], "-s", "-static"]
        if gui:
            args.append("-mwindows")
        args += [f"-l{l}" for l in LIBS]
        self._run(args, cwd=cwd or pathlib.Path(out_exe).parent)

    def exe(self, sources, out_exe, includes=(), defines=(), log=None):
        """Compile + link a small console program in one go (the trace profiler)."""
        self._run([self.cc, "-O2", "-w", *[f"-I{i}" for i in includes], *[f"-D{d}" for d in defines],
                   *[str(s) for s in sources], "-o", str(out_exe), "-static"], cwd=pathlib.Path(out_exe).parent)


class MSVC:
    name = "Microsoft C++ Build Tools"
    obj = ".obj"

    def compile(self, sources, out_dir, includes=(), defines=(), opt=2, warnings=False, log=None,
                progress=None, jobs=None):
        out_dir = pathlib.Path(out_dir)
        flags = ["/nologo", "/c", f"/O{opt}", "/W3" if warnings else "/W0", "/wd4102", "/wd4244", "/utf-8",
                 "/bigobj", "/MP", "/D_CRT_SECURE_NO_WARNINGS", *[f"/I{i}" for i in includes],
                 *[f"/D{d}" for d in defines], f"/Fo{out_dir}\\"]
        sources = [str(s) for s in sources]
        batches = [sources[i:i + 24] for i in range(0, len(sources), 24)]
        done = 0
        for b in batches:
            msvc.run(["cl", *flags, *b], cwd=out_dir, log=log)
            done += len(b)
            if progress:
                progress(done, len(sources))
        return [out_dir / (pathlib.Path(s).stem + self.obj) for s in sources]

    def resource(self, rc_file, out_dir, log=None):
        out = pathlib.Path(out_dir) / "res.res"
        msvc.run(["rc", "/nologo", "/fo", str(out), str(rc_file)], cwd=pathlib.Path(rc_file).parent, log=log)
        return out

    def link(self, objs, out_exe, gui=True, log=None, cwd=None):
        msvc.run(["link", "/nologo", "/OPT:REF", "/SUBSYSTEM:WINDOWS" if gui else "/SUBSYSTEM:CONSOLE",
                  *(["/ENTRY:mainCRTStartup"] if gui else []), f"/OUT:{out_exe}", *[str(o) for o in objs],
                  *[f"{l}.lib" for l in LIBS]], cwd=cwd or pathlib.Path(out_exe).parent, log=log)

    def exe(self, sources, out_exe, includes=(), defines=(), log=None):
        tdir = pathlib.Path(out_exe).parent / (pathlib.Path(out_exe).stem + "_obj")
        tdir.mkdir(exist_ok=True)
        msvc.run(["cl", "/nologo", "/O2", "/W0", "/MP", "/D_CRT_SECURE_NO_WARNINGS", *[f"/I{i}" for i in includes],
                  *[f"/D{d}" for d in defines], *[str(s) for s in sources], f"/Fo{tdir}\\", f"/Fe{out_exe}"],
                 cwd=tdir, log=log)


def find():
    """The toolchain to use: the bundled clang if present, else Microsoft's, else None."""
    root = bundled_clang_root()
    if root:
        return Clang(root)
    if msvc.available():
        msvc.env()
        return MSVC()
    return None


def available():
    return bundled_clang_root() is not None or msvc.available()
