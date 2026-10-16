"""Global mouse/keyboard capture via pynput (F10 stops, GUI hwnds ignored)."""

from __future__ import annotations

import threading
import time
from typing import Callable

from pynput import keyboard, mouse

from .noise import is_noise_process, is_own_process
from .schema import Step, WindowInfo
from .windows_util import (
    own_process_names,
    snapshot_at_point,
    snapshot_foreground,
    window_rect,
)

StepCallback = Callable[[Step], None]
StatusCallback = Callable[[str], None]


class MacroRecorder:
    """Low-level recorder. Thread-safe; callbacks run on the listener threads."""

    def __init__(
        self,
        *,
        ignore_hwnds: Callable[[], set[int]] | None = None,
        on_step: StepCallback | None = None,
        on_status: StatusCallback | None = None,
        on_stop: Callable[[], None] | None = None,
    ) -> None:
        self._ignore_hwnds = ignore_hwnds or (lambda: set())
        self._on_step = on_step
        self._on_status = on_status
        self._on_stop = on_stop
        self._own = own_process_names()
        self._lock = threading.Lock()
        self._active = False
        self._paused = False
        self._steps: list[Step] = []
        self._last_ts: float | None = None
        self._ctrl = self._shift = self._alt = self._win = False
        self._mouse_listener: mouse.Listener | None = None
        self._key_listener: keyboard.Listener | None = None

    @property
    def active(self) -> bool:
        return self._active

    @property
    def paused(self) -> bool:
        return self._paused

    def steps(self) -> list[Step]:
        with self._lock:
            return list(self._steps)

    def clear(self) -> None:
        with self._lock:
            self._steps.clear()
            self._last_ts = None

    def undo(self) -> Step | None:
        with self._lock:
            if not self._steps:
                return None
            return self._steps.pop()

    def set_paused(self, paused: bool) -> None:
        self._paused = paused
        if self._on_status:
            self._on_status("paused" if paused else "recording")

    def start(self) -> None:
        if self._active:
            return
        self._active = True
        self._paused = False
        self._ctrl = self._shift = self._alt = self._win = False
        self._last_ts = time.monotonic()
        self._mouse_listener = mouse.Listener(on_click=self._on_click)
        self._key_listener = keyboard.Listener(
            on_press=self._on_press,
            on_release=self._on_release,
        )
        # Swallow F10 so stress apps never see it (pynput Windows backend).
        self._key_listener.win32_event_filter = self._win32_event_filter
        self._mouse_listener.start()
        self._key_listener.start()
        if self._on_status:
            self._on_status("recording")

    def stop(self) -> None:
        self._active = False
        if self._mouse_listener is not None:
            self._mouse_listener.stop()
            self._mouse_listener = None
        if self._key_listener is not None:
            self._key_listener.stop()
            self._key_listener = None
        if self._on_status:
            self._on_status("stopped")

    def _dt_ms(self) -> int:
        now = time.monotonic()
        if self._last_ts is None:
            self._last_ts = now
            return 0
        dt = int((now - self._last_ts) * 1000)
        self._last_ts = now
        return max(0, dt)

    def _ignored_point(self, x: int, y: int) -> bool:
        from .windows_util import root_from_point

        hwnd = root_from_point(x, y)
        if hwnd and hwnd in self._ignore_hwnds():
            return True
        return False

    def _append(self, step: Step) -> None:
        with self._lock:
            self._steps.append(step)
        if self._on_step:
            self._on_step(step)

    def _on_click(self, x: int, y: int, button: mouse.Button, pressed: bool) -> None:
        if not self._active or self._paused or not pressed:
            return
        if button not in (mouse.Button.left, mouse.Button.right):
            return
        if self._ignored_point(int(x), int(y)):
            return
        snap = snapshot_at_point(int(x), int(y))
        if snap is None:
            return
        if is_own_process(snap.window.process, self._own) or is_noise_process(snap.window.process):
            return
        if snap.hwnd in self._ignore_hwnds():
            return
        left, top, _, _ = window_rect(snap.hwnd)
        step = Step(
            type="click",
            window=snap.window,
            dt_ms=self._dt_ms(),
            button="right" if button == mouse.Button.right else "left",
            rel_x=int(x) - left,
            rel_y=int(y) - top,
            uia=snap.uia,
        )
        self._append(step)

    def _update_modifier(self, key: keyboard.Key | keyboard.KeyCode, pressed: bool) -> bool:
        if key in (keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r):
            self._ctrl = pressed
            return True
        if key in (keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r):
            self._shift = pressed
            return True
        if key in (keyboard.Key.alt, keyboard.Key.alt_l, keyboard.Key.alt_r, keyboard.Key.alt_gr):
            self._alt = pressed
            return True
        if key in (keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r):
            self._win = pressed
            return True
        return False

    def _win32_event_filter(self, msg: int, data) -> bool:
        """Return False + set suppress to consume F10 while recording."""
        VK_F10 = 0x79
        WM_KEYDOWN = 0x0100
        WM_SYSKEYDOWN = 0x0104
        try:
            if data.vkCode == VK_F10 and msg in (WM_KEYDOWN, WM_SYSKEYDOWN):
                self._key_listener._suppress = True  # type: ignore[attr-defined]
                return False
        except Exception:
            pass
        if self._key_listener is not None:
            self._key_listener._suppress = False  # type: ignore[attr-defined]
        return True

    def _on_press(self, key: keyboard.Key | keyboard.KeyCode) -> None | bool:
        # F10 always stops recording (already swallowed by win32_event_filter).
        if key == keyboard.Key.f10:
            was_active = self._active
            self._active = False
            if was_active and self._on_stop:
                self._on_stop()
            return False  # stop key listener

        if not self._active or self._paused:
            return None
        if self._update_modifier(key, True):
            return None

        snap = snapshot_foreground()
        if snap is None:
            return None
        if is_own_process(snap.window.process, self._own) or is_noise_process(snap.window.process):
            return None
        if snap.hwnd in self._ignore_hwnds():
            return None

        vk, name = _key_identity(key)
        if vk == 0 and not name:
            return None
        step = Step(
            type="key",
            window=snap.window if snap else WindowInfo(),
            dt_ms=self._dt_ms(),
            vk=vk,
            key=name,
            ctrl=self._ctrl,
            shift=self._shift,
            alt=self._alt,
            win=self._win,
        )
        self._append(step)
        return None

    def _on_release(self, key: keyboard.Key | keyboard.KeyCode) -> None:
        self._update_modifier(key, False)


