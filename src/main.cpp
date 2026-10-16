#include "../include/app.hpp"

#include <windows.h>

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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (alreadyRunning()) {
        return 0;
    }
    App app(instance);
    return app.run();
}
