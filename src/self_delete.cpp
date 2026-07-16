#include "self_delete.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <string>

namespace {

std::filesystem::path tempRoot() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    if (len == 0 || len > MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer) / L"SingleUpdate";
}

}  // namespace

void SelfDelete::cleanTempFiles() {
    const auto root = tempRoot();
    if (root.empty()) {
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        std::filesystem::remove_all(root, ec);
    }
}

bool SelfDelete::schedule(const std::filesystem::path& executable) {
    if (executable.empty()) {
        return false;
    }

    const std::wstring path = executable.wstring();
    const std::wstring params =
        L"/C ping 127.0.0.1 -n 4 >nul & del /F /Q \"" + path + L"\" & rmdir \"" +
        tempRoot().wstring() + L"\" 2>nul";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring command = L"cmd.exe " + params;

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
