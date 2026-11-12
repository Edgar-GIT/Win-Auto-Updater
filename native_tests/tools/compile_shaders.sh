#!/usr/bin/env bash
# Regenerates native_tests/gpu_shader_bytecode.inc from the HLSL in gpu_shaders.hpp.
#
# The compiler tool is cross-built with MinGW and run under Wine. Pass the folder
# that holds Microsoft's 64-bit d3dcompiler_47.dll (the same file winetricks
# installs) so the bytecode comes from the reference compiler:
#
#   native_tests/tools/compile_shaders.sh /path/to/dir-with-d3dcompiler_47
#
# Without an argument Wine's built-in HLSL compiler is used (fine for a quick
# check, but ship blobs produced by the Microsoft compiler).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../gpu_shader_bytecode.inc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

x86_64-w64-mingw32-g++ -std=c++20 -O1 -static -o "$WORK/compile_shaders.exe" \
    "$HERE/compile_shaders.cpp"

OVERRIDE=""
if [[ $# -ge 1 ]]; then
  cp "$1/d3dcompiler_47.dll" "$WORK/d3dcompiler_47.dll"
  OVERRIDE="d3dcompiler_47=n"
fi

cd "$WORK"
WINEDEBUG=-all WINEDLLOVERRIDES="$OVERRIDE" wine compile_shaders.exe "Z:$WORK/out.inc"
cp "$WORK/out.inc" "$OUT"
echo "Updated $OUT"
