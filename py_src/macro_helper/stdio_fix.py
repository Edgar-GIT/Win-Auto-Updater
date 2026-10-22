"""Restore stdout/stderr when frozen with --noconsole so the parent can pipe us."""

from __future__ import annotations

import os
import sys


def ensure_stdio() -> None:
    if sys.platform != "win32":
        return

    # Prefer UTF-8 with replace so log lines never crash on cp1252 consoles
    # (arrows, accents in window titles, etc.).
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        if stream is None:
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    try:
        import ctypes
        import msvcrt
    except Exception:
        return

    kernel32 = ctypes.windll.kernel32
    STD_OUTPUT_HANDLE = -11
    STD_ERROR_HANDLE = -12

    def _bind(stream_name: str, std_id: int) -> None:
        stream = getattr(sys, stream_name, None)
        if stream is not None and hasattr(stream, "write"):
            try:
                stream.write("")
                return
            except Exception:
                pass
        handle = kernel32.GetStdHandle(std_id)
        if not handle or handle == ctypes.c_void_p(-1).value:
            return
        try:
            fd = msvcrt.open_osfhandle(int(handle), os.O_TEXT)
            setattr(
                sys,
                stream_name,
                open(fd, "w", encoding="utf-8", errors="replace", closefd=False),
            )
        except Exception:
            pass

    _bind("stdout", STD_OUTPUT_HANDLE)
    _bind("stderr", STD_ERROR_HANDLE)


def hide_console_window() -> None:
    """Hide the console if the binary was built with console=True."""
    if sys.platform != "win32":
        return
    try:
        import ctypes

        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)
    except Exception:
        pass
