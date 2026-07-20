#include "cleanup.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <filesystem>

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

void Cleanup::removeTempFiles() {
    const auto root = tempRoot();
    if (root.empty()) {
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        std::filesystem::remove_all(root, ec);
    }
}
