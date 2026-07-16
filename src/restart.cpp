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

    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err != ERROR_NOT_ALL_ASSIGNED;
}

bool Restart::now() {
    if (!enablePrivilege()) {
        return false;
    }

    return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                                                            SHTDN_REASON_MINOR_INSTALLATION |
                                                            SHTDN_REASON_FLAG_PLANNED) != FALSE;
}
