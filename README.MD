# Single Update

Native Windows auto-updater written in C++20. Cross-compiled with MinGW-w64 from Linux.

## Build

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake
cmake --build build -j
```

Output: `build/SingleUpdate.exe`
