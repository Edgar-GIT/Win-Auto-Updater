"""Win32 / UIA helpers for window identity, activation, and element lookup."""

from __future__ import annotations

import ctypes
import sys
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path

from .noise import is_noise_process
from .schema import UiaInfo, WindowInfo

if sys.platform != "win32":
    raise ImportError("macro_helper.windows_util requires Windows")

import win32api  # noqa: E402
import win32con  # noqa: E402
import win32gui  # noqa: E402
import win32process  # noqa: E402

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

GA_ROOT = 2
MONITOR_DEFAULTTONEAREST = 2


@dataclass
class TargetSnapshot:
    hwnd: int
    window: WindowInfo
    uia: UiaInfo | None = None


def process_base_name(hwnd: int) -> str:
    try:
        _, pid = win32process.GetWindowThreadProcessId(hwnd)
        if not pid:
            return ""
        # Prefer limited query (works elevated→elevated on Win10/11).
        for access in (
            win32con.PROCESS_QUERY_LIMITED_INFORMATION,
            win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ,
        ):
            try:
                handle = win32api.OpenProcess(access, False, pid)
            except Exception:
                continue
            try:
                buf = ctypes.create_unicode_buffer(1024)
                size = wintypes.DWORD(1024)
                if kernel32.QueryFullProcessImageNameW(int(handle), 0, buf, ctypes.byref(size)):
                    return Path(buf.value).name
                try:
                    return Path(win32process.GetModuleFileNameEx(handle, 0)).name
                except Exception:
                    pass
            finally:
                win32api.CloseHandle(handle)
    except Exception:
        return ""
    return ""


def class_name_of(hwnd: int) -> str:
    try:
        return win32gui.GetClassName(hwnd) or ""
    except Exception:
        return ""


def title_of(hwnd: int) -> str:
    try:
        return win32gui.GetWindowText(hwnd) or ""
    except Exception:
        return ""


def root_from_point(x: int, y: int) -> int:
    hwnd = win32gui.WindowFromPoint((x, y))
    if not hwnd:
        return 0
    try:
        root = win32gui.GetAncestor(hwnd, GA_ROOT)
        return int(root or hwnd)
    except Exception:
        return int(hwnd)


def root_of(hwnd: int) -> int:
    if not hwnd:
        return 0
    try:
        root = win32gui.GetAncestor(hwnd, GA_ROOT)
        return int(root or hwnd)
    except Exception:
        return int(hwnd)


def window_rect(hwnd: int) -> tuple[int, int, int, int]:
    return win32gui.GetWindowRect(hwnd)


def monitor_effective_dpi(hwnd: int) -> int:
    """Monitor scale DPI (96=100%, 120=125%, 144=150%).

    Prefer GetDpiForMonitor — GetDpiForWindow often stays at 96 for older
    DPI-unaware apps (MemTest, Prime95, …), which broke 100%↔150% replay.
    """
    try:
        monitor = user32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        get_for_mon = getattr(ctypes.windll.shcore, "GetDpiForMonitor", None)
        if get_for_mon is not None and monitor:
            dpi_x = ctypes.c_uint(0)
            dpi_y = ctypes.c_uint(0)
            # MDT_EFFECTIVE_DPI = 0
            if int(get_for_mon(monitor, 0, ctypes.byref(dpi_x), ctypes.byref(dpi_y))) == 0:
                return int(dpi_x.value or dpi_y.value or 96) or 96
    except Exception:
        pass
    try:
        get_dpi = getattr(user32, "GetDpiForWindow", None)
        if get_dpi is not None:
            return int(get_dpi(hwnd) or 96) or 96
    except Exception:
        pass
    return 96


def monitor_and_dpi(hwnd: int) -> tuple[int, int]:
    try:
        dpi = monitor_effective_dpi(hwnd)
        monitor = user32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        index = 0
        try:
            import win32api

            handles = [m[0] for m in win32api.EnumDisplayMonitors()]
            if monitor in handles:
                index = handles.index(monitor)
        except Exception:
            index = 0
        return index, dpi or 96
    except Exception:
        return 0, 96


def _uia_client():
    """Lazy UIAutomation client (comtypes)."""
    import comtypes
    import comtypes.client

    comtypes.client.GetModule("UIAutomationCore.dll")
    from comtypes.gen.UIAutomationClient import (  # type: ignore
        CUIAutomation,
        IUIAutomation,
    )

    return comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)


