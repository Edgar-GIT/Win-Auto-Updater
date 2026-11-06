"""PC speed probe → adaptive macro gap scale / pad / window timeout.

Previously only a flat pad was added on top of the full recorded gaps, so a fast
PC and a slow PC felt almost identical (e.g. 6.5s recorded + 2s vs 4s pad).

Now we also *scale* the recorded gaps: fast machines replay quicker, slow ones
keep more of the original pacing plus a larger pad.
"""

from __future__ import annotations

import os
import statistics
import tempfile
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class ReplayTiming:
    pad_ms: int
    timeout_s: float
    factor: float  # ~0.25 (rápido) … 1.0 (médio) … 1.6 (lento)
    gap_scale: float  # multiplies each recorded dt_ms
    detail: str


def _median(samples: list[float]) -> float:
    if not samples:
        return 0.0
    return float(statistics.median(samples))


def _cpu_ms() -> float:
    """Heavier single-thread loop — short loops all finished in ~same time on modern CPUs."""
    samples: list[float] = []
    for _ in range(3):
        t0 = time.perf_counter()
        acc = 0
        x = 1
        for i in range(12_000_000):
            x = (x * 1664525 + 1013904223 + i) & 0xFFFFFFFF
            acc ^= x
            if i & 0xFF == 0:
                acc = (acc + (i * i)) & 0xFFFFFFFF
        if acc == 0xDEADBEEF:  # never — keep work live
            raise RuntimeError("unreachable")
        samples.append((time.perf_counter() - t0) * 1000.0)
    return _median(samples)


def _enum_ms() -> float:
    if os.name != "nt":
        return 50.0
    try:
        import win32gui

        def _cb(_hwnd, _extra):
            return True

        samples: list[float] = []
        for _ in range(3):
            t0 = time.perf_counter()
            for _ in range(40):
                win32gui.EnumWindows(_cb, None)
            samples.append((time.perf_counter() - t0) * 1000.0)
        return _median(samples)
    except Exception:
        return 50.0


def _disk_ms() -> float:
    payload = b"x" * (1024 * 1024)  # 1 MiB — SSDs were all ~same at 256 KiB
    samples: list[float] = []
    for _ in range(3):
        t0 = time.perf_counter()
        try:
            fd, path = tempfile.mkstemp(prefix="macro_speed_")
            try:
                os.write(fd, payload)
                os.fsync(fd)
                os.lseek(fd, 0, os.SEEK_SET)
                _ = os.read(fd, len(payload))
            finally:
                os.close(fd)
                try:
                    os.remove(path)
                except OSError:
                    pass
        except OSError:
            return 25.0
        samples.append((time.perf_counter() - t0) * 1000.0)
    return _median(samples)


def measure_slowness() -> tuple[float, float, float, float]:
    """Return (factor, cpu_ms, enum_ms, disk_ms).

    factor 1.0 ≈ mid-range office PC. Lower = faster.
    """
    # Baselines from a typical Win11 office desktop (re-tuned for heavier probes).
    cpu_ms = _cpu_ms()
    enum_ms = _enum_ms()
    disk_ms = _disk_ms()
    cpu_ratio = cpu_ms / 220.0
    enum_ratio = enum_ms / 70.0
    disk_ratio = disk_ms / 35.0
    # CPU dominates click/UI feel; enum approximates desktop HWND cost.
    raw = 0.55 * cpu_ratio + 0.30 * enum_ratio + 0.15 * disk_ratio
    factor = max(0.22, min(1.60, raw))
    return factor, cpu_ms, enum_ms, disk_ms


def probe_replay_timing() -> ReplayTiming:
    """Map slowness → gap_scale, pad, and window timeout."""
    factor, cpu_ms, enum_ms, disk_ms = measure_slowness()

    # Fast (0.22) → ~0.35× recorded gaps + ~0.5s pad
    # Mid  (1.00) → ~1.00× recorded gaps + ~2.0s pad
    # Slow (1.60) → ~1.30× recorded gaps + ~3.8s pad
    gap_scale = max(0.30, min(1.35, 0.12 + factor * 0.88))
    pad_ms = int(round(250.0 + factor * 2200.0))
    timeout_s = round(4.0 + factor * 10.0, 1)

    pad_ms = max(200, min(4000, pad_ms))
    timeout_s = max(4.0, min(20.0, timeout_s))

    detail = (
        f"PC speed factor={factor:.2f} (cpu={cpu_ms:.0f}ms enum={enum_ms:.0f}ms "
        f"disk={disk_ms:.0f}ms) -> gap_scale={gap_scale:.2f}x, pad={pad_ms}ms, "
        f"timeout={timeout_s:.1f}s (auto)"
    )
    return ReplayTiming(
        pad_ms=pad_ms,
        timeout_s=timeout_s,
        factor=factor,
        gap_scale=gap_scale,
        detail=detail,
    )
