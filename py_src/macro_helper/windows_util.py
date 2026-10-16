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
        handle = win32api.OpenProcess(
            win32con.PROCESS_QUERY_LIMITED_INFORMATION, False, pid
        )
        try:
            # QueryFullProcessImageName via ctypes for reliability
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(1024)
            if kernel32.QueryFullProcessImageNameW(int(handle), 0, buf, ctypes.byref(size)):
                return Path(buf.value).name
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


def monitor_and_dpi(hwnd: int) -> tuple[int, int]:
    try:
        get_dpi = getattr(user32, "GetDpiForWindow", None)
        dpi = int(get_dpi(hwnd)) if get_dpi else 96
        monitor = user32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        # Best-effort ordinal among active monitors (diagnostic only).
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


def snapshot_at_point(x: int, y: int) -> TargetSnapshot | None:
    hwnd = root_from_point(x, y)
    if not hwnd:
        return None
    proc = process_base_name(hwnd)
    if is_noise_process(proc):
        return None
    left, top, _, _ = window_rect(hwnd)
    mon, dpi = monitor_and_dpi(hwnd)
    window = WindowInfo(
        process=proc,
        class_name=class_name_of(hwnd),
        title=title_of(hwnd),
        monitor=mon,
        dpi=dpi,
    )
    return TargetSnapshot(hwnd=hwnd, window=window, uia=uia_from_point(x, y))


def snapshot_foreground() -> TargetSnapshot | None:
    hwnd = root_of(win32gui.GetForegroundWindow())
    if not hwnd:
        return None
    proc = process_base_name(hwnd)
    if is_noise_process(proc):
        return None
    mon, dpi = monitor_and_dpi(hwnd)
    window = WindowInfo(
        process=proc,
        class_name=class_name_of(hwnd),
        title=title_of(hwnd),
        monitor=mon,
        dpi=dpi,
    )
    return TargetSnapshot(hwnd=hwnd, window=window)


def _match_score(hwnd: int, want: WindowInfo) -> int:
    proc = process_base_name(hwnd)
    if not proc or proc.lower() != want.process.lower():
        return -1
    score = 10
    cls = class_name_of(hwnd)
    title = title_of(hwnd)
    if want.class_name and cls.lower() == want.class_name.lower():
        score += 5
    if want.title and want.title.lower() in title.lower():
        score += 3
    if win32gui.IsWindowVisible(hwnd):
        score += 1
    return score


def find_window(want: WindowInfo, timeout_s: float = 60.0, poll_s: float = 0.25) -> int:
    """Wait until a top-level window matching process (+ optional class/title) appears."""
    if not want.process:
        return 0
    deadline = time.monotonic() + timeout_s
    best = 0
    while True:
        candidates: list[tuple[int, int]] = []

        def _enum(hwnd, _):
            if not win32gui.IsWindowVisible(hwnd):
                return True
            score = _match_score(hwnd, want)
            if score >= 0:
                candidates.append((score, int(hwnd)))
            return True

        win32gui.EnumWindows(_enum, None)
        if candidates:
            candidates.sort(reverse=True)
            best = candidates[0][1]
            return best
        if time.monotonic() >= deadline:
            return 0
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
