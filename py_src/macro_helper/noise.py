"""Processes whose input must never be recorded or replayed."""

from __future__ import annotations

NOISE_PROCESSES = frozenset(
    {
        "explorer.exe",
        "windowsterminal.exe",
        "cmd.exe",
        "powershell.exe",
        "pwsh.exe",
        "conhost.exe",
        "searchhost.exe",
        "searchui.exe",
        "macrohelper.exe",
        "python.exe",
        "pythonw.exe",
        "singleupdate.exe",
    }
)


def is_noise_process(name: str | None) -> bool:
    # Empty name is NOT treated as noise here — OpenProcess can fail on some
    # Windows 10 setups; the caller still records class/title as fallback.
    if not name:
        return False
    return name.strip().lower() in NOISE_PROCESSES


def is_own_process(name: str | None, own_names: set[str]) -> bool:
    if not name:
        return False
    return name.strip().lower() in {n.lower() for n in own_names}
