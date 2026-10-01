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

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {}

int App::run() {
    if (!elevateIfNeeded()) {
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    if (!createUi()) {
        MessageBoxW(nullptr, L"Failed to create the application window.", L"Win Auto Updater",
                    MB_OK | MB_ICONERROR);
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
        postText(hwnd_, WM_APP_STATUS, message);
    });

    logger_.setLogSink([this](std::wstring_view message) {
        if (logFile_.is_open()) {
            logFile_ << message << L"\n";
            logFile_.flush();
        }
        postText(hwnd_, WM_APP_LOG, message);
    });

    logger_.setProgressSink([this](std::wstring_view message) {
        postText(hwnd_, WM_APP_PROGRESS, message);
    });

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
    const int margin = 20;
    const int contentW = width - margin * 2;

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

    const int buttonArea = 56;
    const int terminalTop = 172;
    const int terminalHeight = height - terminalTop - buttonArea - margin;
    if (terminal_ && terminalHeight > 40) {
        MoveWindow(terminal_, margin, terminalTop, contentW, terminalHeight, TRUE);
    }

    const int buttonY = height - margin - kButtonHeight;
    const int gap = 12;
    const int totalButtons = kButtonWidth * 2 + gap;
    const int startX = (width - totalButtons) / 2;
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
    if (currentLabel_) {
        SetWindowTextW(currentLabel_, lastCurrent_.c_str());
    }
}

