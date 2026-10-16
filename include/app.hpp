#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "logger.hpp"
#include "macro.hpp"
#include "update.hpp"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

class App {
public:
    explicit App(HINSTANCE instance);
    int run();

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
        WM_APP_DUMP = WM_APP + 11
    };

    static constexpr int kWidth = 780;
    static constexpr int kHeight = 660;
    static constexpr int kButtonHeight = 40;
    static constexpr int kButtonWidth = 170;
    static constexpr int kHeaderHeight = 100;
    static constexpr int kStatCardCount = 6;
    static constexpr UINT_PTR kRebootTimerId = 1;
    static constexpr UINT_PTR kAnimTimerId = 2;
    static constexpr int kAnimIntervalMs = 40;
    static constexpr int kMaxTerminalChars = 24000;
    static constexpr int kMaxRebootCount = 5;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;

    HWND headerTitle_ = nullptr;
    HWND headerSubtitle_ = nullptr;
    HWND phaseLabel_ = nullptr;
    HWND currentLabel_ = nullptr;
    HWND progressBar_ = nullptr;
    HWND terminal_ = nullptr;
    HWND summaryLabel_ = nullptr;
    HWND uninstallButton_ = nullptr;
    HWND closeButton_ = nullptr;
    HWND updateButton_ = nullptr;
    HWND updateNoDriversButton_ = nullptr;
    HWND runTestsButton_ = nullptr;
    HWND closeReopenButton_ = nullptr;
    HWND dumpButton_ = nullptr;
    HWND recordButton_ = nullptr;
    HWND stressToggleButton_ = nullptr;
    HWND reopenToggleButton_ = nullptr;
    HWND monitorButton_ = nullptr;

    HFONT titleFont_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT terminalFont_ = nullptr;
    HFONT numberFont_ = nullptr;
    HFONT captionFont_ = nullptr;
    HFONT buttonFont_ = nullptr;

    HBRUSH bgBrush_ = nullptr;
    HBRUSH terminalBrush_ = nullptr;
    HBRUSH headerBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HPEN borderPen_ = nullptr;
    HICON appIcon_ = nullptr;

    RECT statsRect_{};
    RECT terminalCardRect_{};
    RECT summaryCardRect_{};
    RECT spinnerRect_{};
    int animFrame_ = 0;

    Logger logger_;
    std::wofstream logFile_;
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> allowClose_{false};
    std::atomic<bool> uiDoneMode_{false};

    std::wstring lastStatus_ = L"Starting...";
    std::wstring lastPhase_ = L"Preparing";
    std::wstring lastCurrent_ = L"Waiting to begin...";

    UpdateStats stats_;    // owned/updated by the worker thread
    UpdateStats uiStats_;  // snapshot rendered by the UI thread

    int lastPercent_ = 0;
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
    // Chooser decision: run the stress bench automatically once the update ends.
    bool autoStressAfterUpdate_ = true;
    bool autoStressStarted_ = false;
    // Done-screen decision: keep the logon task so the tool reopens after every
    // restart until it is uninstalled.
    bool reopenAfterRestart_ = true;
    bool stressRunning_ = false;
    bool dumping_ = false;
    bool recording_ = false;
    Macro::Recorder recorder_;
    std::wstring executionType_;
    std::wstring postSummary_;
    std::wstring doneSummary_;
    std::wstring stressLog_;

    bool elevateIfNeeded();
    bool ensureInternet();
    bool createUi();
    void layoutControls();
    void startWorker();
    void beginUpdate(bool withoutDrivers);
    void runPostUpdateTasks();
    void runStressTests();
    void runUiDump();
    void toggleRecording();
    void finishRecording();
    void applyHint(const std::wstring& hint);
    void updateStressToggleText();
    void updateReopenToggleText();
    void closeAndReopen();
    void setStatus(std::wstring message);
    void setPhase(std::wstring message);
    void setCurrent(std::wstring message);
    void setProgressPercent(int percent);
    void appendLog(std::wstring message);
    void trimTerminalIfNeeded();
    void performUninstall();
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);
    void showFinishedScreen(bool success, const std::wstring& detail);
    void requestReboot();
    void logSummary(const std::wstring& result);
    void showActionButtons(bool enabled);
    bool shouldPostProgress(int percent) const;
    void postProgress(int percent, std::wstring_view title);

    void paint();
    void paintHeader(HDC hdc, int width);
    void paintSpinner(HDC hdc);
    void paintStats(HDC hdc);
    void paintRoundRect(HDC hdc, const RECT& rect, int radius, HBRUSH fill);
    void drawButton(const DRAWITEMSTRUCT* item);
    static void applyWindowChrome(HWND hwnd);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
