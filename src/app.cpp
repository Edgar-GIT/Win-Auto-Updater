#include "app.hpp"

#include "cleanup.hpp"
#include "restart.hpp"
#include "scheduler.hpp"
#include "state.hpp"
#include "update.hpp"

#include "../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wininet.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

constexpr COLORREF kBgColor = RGB(245, 247, 250);
constexpr COLORREF kHeaderColor = RGB(24, 52, 84);
constexpr COLORREF kTerminalBg = RGB(22, 27, 34);
constexpr COLORREF kTerminalFg = RGB(201, 209, 217);
constexpr COLORREF kAccent = RGB(37, 99, 168);

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

std::wstring* heapCopy(std::wstring_view text) {
    return new std::wstring(text);
}

bool postText(HWND hwnd, UINT msg, std::wstring_view text) {
    auto* copy = heapCopy(text);
    if (!PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
        return false;
    }
    return true;
}

unsigned long long nowMs() {
    return GetTickCount64();
}

std::wstring sanitizeLogLine(std::wstring message) {
    for (wchar_t& c : message) {
        if (c == L'\r' || c == L'\n') {
            c = L' ';
        }
    }
    return message;
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {}

int App::run() {
    if (!elevateIfNeeded()) {
        return 0;
    }

    HANDLE singleInstance =
        CreateMutexW(nullptr, TRUE, L"Local\\WinAutoUpdaterSingleInstanceMutex");
    if (!singleInstance) {
        MessageBoxW(nullptr, L"Failed to create the single-instance lock.", L"Win Auto Updater",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        return 0;
    }

    const auto releaseSingleInstance = [&]() {
        if (singleInstance) {
            ReleaseMutex(singleInstance);
            CloseHandle(singleInstance);
            singleInstance = nullptr;
        }
    };

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    if (!createUi()) {
        MessageBoxW(nullptr, L"Failed to create the application window.", L"Win Auto Updater",
                    MB_OK | MB_ICONERROR);
        releaseSingleInstance();
        return 1;
    }

    const auto tempDir = StateManager::baseDir();
    if (tempDir.empty()) {
        appendLog(L"Creating temporary directory...");
        appendLog(L"FAILED: Cannot determine temporary directory path.");
    } else {
        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);
        appendLog(L"Working directory: " + tempDir.wstring());
        if (ec) {
            appendLog(L"FAILED to create temp directory: " +
                      std::wstring(ec.message().begin(), ec.message().end()));
        }
    }

    const auto logPath = tempDir / L"updater.log";
    logFile_.open(logPath);
    appendLog(L"Log file: " + logPath.wstring() + (logFile_.is_open() ? L" (OK)" : L" (FAILED)"));

    logger_.setStatusSink([this](std::wstring_view message) {
        if (!uiDoneMode_) {
            postText(hwnd_, WM_APP_STATUS, message);
        }
    });

    logger_.setLogSink([this](std::wstring_view message) {
        if (logFile_.is_open()) {
            logFile_ << message << L"\n";
            logFile_.flush();
        }
        if (!uiDoneMode_) {
            postText(hwnd_, WM_APP_LOG, message);
        }
    });

    // Progress sink is unused — progress goes only to the bar/label via postProgress.
    logger_.setProgressSink([](std::wstring_view) {});

    workerThread_ = std::thread([this] {
        try {
            worker();
        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            postText(hwnd_, WM_APP_ERROR,
                     L"Worker thread exception: " + std::wstring(what.begin(), what.end()));
        } catch (...) {
            postText(hwnd_, WM_APP_ERROR, L"Worker thread crashed with an unknown exception.");
        }
    });

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    if (titleFont_) {
        DeleteObject(titleFont_);
    }
    if (uiFont_) {
        DeleteObject(uiFont_);
    }
    if (smallFont_) {
        DeleteObject(smallFont_);
    }
    if (terminalFont_) {
        DeleteObject(terminalFont_);
    }
    if (bgBrush_) {
        DeleteObject(bgBrush_);
    }
    if (terminalBrush_) {
        DeleteObject(terminalBrush_);
    }
    if (headerBrush_) {
        DeleteObject(headerBrush_);
    }

    releaseSingleInstance();
    return static_cast<int>(msg.wParam);
}

bool App::elevateIfNeeded() {
    if (isElevated()) {
        return true;
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        MessageBoxW(nullptr, L"Unable to locate the executable path.", L"Win Auto Updater",
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
            MessageBoxW(nullptr, L"Administrator privileges are required.", L"Win Auto Updater",
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
    bgBrush_ = CreateSolidBrush(kBgColor);
    wc.hbrBackground = bgBrush_;
    wc.lpszClassName = L"WinAutoUpdaterWindow";
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenW - kWidth) / 2;
    const int y = (screenH - kHeight) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Win Auto Updater",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, kWidth,
                            kHeight, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }

    titleFont_ = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    uiFont_ = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    smallFont_ = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    terminalFont_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");

    terminalBrush_ = CreateSolidBrush(kTerminalBg);
    headerBrush_ = CreateSolidBrush(kHeaderColor);

    titleLabel_ = CreateWindowExW(0, L"STATIC", L"Win Auto Updater",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd_, nullptr,
                                  instance_, nullptr);

    phaseLabel_ = CreateWindowExW(0, L"STATIC", lastPhase_.c_str(),
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PHASE)),
                                  instance_, nullptr);

    currentLabel_ = CreateWindowExW(0, L"STATIC", lastCurrent_.c_str(),
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0,
                                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CURRENT)),
                                    instance_, nullptr);

    statsLabel_ = CreateWindowExW(0, L"STATIC", lastStats_.c_str(),
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATS)),
                                  instance_, nullptr);

    progressBar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                   WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)),
                                   instance_, nullptr);
    SendMessageW(progressBar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(progressBar_, PBM_SETPOS, 0, 0);
    SendMessageW(progressBar_, PBM_SETBARCOLOR, 0, kAccent);
    SendMessageW(progressBar_, PBM_SETBKCOLOR, 0, RGB(220, 226, 234));

    terminal_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TERMINAL)), instance_,
        nullptr);
    // Raise the default ~32K limit so long sessions don't corrupt the control.
    SendMessageW(terminal_, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);

    summaryLabel_ = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | SS_LEFT,
        0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);

    uninstallButton_ = CreateWindowExW(
        0, L"BUTTON", L"Uninstall tool", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UNINSTALL)), instance_, nullptr);

    closeButton_ = CreateWindowExW(
        0, L"BUTTON", L"Close", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLOSE)), instance_, nullptr);

    if (titleLabel_ && titleFont_) {
        SendMessageW(titleLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    }
    if (phaseLabel_ && uiFont_) {
        SendMessageW(phaseLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
    if (currentLabel_ && smallFont_) {
        SendMessageW(currentLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (statsLabel_ && smallFont_) {
        SendMessageW(statsLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (terminal_ && terminalFont_) {
        SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), TRUE);
    }
    if (summaryLabel_ && uiFont_) {
        SendMessageW(summaryLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
    if (uninstallButton_ && smallFont_) {
        SendMessageW(uninstallButton_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (closeButton_ && smallFont_) {
        SendMessageW(closeButton_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }

    layoutControls();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    setPhase(L"Starting");
    setCurrent(L"Preparing Win Auto Updater...");
    setStatus(L"Starting...");
    appendLog(L"Win Auto Updater started.");
    appendLog(L"This tool searches, downloads, and installs Windows updates automatically.");
    appendLog(L"Do not shut down the PC until the process finishes or asks for a restart.");
    return true;
}

void App::layoutControls() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int margin = 24;
    const int contentW = width - margin * 2;
    const int buttonArea = 64;
    const int buttonY = height - margin - kButtonHeight;
    const int gap = 14;
    const int totalButtons = kButtonWidth * 2 + gap;
    const int startX = (width - totalButtons) / 2;

    if (uiDoneMode_) {
        if (titleLabel_) {
            MoveWindow(titleLabel_, margin, 28, contentW, 32, TRUE);
        }
        if (phaseLabel_) {
            MoveWindow(phaseLabel_, margin, 78, contentW, 28, TRUE);
        }
        if (summaryLabel_) {
            MoveWindow(summaryLabel_, margin, 120, contentW, height - 120 - buttonArea - margin,
                       TRUE);
        }
        if (uninstallButton_) {
            MoveWindow(uninstallButton_, startX, buttonY, kButtonWidth, kButtonHeight, TRUE);
        }
        if (closeButton_) {
            MoveWindow(closeButton_, startX + kButtonWidth + gap, buttonY, kButtonWidth,
                       kButtonHeight, TRUE);
        }
        return;
    }

    if (titleLabel_) {
        MoveWindow(titleLabel_, margin, 16, contentW, 32, TRUE);
    }
    if (phaseLabel_) {
        MoveWindow(phaseLabel_, margin, 54, contentW, 24, TRUE);
    }
    if (currentLabel_) {
        MoveWindow(currentLabel_, margin, 82, contentW, 22, TRUE);
    }
    if (progressBar_) {
        MoveWindow(progressBar_, margin, 112, contentW, 22, TRUE);
    }
    if (statsLabel_) {
        MoveWindow(statsLabel_, margin, 142, contentW, 20, TRUE);
    }

    const int terminalTop = 172;
    const int terminalHeight = height - terminalTop - buttonArea - margin;
    if (terminal_ && terminalHeight > 40) {
        MoveWindow(terminal_, margin, terminalTop, contentW, terminalHeight, TRUE);
    }

    if (uninstallButton_) {
        MoveWindow(uninstallButton_, startX, buttonY, kButtonWidth, kButtonHeight, TRUE);
    }
    if (closeButton_) {
        MoveWindow(closeButton_, startX + kButtonWidth + gap, buttonY, kButtonWidth, kButtonHeight,
                   TRUE);
    }
}

void App::setStatus(std::wstring message) {
    lastStatus_ = std::move(message);
    setPhase(lastStatus_);
}

void App::setPhase(std::wstring message) {
    lastPhase_ = std::move(message);
    if (phaseLabel_) {
        SetWindowTextW(phaseLabel_, lastPhase_.c_str());
    }
}

void App::setCurrent(std::wstring message) {
    lastCurrent_ = std::move(message);
    if (currentLabel_ && !uiDoneMode_) {
        SetWindowTextW(currentLabel_, lastCurrent_.c_str());
    }
}

void App::setStats(std::wstring message) {
    lastStats_ = std::move(message);
    if (statsLabel_ && !uiDoneMode_) {
        SetWindowTextW(statsLabel_, lastStats_.c_str());
    }
}

void App::setProgressPercent(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    lastPercent_ = percent;
    if (progressBar_ && !uiDoneMode_) {
        SendMessageW(progressBar_, PBM_SETPOS, percent, 0);
    }
}

void App::refreshStatsLabel() {
    setStats(L"Found: " + std::to_wstring(totalUpdatesFound_) + L"    Installed: " +
             std::to_wstring(totalUpdatesInstalled_) + L"    Failed: " +
             std::to_wstring(totalUpdatesFailed_));
}

void App::showActionButtons(bool enabled) {
    if (closeButton_) {
        ShowWindow(closeButton_, SW_SHOW);
        EnableWindow(closeButton_, enabled ? TRUE : FALSE);
    }
    if (uninstallButton_) {
        ShowWindow(uninstallButton_, SW_SHOW);
        EnableWindow(uninstallButton_, enabled ? TRUE : FALSE);
        SetWindowTextW(uninstallButton_, L"Uninstall tool");
    }
}

void App::trimTerminalIfNeeded() {
    if (!terminal_) {
        return;
    }

    const int length = GetWindowTextLengthW(terminal_);
    if (length < kMaxTerminalChars) {
        return;
    }

    // Keep roughly the second half so the control never hits the old 32K corruption zone.
    const int keepFrom = length / 2;
    const int lineIndex =
        static_cast<int>(SendMessageW(terminal_, EM_LINEFROMCHAR, keepFrom, 0));
    int cut = static_cast<int>(SendMessageW(terminal_, EM_LINEINDEX, lineIndex, 0));
    if (cut < 0) {
        cut = keepFrom;
    }

    SendMessageW(terminal_, EM_SETSEL, 0, cut);
    SendMessageW(terminal_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

void App::appendLog(std::wstring message) {
    if (!terminal_ || uiDoneMode_) {
        return;
    }

    message = sanitizeLogLine(std::move(message));
    trimTerminalIfNeeded();

    const int length = GetWindowTextLengthW(terminal_);
    std::wstring line = std::move(message);
    if (length > 0) {
        line.insert(0, L"\r\n");
    }

    SendMessageW(terminal_, EM_SETSEL, length, length);
    SendMessageW(terminal_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(terminal_, EM_SCROLLCARET, 0, 0);
}

bool App::shouldPostProgress(int percent) const {
    if (uiDoneMode_) {
        return false;
    }
    const auto now = nowMs();
    const auto lastMs = lastProgressPostMs_.load();
    const int lastPct = lastPostedPercent_.load();
    if (percent == 100 || percent == 0) {
        return true;
    }
    if (percent == lastPct && (now - lastMs) < 400) {
        return false;
    }
    if ((now - lastMs) < 250 && percent != 100) {
        return false;
    }
    return true;
}

void App::postProgress(int percent, std::wstring_view title) {
    if (!shouldPostProgress(percent)) {
        return;
    }
    lastPostedPercent_ = percent;
    lastProgressPostMs_ = nowMs();

    auto* pct = new int(percent);
    auto* titleCopy = heapCopy(title);
    if (!PostMessageW(hwnd_, WM_APP_PROGRESS, reinterpret_cast<WPARAM>(pct),
                      reinterpret_cast<LPARAM>(titleCopy))) {
        delete pct;
        delete titleCopy;
    }
}

void App::showFinishedScreen(bool success, const std::wstring& detail) {
    uiDoneMode_ = true;
    finished_ = true;
    allowClose_ = true;

    if (terminal_) {
        ShowWindow(terminal_, SW_HIDE);
    }
    if (progressBar_) {
        ShowWindow(progressBar_, SW_HIDE);
    }
    if (statsLabel_) {
        ShowWindow(statsLabel_, SW_HIDE);
    }
    if (currentLabel_) {
        ShowWindow(currentLabel_, SW_HIDE);
    }

    setPhase(success ? L"Finished — this computer is ready"
                     : L"Stopped — an error occurred");

    std::wstring summary;
    if (success) {
        summary =
            L"All available Windows updates have been installed.\r\n\r\n"
            L"Installed: " +
            std::to_wstring(totalUpdatesInstalled_) + L"\r\nFailed: " +
            std::to_wstring(totalUpdatesFailed_) +
            L"\r\n\r\n"
            L"Reminder: update Microsoft Defender / antivirus definitions manually in "
            L"Windows Security if needed.\r\n\r\n"
            L"You can close this window, or uninstall this tool.";
    } else {
        summary = detail.empty() ? L"An error occurred during the update process." : detail;
        summary +=
            L"\r\n\r\nReminder: update Microsoft Defender / antivirus definitions manually in "
            L"Windows Security if needed.";
    }

    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_, summary.c_str());
        ShowWindow(summaryLabel_, SW_SHOW);
    }

    showActionButtons(true);
    layoutControls();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::performUninstall() {
    MessageBoxW(
        hwnd_,
        L"Before uninstalling, remember to update Microsoft Defender / antivirus "
        L"definitions manually in Windows Security.\r\n\r\n"
        L"Click OK to continue uninstalling this tool.",
        L"Win Auto Updater", MB_OK | MB_ICONINFORMATION);

    const auto exe = currentExecutable();

    Scheduler scheduler(exe);
    scheduler.remove();

    StateManager stateManager;
    stateManager.remove();
    Cleanup::removeTempFiles();

    if (uninstallButton_) {
        EnableWindow(uninstallButton_, FALSE);
        SetWindowTextW(uninstallButton_, L"Uninstalled");
    }

    if (!exe.empty()) {
        MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_,
                       L"Win Auto Updater has been uninstalled.\r\n\r\n"
                       L"The executable will be removed on the next restart.\r\n\r\n"
                       L"Remember to keep Microsoft Defender / antivirus definitions up to date.");
    }

    allowClose_ = true;
    running_ = false;

    // Leave the window open with Close enabled so the user sees the confirmation.
    if (closeButton_) {
        EnableWindow(closeButton_, TRUE);
        ShowWindow(closeButton_, SW_SHOW);
    }
}

void App::worker() {
    logger_.status(L"Checking Internet connection...");
    postText(hwnd_, WM_APP_CURRENT, L"Waiting for network connectivity...");
    logger_.log(L"Win Auto Updater worker started.");

    for (int attempt = 0; attempt < 30 && running_; ++attempt) {
        if (ensureInternet()) {
            logger_.log(L"Internet connection OK.");
            break;
        }
        if (attempt == 0) {
            logger_.log(L"Waiting for Internet connection...");
        }
        if (attempt == 29) {
            postText(hwnd_, WM_APP_ERROR,
                     L"No Internet connection available.\r\n\r\n"
                     L"Connect to the network and run Win Auto Updater again.");
            return;
        }
        Sleep(2000);
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        postText(hwnd_, WM_APP_ERROR, L"Unable to resolve executable path.");
        return;
    }

    StateManager stateManager;
    const auto savedState = stateManager.load();
    const bool isContinuation = (savedState.phase == L"waiting_reboot");

    runNumber_ = savedState.rebootCount + 1;
    executionType_ = isContinuation ? L"Resume after reboot" : L"Fresh start";

    if (isContinuation) {
        logger_.log(L"Resuming after reboot (reboot #" + std::to_wstring(savedState.rebootCount) +
                    L").");
        postText(hwnd_, WM_APP_CURRENT,
                 L"Resuming update process after reboot #" +
                     std::to_wstring(savedState.rebootCount) + L"...");
    } else {
        logger_.log(L"Fresh start.");
    }

    logger_.status(L"Preparing automatic resume...");
    logger_.log(L"Registering automatic resume after reboot (Task Scheduler + RunOnce)...");
    postText(hwnd_, WM_APP_CURRENT,
             L"Registering automatic resume so the updater reopens after restart...");
    {
        Scheduler scheduler(exe);
        std::wstring scheduleError;
        if (!scheduler.create(&scheduleError)) {
            postText(hwnd_, WM_APP_ERROR,
                     L"Failed to create the automatic resume registration.\r\n\r\n" +
                         scheduleError +
                         L"\r\n\r\nWithout it, the updater cannot reopen after a reboot.");
            return;
        }
        if (!scheduleError.empty()) {
            logger_.log(L"Resume registration note: " + scheduleError);
        }
        logger_.log(L"Automatic resume is ready (scheduled task and/or RunOnce).");
    }

    UpdateEngine engine;
    engine.setStatusCallback([this](UpdateEngine::Phase phase, std::wstring_view detail) {
        switch (phase) {
            case UpdateEngine::Phase::Searching:
                logger_.status(L"Searching for updates...");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"Contacting Windows Update..." : std::wstring(detail));
                break;
            case UpdateEngine::Phase::Downloading:
                logger_.status(L"Downloading updates...");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"Downloading update files..." : std::wstring(detail));
                break;
            case UpdateEngine::Phase::Installing:
                logger_.status(L"Installing updates...");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"Installing update..." : std::wstring(detail));
                break;
            case UpdateEngine::Phase::CheckingAgain:
                logger_.status(L"Checking again...");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"Looking for additional updates..."
                                        : std::wstring(detail));
                break;
            case UpdateEngine::Phase::UpToDate:
                logger_.status(L"System is up to date");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"No more updates are needed." : std::wstring(detail));
                break;
            case UpdateEngine::Phase::RebootRequired:
                logger_.status(L"Restart required");
                postText(hwnd_, WM_APP_CURRENT,
                         detail.empty() ? L"A restart is required to continue."
                                        : std::wstring(detail));
                break;
        }
    });

    engine.setLogCallback([this](std::wstring_view message) { logger_.log(message); });

    engine.setProgressCallback([this](std::wstring_view title, int percent) {
        const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        postProgress(clamped, title);
    });

    int consecutiveFailures = 0;
    constexpr int kMaxConsecutiveFailures = 3;

    while (running_) {
        const auto cycle = engine.runCycle();
        if (!cycle.success) {
            ++consecutiveFailures;
            logger_.log(L"Update cycle failed (" + std::to_wstring(consecutiveFailures) + L"/" +
                        std::to_wstring(kMaxConsecutiveFailures) + L"): " +
                        (cycle.message.empty() ? L"unknown error" : cycle.message));
            if (consecutiveFailures < kMaxConsecutiveFailures) {
                logger_.status(L"Retrying after a temporary failure...");
                postText(hwnd_, WM_APP_CURRENT,
                         L"Temporary failure — retrying in a few seconds...");
                Sleep(5000);
                continue;
            }
            postText(hwnd_, WM_APP_ERROR,
                     cycle.message.empty() ? L"Update cycle failed." : cycle.message);
            return;
        }

        consecutiveFailures = 0;

        totalUpdatesFound_ += cycle.updatesFound;
        totalUpdatesInstalled_ += cycle.updatesInstalled;
        totalUpdatesFailed_ += cycle.updatesFailed;
        finalVerificationDone_ = cycle.finalVerificationDone || finalVerificationDone_;
        postText(hwnd_, WM_APP_STATS, L"");

        if (cycle.upToDate) {
            cleanupDone_ = true;
            logSummary(L"Success");
            logger_.log(L"System is up to date. Cleaning up...");
            Scheduler scheduler(exe);
            scheduler.remove();
            stateManager.remove();
            PostMessageW(hwnd_, WM_APP_DONE, 0, 0);
            return;
        }

        if (cycle.rebootRequired) {
            hadPendingRestart_ = cycle.pendingRestart;
            const int newRebootCount = savedState.rebootCount + 1;
            if (newRebootCount > kMaxRebootCount) {
                logger_.log(L"Max reboot count (" + std::to_wstring(kMaxRebootCount) +
                            L") reached. Aborting.");
                Scheduler scheduler(exe);
                scheduler.remove();
                stateManager.remove();
                cleanupDone_ = true;
                logSummary(L"Error");
                postText(hwnd_, WM_APP_ERROR,
                         L"The update process could not be completed automatically.\r\n\r\n"
                         L"Windows Update appears to be stuck in a reboot cycle.\r\n\r\n"
                         L"Please complete the remaining updates manually via Settings → Windows "
                         L"Update.");
                return;
            }
            StateManager::State state;
            state.phase = L"waiting_reboot";
            state.rebootCount = newRebootCount;
            stateManager.save(state);

            {
                Scheduler scheduler(exe);
                std::wstring scheduleError;
                if (!scheduler.create(&scheduleError)) {
                    logger_.log(L"WARNING: failed to refresh resume registration before reboot: " +
                                scheduleError);
                } else if (!scheduleError.empty()) {
                    logger_.log(L"Resume refresh note: " + scheduleError);
                } else {
                    logger_.log(L"Resume registration refreshed before reboot.");
                }
            }

            PostMessageW(hwnd_, WM_APP_REBOOT, 0, 0);
            return;
        }

        logger_.status(L"Checking again...");
        postText(hwnd_, WM_APP_CURRENT, L"Looking for more updates after the last installation...");
        Sleep(1000);
    }
}

void App::finishSuccess() {
    showFinishedScreen(true, L"");
}

void App::finishError(const std::wstring& message) {
    showFinishedScreen(false, message);
}

void App::logSummary(const std::wstring& result) {
    const auto add = [this](const std::wstring& line) { logger_.log(line); };
    add(L"");
    add(L"===== Win Auto Updater summary =====");
    add(L"");
    add(L"Run number: " + std::to_wstring(runNumber_));
    add(L"Execution type: " + executionType_);
    add(L"Updates found: " + std::to_wstring(totalUpdatesFound_));
    add(L"Updates installed: " + std::to_wstring(totalUpdatesInstalled_));
    add(L"Updates failed: " + std::to_wstring(totalUpdatesFailed_));
    add(L"Pending restart: " + std::wstring(hadPendingRestart_ ? L"Yes" : L"No"));
    add(L"Final verification: " + std::wstring(finalVerificationDone_ ? L"Passed" : L"Skipped"));
    add(L"Cleanup: " + std::wstring(cleanupDone_ ? L"Completed" : L"Skipped"));
    add(L"Result: " + result);
    add(L"");
}

void App::requestReboot() {
    setPhase(L"Restart required");
    setCurrent(L"Windows will restart automatically so updates can continue.");
    setProgressPercent(100);
    logger_.status(L"Restarting...");
    logger_.log(L"Requesting system restart...");
    appendLog(L"A restart is required to continue installing updates.");
    appendLog(L"Windows will restart in 15 seconds. Save your work now.");

    rebootCountdown_ = 15;
    setCurrent(L"Restarting in " + std::to_wstring(rebootCountdown_) +
               L" seconds... Save your work now.");
    SetTimer(hwnd_, kRebootTimerId, 1000, nullptr);
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
        case WM_SIZE:
            layoutControls();
            return 0;
        case WM_CTLCOLORSTATIC: {
            const auto child = reinterpret_cast<HWND>(lParam);
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            if (child == titleLabel_ || child == phaseLabel_) {
                SetTextColor(hdc, kHeaderColor);
            } else {
                SetTextColor(hdc, RGB(55, 65, 80));
            }
            return reinterpret_cast<LRESULT>(bgBrush_);
        }
        case WM_CTLCOLOREDIT: {
            const auto child = reinterpret_cast<HWND>(lParam);
            if (child == terminal_) {
                SetTextColor(reinterpret_cast<HDC>(wParam), kTerminalFg);
                SetBkColor(reinterpret_cast<HDC>(wParam), kTerminalBg);
                return reinterpret_cast<LRESULT>(terminalBrush_);
            }
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_UNINSTALL:
                    if (finished_ || allowClose_) {
                        performUninstall();
                    }
                    return 0;
                case IDC_CLOSE:
                    if (allowClose_) {
                        running_ = false;
                        DestroyWindow(hwnd_);
                    }
                    return 0;
            }
            break;
        }
        case WM_APP_STATUS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_) {
                    setStatus(*text);
                }
                delete text;
            }
            return 0;
        }
        case WM_APP_CURRENT: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_) {
                    setCurrent(*text);
                }
                delete text;
            }
            return 0;
        }
        case WM_APP_STATS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            delete text;
            if (!uiDoneMode_) {
                refreshStatsLabel();
            }
            return 0;
        }
        case WM_APP_LOG: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_) {
                    appendLog(*text);
                }
                delete text;
            }
            return 0;
        }
        case WM_APP_PROGRESS: {
            auto* percentPtr = reinterpret_cast<int*>(wParam);
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (!uiDoneMode_) {
                if (percentPtr) {
                    setProgressPercent(*percentPtr);
                }
                if (text) {
                    // Progress belongs on the label/bar only — never rewrite terminal lines
                    // (that was causing overlapping/garbled log text).
                    setCurrent(*text + L"  [" +
                               std::to_wstring(percentPtr ? *percentPtr : lastPercent_) + L"%]");
                }
            }
            delete percentPtr;
            delete text;
            return 0;
        }
        case WM_APP_DONE:
            finishSuccess();
            return 0;
        case WM_APP_REBOOT:
            requestReboot();
            return 0;
        case WM_APP_ERROR: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            const std::wstring message = text ? *text : L"Unexpected error.";
            delete text;
            finishError(message);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kRebootTimerId) {
                --rebootCountdown_;
                if (rebootCountdown_ > 0) {
                    setCurrent(L"Restarting in " + std::to_wstring(rebootCountdown_) +
                               L" seconds... Save your work now.");
                } else {
                    KillTimer(hwnd_, kRebootTimerId);
                    setCurrent(L"Sending restart command...");
                    if (Restart::now()) {
                        appendLog(L"Restart command sent.");
                        allowClose_ = true;
                    } else {
                        showFinishedScreen(
                            false,
                            L"Automatic restart failed. Please restart Windows manually.\r\n\r\n"
                            L"The scheduled task will resume Win Auto Updater after logon.");
                    }
                }
            }
            return 0;
        case WM_CLOSE:
            if (allowClose_) {
                running_ = false;
                DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kRebootTimerId);
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
