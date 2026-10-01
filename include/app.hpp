#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "logger.hpp"

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
        WM_APP_PHASE = WM_APP + 7,
        WM_APP_CURRENT = WM_APP + 8,
        WM_APP_STATS = WM_APP + 9
    };

    static constexpr int kWidth = 720;
    static constexpr int kHeight = 560;
    static constexpr int kButtonHeight = 34;
    static constexpr int kButtonWidth = 150;
    static constexpr UINT_PTR kRebootTimerId = 1;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND titleLabel_ = nullptr;
    HWND phaseLabel_ = nullptr;
    HWND currentLabel_ = nullptr;
    HWND statsLabel_ = nullptr;
    HWND progressBar_ = nullptr;
    HWND terminal_ = nullptr;
    HWND uninstallButton_ = nullptr;
    HWND closeButton_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT terminalFont_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    HBRUSH terminalBrush_ = nullptr;
    HBRUSH headerBrush_ = nullptr;
    Logger logger_;
    std::wofstream logFile_;
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> allowClose_{false};
    std::wstring lastStatus_ = L"Starting...";
    std::wstring lastPhase_ = L"Preparing";
    std::wstring lastCurrent_ = L"Waiting to begin...";
    std::wstring lastStats_ = L"Found: 0   Installed: 0   Failed: 0";
    int lastPercent_ = 0;
    std::atomic<bool> hasProgressLine_{false};
    std::thread workerThread_;
    int rebootCountdown_ = 0;

    static constexpr int kMaxRebootCount = 5;

    bool elevateIfNeeded();
    bool ensureInternet();
    bool createUi();
    void layoutControls();
    void setStatus(std::wstring message);
    void setPhase(std::wstring message);
    void setCurrent(std::wstring message);
    void setStats(std::wstring message);
    void setProgressPercent(int percent);
    void appendLog(std::wstring message);
    void updateProgressLine(std::wstring message);
    void performUninstall();
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);
    void requestReboot();
    void logSummary(const std::wstring& result);
    void refreshStatsLabel();
    void showActionButtons(bool enabled);

    int runNumber_ = 0;
    int totalUpdatesFound_ = 0;
    int totalUpdatesInstalled_ = 0;
    int totalUpdatesFailed_ = 0;
    bool hadPendingRestart_ = false;
    bool finalVerificationDone_ = false;
    bool cleanupDone_ = false;
    std::wstring executionType_;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
