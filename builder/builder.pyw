"""Magical Pop'n Builder: pick your ROM, press Build, play.

A small tkinter front end for tools/pipeline.py. No ROM data is bundled anywhere; the game
is built from whichever dump of Magical Pop'n you select.
"""
import json
import locale
import os
import pathlib
import queue
import subprocess
import sys
import threading
import traceback
import unicodedata
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

FROZEN = getattr(sys, "frozen", False)            # packaged Builder.exe
REPO = pathlib.Path(sys._MEIPASS) if FROZEN else pathlib.Path(__file__).resolve().parent.parent
APP_DIR = pathlib.Path(sys.executable).resolve().parent if FROZEN else REPO
DATA = (pathlib.Path(os.environ.get("LOCALAPPDATA") or pathlib.Path.home()) / "MagicalPopnBuilder"
        if FROZEN else REPO / "build")
sys.path.insert(0, str(REPO / "tools"))
import snes        # noqa: E402
import toolchain   # noqa: E402

PREFS = DATA / "builder.json"

# ------------------------------------------------------------------ text
T = {
    "title": ("Magical Pop'n Builder", "マジカルポップン ビルダー"),
    "rom": ("Game ROM", "ゲームROM"),
    "choose": ("Choose", "選択"),
    "drop_hint": ("Drag your ROM here. Any dump works: .sfc, .smc or .zip, original or translated.",
                  "ここにROMをドラッグしてください。.sfc、.smc、.zip、オリジナル版・翻訳版どちらも使えます。"),
    "no_rom": ("No ROM selected yet", "ROMが選択されていません"),
    "output": ("Install to", "出力先"),
    "build_note": ("Takes about a minute and a half. Everything is compiled before you play.",
                   "約1分半かかります。プレイ前にすべてコンパイルされます。"),
    "not_writable": ("Can't write to the install folder. Choose another one with \"Install to\".",
                     "出力先に書き込めません。「出力先」で別のフォルダを選んでください。"),
    "build": ("Build", "ビルド"),
    "cancel": ("Cancel", "キャンセル"),
    "play": ("Play", "プレイ"),
    "open": ("Open folder", "フォルダを開く"),
    "log": ("Details", "詳細"),
    "ready": ("Ready", "準備完了"),
    "building": ("Building", "ビルド中"),
    "done": ("Finished. Press Play.", "完成しました。プレイを押してください。"),
    "cancelled": ("Cancelled", "キャンセルしました"),
    "failed": ("Build failed. See Details.", "ビルドに失敗しました。詳細を確認してください。"),
    "compiler_ok": ("Compiler ready", "コンパイラ準備完了"),
    "compiler_missing": ("The toolchain folder is missing. Extract the download again.",
                         "toolchain フォルダがありません。ダウンロードを再展開してください。"),
    "kind_original": ("Original Japanese release", "オリジナル（日本版）"),
    "kind_modified": ("Modified build (translation / patch)", "改造版（翻訳・パッチ）"),
    "kind_unknown": ("Not recognised as Magical Pop'n", "マジカルポップンではないようです"),
    "not_popn": ("This file doesn't look like Magical Pop'n. Build anyway?",
                 "このファイルはマジカルポップンではないようです。ビルドしますか？"),
    "need_compiler": ("The builder's compiler is missing: the \"toolchain\" folder next to Builder.exe.\n\n"
                      "Extract the whole download again (all files and folders), then reopen the builder.",
                      "ビルダーのコンパイラ（Builder.exe の隣の「toolchain」フォルダ）がありません。\n\n"
                      "ダウンロードしたファイルをすべてもう一度展開してから、ビルダーを開き直してください。"),
    "lang": ("日本語", "English"),
    "size": ("Size", "サイズ"),
    "routines": ("routines", "ルーチン"),
}

BG, CARD, INK, DIM = "#15111d", "#221b2e", "#f2ecf7", "#a99cbb"
SOFT, SOFT_HI, SOFT_OFF = "#352b45", "#443858", "#2a2335"
PINK, PINK_HI, PINK_OFF = "#d64c96", "#ea66ad", "#4b3a52"
GREEN, GREEN_HI, GREEN_OFF = "#2f9e77", "#3bb68b", "#2c4a42"
TEXT_OFF = "#8b7d96"
FONT = "Yu Gothic UI"     # draws both English and Japanese well, so nothing falls back to another font