def uia_from_point(x: int, y: int) -> UiaInfo | None:
    try:
        uia = _uia_client()
        pt = wintypes.POINT(x, y)
        element = uia.ElementFromPoint(pt)
        if element is None:
            return None
        auto_id = str(element.CurrentAutomationId or "")
        name = str(element.CurrentName or "")
        ctype = ""
        try:
            ctype = str(element.CurrentControlType)
        except Exception:
            ctype = ""
        # Prefer a human-readable control type name when possible
        try:
            from comtypes.gen.UIAutomationClient import (  # type: ignore
                UIA_ButtonControlTypeId,
                UIA_CheckBoxControlTypeId,
                UIA_ComboBoxControlTypeId,
                UIA_EditControlTypeId,
                UIA_HyperlinkControlTypeId,
                UIA_ListItemControlTypeId,
                UIA_MenuItemControlTypeId,
                UIA_RadioButtonControlTypeId,
                UIA_TabItemControlTypeId,
                UIA_TextControlTypeId,
            )

            mapping = {
                UIA_ButtonControlTypeId: "Button",
                UIA_CheckBoxControlTypeId: "CheckBox",
                UIA_ComboBoxControlTypeId: "ComboBox",
                UIA_EditControlTypeId: "Edit",
                UIA_HyperlinkControlTypeId: "Hyperlink",
                UIA_ListItemControlTypeId: "ListItem",
                UIA_MenuItemControlTypeId: "MenuItem",
                UIA_RadioButtonControlTypeId: "RadioButton",
                UIA_TabItemControlTypeId: "TabItem",
                UIA_TextControlTypeId: "Text",
            }
            ctype = mapping.get(int(element.CurrentControlType), ctype)
        except Exception:
            pass
        info = UiaInfo(automation_id=auto_id, name=name, control_type=ctype)
        return info if info.useful() else None
    except Exception:
        return None


def snapshot_at_point(x: int, y: int, *, include_uia: bool = False) -> TargetSnapshot | None:
    """Window identity under a screen point.

    include_uia defaults to False: UIA ElementFromPoint is slow and, when called
    inside a low-level mouse hook (pynput), Windows 10 drops subsequent events —
    that caused "only 2 clicks recorded". Replay uses relative coords anyway.
    """
    hwnd = root_from_point(x, y)
    if not hwnd:
        return None
    proc = process_base_name(hwnd)
    if is_noise_process(proc):
        return None
    mon, dpi = monitor_and_dpi(hwnd)
    left, top, right, bottom = window_rect(hwnd)
    window = WindowInfo(
        process=proc,
        class_name=class_name_of(hwnd),
        title=title_of(hwnd),
        monitor=mon,
        dpi=dpi,
        width=max(0, right - left),
        height=max(0, bottom - top),
    )
    uia = uia_from_point(x, y) if include_uia else None
    return TargetSnapshot(hwnd=hwnd, window=window, uia=uia)


def snapshot_foreground() -> TargetSnapshot | None:
    hwnd = root_of(win32gui.GetForegroundWindow())
    if not hwnd:
        return None
    proc = process_base_name(hwnd)
    if is_noise_process(proc):
        return None
    mon, dpi = monitor_and_dpi(hwnd)
    left, top, right, bottom = window_rect(hwnd)
    window = WindowInfo(
        process=proc,
        class_name=class_name_of(hwnd),
        title=title_of(hwnd),
        monitor=mon,
        dpi=dpi,
        width=max(0, right - left),
        height=max(0, bottom - top),
    )
    return TargetSnapshot(hwnd=hwnd, window=window)


def _title_fuzzy_bonus(want_title: str, have_title: str) -> int:
    """Score title similarity — exact, substring, or shared significant tokens."""
    want = (want_title or "").strip().lower()
    have = (have_title or "").strip().lower()
    if not want:
        return 0
    if want == have:
        return 5
    if want in have or have in want:
        return 3
    # "Could not allocate 4095 MB" vs "Could not allocate 2048 MB"
    want_tokens = [t for t in want.replace("-", " ").split() if len(t) > 2 and not t.isdigit()]
    if not want_tokens:
        return 0
    hits = sum(1 for t in want_tokens if t in have)
    if hits >= max(2, len(want_tokens) - 1):
        return 2
    return 0


