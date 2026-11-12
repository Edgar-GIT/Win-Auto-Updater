#include "../include/app.hpp"

#include "../native_tests/native_tests.hpp"

#include <shellapi.h>
#include <windows.h>

#include <string>

namespace {

constexpr const wchar_t* kInstanceMutex = L"WinAutoUpdaterSingleInstance";
constexpr const wchar_t* kWindowClass = L"WinAutoUpdaterWindow";

// A persistent logon task and a "Close & reopen" RunOnce entry can both fire on
// the same sign-in; make sure only ever one window exists.
bool alreadyRunning() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex == nullptr) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Re-focus the window of the instance that is already up.
        if (HWND window = FindWindowW(kWindowClass, nullptr)) {
            if (IsIconic(window)) {
                ShowWindow(window, SW_RESTORE);
            }
            SetForegroundWindow(window);
        }
        CloseHandle(mutex);
        return true;
    }
    // Keep the mutex alive for the process lifetime (never ReleaseMutex).
    return false;
}

// "--wait-pid=<pid>": this copy was started by an instance that is closing so
// it can reopen itself (automatic retry after a failed update). Wait for that
// instance to exit first, otherwise the single-instance check would reject us.
void waitForPreviousInstance(int argc, wchar_t** argv) {
    const std::wstring prefix = L"--wait-pid=";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(prefix, 0) != 0) {
            continue;
        }
        DWORD pid = 0;
        try {
            pid = static_cast<DWORD>(std::stoul(arg.substr(prefix.size())));
        } catch (...) {
            pid = 0;
        }
        if (pid == 0) {
            return;
        }
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
        // Give the old instance's COM / Windows Update handles a moment to go.
        Sleep(700);
        return;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

#ifdef SU_UI_SNAPSHOT
    if (argv != nullptr && argc >= 3 && std::wstring(argv[1]) == L"--ui-snapshot") {
        App snapshot(instance);
        return snapshot.runSnapshot(argv[2]);
    }
#endif

#ifdef SU_UI_SNAPSHOT
    if (argv != nullptr && argc >= 3 && std::wstring(argv[1]) == L"--native-selftest") {
        return nt::runNativeSelfTest(argv[2]);
    }
    if (argv != nullptr && argc >= 3 && std::wstring(argv[1]) == L"--gpu-snapshot") {
        return nt::runGpuSnapshot(argv[2]);
    }
    if (argv != nullptr && argc >= 3 && std::wstring(argv[1]) == L"--native-snapshot") {
        return nt::runNativeSnapshot(instance, argv[2]);
    }
#endif

    // "--native-tests [--autostart=<minutes>]": the native test bench (beta). It is
    // its own window in its own process, independent from the updater instance.
    if (argv != nullptr) {
        bool nativeTests = false;
        nt::LaunchOptions nativeOptions;
        const std::wstring autostart = L"--autostart=";
        const std::wstring autostartSeconds = L"--autostart-seconds=";
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--native-tests") {
                nativeTests = true;
            } else if (arg.rfind(autostartSeconds, 0) == 0) {
                try {
                    nativeOptions.autostartSecondsOverride = std::stod(arg.substr(autostartSeconds.size()));
                } catch (...) {
                    nativeOptions.autostartSecondsOverride = 0.0;
                }
            } else if (arg.rfind(autostart, 0) == 0) {
                try {
                    nativeOptions.autostartMinutes = std::stoi(arg.substr(autostart.size()));
                } catch (...) {
                    nativeOptions.autostartMinutes = 0;
                }
            }
        }
        if (nativeTests) {
            LocalFree(argv);
            return nt::runNativeTests(instance, nativeOptions);
        }
    }

    if (argv != nullptr) {
        waitForPreviousInstance(argc, argv);
        LocalFree(argv);
    }

    if (alreadyRunning()) {
        return 0;
    }
    App app(instance);
    return app.run();
}
