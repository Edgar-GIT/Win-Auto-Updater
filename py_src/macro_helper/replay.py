"""Active-wait replay: relative coordinates first, UIA fallback, hotkeys via PyAutoGUI.

Pacing: a short speed-dependent gap between actions, then an *active* wait for
the target window to exist and be ready (see speed.py). Recorded delays are not
replayed, apart from a small allowance after unusually long recorded pauses.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable

import pyautogui

from .schema import MacroDocument, Step, load_macro
from .speed import ReplayTiming, timing_for_factor
from .windows_util import (
    activate_window,
    find_window,
    find_window_timed,
    invoke_uia,
    wait_window_ready,
    window_rect,
)

LogFn = Callable[[str], None]

# Never fail the whole OS with a fail-safe corner click during automation.
pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0.02

# Pacing used when no speed probe result is supplied (mid-range PC).
DEFAULT_TIMING = timing_for_factor(1.0, "default timing (factor 1.00)")
DEFAULT_WINDOW_TIMEOUT_S = DEFAULT_TIMING.timeout_s

# A recorded pause longer than this suggests the operator waited for something
# that cannot be observed from outside (content loading inside a window); a
# fraction of the excess is kept, capped, instead of the whole recorded delay.
LONG_PAUSE_MS = 4000
LONG_PAUSE_BONUS_CAP_MS = 2000


def recorded_bonus_ms(dt_ms: int, factor: float) -> int:
    excess = int(dt_ms) - LONG_PAUSE_MS
    if excess <= 0:
        return 0
    scale = max(0.6, min(1.5, factor))
    return int(min(LONG_PAUSE_BONUS_CAP_MS, excess * 0.5) * scale)


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
    try:
        print(message, flush=True)
    except UnicodeEncodeError:
        safe = message.encode("ascii", errors="replace").decode("ascii")
        print(safe, flush=True)
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


# Dialogs that only appear on some PCs (e.g. MemTest OOM) — skip if absent.
_OPTIONAL_TITLE_MARKERS = (
    "could not allocate",
    "out of memory",
    "insufficient memory",
    "failed to allocate",
    "not enough memory",
    "unable to allocate",
)


def is_optional_dialog(step: Step) -> bool:
    title = (step.window.title or "").lower()
    return any(m in title for m in _OPTIONAL_TITLE_MARKERS)


def play_step(
    step: Step,
    *,
    index: int,
    total: int,
    window_timeout_s: float = DEFAULT_WINDOW_TIMEOUT_S,
    timing: ReplayTiming = DEFAULT_TIMING,
    on_fail: str = "continue",  # abort | continue
    prefer_coords: bool = True,
    log: LogFn | None = None,
) -> str:
    """Returns 'ok' | 'fail' | 'skip'."""
    label = f"[{index}/{total}] {step.summary()}"
    _log(log, f"STEP {label}")

    optional = is_optional_dialog(step)
    # Optional OOM/error boxes: short wait — they either pop quickly or never.
    timeout = min(window_timeout_s, 1.5 + timing.gap_ms / 1000.0) if optional else window_timeout_s
    hwnd, waited = find_window_timed(step.window, timeout_s=timeout)
    if not hwnd:
        if optional:
            _log(
                log,
                f"SKIP step {index}: optional dialog not shown "
                f"(title={step.window.title!r}) - continuing",
            )
            return "skip"
        msg = (
            f"FAIL step {index}: window not found "
            f"(proc={step.window.process!r} class={step.window.class_name!r} "
            f"title={step.window.title!r}) after {waited:.1f}s"
        )
        _log(log, msg)
        return "fail"

    activate_window(hwnd)
    time.sleep(0.06)
    import win32gui

    if win32gui.GetForegroundWindow() != hwnd:
        activate_window(hwnd)
        time.sleep(0.06)

    # Active wait: click as soon as the window can take it, however long (or
    # short) that is on this PC, instead of sleeping a fixed worst-case time.
    ready, ready_wait, latency = wait_window_ready(hwnd, timeout_s=timing.ready_timeout_s)
    settle_ms = 0
    if waited > 0.25:
        # The window only just appeared — let it finish painting its controls.
        settle_ms += timing.settle_ms
    if latency > 0.08:
        # Sluggish message loop (stress tests already running): be more patient.
        settle_ms += int(min(1500.0, latency * 4000.0))
    if settle_ms:
        time.sleep(settle_ms / 1000.0)
    _log(
        log,
        f"  window {'ready' if ready else 'NOT ready (clicking anyway)'}: "
        f"appeared after {waited:.2f}s, ready after {ready_wait:.2f}s, "
        f"latency {latency * 1000.0:.0f}ms, settle {settle_ms}ms",
    )

    if step.type == "click":
        # Cross-DPI: prefer normalized (nx,ny) = fraction of window size.
        # Blind monitor-DPI multiply is wrong for these apps (overshoots Y).
        def _click_coords() -> bool:
            left, top, right, bottom = window_rect(hwnd)
            cur_w = max(1, right - left)
            cur_h = max(1, bottom - top)
            scale_note = ""
            if step.nx >= 0.0 and step.ny >= 0.0:
                rel_x = int(round(step.nx * cur_w))
                rel_y = int(round(step.ny * cur_h))
                scale_note = f" norm=({step.nx:.4f},{step.ny:.4f})"
            elif step.window.width > 0 and step.window.height > 0:
                sx = cur_w / float(step.window.width)
                sy = cur_h / float(step.window.height)
                rel_x = int(round(step.rel_x * sx))
                rel_y = int(round(step.rel_y * sy))
                if abs(sx - 1.0) > 0.02 or abs(sy - 1.0) > 0.02:
                    scale_note = f" size-scale=({sx:.2f},{sy:.2f})"
            else:
                rel_x = step.rel_x
                rel_y = step.rel_y
                scale_note = " (no norm/size - re-record once for DPI portability)"
            x = left + rel_x
            y = top + rel_y
            x = min(max(x, left + 1), right - 2)
            y = min(max(y, top + 1), bottom - 2)
            button = "right" if step.button == "right" else "left"
            pyautogui.click(x=x, y=y, button=button)
            _log(
                log,
                f"  OK click {button} at screen ({x},{y}) "
                f"rel ({rel_x},{rel_y}) recorded ({step.rel_x},{step.rel_y})"
                f"{scale_note}",
            )
            return True

        if prefer_coords:
            return "ok" if _click_coords() else "fail"

        if step.uia and step.uia.useful():
            if invoke_uia(hwnd, step.uia):
                _log(log, f"  OK via UIA ({step.uia.name or step.uia.automation_id})")
                return "ok"
            _log(log, "  UIA miss - falling back to relative coordinates")
        return "ok" if _click_coords() else "fail"

    if step.type == "key":
        keys = _vk_to_pyautogui(step)
        if not keys:
            _log(log, f"FAIL step {index}: empty key combo")
            return "fail"
        _press_keys(keys)
        _log(log, f"  OK key {'+'.join(keys)}")
        return "ok"

    _log(log, f"FAIL step {index}: unknown type {step.type!r}")
    return "fail"


def play_document(
    doc: MacroDocument,
    *,
    timing: ReplayTiming = DEFAULT_TIMING,
    on_fail: str = "continue",
    log: LogFn | None = None,
) -> ReplayReport:
    report = ReplayReport()
    report.total = len(doc.steps)
    if not doc.steps:
        _log(log, "No steps in macro - nothing to replay.")
        return report

    _log(
        log,
        f"Replaying {report.total} step(s): gap {timing.gap_ms}ms between actions + active "
        f"window wait (timeout {timing.timeout_s:.0f}s), on-fail={on_fail}",
    )
    started = time.monotonic()
    last_action = started
    # Windows that were not found: later steps for the same window fail fast
    # instead of burning the full timeout again (e.g. a tool that never opened).
    missing: set[tuple[str, str, str]] = set()

    for i, step in enumerate(doc.steps, start=1):
        gap_ms = timing.gap_ms
        if not is_optional_dialog(step):
            gap_ms += recorded_bonus_ms(step.dt_ms, timing.factor)
        remaining = gap_ms / 1000.0 - (time.monotonic() - last_action)
        if remaining > 0:
            _log(
                log,
                f"  pace {remaining:.2f}s (gap {gap_ms}ms; recorded pause was {step.dt_ms}ms) "
                f"before step {i}",
            )
            time.sleep(remaining)

        key = (
            step.window.process.lower(),
            step.window.class_name.lower(),
            step.window.title.strip().lower(),
        )
        timeout_s = min(timing.timeout_s, 2.5) if key in missing else timing.timeout_s

        result = play_step(
            step,
            index=i,
            total=report.total,
            window_timeout_s=timeout_s,
            timing=timing,
            on_fail=on_fail,
            prefer_coords=True,
            log=log,
        )
        last_action = time.monotonic()
        if result == "ok":
            report.ok += 1
            missing.discard(key)
        elif result == "skip":
            report.skipped += 1
        else:
            report.failed += 1
            missing.add(key)
            report.errors.append(f"step {i}: {step.summary()}")
            if on_fail == "abort":
                _log(log, "Aborting replay after failure.")
                break
            _log(log, "Continuing after failure (--on-fail continue).")

    _log(log, report.summary() + f" in {time.monotonic() - started:.1f}s")
    return report


def play_file(
    path: Path,
    *,
    timing: ReplayTiming = DEFAULT_TIMING,
    on_fail: str = "continue",
    log: LogFn | None = None,
) -> ReplayReport:
    doc = load_macro(path)
    _log(log, f"Loaded macro: {path} ({len(doc.steps)} steps)")
    missing_norm = sum(
        1
        for s in doc.steps
        if s.type == "click" and (s.nx < 0.0 or s.ny < 0.0) and s.window.width <= 0
    )
    if missing_norm:
        _log(
            log,
            f"WARNING: {missing_norm} click(s) lack size/norm data - DPI scale "
            f"will NOT be applied (re-record once for 100%/150% portability).",
        )
    return play_document(doc, timing=timing, on_fail=on_fail, log=log)


def enrich_macro_sizes(
    path: Path,
    *,
    timeout_s: float = 8.0,
    log: LogFn | None = None,
) -> int:
    """Fill width/height/nx/ny from currently open windows.

    Must run at the SAME Windows scale as when the macro was recorded
    (e.g. 100% if that is where clicks were captured). Open the stress tools
    first, then run: MacroHelper.exe --fix-sizes macro.json
    """
    from .schema import save_macro

    doc = load_macro(path)
    updated = 0
    for i, step in enumerate(doc.steps, start=1):
        if step.type != "click":
            continue
        hwnd = find_window(step.window, timeout_s=timeout_s)
        if not hwnd:
            _log(log, f"step {i}: window not found - skipped")
            continue
        left, top, right, bottom = window_rect(hwnd)
        w = max(1, right - left)
        h = max(1, bottom - top)
        step.window.width = w
        step.window.height = h
        step.nx = step.rel_x / float(w)
        step.ny = step.rel_y / float(h)
        # Keep rel in sync with the measured window (same scale as recording).
        updated += 1
        _log(
            log,
            f"step {i}: size={w}x{h} norm=({step.nx:.4f},{step.ny:.4f}) "
            f"proc={step.window.process}",
        )
    if updated:
        save_macro(path, doc)
        _log(log, f"Updated {updated} click step(s) in {path}")
    else:
        _log(log, "No steps updated - open the target apps and retry.")
    return updated


def default_tools_base() -> Path:
    """helpers\\programas de teste (MacroHelper.exe lives in helpers\\)."""
    import sys

    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "programas de teste"
    return Path.cwd() / "programas de teste"


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
        # No long fixed sleep: replay waits for each window as it needs it.
        time.sleep(1.0)
    return launched
