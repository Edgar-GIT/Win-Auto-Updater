"""Enable Per-Monitor DPI awareness so coordinates match Win32 GetWindowRect."""

from __future__ import annotations

import sys


def enable_dpi_awareness() -> str:
    """Best-effort DPI awareness. Returns the mode that succeeded."""
    if sys.platform != "win32":
        return "n/a"

    try:
        import ctypes

        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        ctx = ctypes.c_void_p(-4)
        set_ctx = getattr(ctypes.windll.user32, "SetProcessDpiAwarenessContext", None)
        if set_ctx is not None and set_ctx(ctx) != 0:
            return "per_monitor_v2"
    except Exception:
        pass

    try:
        import ctypes

        # PROCESS_PER_MONITOR_DPI_AWARE = 2
        hr = ctypes.windll.shcore.SetProcessDpiAwareness(2)
        if hr == 0:
            return "per_monitor"
    except Exception:
        pass

    try:
        import ctypes

        if ctypes.windll.user32.SetProcessDPIAware() != 0:
            return "system"
    except Exception:
        pass

    return "unaware"
