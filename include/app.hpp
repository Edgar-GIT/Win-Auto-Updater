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
#include <string>

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
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> allowClose_{false};
    std::wstring lastStatus_ = L"Starting...";
    bool hasProgressLine_ = false;

    bool elevateIfNeeded();
    bool ensureInternet();
    bool createUi();
    void setStatus(std::wstring message);
    void appendLog(std::wstring message);
    void updateProgressLine(std::wstring message);
    void showCompletionUi();
    void performUninstall();
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);
    void requestReboot();

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
