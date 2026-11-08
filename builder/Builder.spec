# PyInstaller spec for the packaged Builder.exe:  python -m PyInstaller builder/Builder.spec
import pathlib
R = pathlib.Path(SPECPATH).parent
datas = [(str(R / "runtime"), "runtime"),
         (str(R / "third_party" / "LakeSnes"), "third_party/LakeSnes"),
         (str(R / "data" / "codemap.txt"), "data"),
         (str(R / "assets" / "icon.ico"), "assets"),
         (str(R / "assets" / "builder_banner.png"), "assets")]
a = Analysis([str(R / "builder" / "builder.pyw")], pathex=[str(R / "tools")], datas=datas,
             hiddenimports=["pipeline", "toolchain", "snes", "icon", "emit_c", "disasm65816", "msvc"],
             excludes=["numpy", "PIL", "matplotlib", "unittest", "pydoc", "test"])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="Builder", console=False,
          icon=str(R / "assets" / "icon.ico"), upx=False)
coll = COLLECT(exe, a.binaries, a.datas, name="MagicalPopn-Builder", upx=False)
