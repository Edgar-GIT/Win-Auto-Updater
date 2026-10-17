#!/usr/bin/env bash
# Cross-build MacroHelper.exe on Linux via Wine + Windows embeddable Python.
# Output: ../build/MacroHelper.exe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer full Python with tcl/tk (embeddable builds lack tkinter).
WINPY="${WINPY:-$HOME/.wine/drive_c/Python312Full}"
if [[ ! -x "$WINPY/python.exe" ]]; then
  WINPY="$HOME/.wine/drive_c/Python312"
fi
STAGE="$HOME/.wine/drive_c/src/macro_helper_build"

if [[ ! -x "$WINPY/python.exe" ]]; then
  echo "Windows Python not found at $WINPY"
  echo "Install full CPython 3.12 (with tcl/tk) under Wine first."
  exit 1
fi
WINEDEBUG=-all wine "$WINPY/python.exe" -c "import tkinter" 2>/dev/null \
  || { echo "ERROR: this Python has no tkinter — use a full install (Include_tcltk=1)."; exit 1; }

rm -rf "$STAGE"
cp -a "$ROOT/py_src" "$STAGE"
cd "$STAGE"

WINEDEBUG=-all wine "$WINPY/python.exe" -m PyInstaller --noconfirm MacroHelper.spec
mkdir -p "$ROOT/build"
cp -f dist/MacroHelper.exe "$ROOT/build/MacroHelper.exe"
ls -lh "$ROOT/build/MacroHelper.exe"
file "$ROOT/build/MacroHelper.exe"
echo "Done → $ROOT/build/MacroHelper.exe"
