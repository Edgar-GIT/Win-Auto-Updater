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

}  // namespace

bool Restart::now() {
    enableShutdownPrivilege();

    constexpr DWORD reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                             SHTDN_REASON_MINOR_INSTALLATION |
                             SHTDN_REASON_FLAG_PLANNED;

    wchar_t message[] = L"Win Auto Updater needs to restart to finish installing updates.";
    if (InitiateSystemShutdownExW(nullptr, message, 15, TRUE, TRUE, reason)) {
        return true;
    }

    return ExitWindowsEx(EWX_REBOOT | EWX_FORCE, reason) != FALSE;
}
