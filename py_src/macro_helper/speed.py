"""PC speed probe → macro pacing (gap between clicks, settle time, window timeout).

How replay is paced now
-----------------------
The recorded delays are NOT replayed any more. They only measured how fast a
person clicked while recording, and replaying them (plus a multi-second pad)
wasted more than a minute per run — while even an old Core i3 handles a click
every 1–2 seconds without trouble.

Replay is driven by the *state of the target window* instead:

1. keep a short minimum gap between two actions (``gap_ms``, about 0.7 s on a
   fast PC, 1 s on a mid-range one, up to 2 s on a very slow one);
2. then actively wait until the target window exists, is enabled and answers
   messages (see ``windows_util.wait_window_ready``) — a slow PC simply waits
   longer here, a fast one does not wait at all;
3. click.

The probe below therefore only has to pick the base gap. It runs in a fraction
of a second. (The previous probe looped 36 million times in Python: it took
several seconds itself and its result was so far above the baseline that every
PC was classified as "slowest".)
"""

from __future__ import annotations

import time
from dataclasses import dataclass

# Reference timing for a mid-range office PC (factor 1.0), measured with the
# bundled CPython 3.12: ~7.5 ms on a Core i7-11850H, ~10 ms on an i5-8500 class
# desktop, ~15 ms on a 2nd/3rd-gen Core i3, 20 ms+ on Celeron/Atom machines.
_CPU_REF_MS = 10.0

GAP_MIN_MS = 600
GAP_MAX_MS = 2000


@dataclass(frozen=True)
class ReplayTiming:
    gap_ms: int  # minimum time between two macro actions
    settle_ms: int  # extra pause after a window has only just appeared
    timeout_s: float  # how long to wait for a missing target window
    ready_timeout_s: float  # how long to wait for a busy window before clicking anyway
    factor: float  # ~0.5 fast … 1.0 mid … 3.0 very slow
    detail: str


def _cpu_ms() -> float:
    """Short single-thread workload; best of 3 (the minimum is the least noisy)."""
    best = float("inf")
    for _ in range(3):
        t0 = time.perf_counter()
        acc = 0
        x = 1
        for i in range(60_000):
            x = (x * 1664525 + 1013904223 + i) & 0xFFFFFFFF
            acc ^= x
        best = min(best, (time.perf_counter() - t0) * 1000.0)
        if acc < 0:  # never — keeps the loop from being optimised away
            raise RuntimeError("unreachable")
    return best


def measure_slowness() -> tuple[float, float]:
    """Return (factor, cpu_ms). factor 1.0 ≈ mid-range office PC.

    Only raw CPU speed is probed: how busy the machine is at any given moment
    is handled live by the active window wait during replay.
    """
    cpu_ms = _cpu_ms()
    return max(0.4, min(3.0, cpu_ms / _CPU_REF_MS)), cpu_ms


def timing_for_factor(factor: float, detail: str = "") -> ReplayTiming:
    """Map a slowness factor to pacing values (also used for manual overrides)."""
    # factor 0.5 → 0.75 s · 1.0 → 1.0 s · 1.5 (old i3) → 1.25 s · 3.0 → 2.0 s
    gap_ms = int(round(500.0 + 500.0 * factor))
    gap_ms = max(GAP_MIN_MS, min(GAP_MAX_MS, gap_ms))
    settle_ms = int(max(250, min(900, gap_ms * 0.45)))
    # Timeouts only cost time when something is really missing, so they can be
    # generous on slow machines without slowing down a normal run.
    timeout_s = round(max(10.0, min(25.0, 8.0 + 5.0 * factor)), 1)
    ready_timeout_s = round(max(2.5, min(8.0, 2.0 + 2.0 * factor)), 1)
    return ReplayTiming(
        gap_ms=gap_ms,
        settle_ms=settle_ms,
        timeout_s=timeout_s,
        ready_timeout_s=ready_timeout_s,
        factor=factor,
        detail=detail,
    )


def probe_replay_timing() -> ReplayTiming:
    factor, cpu_ms = measure_slowness()
    timing = timing_for_factor(factor)
    detail = (
        f"PC speed factor={factor:.2f} (cpu probe={cpu_ms:.1f}ms) -> "
        f"gap={timing.gap_ms}ms settle={timing.settle_ms}ms "
        f"window-timeout={timing.timeout_s:.0f}s (auto, active wait)"
    )
    return ReplayTiming(
        gap_ms=timing.gap_ms,
        settle_ms=timing.settle_ms,
        timeout_s=timing.timeout_s,
        ready_timeout_s=timing.ready_timeout_s,
        factor=factor,
        detail=detail,
    )
