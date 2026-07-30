#include "state.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

constexpr const wchar_t* kDirName = L"WinAutoUpdater";

std::filesystem::path appTempDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    if (len == 0 || len > MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer) / kDirName;
}

std::wstring trim(std::wstring_view text) {
    const auto start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(L" \t\r\n");
    return std::wstring(text.substr(start, end - start + 1));
}

std::wstring parseString(std::wstring_view json, std::wstring_view key) {
    const auto keyPos = json.find(key);
    if (keyPos == std::wstring_view::npos) {
        return {};
    }

    const auto colon = json.find(L':', keyPos + key.size());
    if (colon == std::wstring_view::npos) {
        return {};
    }

    const auto afterColon = json.find_first_not_of(L" \t\r\n", colon + 1);
    if (afterColon == std::wstring_view::npos) {
        return {};
    }

    if (json[afterColon] == L'"') {
        const auto quoteEnd = json.find(L'"', afterColon + 1);
        if (quoteEnd == std::wstring_view::npos) {
            return {};
        }
        return std::wstring(json.substr(afterColon + 1, quoteEnd - afterColon - 1));
    }

    return {};
}

int parseInt(std::wstring_view json, std::wstring_view key) {
    const auto keyPos = json.find(key);
    if (keyPos == std::wstring_view::npos) {
        return 0;
    }

    const auto colon = json.find(L':', keyPos + key.size());
    if (colon == std::wstring_view::npos) {
        return 0;
    }

    const auto afterColon = json.find_first_not_of(L" \t\r\n", colon + 1);
    if (afterColon == std::wstring_view::npos) {
        return 0;
    }

    const auto end = json.find_first_of(L",}\r\n", afterColon);
    const auto numStr = trim(json.substr(afterColon, end == std::wstring_view::npos ? std::wstring_view::npos : end - afterColon));
    if (numStr.empty()) {
        return 0;
    }

    wchar_t* endPtr = nullptr;
    const long value = wcstol(numStr.c_str(), &endPtr, 10);
    if (endPtr == numStr.c_str()) {
        return 0;
    }
    return static_cast<int>(value);
}

}  // namespace

std::filesystem::path StateManager::baseDir() {
    return appTempDir();
}

std::filesystem::path StateManager::filePath() {
    return baseDir() / L"state.json";
}

StateManager::StateManager() : filePath_(filePath()) {}

StateManager::State StateManager::load(std::wstring* error) const {
    State state;

    std::error_code ec;
    if (!std::filesystem::exists(filePath_, ec)) {
        return state;
    }

    std::wifstream file(filePath_);
    if (!file.is_open()) {
        if (error) *error = L"Failed to open " + filePath_.wstring() + L" for reading.";
        return state;
    }

    std::wstringstream buffer;
    buffer << file.rdbuf();
    const std::wstring content = buffer.str();

    const auto phase = parseString(content, L"\"phase\"");
    if (!phase.empty()) {
        state.phase = phase;
    }

    state.rebootCount = parseInt(content, L"\"rebootCount\"");

    return state;
}

bool StateManager::save(const State& state, std::wstring* error) const {
    std::error_code ec;
    const auto dir = filePath_.parent_path();
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (error) *error = L"Failed to create directory " + dir.wstring() + L": " +
                                 std::wstring(ec.message().begin(), ec.message().end());
            return false;
        }
    }

    std::wofstream file(filePath_);
    if (!file.is_open()) {
        if (error) *error = L"Failed to open " + filePath_.wstring() + L" for writing.";
        return false;
    }

    file << L"{\n";
    file << L"    \"phase\": \"" << state.phase << L"\",\n";
    file << L"    \"rebootCount\": " << state.rebootCount << L"\n";
    file << L"}\n";

    return true;
}

bool StateManager::remove() const {
    std::error_code ec;
    return std::filesystem::remove(filePath_, ec);
}
