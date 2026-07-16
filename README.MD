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
4. Searches, downloads, and installs Windows Updates (including drivers)  
5. Reboots automatically when required and continues until the system is up to date  
6. Shows a final message, removes the scheduled task, and deletes itself

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

## License

See [LICENSE](LICENSE).
