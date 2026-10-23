#include "cleanup.hpp"
#include "state.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path desktopDir() {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        result = raw;
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    if (result.empty()) {
        wchar_t buffer[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH) > 0) {
            result = std::filesystem::path(buffer) / L"Desktop";
        }
    }
    return result;
}

void deleteNowOrOnReboot(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    if (std::filesystem::is_directory(path, ec)) {
        std::filesystem::remove_all(path, ec);
        if (!ec) {
            return;
        }
        for (std::filesystem::recursive_directory_iterator it(path, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                const auto file = it->path();
                if (!DeleteFileW(file.wstring().c_str())) {
                    MoveFileExW(file.wstring().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                }
            }
        }
        std::filesystem::remove_all(path, ec);
        return;
    }

    if (DeleteFileW(path.wstring().c_str())) {
        return;
    }
    MoveFileExW(path.wstring().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

void deleteIfExists(const std::filesystem::path& path) {
    deleteNowOrOnReboot(path);
}

}  // namespace

void Cleanup::removeTempFiles() {
    const auto root = StateManager::baseDir();
    if (root.empty()) {
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        std::filesystem::remove_all(root, ec);
    }
}

void Cleanup::removeBundle(const std::filesystem::path& executable) {
    // Canonical layout: Desktop\helpers\ (tools, macro, MacroHelper, ini, …).
    if (!executable.empty()) {
        deleteIfExists(executable.parent_path() / L"helpers");
    }

    // Legacy locations from older deploys (best-effort).
    const auto desktop = desktopDir();
    if (!desktop.empty()) {
        deleteIfExists(desktop / L"programas de teste");
        deleteIfExists(desktop / L"macro.json");
        deleteIfExists(desktop / L"macro.json.log");
        // nvidia_app.exe / amd_drivers.exe on the Desktop are kept on purpose.
    }
    if (!executable.empty()) {
        const auto dir = executable.parent_path();
        deleteIfExists(dir / L"MacroHelper.exe");
        deleteIfExists(dir / L"stress.ini");
        deleteIfExists(dir / L"uia_stress.ps1");
        deleteIfExists(dir / L"macro.json");
        deleteIfExists(dir / L"macro.json.log");
        deleteIfExists(dir / L"macro.txt");
    }
}
