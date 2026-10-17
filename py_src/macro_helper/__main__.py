"""python -m macro_helper entry (dev). Frozen builds use run_macro_helper.py."""

try:
    from .cli import main
except ImportError:
    from macro_helper.cli import main

raise SystemExit(main())
