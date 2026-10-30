#pragma once

#include <filesystem>
#include <string>

class StateManager {
public:
    struct State {
        std::wstring phase = L"installing";
        int rebootCount = 0;
        bool withoutDrivers = false;
        // 0 = Full, 1 = NoDrivers, 2 = DriversOnly (matches App::UpdateMode).
        int updateMode = 0;
    };

    StateManager();

    State load(std::wstring* error = nullptr) const;
    bool save(const State& state, std::wstring* error = nullptr) const;
    bool remove() const;

    static std::filesystem::path baseDir();
    static std::filesystem::path filePath();

private:
    std::filesystem::path filePath_;
};
