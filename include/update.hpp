#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "com_ptr.hpp"

struct IUpdate;
struct IUpdateCollection;
struct IUpdateSession;

// Live, cumulative counters for the whole update session. Reported to the UI
// after every meaningful step so the on-screen numbers move in real time
// instead of only updating once a full cycle has completed.
struct UpdateStats {
    int found = 0;
    int downloaded = 0;
    int installed = 0;
    int skipped = 0;
    int failed = 0;
    int finished = 0;  // installed + skipped + failed (items fully processed)
};

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
    using StatsCallback = std::function<void(const UpdateStats&)>;

    struct Result {
        bool success = false;
        bool rebootRequired = false;
        bool pendingRestart = false;
        bool upToDate = false;
        bool hadFailures = false;
        int updatesFound = 0;
        int updatesDownloaded = 0;
        int updatesInstalled = 0;
        int updatesFailed = 0;
        int updatesSkipped = 0;
        bool finalVerificationDone = false;
        std::wstring message;
    };

    void setStatusCallback(StatusCallback callback);
    void setLogCallback(LogCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setStatsCallback(StatsCallback callback);
    Result runCycle();

    struct StepOutcome {
        bool ok = false;
        bool rebootRequired = false;
        std::wstring detail;
    };

    const UpdateStats& totals() const {
        return totals_;
    }

private:
    StatusCallback statusCallback_;
    LogCallback logCallback_;
    ProgressCallback progressCallback_;
    StatsCallback statsCallback_;

    UpdateStats totals_;
    bool avNoticeShown_ = false;

    void notify(Phase phase, std::wstring_view detail = L"") const;
    void log(std::wstring_view message) const;
    void progress(std::wstring_view title, int percent) const;
    void notifyStats() const;
    void refreshFinished();

    StepOutcome downloadOne(IUpdateSession* session, IUpdate* update, IUpdateCollection* searchResults,
                            LONG updateIndex, std::wstring_view title) const;
    StepOutcome installOne(IUpdateSession* session, IUpdate* update, IUpdateCollection* searchResults,
                           LONG updateIndex, std::wstring_view title) const;
    bool hasPendingRestart(IUpdateSession* session) const;
    ComPtr<IUpdateCollection> makeSingleCollection(IUpdate* update) const;
    ComPtr<IUpdateCollection> makeCollectionFallback(IUpdateCollection* searchResults, LONG updateIndex) const;
};