def show_path(p):
    """A path for display: Japanese fonts draw the backslash as a yen sign."""
    return str(p).replace("\\", "/")


def load_prefs():
    try:
        return json.loads(PREFS.read_text(encoding="utf-8"))
    except Exception:
        lang = 1 if (locale.getlocale()[0] or "").lower().startswith(("ja", "japanese")) else 0
        return {"lang": lang, "rom": "", "out": str(APP_DIR / "MagicalPopn")}


PERSIST = True      # False while updating a game: that run's ROM and folder aren't the user's choices


def save_prefs(p):
    if not PERSIST:
        return
    PREFS.parent.mkdir(parents=True, exist_ok=True)
    PREFS.write_text(json.dumps(p, indent=1, ensure_ascii=False), encoding="utf-8")


def enable_file_drop(root, callback):
    """Let files be dragged from Explorer onto the window (plain Win32, no extra packages)."""
    try:
        import ctypes
        from ctypes import wintypes
        u32, sh32 = ctypes.windll.user32, ctypes.windll.shell32
        WM_DROPFILES, GWLP_WNDPROC = 0x0233, -4
        LRESULT = ctypes.c_ssize_t
        WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        u32.GetParent.restype = wintypes.HWND
        u32.SetWindowLongPtrW.restype = ctypes.c_void_p
        u32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
        u32.CallWindowProcW.restype = LRESULT
        u32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        sh32.DragQueryFileW.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_wchar_p, wintypes.UINT]
        sh32.DragFinish.argtypes = [ctypes.c_void_p]
        hwnd = u32.GetParent(root.winfo_id()) or root.winfo_id()
        old = None

        def proc(h, msg, wp, lp):
            if msg == WM_DROPFILES:
                n = sh32.DragQueryFileW(wp, 0xFFFFFFFF, None, 0)
                files = []
                for i in range(n):
                    buf = ctypes.create_unicode_buffer(sh32.DragQueryFileW(wp, i, None, 0) + 1)
                    sh32.DragQueryFileW(wp, i, buf, len(buf))
                    files.append(buf.value)
                sh32.DragFinish(wp)
                root.after(0, lambda: callback(files))
                return 0
            return u32.CallWindowProcW(old, h, msg, wp, lp)

        cb = WNDPROC(proc)
        root._drop_proc = cb                                    # keep the callback alive
        old = u32.SetWindowLongPtrW(hwnd, GWLP_WNDPROC, ctypes.cast(cb, ctypes.c_void_p))
        sh32.DragAcceptFiles(hwnd, True)
    except Exception:
        pass


