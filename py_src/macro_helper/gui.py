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


class RecordApp:
    def __init__(self, macro_path: Path) -> None:
        self.macro_path = macro_path
        self.dpi_mode = enable_dpi_awareness()

        self.root = tk.Tk()
        self.root.title("MacroHelper — Record")
        self.root.geometry("520x480")
        self.root.minsize(420, 360)
        self.root.attributes("-topmost", True)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._ignore_hwnds: set[int] = set()
        self._status = tk.StringVar(value="Parado")
        self._path_var = tk.StringVar(value=str(macro_path))

        self._build()
        self.root.update_idletasks()
        self._refresh_ignore_hwnds()

        self.recorder = MacroRecorder(
            ignore_hwnds=lambda: set(self._ignore_hwnds),
            on_step=self._on_step_threadsafe,
            on_status=self._on_status_threadsafe,
            on_stop=self._on_stop_threadsafe,
        )

        # Refresh HWND set periodically (window can be recreated / moved).
        self.root.after(1000, self._poll_hwnds)

    def _build(self) -> None:
        pad = {"padx": 10, "pady": 6}
        top = tk.Frame(self.root)
        top.pack(fill=tk.X, **pad)

        self.btn_start = tk.Button(
            top,
            text="Iniciar gravação",
            font=("Segoe UI", 12, "bold"),
            bg="#c62828",
            fg="white",
            activebackground="#b71c1c",
            command=self.start_recording,
            height=2,
        )
        self.btn_start.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4))

        self.btn_stop = tk.Button(
            top,
            text="Parar (F10)",
            font=("Segoe UI", 12, "bold"),
            command=self.stop_recording,
            height=2,
            state=tk.DISABLED,
        )
        self.btn_stop.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))

        status_row = tk.Frame(self.root)
        status_row.pack(fill=tk.X, **pad)
        self.status_label = tk.Label(
            status_row,
            textvariable=self._status,
            font=("Segoe UI", 11, "bold"),
            fg="#333333",
            anchor="w",
        )
        self.status_label.pack(side=tk.LEFT, fill=tk.X, expand=True)
        tk.Label(status_row, text="DPI: " + self.dpi_mode, fg="#666666").pack(side=tk.RIGHT)

        path_row = tk.Frame(self.root)
        path_row.pack(fill=tk.X, **pad)
        tk.Entry(path_row, textvariable=self._path_var).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4)
        )
        tk.Button(path_row, text="…", width=3, command=self._browse).pack(side=tk.LEFT)

        mid = tk.Frame(self.root)
        mid.pack(fill=tk.BOTH, expand=True, **pad)
        tk.Label(mid, text="Passos capturados:", anchor="w").pack(fill=tk.X)
        list_frame = tk.Frame(mid)
        list_frame.pack(fill=tk.BOTH, expand=True)
        scroll = tk.Scrollbar(list_frame)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.listbox = tk.Listbox(list_frame, yscrollcommand=scroll.set, font=("Consolas", 10))
        self.listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll.config(command=self.listbox.yview)

        actions = tk.Frame(self.root)
        actions.pack(fill=tk.X, **pad)
        tk.Button(actions, text="Desfazer", command=self.undo).pack(side=tk.LEFT, padx=2)
        tk.Button(actions, text="Apagar selecionado", command=self.delete_selected).pack(
            side=tk.LEFT, padx=2
        )
        tk.Button(actions, text="Limpar tudo", command=self.clear_all).pack(side=tk.LEFT, padx=2)
        self.btn_pause = tk.Button(actions, text="Pausa", command=self.toggle_pause)
        self.btn_pause.pack(side=tk.LEFT, padx=2)

        bottom = tk.Frame(self.root)
        bottom.pack(fill=tk.X, **pad)
        tk.Button(
            bottom,
            text="Guardar macro",
            font=("Segoe UI", 10, "bold"),
            command=self.save,
        ).pack(side=tk.LEFT, padx=2)
        tk.Button(bottom, text="Abrir pasta", command=self.open_folder).pack(side=tk.LEFT, padx=2)
        tk.Button(bottom, text="Carregar…", command=self.load).pack(side=tk.LEFT, padx=2)
        self._topmost = tk.BooleanVar(value=True)

        def _toggle_topmost() -> None:
            self.root.attributes("-topmost", bool(self._topmost.get()))

        tk.Checkbutton(
            bottom,
            text="Sempre no topo",
            variable=self._topmost,
            command=_toggle_topmost,
        ).pack(side=tk.RIGHT)

        hint = (
            "Abre as ferramentas, espera as janelas e clica/tecla normalmente.\n"
            "F10 para a gravação (consumido). Cliques nesta janela não são gravados.\n"
            "Corre como Administrador se as apps alvo forem elevadas."
        )
        tk.Label(self.root, text=hint, justify=tk.LEFT, fg="#555555").pack(
            fill=tk.X, padx=10, pady=(0, 8)
        )

    def run(self) -> int:
        self.root.mainloop()
        return 0

    def _refresh_ignore_hwnds(self) -> None:
        try:
            import win32gui

            hwnd = int(self.root.winfo_id())
            # Tk on Windows: winfo_id is the HWND of the Tk child; get the root.
            root = root_of(hwnd) or hwnd
            self._ignore_hwnds = {root, hwnd}
            # Also ignore any owned popups
            def _enum(h, _):
                try:
                    if win32gui.GetParent(h) in self._ignore_hwnds or h == root:
                        self._ignore_hwnds.add(int(h))
                except Exception:
                    pass
                return True

            win32gui.EnumWindows(_enum, None)
        except Exception:
            self._ignore_hwnds = set()

    def _poll_hwnds(self) -> None:
        self._refresh_ignore_hwnds()
        self.root.after(1000, self._poll_hwnds)

    def _set_recording_ui(self, recording: bool) -> None:
        if recording:
            self._status.set("A GRAVAR")
            self.status_label.configure(fg="#c62828")
            self.btn_start.configure(state=tk.DISABLED)
            self.btn_stop.configure(state=tk.NORMAL)
            self.btn_pause.configure(state=tk.NORMAL, text="Pausa")
        else:
            self._status.set("Parado")
            self.status_label.configure(fg="#333333")
            self.btn_start.configure(state=tk.NORMAL)
            self.btn_stop.configure(state=tk.DISABLED)
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
        self.status_label.configure(fg="#ef6c00" if paused else "#c62828")

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
        self.root.after(0, lambda: self.listbox.insert(tk.END, step.summary()))

    def _on_status_threadsafe(self, status: str) -> None:
        def _apply() -> None:
            if status == "recording":
                self._set_recording_ui(True)
            elif status == "paused":
                self._status.set("Em pausa")
                self.status_label.configure(fg="#ef6c00")
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
