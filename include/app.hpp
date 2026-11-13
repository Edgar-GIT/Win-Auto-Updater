#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "logger.hpp"
#include "macro.hpp"
#include "ui.hpp"
#include "update.hpp"
#include "updatepicker.hpp"

#include <windows.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class App {
public:
    explicit App(HINSTANCE instance);
    int run();

#ifdef SU_UI_SNAPSHOT
    // Developer aid: renders every screen at several sizes into PNG files.
    int runSnapshot(const std::filesystem::path& directory);
#endif

private:
    enum : UINT {
        WM_APP_STATUS = WM_APP + 1,
        WM_APP_LOG = WM_APP + 2,
        WM_APP_PROGRESS = WM_APP + 3,
        WM_APP_DONE = WM_APP + 4,
        WM_APP_ERROR = WM_APP + 5,
        WM_APP_REBOOT = WM_APP + 6,
        WM_APP_CURRENT = WM_APP + 8,
        WM_APP_STATS = WM_APP + 9,
        WM_APP_STRESS = WM_APP + 10,
        WM_APP_DUMP = WM_APP + 11,
        // WM_APP + 12 is Macro::kStopMessage.
        WM_APP_UNINSTALL_DONE = WM_APP + 13,
        WM_APP_PHASE = WM_APP + 14
    };

    enum class UpdateMode { Full = 0, NoDrivers = 1, DriversOnly = 2 };
    enum class Screen { Home, Working, Done };
    // What the progress ring is showing while the worker runs.
    enum class RingPhase { Preparing, Searching, Downloading, Installing, Checking, Rebooting };

    // A clickable element drawn by the app itself (no child HWNDs): the whole
    // window is one GDI+ surface, which is what makes it scale cleanly.
    struct Widget {
        int id = 0;
        RECT rc{};
        std::wstring text;
        std::wstring sub;
        ui::ButtonStyle style = ui::ButtonStyle::Glass;
        ui::Icon icon = ui::Icon::None;
        bool toggle = false;
        bool visible = false;
        bool enabled = true;
    };

    // Layout is authored for this client size (96-DPI pixels) and scaled.
    static constexpr int kDesignW = 940;
    static constexpr int kDesignH = 632;
    static constexpr UINT_PTR kRebootTimerId = 1;
    static constexpr UINT_PTR kAnimTimerId = 2;
    static constexpr UINT_PTR kRetryTimerId = 3;
    static constexpr int kAnimIntervalMs = 50;
    static constexpr int kAnimFramesPerTurn = 40;
    static constexpr int kMaxRebootCount = 5;
    static constexpr size_t kMaxRecentLog = 4;
    static constexpr int kAutoRetrySeconds = 4;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;

    // ---- rendering state ----
    float scale_ = 1.0f;
    float fitScale_ = 1.0f;  // scale chosen when the window was last fitted to the screen
    int clientW_ = kDesignW;
    int clientH_ = kDesignH;
    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0;
    int backH_ = 0;
    HDC bgDc_ = nullptr;
    HBITMAP bgBitmap_ = nullptr;
    HGDIOBJ bgOld_ = nullptr;
    int bgW_ = 0;
    int bgH_ = 0;
    HICON appIcon_ = nullptr;
    int appIconSize_ = 0;
    int animFrame_ = 0;

    std::vector<Widget> widgets_;
    int hoverId_ = 0;
    int pressedId_ = 0;
    bool trackingMouse_ = false;

    RECT iconRect_{};
    RECT pillRect_{};
    RECT cardLeft_{};
    RECT cardRight_{};
    RECT hintRect_{};
    RECT chipsRect_{};
    RECT heroRect_{};
    RECT ringRect_{};
    RECT statsRect_{};
    RECT activityRect_{};
    RECT detailRect_{};
    RECT footerTextRect_{};
    int detailScroll_ = 0;
    int detailContentH_ = 0;
    bool detailStickBottom_ = false;

    // ---- app state ----
    Logger logger_;
    std::ofstream logFile_;
    std::mutex logMutex_;
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> allowClose_{false};
    std::atomic<bool> uiDoneMode_{false};

    std::wstring lastStatus_ = L"A iniciar...";
    std::wstring lastPhase_ = L"A preparar";
    std::wstring lastCurrent_ = L"À espera de começar...";
    std::deque<std::wstring> recentLog_;

    UpdateStats stats_;    // owned/updated by the worker thread
    UpdateStats uiStats_;  // snapshot rendered by the UI thread

    int lastPercent_ = 0;
    RingPhase ringPhase_ = RingPhase::Preparing;
    unsigned long long workStartMs_ = 0;
    std::thread workerThread_;
    std::thread stressThread_;
    int rebootCountdown_ = 0;
    std::atomic<int> lastPostedPercent_{-1};
    std::atomic<unsigned long long> lastProgressPostMs_{0};

    int runNumber_ = 0;
    bool hadPendingRestart_ = false;
    bool finalVerificationDone_ = false;
    bool cleanupDone_ = false;
    bool choosing_ = true;
    bool updateWithoutDrivers_ = false;
    UpdateMode updateMode_ = UpdateMode::Full;
    // Blocked WU titles for "Atualizar tudo". Persisted in helpers\update_filter.txt
    // so the same filter travels with the USB helpers folder across PCs.
    std::vector<UpdateChoice> updateChoices_;
    std::unordered_set<std::wstring> excludedTitles_;
    // Chooser decision: run the stress bench automatically once the update ends.
    bool autoStressAfterUpdate_ = true;
    bool autoStressStarted_ = false;
    // Chooser decision: the bench that runs after the update is the app's own
    // native tests instead of the tools + macro. Kept in helpers\stress.ini
    // (NativeTests=) so it survives the restarts of an update.
    bool nativeAfterUpdate_ = false;
    // Done-screen decision: keep the logon task so the tool reopens after every
    // restart until it is uninstalled.
    bool reopenAfterRestart_ = true;
    bool stressRunning_ = false;
    bool dumping_ = false;
    bool recording_ = false;
    Macro::Recorder recorder_;
    std::wstring executionType_;
    std::wstring postSummary_;

    // Done screen content.
    bool doneSuccess_ = false;
    bool showDoneStats_ = true;
    int uninstallState_ = 0;  // 0 = no, 1 = running, 2 = finished
    std::wstring doneTitle_;
    std::wstring doneSubtitle_;
    std::wstring doneSummary_;
    std::wstring doneHint_;
    std::wstring stressLog_;

    // Presence of the stress-bench pieces under helpers\ (shown on the home screen).
    bool toolsPresent_ = false;
    bool helperPresent_ = false;
    bool macroPresent_ = false;

    // Automatic "close and reopen" after a failed update. A fresh process fixes
    // most transient Windows Update failures, so it is done once on its own.
    int autoRetryMode_ = -1;        // --auto-retry=<mode>: start that update on launch
    int autoStressOverride_ = -1;   // --auto-stress=<0|1>
    bool retried_ = false;          // --retried: this process already is the retry
    int retryCountdown_ = 0;
    std::wstring pendingRetryError_;

    bool elevateIfNeeded();
    bool ensureInternet();
    void parseCommandLine();
    bool createUi();
    void initWidgets();
    void startWorker();
    void beginUpdate(UpdateMode mode);
    void configureUpdates();
    void syncExcludedFromChoices();
    void saveExcludedTitles() const;
    void loadExcludedTitles();
    void runPostUpdateTasks();
    void runStressTests();
    void openTestTools();
    void launchMonitorTools();
    void launchNativeTests(bool automatic);
    void toggleRecording();
    void finishRecording();
    void applyHint(const std::wstring& hint);
    void refreshHelperStatus();
    void refreshFilterCaption();
    void showGuide();
    bool relaunchSelf(bool markRetried);
    bool scheduleAutoRetry(const std::wstring& message);
    void setStatus(std::wstring message);
    void setPhase(std::wstring message);
    void setCurrent(std::wstring message);
    void setProgressPercent(int percent);
    void setRingPhase(RingPhase phase);
    void openLogFile();
    void writeLogLine(std::wstring_view message);
    void appendLog(std::wstring message);
    void pushRecentLog(std::wstring message);
    void performUninstall();
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);
    void showFinishedScreen(bool success, const std::wstring& detail);
    void requestReboot();
    void logSummary(const std::wstring& result);
    bool shouldPostProgress(int percent) const;
    void postProgress(int percent, std::wstring_view title);

    // ---- UI plumbing ----
    Screen screen() const;
    Widget* widget(int id);
    void setWidgetText(int id, const std::wstring& text);
    void setWidgetEnabled(int id, bool enabled);
    void invalidate(const RECT* rect = nullptr);
    void invalidateWidget(int id);
    int px(float du) const;
    void fitWindowToScreen();
    void layout();
    void layout(int width, int height);
    void layoutHome(int top, int bottom);
    void layoutWorking(int top, int bottom);
    void layoutDone(int top, int bottom);
    void place(int id, int x, int y, int w, int h);
    std::wstring detailText() const;
    std::wstring statusPillText(ui::Color* dot, bool* pulse) const;

    void paint();
    void render(HDC dc, int width, int height, const RECT& clip);
    void ensureSurfaces(HDC reference, int width, int height);
    void releaseSurfaces();
    void drawHeader(ui::Canvas& canvas, const RECT& clip);
    void drawHome(ui::Canvas& canvas, const RECT& clip);
    void drawWorking(ui::Canvas& canvas, const RECT& clip);
    void drawDone(ui::Canvas& canvas, const RECT& clip);
    void drawStats(ui::Canvas& canvas, const RECT& clip);
    void drawWidgets(ui::Canvas& canvas, const RECT& clip);

    int widgetAt(POINT pt) const;
    void onMouseMove(POINT pt);
    void onMouseLeave();
    void onLeftDown(POINT pt);
    void onLeftUp(POINT pt);
    void onMouseWheel(int delta);
    void onCommand(int id);
    void onAnimTick();

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
