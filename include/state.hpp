#pragma once

#include <filesystem>
#include <string>

class StateManager {
public:
    struct State {
        std::wstring phase = L"installing";
        int rebootCount = 0;
    };

    StateManager();

    State load() const;
    bool save(const State& state) const;
    bool remove() const;

    static std::filesystem::path filePath();

private:
    std::filesystem::path filePath_;
};
