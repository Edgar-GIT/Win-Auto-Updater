"""Tkinter recording GUI — always-on-top, never records its own input."""

from __future__ import annotations

import os
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox

from .capture import MacroRecorder
from .dpi import enable_dpi_awareness
from .schema import empty_document, load_macro, save_macro
from .windows_util import root_of

# Match the SingleUpdate dark glass theme (include/ui.hpp).
BG = "#0A0F1E"
HEADER = "#101A36"
HEADER_SUB = "#96A3BA"
CARD = "#131B30"
BORDER = "#2A3654"
TEXT = "#EAF0FA"
TEXT_SEC = "#96A3BA"
ACCENT = "#3DD6F5"
ACCENT_DARK = "#2BB8D6"
INK = "#051221"  # text on bright accent buttons
RED = "#F87171"
RED_DARK = "#DC5B5B"
AMBER = "#FBBF24"
HOVER = "#1D2844"
FIELD = "#0D1426"
TERMINAL_BG = "#0B1122"
TERMINAL_FG = "#C9D4E6"


def _resolve_icon() -> Path | None:
    names = ("macro_icon.ico", "icon.ico")
    candidates: list[Path] = []
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        exe_dir = Path(sys.executable).resolve().parent
        for name in names:
            if meipass:
                candidates.append(Path(meipass) / name)
            candidates.append(exe_dir / name)
    # Dev / source tree
    here = Path(__file__).resolve().parent
    for name in names:
        candidates.append(here.parent / "assets" / name)
        candidates.append(here.parents[1] / "resources" / name)
    for path in candidates:
        if path.is_file():
            return path
    return None


