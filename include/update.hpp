#pragma once

#include <functional>
#include <string>
#include <string_view>

struct IUpdate;
struct IUpdateSession;

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
    using LogCallback = std::function<void(std::wstring_view)>;
    using ProgressCallback = std::function<void(std::wstring_view title, int percent)>;

    struct Result {
        bool success = false;
        bool rebootRequired = false;
        bool upToDate = false;
        bool hadFailures = false;
        std::wstring message;
    };

    void setStatusCallback(StatusCallback callback);
    void setLogCallback(LogCallback callback);
    void setProgressCallback(ProgressCallback callback);
    Result runCycle();

private:
    struct StepOutcome {
        bool ok = false;
        bool rebootRequired = false;
        std::wstring detail;
    };

    StatusCallback statusCallback_;
    LogCallback logCallback_;
    ProgressCallback progressCallback_;

    void notify(Phase phase, std::wstring_view detail = L"") const;
    void log(std::wstring_view message) const;
    void progress(std::wstring_view title, int percent) const;

    StepOutcome downloadOne(IUpdateSession* session, IUpdate* update, std::wstring_view title) const;
    StepOutcome installOne(IUpdateSession* session, IUpdate* update, std::wstring_view title) const;
};
