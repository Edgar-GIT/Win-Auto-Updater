#pragma once

#include <functional>
#include <string_view>

class UpdateEngine {
public:
    enum class Phase {
        Searching,
        Downloading,
        Installing,
        CheckingAgain,
        UpToDate,
        RebootRequired
    };

    using StatusCallback = std::function<void(Phase, std::wstring_view)>;

    struct Result {
        bool success = false;
        bool rebootRequired = false;
        bool upToDate = false;
        std::wstring message;
    };

    void setStatusCallback(StatusCallback callback);
    Result runCycle();

private:
    StatusCallback callback_;

    void notify(Phase phase, std::wstring_view detail = L"") const;
};
