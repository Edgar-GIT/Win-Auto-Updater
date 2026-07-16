#include "app.hpp"

#include "restart.hpp"
#include "scheduler.hpp"
#include "self_delete.hpp"
#include "update.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <wininet.h>

#include <filesystem>
#include <thread>

namespace {

std::filesystem::path currentExecutable() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
}

bool isElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated;
    }
    CloseHandle(token);
    return elevated == TRUE;
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {}

int App::run() {
    if (!elevateIfNeeded()) {
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    if (!createUi()) {
        MessageBoxW(nullptr, L"Failed to create the application window.", L"Single Update",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    logger_.setSink([this](std::wstring_view message) {
        auto* copy = new std::wstring(message);
        if (!PostMessageW(hwnd_, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    std::thread([this] { worker(); }).detach();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }

    return static_cast<int>(msg.wParam);
}

bool App::elevateIfNeeded() {
    if (isElevated()) {
        return true;
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        MessageBoxW(nullptr, L"Unable to locate the executable path.", L"Single Update",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        if (GetLastError() != ERROR_CANCELLED) {
            MessageBoxW(nullptr, L"Administrator privileges are required.", L"Single Update",
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return false;
}

bool App::ensureInternet() {
    DWORD flags = 0;
    return InternetGetConnectedState(&flags, 0) == TRUE;
}

bool App::createUi() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = App::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SingleUpdateWindow";
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenW - kWidth) / 2;
    const int y = (screenH - kHeight) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Single Update",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, kWidth,
                            kHeight, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }

    font_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    statusLabel_ = CreateWindowExW(0, L"STATIC", lastStatus_.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 20, 36,
                                   kWidth - 60, 40, hwnd_, nullptr, instance_, nullptr);
    if (statusLabel_ && font_) {
        SendMessageW(statusLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void App::setStatus(std::wstring message) {
    lastStatus_ = std::move(message);
    if (statusLabel_) {
        SetWindowTextW(statusLabel_, lastStatus_.c_str());
    }
}

void App::worker() {
    logger_.status(L"Checking connection...");

    for (int attempt = 0; attempt < 30 && running_; ++attempt) {
        if (ensureInternet()) {
            break;
        }
        if (attempt == 29) {
            auto* msg = new std::wstring(L"No Internet connection available.");
            PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            return;
        }
        Sleep(2000);
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        auto* msg = new std::wstring(L"Unable to resolve executable path.");
        PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
        return;
    }

    logger_.status(L"Preparing persistence...");
    Scheduler scheduler(exe);
    if (!scheduler.create()) {
        auto* msg = new std::wstring(L"Failed to create the scheduled task.");
        PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
        return;
    }

    UpdateEngine engine;
    engine.setStatusCallback([this](UpdateEngine::Phase phase, std::wstring_view) {
        switch (phase) {
            case UpdateEngine::Phase::Searching:
                logger_.status(L"Searching updates...");
                break;
            case UpdateEngine::Phase::Downloading:
                logger_.status(L"Downloading...");
                break;
            case UpdateEngine::Phase::Installing:
                logger_.status(L"Installing...");
                break;
            case UpdateEngine::Phase::CheckingAgain:
                logger_.status(L"Checking again...");
                break;
            case UpdateEngine::Phase::UpToDate:
                logger_.status(L"System is up to date.");
                break;
            case UpdateEngine::Phase::RebootRequired:
                logger_.status(L"Restarting...");
                break;
        }
    });

    while (running_) {
        const auto cycle = engine.runCycle();
        if (!cycle.success) {
            auto* msg = new std::wstring(cycle.message.empty() ? L"Update cycle failed."
                                                               : cycle.message);
            PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            return;
        }

        if (cycle.upToDate) {
            PostMessageW(hwnd_, WM_APP_DONE, 0, 0);
            return;
        }

        if (cycle.rebootRequired) {
            logger_.status(L"Restarting...");
            Sleep(1500);
            if (!Restart::now()) {
                auto* msg = new std::wstring(L"Failed to restart the system.");
                PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            }
            return;
        }

        logger_.status(L"Checking again...");
        Sleep(1000);
    }
}

void App::finishSuccess() {
    MessageBoxW(hwnd_, L"This computer is ready.\nAll available updates have been installed.",
                L"Single Update", MB_OK | MB_ICONINFORMATION);

    const auto exe = currentExecutable();
    Scheduler scheduler(exe);
    scheduler.remove();
    SelfDelete::cleanTempFiles();
    SelfDelete::schedule(exe);

    running_ = false;
    DestroyWindow(hwnd_);
}

void App::finishError(const std::wstring& message) {
    MessageBoxW(hwnd_, message.c_str(), L"Single Update", MB_OK | MB_ICONERROR);
    running_ = false;
    DestroyWindow(hwnd_);
}

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_STATUS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                setStatus(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_DONE:
            finishSuccess();
            return 0;
        case WM_APP_ERROR: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            const std::wstring message = text ? *text : L"Unexpected error.";
            delete text;
            finishError(message);
            return 0;
        }
        case WM_CLOSE:
            return 0;
        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
