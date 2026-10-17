"""PyInstaller entry point (absolute imports — required when frozen)."""

from macro_helper.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
