#include "cleanup.hpp"
#include "state.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <filesystem>

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
