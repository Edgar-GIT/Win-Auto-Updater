"""Active-wait replay: UIA first, relative coordinates fallback, hotkeys via PyAutoGUI."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable

import pyautogui

from .schema import MacroDocument, Step, load_macro
from .windows_util import activate_window, find_window, invoke_uia, window_rect

LogFn = Callable[[str], None]

# Never fail the whole OS with a fail-safe corner click during automation.
pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0.02

# Extra settle time added on top of every recorded inter-step delay (lag / slower PCs).
DEFAULT_PAD_MS = 4000
# After the timed wait, how long to keep hunting the target window.
DEFAULT_WINDOW_TIMEOUT_S = 15.0


class ReplayReport:
    def __init__(self) -> None:
        self.total = 0
        self.ok = 0
        self.failed = 0
        self.skipped = 0
        self.errors: list[str] = []

    @property
    def success(self) -> bool:
        return self.failed == 0 and self.ok > 0

    def summary(self) -> str:
        return (
            f"Replay done: {self.ok} ok, {self.failed} failed, {self.skipped} skipped "
            f"(total {self.total})"
        )


def _log(log: LogFn | None, message: str) -> None:
    print(message, flush=True)
    if log:
        log(message)


def _vk_to_pyautogui(step: Step) -> list[str]:
    """Build a pyautogui hotkey argument list from a recorded key step."""
    keys: list[str] = []
    if step.ctrl:
        keys.append("ctrl")
    if step.shift:
        keys.append("shift")
    if step.alt:
        keys.append("alt")
    if step.win:
        keys.append("win")

    name = (step.key or "").lower()
    aliases = {
        "esc": "esc",
        "escape": "esc",
        "enter": "enter",
        "return": "enter",
        "tab": "tab",
        "space": "space",
        "backspace": "backspace",
        "delete": "delete",
        "up": "up",
        "down": "down",
        "left": "left",
        "right": "right",
        "home": "home",
        "end": "end",
        "pageup": "pageup",
        "pagedown": "pagedown",
        "insert": "insert",
    }
    if name in aliases:
        keys.append(aliases[name])
    elif name.startswith("f") and name[1:].isdigit():
        keys.append(name)
    elif len(name) == 1:
        keys.append(name)
    elif step.vk:
        # Fall back to virtual-key via ctypes SendInput-like mapping through pyautogui
        mapped = _vk_name(step.vk)
        if mapped:
            keys.append(mapped)
        else:
            keys.append(f"vk_{step.vk}")
    return keys


def _vk_name(vk: int) -> str:
    table = {
        0x1B: "esc",
        0x0D: "enter",
        0x09: "tab",
        0x20: "space",
        0x08: "backspace",
        0x2E: "delete",
        0x25: "left",
        0x26: "up",
        0x27: "right",
        0x28: "down",
    }
    if vk in table:
        return table[vk]
    if 0x70 <= vk <= 0x7B:
        return f"f{vk - 0x6F}"
    if ord("A") <= vk <= ord("Z") or ord("0") <= vk <= ord("9"):
        return chr(vk).lower()
    return ""


def _press_keys(keys: list[str]) -> None:
    # Filter unknown vk_* tokens — send via win32 if needed
    real = [k for k in keys if not k.startswith("vk_")]
    orphan_vk = [int(k[3:]) for k in keys if k.startswith("vk_")]
    if real:
        if len(real) == 1:
            pyautogui.press(real[0])
        else:
            pyautogui.hotkey(*real)
    for vk in orphan_vk:
        _send_vk(vk)


def _send_vk(vk: int) -> None:
    import ctypes

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [
            ("wVk", ctypes.c_ushort),
            ("wScan", ctypes.c_ushort),
            ("dwFlags", ctypes.c_ulong),
            ("time", ctypes.c_ulong),
            ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
        ]

    class INPUT(ctypes.Structure):
        class _I(ctypes.Union):
            _fields_ = [("ki", KEYBDINPUT)]

        _anonymous_ = ("i",)
        _fields_ = [("type", ctypes.c_ulong), ("i", _I)]

    down = INPUT(1, KEYBDINPUT(vk, 0, 0, 0, None))
    up = INPUT(1, KEYBDINPUT(vk, 0, 0x0002, 0, None))
    arr = (INPUT * 2)(down, up)
    ctypes.windll.user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT))


def play_step(
    step: Step,
    *,
    index: int,
    total: int,
    window_timeout_s: float = DEFAULT_WINDOW_TIMEOUT_S,
    on_fail: str = "abort",  # abort | continue
    prefer_coords: bool = True,
    log: LogFn | None = None,
) -> bool:
    label = f"[{index}/{total}] {step.summary()}"
    _log(log, f"STEP {label}")

    hwnd = find_window(step.window, timeout_s=window_timeout_s)
    if not hwnd:
        msg = (
            f"FAIL step {index}: window not found "
            f"(proc={step.window.process!r} class={step.window.class_name!r} "
            f"title={step.window.title!r}) after {window_timeout_s:.0f}s"
        )
        _log(log, msg)
        return False

    activate_window(hwnd)
    time.sleep(0.08)
    import win32gui

    if win32gui.GetForegroundWindow() != hwnd:
        activate_window(hwnd)
        time.sleep(0.08)

    if step.type == "click":
        # Exact recorded click first (relative coords). UIA only as fallback —
        # timing+position must match what the user did.
        def _click_coords() -> bool:
            left, top, right, bottom = window_rect(hwnd)
            x = left + step.rel_x
            y = top + step.rel_y
            x = min(max(x, left + 1), right - 2)
            y = min(max(y, top + 1), bottom - 2)
            button = "right" if step.button == "right" else "left"
            pyautogui.click(x=x, y=y, button=button)
            _log(log, f"  OK click {button} at screen ({x},{y}) rel ({step.rel_x},{step.rel_y})")
            return True

        if prefer_coords:
            return _click_coords()

        if step.uia and step.uia.useful():
            if invoke_uia(hwnd, step.uia):
                _log(log, f"  OK via UIA ({step.uia.name or step.uia.automation_id})")
                return True
            _log(log, "  UIA miss — falling back to relative coordinates")
        return _click_coords()

    if step.type == "key":
        keys = _vk_to_pyautogui(step)
        if not keys:
            _log(log, f"FAIL step {index}: empty key combo")
            return False
        _press_keys(keys)
        _log(log, f"  OK key {'+'.join(keys)}")
        return True

    _log(log, f"FAIL step {index}: unknown type {step.type!r}")
    return False


def play_document(
    doc: MacroDocument,
    *,
    window_timeout_s: float = DEFAULT_WINDOW_TIMEOUT_S,
    pad_ms: int = DEFAULT_PAD_MS,
    on_fail: str = "abort",
    log: LogFn | None = None,
) -> ReplayReport:
    report = ReplayReport()
    report.total = len(doc.steps)
    if not doc.steps:
        _log(log, "No steps in macro — nothing to replay.")
        return report

    _log(
        log,
        f"Replaying {report.total} step(s): recorded timing + {pad_ms}ms pad each, "
        f"window timeout={window_timeout_s:.0f}s",
    )
    for i, step in enumerate(doc.steps, start=1):
        # Pace exactly like the recording, then add lag margin for slower PCs.
        wait_ms = max(0, int(step.dt_ms)) + max(0, int(pad_ms))
        if wait_ms > 0:
            _log(
                log,
                f"  wait {wait_ms / 1000.0:.1f}s "
                f"(recorded {step.dt_ms}ms + pad {pad_ms}ms) before step {i}",
            )
            time.sleep(wait_ms / 1000.0)

        ok = play_step(
            step,
            index=i,
            total=report.total,
            window_timeout_s=window_timeout_s,
            on_fail=on_fail,
            prefer_coords=True,
            log=log,
        )
        if ok:
            report.ok += 1
        else:
            report.failed += 1
            report.errors.append(f"step {i}: {step.summary()}")
            if on_fail == "abort":
                _log(log, "Aborting replay after failure.")
                break
            _log(log, "Continuing after failure (--on-fail continue).")

    _log(log, report.summary())
    return report


def play_file(
    path: Path,
    *,
    window_timeout_s: float = DEFAULT_WINDOW_TIMEOUT_S,
    pad_ms: int = DEFAULT_PAD_MS,
    on_fail: str = "abort",
    log: LogFn | None = None,
) -> ReplayReport:
    doc = load_macro(path)
    _log(log, f"Loaded macro: {path} ({len(doc.steps)} steps)")
    return play_document(
        doc,
        window_timeout_s=window_timeout_s,
        pad_ms=pad_ms,
        on_fail=on_fail,
        log=log,
    )


def default_tools_base() -> Path:
    """Desktop\\programas de teste — always (same as Stress::toolsBaseDir())."""
    import os

    desktop = Path(os.path.expandvars(r"%USERPROFILE%\Desktop"))
    # Localized Windows still exposes Desktop via USERPROFILE\Desktop for most setups;
    # FOLDERID_Desktop is used on the C++ side.
    return desktop / "programas de teste"


def launch_stress_tools(log: LogFn | None = None) -> int:
    """Optional --replay-launch: start the five bench tools."""
    import subprocess

    base = default_tools_base()
    tools = [
        ("FurMark", base / "FurMark2_x64" / "FurMark_GUI.exe"),
        ("Prime95", base / "Prime95" / "prime95.exe"),
        ("CrystalDiskMark", base / "CrystalDiskMark9" / "DiskMark64.exe"),
        ("powerMAX", base / "powerMAX" / "powerMAX.exe"),
        ("MemTest", base / "memtest.exe"),
    ]
    launched = 0
    for label, exe in tools:
        if not exe.exists():
            _log(log, f"Missing {label}: {exe}")
            continue
        try:
            subprocess.Popen([str(exe)], cwd=str(exe.parent))
            _log(log, f"Launched {label}")
            launched += 1
        except Exception as exc:
            _log(log, f"Failed to launch {label}: {exc}")
    if launched:
        _log(log, "Waiting 4s for windows to appear...")
        time.sleep(4.0)
    return launched
