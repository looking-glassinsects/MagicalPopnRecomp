"""Locate Visual Studio (Build Tools or any edition with the C++ workload) and run cl/link/rc."""
import os
import pathlib
import subprocess

VSWHERE = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
    "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
DOWNLOAD_URL = "https://visualstudio.microsoft.com/visual-cpp-build-tools/"
_env_cache = None


class CompilerMissing(Exception):
    pass


def find_vcvars():
    if VSWHERE.exists():
        out = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-property", "installationPath"],
                             capture_output=True, text=True, creationflags=_NOWINDOW).stdout.strip()
        for line in out.splitlines():
            p = pathlib.Path(line) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if p.exists():
                return p
    raise CompilerMissing("Microsoft C++ Build Tools were not found.")


_NOWINDOW = 0x08000000 if os.name == "nt" else 0


def env():
    """Environment with cl.exe/link.exe/rc.exe on PATH (cached)."""
    global _env_cache
    if _env_cache is None:
        vcvars = find_vcvars()
        out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True,
                             shell=False, creationflags=_NOWINDOW).stdout
        e = {}
        for line in out.splitlines():
            k, sep, v = line.partition("=")
            if sep:
                e[k] = v
        if "INCLUDE" not in e:
            raise CompilerMissing("Could not initialise the Visual C++ environment.")
        _env_cache = e
    return _env_cache


def tool(name):
    import shutil
    e = env()
    path = e.get("Path") or e.get("PATH") or ""
    found = shutil.which(name, path=path)
    if not found:
        raise CompilerMissing(f"{name} was not found in the Visual C++ environment.")
    return found


def run(args, cwd, log=None):
    args = [tool(args[0])] + list(args[1:])
    p = subprocess.Popen(args, cwd=cwd, env=env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace", creationflags=_NOWINDOW)
    lines = []
    for line in p.stdout:
        line = line.rstrip()
        if line:
            lines.append(line)
            if log and ("error" in line.lower() or "warning C" in line):
                log(line)
    if p.wait():
        raise RuntimeError(f"{pathlib.Path(args[0]).name} failed:\n" + "\n".join(lines[-40:]))
    return lines


def available():
    try:
        find_vcvars()
        return True
    except CompilerMissing:
        return False
