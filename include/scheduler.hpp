#pragma once

#include <filesystem>
#include <string>

class Scheduler {
public:
    static constexpr const wchar_t* kTaskName = L"WinAutoUpdaterPersist";
    static constexpr const wchar_t* kLegacyTaskName = L"SingleUpdatePersist";

    explicit Scheduler(std::filesystem::path executable);

    // Creates Task Scheduler logon task + RunOnce backup so the app resumes after reboot.
    bool create(std::wstring* error = nullptr) const;
    // Registers a one-shot RunOnce entry so the app opens once on the next sign-in.
    bool armReopen(std::wstring* error = nullptr) const;
    bool remove() const;
    bool exists() const;

private:
    std::filesystem::path executable_;

    bool createTask(std::wstring* error) const;
    bool createRunOnce(std::wstring* error) const;
    bool removeRunOnce() const;
};
