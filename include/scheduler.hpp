#pragma once

#include <filesystem>
#include <string>

class Scheduler {
public:
    static constexpr const wchar_t* kTaskName = L"SingleUpdatePersist";

    explicit Scheduler(std::filesystem::path executable);

    bool create() const;
    bool remove() const;
    bool exists() const;

private:
    std::filesystem::path executable_;
};
