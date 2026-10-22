"""Enable Per-Monitor DPI awareness so coordinates match Win32 GetWindowRect."""

from __future__ import annotations

import sys


def enable_dpi_awareness() -> str:
    """Best-effort DPI awareness (Win10 + Win11). Returns the mode that succeeded."""
    if sys.platform != "win32":
        return "n/a"

    try:
        import ctypes

        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4
        # Available since Windows 10 1703; fails cleanly on older builds.
        set_ctx = getattr(ctypes.windll.user32, "SetProcessDpiAwarenessContext", None)
        if set_ctx is not None:
            # Use ctypes.c_ssize_t(-4) — void* cast of -4 is the documented context.
            if set_ctx(ctypes.c_void_p(-4)) != 0:
                return "per_monitor_v2"
    except Exception:
        pass

    try:
        import ctypes

        # PROCESS_PER_MONITOR_DPI_AWARE = 2 (Windows 8.1+)
        set_aware = getattr(ctypes.windll.shcore, "SetProcessDpiAwareness", None)
        if set_aware is not None:
            hr = int(set_aware(2)) & 0xFFFFFFFF
            # S_OK = 0, E_ACCESSDENIED = already set by the process
            if hr in (0, 0x80070005):
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
