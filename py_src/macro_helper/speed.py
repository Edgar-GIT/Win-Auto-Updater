"""Lightweight PC speed probe → adaptive macro pad / window timeout."""

from __future__ import annotations

import os
import tempfile
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class ReplayTiming:
    pad_ms: int
    timeout_s: float
    factor: float  # 0.25 (muito rápido) … 1.0 (baseline) … 1.25 (lento)
    detail: str


def _cpu_ms() -> float:
    t0 = time.perf_counter()
    acc = 0
    for i in range(2_500_000):
        acc += (i * i) & 0xFFFF
    # Keep the work from being optimized away.
    if acc < 0:
        raise RuntimeError("unreachable")
    return (time.perf_counter() - t0) * 1000.0


def _enum_ms() -> float:
    if os.name != "nt":
        return 40.0
    try:
        import win32gui

        def _cb(_hwnd, _):
            return True

        t0 = time.perf_counter()
        for _ in range(25):
            win32gui.EnumWindows(_cb, None)
        return (time.perf_counter() - t0) * 1000.0
    except Exception:
        return 40.0


def _disk_ms() -> float:
    payload = b"x" * (256 * 1024)
    t0 = time.perf_counter()
    try:
        fd, path = tempfile.mkstemp(prefix="macro_speed_")
        try:
            os.write(fd, payload)
            os.fsync(fd)
        finally:
            os.close(fd)
            try:
                os.remove(path)
            except OSError:
                pass
    except OSError:
        return 20.0
    return (time.perf_counter() - t0) * 1000.0


def measure_slowness() -> float:
    """Return a slowness factor relative to a mid-range Win10/11 PC.

    1.0 ≈ baseline (pad 4s / timeout 15s). Lower = faster machine.
    """
    # Baselines measured against a typical office desktop.
    cpu_ratio = _cpu_ms() / 90.0
    enum_ratio = _enum_ms() / 45.0
    disk_ratio = _disk_ms() / 18.0
    raw = 0.45 * cpu_ratio + 0.35 * enum_ratio + 0.20 * disk_ratio
    return max(0.25, min(1.25, raw))


def probe_replay_timing() -> ReplayTiming:
    """Map slowness → pad (0.6–4.0s) and window timeout (5–15s)."""
    factor = measure_slowness()
    # Fast PC (0.25) → ~0.6s pad / 5s timeout; slow (1.0+) → ~4s / 15s.
    pad_ms = int(round(600.0 + factor * 3400.0))
    timeout_s = round(5.0 + factor * 10.0, 1)
    pad_ms = max(500, min(4500, pad_ms))
    timeout_s = max(5.0, min(18.0, timeout_s))
    detail = (
        f"PC speed factor={factor:.2f} -> pad={pad_ms}ms, "
        f"timeout={timeout_s:.1f}s (auto)"
    )
    return ReplayTiming(pad_ms=pad_ms, timeout_s=timeout_s, factor=factor, detail=detail)
