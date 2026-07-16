# Single Update

Native Windows auto-updater written in C++20.  
Cross-compiled with MinGW-w64 (no .NET, Python, Qt, or Electron).

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

### Build (Arch Linux)

```bash
sudo pacman -S cmake mingw-w64-gcc
```

### Run

- Windows 10 / 11 (x64)
- Administrator privileges
- Internet connection

---

## Build (step by step)

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

4. The executable is created at:

```text
build/SingleUpdate.exe
```

---

## Run (step by step)

1. Copy `SingleUpdate.exe` to the Windows machine.
2. Right-click the file and choose **Run as administrator**  
   (or double-click — the app will request elevation).
3. Wait while it searches, downloads, and installs updates.
4. If a reboot is needed, the PC restarts and the app continues automatically.
5. When finished, click **OK** on the final window.  
   The scheduled task is removed and the executable deletes itself.

---

## License

See [LICENSE](LICENSE).