void App::setStats(std::wstring message) {
    lastStats_ = std::move(message);
    if (statsLabel_) {
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
    if (progressBar_) {
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

void App::appendLog(std::wstring message) {
    if (!terminal_) {
        return;
    }

    hasProgressLine_ = false;

    const int length = GetWindowTextLengthW(terminal_);
    std::wstring line = std::move(message);
    if (length > 0) {
        line.insert(0, L"\r\n");
    }

    SendMessageW(terminal_, EM_SETSEL, length, length);
    SendMessageW(terminal_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(terminal_, EM_SCROLLCARET, 0, 0);
}

void App::updateProgressLine(std::wstring message) {
    if (!terminal_) {
        return;
    }

    const int length = GetWindowTextLengthW(terminal_);
    if (!hasProgressLine_ || length == 0) {
        appendLog(std::move(message));
        hasProgressLine_ = true;
        return;
    }

    const int lineCount = static_cast<int>(SendMessageW(terminal_, EM_GETLINECOUNT, 0, 0));
    if (lineCount < 1) {
        appendLog(std::move(message));
        hasProgressLine_ = true;
        return;
    }

    const int lineIndex = lineCount - 1;
    const int lineStart = static_cast<int>(SendMessageW(terminal_, EM_LINEINDEX, lineIndex, 0));
    if (lineStart < 0) {
        appendLog(std::move(message));
        hasProgressLine_ = true;
        return;
    }

    SendMessageW(terminal_, EM_SETSEL, lineStart, length);
    SendMessageW(terminal_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(message.c_str()));
    SendMessageW(terminal_, EM_SCROLLCARET, 0, 0);
    hasProgressLine_ = true;
}

void App::performUninstall() {
    const auto exe = currentExecutable();

    appendLog(L"Uninstalling Win Auto Updater...");

    Scheduler scheduler(exe);
    const bool taskRemoved = scheduler.remove();
    appendLog(taskRemoved ? L"Scheduled resume task removed." : L"No scheduled task was found.");

    StateManager stateManager;
    stateManager.remove();
    appendLog(L"State file removed.");

    Cleanup::removeTempFiles();
    appendLog(L"Temporary files removed.");

    if (uninstallButton_) {
        EnableWindow(uninstallButton_, FALSE);
        SetWindowTextW(uninstallButton_, L"Uninstalled");
    }

    if (!exe.empty()) {
        MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        appendLog(L"Executable will be removed on the next restart.");
    }

    allowClose_ = true;
    running_ = false;
    DestroyWindow(hwnd_);
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

    if (!isContinuation) {
        logger_.status(L"Preparing automatic resume...");
        logger_.log(L"Creating scheduled task for reboot resume...");
        postText(hwnd_, WM_APP_CURRENT, L"Creating scheduled task so updates can continue after restart...");
        Scheduler scheduler(exe);
        if (!scheduler.create()) {
            postText(hwnd_, WM_APP_ERROR,
                     L"Failed to create the scheduled task.\r\n\r\n"
                     L"Without it, the updater cannot resume after a reboot.");
            return;
        }
        logger_.log(L"Scheduled task ready.");
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
        logger_.status(L"Working... " + std::to_wstring(clamped) + L"%");
        postText(hwnd_, WM_APP_CURRENT, std::wstring(title));
        auto* pct = new int(clamped);
        auto* line = heapCopy(std::wstring(title) + L"  [" + std::to_wstring(clamped) + L"%]");
        if (!PostMessageW(hwnd_, WM_APP_PROGRESS, reinterpret_cast<WPARAM>(pct),
                          reinterpret_cast<LPARAM>(line))) {
            delete pct;
            delete line;
        }
    });

    while (running_) {
        const auto cycle = engine.runCycle();
        if (!cycle.success) {
            postText(hwnd_, WM_APP_ERROR,
                     cycle.message.empty() ? L"Update cycle failed." : cycle.message);
            return;
        }

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
            PostMessageW(hwnd_, WM_APP_REBOOT, 0, 0);
            return;
        }

        logger_.status(L"Checking again...");
        postText(hwnd_, WM_APP_CURRENT, L"Looking for more updates after the last installation...");
        Sleep(1000);
    }
}

void App::finishSuccess() {
    finished_ = true;
    allowClose_ = true;
    setPhase(L"Finished — this computer is ready");
    setCurrent(L"All available Windows updates have been installed.");
    setProgressPercent(100);
    appendLog(L"");
    appendLog(L"All available updates have been installed.");
    appendLog(L"The scheduled resume task has been removed automatically.");
    appendLog(L"You can close this window, or uninstall the tool if you no longer need it.");
    showActionButtons(true);
}

void App::finishError(const std::wstring& message) {
    allowClose_ = true;
    setPhase(L"Stopped — an error occurred");
    setCurrent(L"See the activity log below for details.");
    for (size_t pos = 0; pos < message.size();) {
        const size_t next = message.find(L'\r', pos);
        const std::wstring line =
            message.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (!line.empty() && line != L"\n") {
            appendLog(line);
        }
        if (next == std::wstring::npos) {
            break;
        }
        pos = next + 1;
        if (pos < message.size() && message[pos] == L'\n') {
            ++pos;
        }
    }
    showActionButtons(true);
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
                    if (finished_) {
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
                setStatus(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_PHASE: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                setPhase(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_CURRENT: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                setCurrent(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_STATS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            delete text;
            refreshStatsLabel();
            return 0;
        }
        case WM_APP_LOG: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                appendLog(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_PROGRESS: {
            // wParam may carry percent pointer from progress callback; lParam is log line.
            auto* percentPtr = reinterpret_cast<int*>(wParam);
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (percentPtr) {
                setProgressPercent(*percentPtr);
                delete percentPtr;
            }
            if (text) {
                updateProgressLine(*text);
                delete text;
            }
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
                        allowClose_ = true;
                        setPhase(L"Restart required — please reboot manually");
                        setCurrent(L"Automatic restart failed. Restart Windows yourself to continue.");
                        appendLog(L"Automatic restart failed. Please restart Windows manually.");
                        appendLog(L"The scheduled task will resume Win Auto Updater after logon.");
                        showActionButtons(true);
                    }
                }
            }
            return 0;
        case WM_CLOSE:
            if (allowClose_) {
                running_ = false;
                DestroyWindow(hwnd_);
            } else {
                appendLog(L"Please wait — updates are still in progress. Closing is disabled until finished.");
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
