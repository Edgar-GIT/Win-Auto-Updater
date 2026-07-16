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
#include <memory>
#include <string>

class App {
public:
    explicit App(HINSTANCE instance);
    int run();

private:
    enum : UINT {
        WM_APP_STATUS = WM_APP + 1,
        WM_APP_DONE = WM_APP + 2,
        WM_APP_ERROR = WM_APP + 3
    };

    static constexpr int kWidth = 420;
    static constexpr int kHeight = 140;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND statusLabel_ = nullptr;
    HFONT font_ = nullptr;
    Logger logger_;
    std::atomic<bool> running_{true};
    std::wstring lastStatus_ = L"Starting...";

    bool elevateIfNeeded();
    bool ensureInternet();
    bool createUi();
    void setStatus(std::wstring message);
    void worker();
    void finishSuccess();
    void finishError(const std::wstring& message);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
