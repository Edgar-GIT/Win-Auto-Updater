# Single Update

Native Windows auto-updater written in C++20.  
Built with MinGW-w64 and CMake (no .NET, Python, Qt, or Electron).

**Author:** [Edgar-GIT](https://github.com/Edgar-GIT)  
**Repository:** [Win-Auto-Updater](https://github.com/Edgar-GIT/Win-Auto-Updater)

---

## What it does

1. Requests administrator privileges  
2. Checks Internet connectivity  
3. Creates a scheduled task to resume after reboot  
4. Searches, downloads, and installs Windows Updates (including drivers and
   Microsoft Defender updates — definitions fall back to `MpCmdRun -SignatureUpdate`)  
5. Reboots automatically when required and continues until the system is up to date  
6. Shows a final message, removes the scheduled task, and deletes itself

If an update run fails, the tool closes and reopens itself once
(`--wait-pid/--auto-retry/--retried`) and retries; only a second failure shows
the error screen (with a manual **Tentar novamente** button). The full log is
appended to `%TEMP%\WinAutoUpdater\updater.log`.

### Interface

Single GDI+ surface (`src/ui.cpp`, no child controls): dark glass theme, per-monitor
DPI aware, resizable. The layout is authored at 940×580 and scaled to the window,
and the window is fitted to the monitor work area at startup and whenever the
display mode changes — it works from 800×600 up to 4K at any Windows scaling.

Developer aid: configure with `-DSU_UI_SNAPSHOT=ON` and run
`SingleUpdate.exe --ui-snapshot <dir>` to render every screen to PNG.

---

## Requirements

### Run

- Windows 10 / 11 (x64)
- Administrator privileges
- Internet connection

### Build on Arch Linux

```bash
sudo pacman -S cmake mingw-w64-gcc
```

### Build on Windows

Install [MSYS2](https://www.msys2.org/), then in an **MSYS2 UCRT64** terminal:

```bash
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc make git
```

---

## Build on Arch Linux (cross-compile)

1. Clone the repository:

```bash
git clone https://github.com/Edgar-GIT/Win-Auto-Updater.git
cd Win-Auto-Updater
```

2. Configure CMake with the MinGW toolchain:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake
```

3. Compile:

```bash
cmake --build build -j
```

4. Output:

```text
build/SingleUpdate.exe
```

---

## Build on Windows

1. Open an **MSYS2 UCRT64** terminal.

2. Clone the repository:

```bash
git clone https://github.com/Edgar-GIT/Win-Auto-Updater.git
cd Win-Auto-Updater
```

3. Configure CMake (native MinGW, no toolchain file):

```bash
cmake -S . -B build -G "MinGW Makefiles"
```

4. Compile:

```bash
cmake --build build -j
```

5. Output:

```text
build/SingleUpdate.exe
```

---

## Run (step by step)

1. Copy `SingleUpdate.exe` to the Windows machine (if you built on Linux).
2. Right-click the file and choose **Run as administrator**  
   (or double-click — the app will request elevation).
3. Wait while it searches, downloads, and installs updates.
4. If a reboot is needed, the PC restarts and the app continues automatically.
5. When finished, click **OK** on the final window.  
   The scheduled task is removed and the executable deletes itself.

---

## Stress macros (MacroHelper)

GUI recording / replay for the hardware bench lives in `py_src/` (Python).  
`build/MacroHelper.exe` can be cross-built from Linux with Wine + Windows Python
(`py_src/build_exe_wine.sh`) or natively on Windows (`py_src/build_exe.bat`).

Deploy on the USB stick: `SingleUpdate.exe` + `MacroHelper.exe` (+ `macro.json`
after recording). The folder `Desktop\programas de teste\` must exist on the PC.

Buttons: **Open test tools** (launch only, MemTest on top) · **Run stress tests**
(launch + replay macro) · **Record macro**. Details: [`py_src/README.md`](py_src/README.md).

`stress.ini`: `MacroEngine=python` (default), `MacroFile=macro.json`, `UseMacro=1`.  
`MacroGapMs=0` (default) lets MacroHelper pace the clicks from a quick CPU probe
(≈0.7 s on a fast PC … 2 s on a very slow one) plus an active wait for each target
window; set e.g. `MacroGapMs=1000` to force a fixed gap.  
Set `MacroEngine=cpp` for the legacy in-process recorder (`macro.txt`).

---

## License

See [LICENSE](LICENSE).
