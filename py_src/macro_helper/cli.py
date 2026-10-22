"""Command-line entry for MacroHelper."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _default_macro_path() -> Path:
    # Prefer Desktop\\macro.json (visible after recording). Fall back to beside the
    # frozen exe / cwd when Desktop cannot be resolved.
    desktop = Path.home() / "Desktop"
    if sys.platform == "win32":
        try:
            import os

            userprofile = os.environ.get("USERPROFILE")
            if userprofile:
                desktop = Path(userprofile) / "Desktop"
        except Exception:
            pass
    if desktop.is_dir():
        return desktop / "macro.json"
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "macro.json"
    return Path.cwd() / "macro.json"


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="MacroHelper",
        description="Record / replay GUI macros for Win-Auto-Updater stress tools.",
    )
    p.add_argument(
        "--macro",
        type=Path,
        default=None,
        help="Path to macro.json (default: next to MacroHelper.exe / cwd)",
    )
    p.add_argument(
        "--record",
        action="store_true",
        help="Open the recording GUI",
    )
    p.add_argument(
        "--play",
        nargs="?",
        const="",
        default=None,
        metavar="FILE",
        help="Replay a macro file (default: --macro / macro.json)",
    )
    p.add_argument(
        "--list",
        nargs="?",
        const="",
        default=None,
        metavar="FILE",
        help="List steps in a macro file",
    )
    p.add_argument(
        "--replay-launch",
        action="store_true",
        help="Before --play, also launch the five Desktop stress tools",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=None,
        metavar="SEC",
        help="Seconds to wait for each target window (default: auto from PC speed probe)",
    )
    p.add_argument(
        "--pad-seconds",
        type=float,
        default=None,
        metavar="SEC",
        help="Extra seconds on top of every recorded gap (default: auto from PC speed probe)",
    )
    p.add_argument(
        "--on-fail",
        choices=("abort", "continue"),
        default="continue",
        help="Behaviour when a step fails (default: continue — keep going for other apps)",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    from .stdio_fix import ensure_stdio, hide_console_window

    ensure_stdio()

    args = build_parser().parse_args(argv)
    macro = Path(args.macro) if args.macro else _default_macro_path()

    # Default action: record GUI (matches SingleUpdate launching --record,
    # and double-click of MacroHelper.exe).
    modes = sum(
        [
            bool(args.record),
            args.play is not None,
            args.list is not None,
        ]
    )
    if modes == 0:
        args.record = True
    if modes > 1:
        print("Use only one of --record / --play / --list.", file=sys.stderr)
        return 2

    if sys.platform != "win32":
        print("MacroHelper only runs on Windows 10/11 x64.", file=sys.stderr)
        return 2

    from .dpi import enable_dpi_awareness

    dpi = enable_dpi_awareness()

    if args.list is not None:
        path = Path(args.list) if args.list else macro
        from .schema import format_list, load_macro

        print(f"DPI awareness: {dpi}", flush=True)
        if not path.exists():
            print(f"Macro file not found: {path}", file=sys.stderr)
            return 1
        doc = load_macro(path)
        print(format_list(doc))
        return 0
    if args.record:
        hide_console_window()
        from .gui import run_record_gui

        return run_record_gui(macro)

    # --play
    path = Path(args.play) if args.play else macro
    if not path.exists():
        print(f"Macro file not found: {path}", file=sys.stderr)
        return 1

    print(f"DPI awareness: {dpi}", flush=True)

    # Sidecar log next to the macro so SingleUpdate / the user can inspect failures
    # even if the stdout pipe is missing (noconsole builds).
    log_path = path.with_suffix(path.suffix + ".log")
    log_lines: list[str] = []

    def _tee(message: str) -> None:
        log_lines.append(message)

    from .replay import launch_stress_tools, play_file
    from .speed import probe_replay_timing

    if args.replay_launch:
        n = launch_stress_tools(log=_tee)
        if n == 0:
            print("No stress tools launched - aborting play.", file=sys.stderr)
            return 1

    timing = probe_replay_timing()
    _tee(timing.detail)
    print(timing.detail, flush=True)

    pad_ms = (
        int(max(0.0, float(args.pad_seconds)) * 1000)
        if args.pad_seconds is not None
        else timing.pad_ms
    )
    timeout_s = float(args.timeout) if args.timeout is not None else timing.timeout_s
    if args.pad_seconds is not None or args.timeout is not None:
        override = f"Overrides: pad={pad_ms}ms timeout={timeout_s:.1f}s"
        _tee(override)
        print(override, flush=True)

    report = play_file(
        path,
        window_timeout_s=timeout_s,
        pad_ms=pad_ms,
        on_fail=args.on_fail,
        log=_tee,
    )
    try:
        log_path.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
        print(f"Wrote replay log: {log_path}", flush=True)
    except Exception as exc:
        print(f"Could not write replay log: {exc}", flush=True)

    if report.total == 0:
        return 1
    return 0 if report.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