def _match_score(hwnd: int, want: WindowInfo) -> tuple[int, bool]:
    """(score, exact). score < 0 = no match.

    exact = the recorded class and title both match literally. A merely similar
    window (same process, e.g. "powerMAX Legal Disclaimer" for "powerMAX", or the
    main dialog instead of its drop-down list) is only accepted after a grace
    period — see find_window_timed.
    """
    proc = process_base_name(hwnd)
    if not proc or proc.lower() != want.process.lower():
        return -1, False
    score = 10
    exact = True
    cls = class_name_of(hwnd)
    title = title_of(hwnd)
    if want.class_name:
        if cls.lower() == want.class_name.lower():
            score += 5
        else:
            exact = False
    # Title was recorded — require a fuzzy hit so we don't click a different
    # dialog of the same process (e.g. main MemTest vs OOM message box).
    if want.title:
        title_bonus = _title_fuzzy_bonus(want.title, title)
        if title_bonus <= 0:
            return -1, False
        score += title_bonus
        if title_bonus < 5:
            exact = False
    if win32gui.IsWindowVisible(hwnd):
        score += 1
    return score, exact


def process_running(image_name: str) -> bool:
    """True if the process still has a visible top-level window (fast, no ACL noise)."""
    if not image_name:
        return False
    want = image_name.lower()
    found = {"ok": False}

    def _enum(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd):
            return True
        if process_base_name(hwnd).lower() == want:
            found["ok"] = True
            return False
        return True

    try:
        win32gui.EnumWindows(_enum, None)
    except Exception:
        return False
    return bool(found["ok"])


def find_window_timed(
    want: WindowInfo,
    timeout_s: float = 60.0,
    poll_s: float = 0.1,
    fuzzy_grace_s: float = 1.5,
) -> tuple[int, float]:
    """Wait for a top-level window matching process (+ optional class/title).

    Returns (hwnd, seconds_waited); hwnd 0 = not found. An exact class/title
    match is returned immediately. A merely similar window of the same process
    is only accepted once ``fuzzy_grace_s`` has passed, so a fast replay does not
    click a dialog that is about to be replaced by the recorded one.

    Fail-fast if the process is not running for a few seconds (avoids long waits
    after e.g. MemTest already exited).
    """
    if not want.process:
        return 0, 0.0
    start = time.monotonic()
    deadline = start + timeout_s
    grace_deadline = start + min(timeout_s, max(0.0, fuzzy_grace_s))
    missing_proc_since: float | None = None

    while True:
        candidates: list[tuple[int, int, bool]] = []

        def _enum(hwnd, _):
            if not win32gui.IsWindowVisible(hwnd):
                return True
            score, exact = _match_score(hwnd, want)
            if score >= 0:
                candidates.append((score, int(hwnd), exact))
            return True

        win32gui.EnumWindows(_enum, None)
        now = time.monotonic()
        if candidates:
            candidates.sort(reverse=True)
            score, hwnd, exact = candidates[0]
            if exact or now >= grace_deadline:
                return hwnd, now - start

        # Process gone → don't burn the full timeout (common after MemTest closes).
        if not candidates and not process_running(want.process):
            if missing_proc_since is None:
                missing_proc_since = now
            elif now - missing_proc_since >= 2.0:
                return 0, now - start
        else:
            missing_proc_since = None

        if now >= deadline:
            return 0, now - start
        time.sleep(poll_s)


def find_window(want: WindowInfo, timeout_s: float = 60.0, poll_s: float = 0.1) -> int:
    """Compatibility wrapper around find_window_timed (returns only the hwnd)."""
    return find_window_timed(want, timeout_s=timeout_s, poll_s=poll_s)[0]


_SMTO_ABORTIFHUNG = 0x0002
_WM_NULL = 0x0000

user32.SendMessageTimeoutW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
    wintypes.UINT,
    wintypes.UINT,
    ctypes.POINTER(ctypes.c_size_t),
]
user32.SendMessageTimeoutW.restype = ctypes.c_size_t


def window_latency(hwnd: int, timeout_ms: int = 250) -> float | None:
    """Seconds the window's thread takes to answer a WM_NULL; None = no answer."""
    result = ctypes.c_size_t(0)
    t0 = time.perf_counter()
    ok = user32.SendMessageTimeoutW(
        hwnd, _WM_NULL, 0, 0, _SMTO_ABORTIFHUNG, timeout_ms, ctypes.byref(result)
    )
    if not ok:
        return None
    return time.perf_counter() - t0


