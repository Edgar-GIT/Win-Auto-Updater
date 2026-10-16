#!/usr/bin/env bash
# Cross-build MacroHelper.exe on Linux via Wine + Windows embeddable Python.
# Output: ../build/MacroHelper.exe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINPY="${WINPY:-$HOME/.wine/drive_c/Python312}"
STAGE="$HOME/.wine/drive_c/src/macro_helper_build"

if [[ ! -x "$WINPY/python.exe" ]]; then
  echo "Windows Python not found at $WINPY"
  echo "Install embeddable CPython 3.12 under Wine first (see py_src/README.md)."
  exit 1
fi

rm -rf "$STAGE"
cp -a "$ROOT/py_src" "$STAGE"
cd "$STAGE"

WINEDEBUG=-all wine "$WINPY/python.exe" -m PyInstaller --noconfirm MacroHelper.spec
mkdir -p "$ROOT/build"
cp -f dist/MacroHelper.exe "$ROOT/build/MacroHelper.exe"
ls -lh "$ROOT/build/MacroHelper.exe"
file "$ROOT/build/MacroHelper.exe"
echo "Done → $ROOT/build/MacroHelper.exe"