class RecordApp:
    def __init__(self, macro_path: Path) -> None:
        self.macro_path = macro_path
        self.dpi_mode = enable_dpi_awareness()

        self.root = tk.Tk()
        self.root.title("Win Auto Updater — MacroHelper")
        self.root.configure(bg=BG)
        self.root.attributes("-topmost", True)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._icon_path = _resolve_icon()
        self._apply_icon()

        self._ignore_hwnds: set[int] = set()
        self._status = tk.StringVar(value="Parado")
        self._path_var = tk.StringVar(value=str(macro_path))

        self._build()
        self._fit_window()
        self._refresh_ignore_hwnds()

        self.recorder = MacroRecorder(
            ignore_hwnds=lambda: set(self._ignore_hwnds),
            on_step=self._on_step_threadsafe,
            on_status=self._on_status_threadsafe,
            on_stop=self._on_stop_threadsafe,
        )

        # Refresh HWND set periodically (window can be recreated / moved).
        self.root.after(1000, self._poll_hwnds)

    def _apply_icon(self) -> None:
        if not self._icon_path:
            return
        try:
            self.root.iconbitmap(default=str(self._icon_path))
        except Exception:
            try:
                self.root.iconbitmap(str(self._icon_path))
            except Exception:
                pass

    def _card(self, parent: tk.Misc, **pack) -> tk.Frame:
        outer = tk.Frame(parent, bg=BG)
        outer.pack(**pack)
        card = tk.Frame(outer, bg=CARD, highlightbackground=BORDER, highlightthickness=1)
        card.pack(fill=tk.BOTH, expand=True, padx=12, pady=6)
        return card

    def _btn(
        self,
        parent: tk.Misc,
        text: str,
        command,
        *,
        primary: bool = False,
        danger: bool = False,
        **pack,
    ) -> tk.Button:
        if danger:
            bg, fg, active = RED, INK, RED_DARK
        elif primary:
            bg, fg, active = ACCENT, INK, ACCENT_DARK
        else:
            bg, fg, active = HOVER, TEXT, BORDER
        btn = tk.Button(
            parent,
            text=text,
            command=command,
            font=("Segoe UI", 10, "bold"),
            bg=bg,
            fg=fg,
            activebackground=active,
            activeforeground=fg,
            disabledforeground="#5B6880",
            relief=tk.FLAT,
            bd=0,
            padx=12,
            pady=8,
            cursor="hand2",
            highlightthickness=1 if not primary and not danger else 0,
            highlightbackground=BORDER,
        )
        btn.pack(**pack)
        return btn

    def _fit_window(self) -> None:
        """Size for the content but never larger than the screen.

        The steps list is the only stretchy part, so on small screens
        (1366x768, 1024x768, 800x600) it simply gets shorter instead of the
        bottom buttons falling off the screen.
        """
        self.root.update_idletasks()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        # Leave room for the title bar and the taskbar.
        max_w = max(420, sw - 40)
        max_h = max(360, sh - 110)
        req_w = min(max_w, max(620, self.root.winfo_reqwidth() + 24))
        req_h = min(max_h, max(600, self.root.winfo_reqheight() + 60))
        self.root.minsize(min(480, req_w), min(400, req_h))
        x = max(0, (sw - req_w) // 2)
        y = max(0, (sh - req_h - 70) // 2)
        self.root.geometry(f"{req_w}x{req_h}+{x}+{y}")

    def _build(self) -> None:
        # Header (SingleUpdate-style blue bar) — tall enough for title + subtitle
        header = tk.Frame(self.root, bg=HEADER)
        header.pack(fill=tk.X)
        title_row = tk.Frame(header, bg=HEADER)
        title_row.pack(fill=tk.X, padx=18, pady=14)
        tk.Label(
            title_row,
            text="MacroHelper",
            font=("Segoe UI", 18, "bold"),
            bg=HEADER,
            fg="#FFFFFF",
            anchor="w",
        ).pack(fill=tk.X)
        tk.Label(
            title_row,
            text="Gravação de macro · Win Auto Updater",
            font=("Segoe UI", 10),
            bg=HEADER,
            fg=HEADER_SUB,
            anchor="w",
        ).pack(fill=tk.X, pady=(2, 0))

        # Main actions
        actions_card = self._card(self.root, fill=tk.X)
        inner = tk.Frame(actions_card, bg=CARD)
        inner.pack(fill=tk.X, padx=10, pady=10)

        row = tk.Frame(inner, bg=CARD)
        row.pack(fill=tk.X)
        self.btn_start = self._btn(
            row,
            "Iniciar gravação",
            self.start_recording,
            danger=True,
            side=tk.LEFT,
            fill=tk.X,
            expand=True,
            padx=(0, 6),
        )
        self.btn_start.configure(font=("Segoe UI", 11, "bold"), pady=12)
        self.btn_stop = self._btn(
            row,
            "Parar (F10)",
            self.stop_recording,
            primary=True,
            side=tk.LEFT,
            fill=tk.X,
            expand=True,
            padx=(6, 0),
        )
        self.btn_stop.configure(font=("Segoe UI", 11, "bold"), pady=12, state=tk.DISABLED, bg=HOVER)

        status_row = tk.Frame(inner, bg=CARD)
        status_row.pack(fill=tk.X, pady=(10, 0))
        self.status_label = tk.Label(
            status_row,
            textvariable=self._status,
            font=("Segoe UI", 11, "bold"),
            bg=CARD,
            fg=TEXT,
            anchor="w",
        )
        self.status_label.pack(side=tk.LEFT, fill=tk.X, expand=True)
        tk.Label(
            status_row,
            text="DPI: " + self.dpi_mode,
            font=("Segoe UI", 9),
            bg=CARD,
            fg=TEXT_SEC,
        ).pack(side=tk.RIGHT)

        # Path
        path_card = self._card(self.root, fill=tk.X)
        path_inner = tk.Frame(path_card, bg=CARD)
        path_inner.pack(fill=tk.X, padx=10, pady=8)
        tk.Label(
            path_inner,
            text="Ficheiro da macro",
            font=("Segoe UI", 9),
            bg=CARD,
            fg=TEXT_SEC,
            anchor="w",
        ).pack(fill=tk.X)
        path_row = tk.Frame(path_inner, bg=CARD)
        path_row.pack(fill=tk.X, pady=(4, 0))
        entry = tk.Entry(
            path_row,
            textvariable=self._path_var,
            font=("Segoe UI", 10),
            bg=FIELD,
            fg=TEXT,
            insertbackground=TEXT,
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=BORDER,
            highlightcolor=ACCENT,
        )
        entry.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=6, padx=(0, 6))
        self._btn(path_row, "…", self._browse, side=tk.LEFT).configure(padx=10, pady=6)

        # Bottom bar
        bottom_card = self._card(self.root, fill=tk.X, side=tk.BOTTOM)
        bottom = tk.Frame(bottom_card, bg=CARD)
        bottom.pack(fill=tk.X, padx=10, pady=8)
        self._btn(bottom, "Guardar macro", self.save, primary=True, side=tk.LEFT, padx=(0, 4))
        self._btn(bottom, "Abrir pasta", self.open_folder, side=tk.LEFT, padx=4)
        self._btn(bottom, "Carregar…", self.load, side=tk.LEFT, padx=4)
        self._topmost = tk.BooleanVar(value=True)

        def _toggle_topmost() -> None:
            self.root.attributes("-topmost", bool(self._topmost.get()))

        tk.Checkbutton(
            bottom,
            text="Sempre no topo",
            variable=self._topmost,
            command=_toggle_topmost,
            font=("Segoe UI", 9),
            bg=CARD,
            fg=TEXT_SEC,
            activebackground=CARD,
            activeforeground=TEXT,
            selectcolor=FIELD,
            highlightthickness=0,
        ).pack(side=tk.RIGHT)

        hint = (
            "Abre as ferramentas, espera as janelas e clica/tecla normalmente.\n"
            "F10 para a gravação (consumido). Cliques nesta janela não são gravados.\n"
            "Corre como Administrador se as apps alvo forem elevadas."
        )
        hint_label = tk.Label(
            self.root,
            text=hint,
            justify=tk.LEFT,
            font=("Segoe UI", 9),
            bg=BG,
            fg=TEXT_SEC,
        )
        hint_label.pack(fill=tk.X, padx=16, pady=(0, 10), side=tk.BOTTOM)
        # The bottom bar was packed first, so re-pack it above the hint.
        bottom_card.master.pack_forget()
        bottom_card.master.pack(fill=tk.X, side=tk.BOTTOM)

        # Steps list
        list_card = self._card(self.root, fill=tk.BOTH, expand=True)
        list_inner = tk.Frame(list_card, bg=CARD)
        list_inner.pack(fill=tk.BOTH, expand=True, padx=10, pady=8)
        tk.Label(
            list_inner,
            text="Passos capturados",
            font=("Segoe UI", 9),
            bg=CARD,
            fg=TEXT_SEC,
            anchor="w",
        ).pack(fill=tk.X)
        list_frame = tk.Frame(list_inner, bg=TERMINAL_BG)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(4, 0))
        scroll = tk.Scrollbar(list_frame, bg=CARD, troughcolor=TERMINAL_BG, bd=0)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.listbox = tk.Listbox(
            list_frame,
            yscrollcommand=scroll.set,
            font=("Consolas", 10),
            bg=TERMINAL_BG,
            fg=TERMINAL_FG,
            height=6,
            selectbackground=ACCENT,
            selectforeground=INK,
            relief=tk.FLAT,
            activestyle="none",
            highlightthickness=0,
            borderwidth=0,
        )
        self.listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=4, pady=4)
        scroll.config(command=self.listbox.yview)

        # Edit actions
        edit = tk.Frame(list_inner, bg=CARD)
        edit.pack(fill=tk.X, pady=(8, 0))
        self._btn(edit, "Desfazer", self.undo, side=tk.LEFT, padx=(0, 4))
        self._btn(edit, "Apagar selecionado", self.delete_selected, side=tk.LEFT, padx=4)
        self._btn(edit, "Limpar tudo", self.clear_all, side=tk.LEFT, padx=4)
        self.btn_pause = self._btn(edit, "Pausa", self.toggle_pause, side=tk.LEFT, padx=4)
        self.btn_pause.configure(state=tk.DISABLED)

    def run(self) -> int:
        self.root.mainloop()
        return 0

    def _refresh_ignore_hwnds(self) -> None:
        """Only the MacroHelper top-level HWND — IsChild covers descendants.

        Do NOT EnumWindows and add every window with GetParent==0 (Win10 bug that
        could mark unrelated HWNDs and drop most clicks).
        """
        try:
            hwnd = int(self.root.winfo_id())
            root = root_of(hwnd) or hwnd
            self._ignore_hwnds = {int(root), int(hwnd)}
        except Exception:
            self._ignore_hwnds = set()

    def _poll_hwnds(self) -> None:
        self._refresh_ignore_hwnds()
        self.root.after(1000, self._poll_hwnds)

    def _set_recording_ui(self, recording: bool) -> None:
        if recording:
            self._status.set("A GRAVAR")
            self.status_label.configure(fg=RED)
            # Disabled accent buttons are dimmed so the active one stands out.
            self.btn_start.configure(state=tk.DISABLED, bg=HOVER)
            self.btn_stop.configure(state=tk.NORMAL, bg=ACCENT)
            self.btn_pause.configure(state=tk.NORMAL, text="Pausa")
        else:
            self._status.set("Parado")
            self.status_label.configure(fg=TEXT)
            self.btn_start.configure(state=tk.NORMAL, bg=RED)
            self.btn_stop.configure(state=tk.DISABLED, bg=HOVER)
            self.btn_pause.configure(state=tk.DISABLED, text="Pausa")

    def start_recording(self) -> None:
        self._refresh_ignore_hwnds()
        if not self.recorder.steps():
            self.listbox.delete(0, tk.END)
        self.recorder.start()
        self._set_recording_ui(True)

    def stop_recording(self) -> None:
        self.recorder.stop()
        self._set_recording_ui(False)

    def toggle_pause(self) -> None:
        if not self.recorder.active:
            return
        paused = not self.recorder.paused
        self.recorder.set_paused(paused)
        self.btn_pause.configure(text="Retomar" if paused else "Pausa")
        self._status.set("Em pausa" if paused else "A GRAVAR")
        self.status_label.configure(fg=AMBER if paused else RED)

    def undo(self) -> None:
        step = self.recorder.undo()
        if step is None:
            return
        size = self.listbox.size()
        if size:
            self.listbox.delete(size - 1)

    def delete_selected(self) -> None:
        sel = list(self.listbox.curselection())
        if not sel:
            return
        steps = self.recorder.steps()
        for index in reversed(sel):
            if 0 <= index < len(steps):
                steps.pop(index)
        self.recorder.clear()
        for step in steps:
            self.recorder._steps.append(step)  # noqa: SLF001 — sync after edit
        self.listbox.delete(0, tk.END)
        for step in steps:
            self.listbox.insert(tk.END, step.summary())

    def clear_all(self) -> None:
        if self.recorder.steps() and not messagebox.askyesno(
            "MacroHelper", "Apagar todos os passos?"
        ):
            return
        self.recorder.clear()
        self.listbox.delete(0, tk.END)

    def save(self) -> None:
        path = Path(self._path_var.get().strip() or self.macro_path)
        self.macro_path = path
        doc = empty_document(dpi_awareness=self.dpi_mode)
        doc.steps = self.recorder.steps()
        if not doc.steps:
            messagebox.showinfo("MacroHelper", "Nada para guardar — nenhum passo capturado.")
            return
        try:
            save_macro(path, doc)
        except Exception as exc:
            messagebox.showerror("MacroHelper", f"Falha ao guardar:\n{exc}")
            return
        messagebox.showinfo(
            "MacroHelper",
            f"Macro guardada ({len(doc.steps)} passos):\n{path}",
        )
        # Mirror old C++ behaviour: open the file for inspection.
        try:
            os.startfile(str(path))  # type: ignore[attr-defined]
        except Exception:
            pass

    def load(self) -> None:
        path = filedialog.askopenfilename(
            title="Abrir macro",
            filetypes=[("Macro JSON", "*.json"), ("All", "*.*")],
            initialdir=str(self.macro_path.parent),
        )
        if not path:
            return
        try:
            doc = load_macro(Path(path))
        except Exception as exc:
            messagebox.showerror("MacroHelper", f"Falha ao carregar:\n{exc}")
            return
        self.macro_path = Path(path)
        self._path_var.set(path)
        self.recorder.clear()
        self.listbox.delete(0, tk.END)
        for step in doc.steps:
            self.recorder._steps.append(step)  # noqa: SLF001
            self.listbox.insert(tk.END, step.summary())

    def open_folder(self) -> None:
        folder = str(Path(self._path_var.get() or self.macro_path).resolve().parent)
        try:
            subprocess.Popen(["explorer", folder])
        except Exception as exc:
            messagebox.showerror("MacroHelper", str(exc))

    def _browse(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Guardar macro como",
            defaultextension=".json",
            filetypes=[("Macro JSON", "*.json")],
            initialfile=self.macro_path.name,
            initialdir=str(self.macro_path.parent),
        )
        if path:
            self._path_var.set(path)
            self.macro_path = Path(path)

    def _on_step_threadsafe(self, step) -> None:
        summary = step.summary()
        self.root.after(0, lambda s=summary: self.listbox.insert(tk.END, s))

    def _on_status_threadsafe(self, status: str) -> None:
        def _apply() -> None:
            if status == "recording":
                self._set_recording_ui(True)
            elif status == "paused":
                self._status.set("Em pausa")
                self.status_label.configure(fg=AMBER)
            else:
                self._set_recording_ui(False)

        self.root.after(0, _apply)

    def _on_stop_threadsafe(self) -> None:
        def _apply() -> None:
            self.recorder.stop()
            self._set_recording_ui(False)
            # Auto-save convenience when stopping with F10
            if self.recorder.steps():
                try:
                    doc = empty_document(dpi_awareness=self.dpi_mode)
                    doc.steps = self.recorder.steps()
                    path = Path(self._path_var.get().strip() or self.macro_path)
                    save_macro(path, doc)
                    self._status.set(f"Parado — guardado ({len(doc.steps)} passos)")
                except Exception:
                    pass

        self.root.after(0, _apply)

    def _on_close(self) -> None:
        if self.recorder.active:
            self.recorder.stop()
        self.root.destroy()


def run_record_gui(macro_path: Path) -> int:
    if sys.platform != "win32":
        print("MacroHelper GUI requires Windows.", file=sys.stderr)
        return 2
    app = RecordApp(macro_path)
    return app.run()
