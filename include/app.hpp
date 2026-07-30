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
        WM_APP_REBOOT = WM_APP + 6
    };

    static constexpr int kWidth = 560;
    static constexpr int kHeight = 460;
    static constexpr int kButtonHeight = 32;
    static constexpr int kButtonWidth = 140;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND terminal_ = nullptr;
    HWND uninstallButton_ = nullptr;
    HWND closeButton_ = nullptr;
    HFONT font_ = nullptr;
    HFONT terminalFont_ = nullptr;
    HBRUSH terminalBrush_ = nullptr;
    Logger logger_;
    std::wofstream logFile_;
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> allowClose_{false};
    std::wstring lastStatus_ = L"Starting...";
    std::atomic<bool> hasProgressLine_{false};
    std::thread workerThread_;

    static constexpr int kMaxRebootCount = 5;

    bool elevateIfNeeded();
    bool ensureInternet();
    bool createUi();
    void setStatus(std::wstring message);
    void appendLog(std::wstring message);
    void updateProgressLine(std::wstring message);
    void performUninstall();
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);
    void requestReboot();
    void logSummary(const std::wstring& result);

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
