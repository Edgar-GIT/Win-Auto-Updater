#include "restart.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <reason.h>
#include <windows.h>

bool Restart::enablePrivilege() {
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

bool Restart::now() {
    enablePrivilege();

    constexpr DWORD reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                             SHTDN_REASON_MINOR_INSTALLATION |
                             SHTDN_REASON_FLAG_PLANNED;

    wchar_t message[] = L"Single Update requires a restart to continue installing updates.";
    if (InitiateSystemShutdownExW(nullptr, message, 5, TRUE, TRUE, reason)) {
        return true;
    }

    if (ExitWindowsEx(EWX_REBOOT | EWX_FORCE | EWX_FORCEIFHUNG, reason)) {
        return true;
    }

    return InitiateSystemShutdownW(nullptr, message, 0, TRUE, TRUE) != FALSE;
}