def wait_window_ready(
    hwnd: int,
    timeout_s: float = 4.0,
    stable_s: float = 0.12,
    poll_s: float = 0.04,
) -> tuple[bool, float, float]:
    """Wait until the window can take a click.

    Ready = visible, enabled (not blocked by a modal child), its message loop
    answers, and its rectangle has stopped moving for ``stable_s``. This is what
    lets a fast PC replay quickly while a slow or busy one automatically waits
    longer. Returns (ready, seconds_waited, last_latency_s).
    """
    start = time.monotonic()
    deadline = start + timeout_s
    last_rect: tuple[int, int, int, int] | None = None
    stable_since: float | None = None
    latency = 0.0
    while True:
        now = time.monotonic()
        try:
            alive = bool(win32gui.IsWindow(hwnd))
            usable = alive and bool(win32gui.IsWindowVisible(hwnd)) and bool(
                win32gui.IsWindowEnabled(hwnd)
            )
            rect = win32gui.GetWindowRect(hwnd) if alive else None
        except Exception:
            alive, usable, rect = False, False, None
        if not alive:
            return False, now - start, latency

        if usable:
            answer = window_latency(hwnd)
            if answer is None:
                usable = False
            else:
                latency = answer
        if usable and rect == last_rect:
            if stable_since is None:
                stable_since = now
            if now - stable_since >= stable_s:
                return True, now - start, latency
        else:
            stable_since = None
        last_rect = rect

        if now >= deadline:
            return False, now - start, latency
        time.sleep(poll_s)


def activate_window(hwnd: int) -> None:
    """Reliable foreground activation (AttachThreadInput dance)."""
    if not hwnd:
        return
    try:
        if win32gui.IsIconic(hwnd):
            win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        else:
            win32gui.ShowWindow(hwnd, win32con.SW_SHOW)
    except Exception:
        pass

    fg = win32gui.GetForegroundWindow()
    current_tid = kernel32.GetCurrentThreadId()
    fg_tid = 0
    if fg:
        fg_tid, _ = win32process.GetWindowThreadProcessId(fg)
    target_tid, _ = win32process.GetWindowThreadProcessId(hwnd)

    attached_fg = False
    attached_target = False
    try:
        if fg_tid and fg_tid != current_tid:
            attached_fg = bool(user32.AttachThreadInput(current_tid, fg_tid, True))
        if target_tid and target_tid != current_tid and target_tid != fg_tid:
            attached_target = bool(user32.AttachThreadInput(current_tid, target_tid, True))
        win32gui.BringWindowToTop(hwnd)
        win32gui.SetForegroundWindow(hwnd)
    except Exception:
        pass
    finally:
        if attached_target:
            user32.AttachThreadInput(current_tid, target_tid, False)
        if attached_fg:
            user32.AttachThreadInput(current_tid, fg_tid, False)


def invoke_uia(hwnd: int, info: UiaInfo) -> bool:
    """Try to Invoke / Select a control under hwnd matching recorded UIA identity."""
    if not info or not info.useful():
        return False
    try:
        from pywinauto import Desktop
        from pywinauto.controls.uiawrapper import UIAWrapper

        desktop = Desktop(backend="uia")
        try:
            window = desktop.window(handle=hwnd)
        except Exception:
            window = desktop

        # Try increasingly loose criteria so a rename-resistant automation_id wins,
        # then visible Name, then Name+control_type.
        attempts: list[dict] = []
        if info.automation_id:
            attempts.append({"auto_id": info.automation_id})
        if info.name and info.control_type:
            attempts.append({"title": info.name, "control_type": info.control_type})
        if info.name:
            attempts.append({"title": info.name})

        for criteria in attempts:
            try:
                ctrl = window.child_window(**criteria)
                ctrl.wait("exists enabled visible ready", timeout=2)
                wrapper: UIAWrapper = ctrl.wrapper_object()
                try:
                    wrapper.invoke()
                    return True
                except Exception:
                    wrapper.click_input()
                    return True
            except Exception:
                continue
        return False
    except Exception:
        return False


def own_process_names() -> set[str]:
    names = {"macrohelper.exe", "python.exe", "pythonw.exe"}
    try:
        import sys as _sys

        exe = Path(_sys.executable).name.lower()
        if exe:
            names.add(exe)
    except Exception:
        pass
    return names
