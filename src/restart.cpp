#include "restart.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <reason.h>
#include <windows.h>

#include <string>

namespace {

bool enableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    return ok != FALSE && err == ERROR_SUCCESS;
}

bool runShutdownCommand() {
    wchar_t systemDir[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
        return false;
    }

    std::wstring command = std::wstring(systemDir) +
                           L"\\shutdown.exe /r /t 15 /f /c \"Single Update needs to restart to "
                           L"finish installing updates.\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

bool Restart::now() {
    if (runShutdownCommand()) {
        return true;
    }

    enableShutdownPrivilege();

    constexpr DWORD reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                             SHTDN_REASON_MINOR_INSTALLATION |
                             SHTDN_REASON_FLAG_PLANNED;

    wchar_t message[] = L"Single Update needs to restart to finish installing updates.";
    if (InitiateSystemShutdownExW(nullptr, message, 15, TRUE, TRUE, reason)) {
        return true;
    }

    return ExitWindowsEx(EWX_REBOOT | EWX_FORCE, reason) != FALSE;
}