def _key_identity(key: keyboard.Key | keyboard.KeyCode) -> tuple[int, str]:
    """Return (vk, friendly name) for a pynput key."""
    # Special keys
    special = {
        keyboard.Key.esc: (0x1B, "esc"),
        keyboard.Key.enter: (0x0D, "enter"),
        keyboard.Key.tab: (0x09, "tab"),
        keyboard.Key.space: (0x20, "space"),
        keyboard.Key.backspace: (0x08, "backspace"),
        keyboard.Key.delete: (0x2E, "delete"),
        keyboard.Key.up: (0x26, "up"),
        keyboard.Key.down: (0x28, "down"),
        keyboard.Key.left: (0x25, "left"),
        keyboard.Key.right: (0x27, "right"),
        keyboard.Key.home: (0x24, "home"),
        keyboard.Key.end: (0x23, "end"),
        keyboard.Key.page_up: (0x21, "pageup"),
        keyboard.Key.page_down: (0x22, "pagedown"),
        keyboard.Key.insert: (0x2D, "insert"),
        keyboard.Key.f1: (0x70, "f1"),
        keyboard.Key.f2: (0x71, "f2"),
        keyboard.Key.f3: (0x72, "f3"),
        keyboard.Key.f4: (0x73, "f4"),
        keyboard.Key.f5: (0x74, "f5"),
        keyboard.Key.f6: (0x75, "f6"),
        keyboard.Key.f7: (0x76, "f7"),
        keyboard.Key.f8: (0x77, "f8"),
        keyboard.Key.f9: (0x78, "f9"),
        keyboard.Key.f11: (0x7A, "f11"),
        keyboard.Key.f12: (0x7B, "f12"),
    }
    if key in special:
        return special[key]
    if isinstance(key, keyboard.KeyCode):
        vk = int(key.vk or 0)
        char = key.char or ""
        if char:
            return vk or (ord(char.upper()) if len(char) == 1 else 0), char
        return vk, f"vk_{vk}" if vk else ""
    return 0, ""