class App:
    def __init__(self, root, initial_rom=None, out_dir=None, update=False):
        self.root = root
        self.prefs = load_prefs()
        if initial_rom:
            self.prefs["rom"] = initial_rom
        if out_dir:
            self.prefs["out"] = out_dir
        # update mode (started by the game's "update now"): build the given ROM into the game's
        # folder, then delete that copy of the ROM, start the game and close
        self.update_mode = update
        self.q = queue.Queue()
        self.worker = None
        self.cancel_flag = False
        self.rom_info = None
        self.result = None
        self.compiler_ok = toolchain.available()
        self.autostart = bool(initial_rom)

        root.title(self.t("title"))
        root.configure(bg=BG)
        root.resizable(False, False)
        self.font = (FONT, 10)
        self._style()
        self._build_ui()
        self._set_icon()
        self._dark_title_bar()
        self.refresh_rom()
        self.root.after(100, self.poll)
        self.root.after(300, lambda: enable_file_drop(self.root, self.dropped))
        if self.autostart and self.rom_info and self.rom_info["is_popn"] and self.compiler_ok:
            self.root.after(500, self.start_build)            # ROM dragged onto Builder.exe: just go

    def dropped(self, paths):
        if self.worker or not paths:
            return
        self.prefs["rom"] = paths[0]
        save_prefs(self.prefs)
        self.refresh_rom()
        if self.rom_info and self.rom_info["is_popn"] and self.compiler_ok:
            self.start_build()

    # ------------------------------------------------------------ helpers
    @property
    def lang(self):
        return self.prefs.get("lang", 0)

    def t(self, k):
        return T[k][self.lang]

    def _style(self):
        s = ttk.Style(self.root)
        s.theme_use("clam")
        s.configure(".", background=BG, foreground=INK, font=self.font)
        s.configure("Card.TFrame", background=CARD, relief="flat")
        s.configure("Card.TLabel", background=CARD, foreground=INK)
        s.configure("Dim.TLabel", background=CARD, foreground=DIM, font=(FONT, 9))
        s.configure("TCheckbutton", background=BG, foreground=INK, indicatorbackground=CARD, indicatorforeground=INK,
                    upperbordercolor=SOFT, lowerbordercolor=SOFT, focuscolor=BG)
        s.map("TCheckbutton", background=[("active", BG)], indicatorbackground=[("active", SOFT), ("selected", CARD)])
        s.configure("Pink.Horizontal.TProgressbar", troughcolor=CARD, background=PINK, bordercolor=CARD,
                    lightcolor=PINK, darkcolor=PINK, thickness=14)

        def button(name, normal, hover, off, **opts):
            s.configure(name, foreground=INK, background=normal, borderwidth=0, focuscolor=normal,
                        bordercolor=normal, lightcolor=normal, darkcolor=normal, **opts)
            s.map(name, background=[("disabled", off), ("active", hover)], foreground=[("disabled", TEXT_OFF)],
                  bordercolor=[("disabled", off), ("active", hover)], lightcolor=[("disabled", off), ("active", hover)],
                  darkcolor=[("disabled", off), ("active", hover)], focuscolor=[("disabled", off), ("active", hover)])

        big = {"font": (FONT, 12, "bold"), "padding": (10, 8), "anchor": "center"}   # the row of main buttons: all alike
        button("Build.TButton", PINK, PINK_HI, PINK_OFF, **big)
        button("Play.TButton", GREEN, GREEN_HI, GREEN_OFF, **big)
        button("Folder.TButton", SOFT, SOFT_HI, SOFT_OFF, **big)
        button("Soft.TButton", SOFT, SOFT_HI, SOFT_OFF, padding=(10, 5))

    def _set_icon(self):
        try:
            import icon as icon_mod
            self.root.iconbitmap(str(icon_mod._repo() / "assets" / "icon.ico"))
        except Exception:
            pass

    def _dark_title_bar(self):
        try:
            import ctypes
            self.root.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id()) or self.root.winfo_id()
            on = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), ctypes.sizeof(on))   # immersive dark mode
        except Exception:
            pass

    def _header(self, parent):
        self.banner = tk.PhotoImage(file=str(REPO / "assets" / "builder_banner.png"))   # kept: Tk doesn't hold a reference
        return tk.Label(parent, image=self.banner, bg=BG, borderwidth=0, highlightthickness=0)

    # ------------------------------------------------------------ UI
    def _build_ui(self):
        for w in self.root.winfo_children():
            w.destroy()
        root = self.root
        self._header(root).pack(fill="x")

        card = ttk.Frame(root, style="Card.TFrame", padding=14)
        card.pack(fill="x", padx=14, pady=(12, 6))
        top = ttk.Frame(card, style="Card.TFrame")
        top.pack(fill="x")
        ttk.Label(top, text=self.t("rom"), style="Card.TLabel", font=(self.font[0], 10, "bold")).pack(side="left")
        ttk.Button(top, text=self.t("choose"), style="Soft.TButton", command=self.choose_rom).pack(side="right")
        self.rom_label = ttk.Label(card, text="", style="Card.TLabel", wraplength=480, justify="left")
        self.rom_label.pack(anchor="w", pady=(8, 2))
        self.rom_detail = ttk.Label(card, text=self.t("drop_hint"), style="Dim.TLabel", wraplength=480, justify="left")
        self.rom_detail.pack(anchor="w")

        card2 = ttk.Frame(root, style="Card.TFrame", padding=14)
        card2.pack(fill="x", padx=14, pady=6)
        row = ttk.Frame(card2, style="Card.TFrame"); row.pack(fill="x")
        ttk.Label(row, text=self.t("output"), style="Card.TLabel", font=(self.font[0], 10, "bold")).pack(side="left")
        ttk.Button(row, text=self.t("choose"), style="Soft.TButton", command=self.choose_out).pack(side="right")
        self.out_label = ttk.Label(card2, text=show_path(self.prefs["out"]), style="Dim.TLabel", wraplength=480)
        self.out_label.pack(anchor="w", pady=(6, 8))
        ttk.Label(card2, text=self.t("build_note"), style="Dim.TLabel", wraplength=480).pack(anchor="w")

        actions = ttk.Frame(root, padding=(11, 8))             # 498 px inside: divides evenly by three
        actions.pack(fill="x")
        self.build_btn = ttk.Button(actions, text=self.t("build"), style="Build.TButton", command=self.start_build)
        self.play_btn = ttk.Button(actions, text=self.t("play"), style="Play.TButton", command=self.play)
        self.open_btn = ttk.Button(actions, text=self.t("open"), style="Folder.TButton", command=self.open_folder)
        for i, b in enumerate((self.build_btn, self.play_btn, self.open_btn)):     # equal columns: same size whatever the text
            actions.columnconfigure(i, weight=1, uniform="main")
            b.grid(row=0, column=i, sticky="nsew", padx=4)

        self.progress = ttk.Progressbar(root, style="Pink.Horizontal.TProgressbar", maximum=1.0, length=492)
        self.progress.pack(padx=14, pady=(2, 4))
        self.status = ttk.Label(root, text=self.t("ready"))
        self.status.pack(anchor="w", padx=16)

        bottom = ttk.Frame(root, padding=(14, 6, 14, 12))
        bottom.pack(fill="x")
        self.log_shown = tk.BooleanVar(value=False)
        ttk.Checkbutton(bottom, text=self.t("log"), variable=self.log_shown, command=self.toggle_log).pack(side="left")
        lang_btn = ttk.Button(bottom, text=self.t("lang"), style="Soft.TButton", command=self.toggle_lang)
        lang_btn.pack(side="right")
        comp = tk.Label(root, text=self.t("compiler_ok" if self.compiler_ok else "compiler_missing"), bg=BG,
                        fg="#5fcfa6" if self.compiler_ok else "#f07aa8", font=(FONT, 9),
                        cursor="" if self.compiler_ok else "hand2")
        comp.pack(anchor="w", padx=16, pady=(0, 6))

        self.log_frame = ttk.Frame(root, padding=(14, 0, 14, 12))
        self.log_text = tk.Text(self.log_frame, height=12, width=70, bg="#0e0b14", fg=INK, font=("Consolas", 9),
                                relief="flat", insertbackground="white", wrap="word")
        self.log_text.pack(fill="both", expand=True)
        self.update_buttons()

    def toggle_log(self):
        if self.log_shown.get():
            self.log_frame.pack(fill="both", expand=True)
        else:
            self.log_frame.pack_forget()

    def toggle_lang(self):
        if self.worker:
            return
        self.prefs["lang"] = 1 - self.lang
        save_prefs(self.prefs)
        self._style()
        self._build_ui()
        self.refresh_rom()

    # ------------------------------------------------------------ ROM / output
    def choose_rom(self):
        p = filedialog.askopenfilename(title=self.t("rom"), filetypes=[
            ("SNES ROM", "*.sfc *.smc *.swc *.fig *.zip"), ("All files", "*.*")])
        if p:
            self.prefs["rom"] = p
            save_prefs(self.prefs)
            self.refresh_rom()

    def choose_out(self):
        p = filedialog.askdirectory(title=self.t("output"), initialdir=self.prefs["out"])
        if p:
            self.prefs["out"] = p
            save_prefs(self.prefs)
            self.out_label.config(text=show_path(p))
            self.update_buttons()

    def refresh_rom(self):
        p = self.prefs.get("rom")
        self.rom_info = None
        if not p or not os.path.exists(p):
            self.rom_label.config(text=self.t("no_rom"))
            self.rom_detail.config(text=self.t("drop_hint"))
        else:
            try:
                _, info = snes.load(p)
                self.rom_info = info
                kind = {"original": "kind_original", "modified": "kind_modified"}.get(info["variant"], "kind_unknown")
                self.rom_label.config(text=f"{os.path.basename(p)}   {self.t(kind)}")
                # ROM headers use half-width katakana, which reads badly at this size: show it full width
                title = unicodedata.normalize("NFKC", info["title"])
                self.rom_detail.config(text=f"{title}   {self.t('size')} {info['size'] // 1024} KB   SHA-1 {info['sha1'][:16]}")
            except Exception as e:
                self.rom_label.config(text=os.path.basename(p))
                self.rom_detail.config(text=str(e))
        self.update_buttons()

    def exe_path(self):
        return pathlib.Path(self.prefs["out"]) / "MagicalPopn.exe"

    def update_buttons(self):
        busy = self.worker is not None
        self.build_btn.config(text=self.t("cancel") if busy else self.t("build"),
                              state="normal" if (busy or (self.rom_info and self.compiler_ok)) else "disabled")
        self.play_btn.config(state="normal" if (not busy and self.exe_path().exists()) else "disabled")
        self.open_btn.config(state="normal" if pathlib.Path(self.prefs["out"]).exists() else "disabled")

    # ------------------------------------------------------------ build
    def start_build(self):
        if self.worker:
            self.cancel_flag = True
            return
        if not self.compiler_ok:
            messagebox.showinfo(self.t("title"), self.t("need_compiler"))
            return
        force = False
        if self.rom_info and not self.rom_info["is_popn"]:
            if not messagebox.askyesno(self.t("title"), self.t("not_popn")):
                return
            force = True
        save_prefs(self.prefs)
        self.cancel_flag = False
        self.log_text.delete("1.0", "end")
        self.status.config(text=self.t("building"))
        self.progress["value"] = 0
        args = (self.prefs["rom"], self.prefs["out"], force)
        self.worker = threading.Thread(target=self.run_build, args=args, daemon=True)
        self.worker.start()
        self.update_buttons()

    def run_build(self, rom, out, force):
        import pipeline
        try:
            res = pipeline.build(rom, out, force_unknown=force,
                                 progress=lambda f, m: self.q.put(("progress", f, m)),
                                 log=lambda m: self.q.put(("log", m)),
                                 cancel=lambda: self.cancel_flag)
            self.q.put(("done", res))
        except pipeline.Cancelled:
            self.q.put(("cancelled",))
        except pipeline.NotWritable as e:
            self.q.put(("log", f"Can't write to {e}"))
            self.q.put(("failed", self.t("not_writable")))
        except Exception as e:
            self.q.put(("log", traceback.format_exc()))
            self.q.put(("failed", str(e)))

    def poll(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "progress":
                    self.progress["value"] = msg[1]
                    self.status.config(text=msg[2].strip())
                elif kind == "log":
                    self.log_text.insert("end", msg[1] + "\n")
                    self.log_text.see("end")
                elif kind in ("done", "cancelled", "failed"):
                    self.worker = None
                    if kind == "done":
                        self.result = msg[1]
                        self.progress["value"] = 1.0
                        self.status.config(text=f"{self.t('done')}   ({msg[1]['routines']} {self.t('routines')})")
                        if self.update_mode:
                            try:
                                os.remove(self.prefs["rom"])
                            except OSError:
                                pass
                            self.play()
                            self.root.after(400, self.root.destroy)
                    elif kind == "cancelled":
                        self.status.config(text=self.t("cancelled"))
                    elif msg[1] == self.t("not_writable"):
                        self.status.config(text=msg[1])
                    else:
                        self.status.config(text=self.t("failed"))
                        self.log_shown.set(True)
                        self.toggle_log()
                    self.update_buttons()
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def play(self):
        exe = self.exe_path()
        if exe.exists():
            subprocess.Popen([str(exe)], cwd=str(exe.parent))

    def open_folder(self):
        p = pathlib.Path(self.prefs["out"])
        if p.exists():
            os.startfile(str(p))


def main():
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    # Builder.exe [ROM] [--out FOLDER] [--update]
    global PERSIST
    rom = out_dir = None
    update = False
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == "--out" and args:
            out_dir = args.pop(0)
        elif a == "--update":
            update = True
        elif rom is None and not a.startswith("--"):
            rom = a
    update = update and bool(rom and out_dir)
    PERSIST = not update
    root = tk.Tk()
    App(root, rom, out_dir, update)
    root.mainloop()


if __name__ == "__main__":
    main()
