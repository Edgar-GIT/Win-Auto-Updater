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

#include <filesystem>
#include <fstream>
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

std::wstring* heapCopy(std::wstring_view text) {
    return new std::wstring(text);
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

    const auto tempDir = StateManager::baseDir();
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    appendLog(L"Creating temporary directory...");
    appendLog(tempDir.wstring());
    if (ec) {
        appendLog(L"FAILED: " + std::wstring(ec.message().begin(), ec.message().end()));
    } else {
        appendLog(L"OK");
    }

    const auto logPath = tempDir / L"updater.log";
    logFile_.open(logPath);
    appendLog(L"Creating updater.log...");
    appendLog(logPath.wstring());
    if (logFile_.is_open()) {
        appendLog(L"OK");
    } else {
        appendLog(L"FAILED");
    }

    logger_.setStatusSink([this](std::wstring_view message) {
        auto* copy = heapCopy(message);
        if (!PostMessageW(hwnd_, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    logger_.setLogSink([this](std::wstring_view message) {
        if (logFile_.is_open()) {
            logFile_ << message << L"\n";
            logFile_.flush();
        }
        auto* copy = heapCopy(message);
        if (!PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });

    logger_.setProgressSink([this](std::wstring_view message) {
        auto* copy = heapCopy(message);
        if (!PostMessageW(hwnd_, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy))) {
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
    }
    if (terminalFont_) {
        DeleteObject(terminalFont_);
    }
    if (terminalBrush_) {
        DeleteObject(terminalBrush_);
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
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
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

    font_ = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    terminalFont_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");

    terminalBrush_ = CreateSolidBrush(RGB(30, 30, 30));

    statusLabel_ = CreateWindowExW(0, L"STATIC", lastStatus_.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_CENTER, 16, 14, kWidth - 48, 28,
                                   hwnd_, nullptr, instance_, nullptr);
    if (statusLabel_ && font_) {
        SendMessageW(statusLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    terminal_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        16, 52, kWidth - 48, kHeight - 150, hwnd_, nullptr, instance_, nullptr);
    if (terminal_ && terminalFont_) {
        SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), TRUE);
    }

    const int buttonY = kHeight - 88;
    const int uninstallX = (kWidth - (kButtonWidth * 2 + 16)) / 2;

    uninstallButton_ = CreateWindowExW(
        0, L"BUTTON", L"Uninstall", WS_CHILD | BS_PUSHBUTTON, uninstallX, buttonY, kButtonWidth,
        kButtonHeight, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UNINSTALL)), instance_,
        nullptr);

    closeButton_ = CreateWindowExW(
        0, L"BUTTON", L"Close", WS_CHILD | BS_PUSHBUTTON, uninstallX + kButtonWidth + 16, buttonY,
        kButtonWidth, kButtonHeight, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLOSE)),
        instance_, nullptr);

    if (uninstallButton_ && font_) {
        SendMessageW(uninstallButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (closeButton_ && font_) {
        SendMessageW(closeButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
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
    const int lineStart =
        static_cast<int>(SendMessageW(terminal_, EM_LINEINDEX, lineIndex, 0));
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

    appendLog(L"Uninstalling...");

    Scheduler scheduler(exe);
    const bool taskRemoved = scheduler.remove();
    appendLog(taskRemoved ? L"Scheduled task removed." : L"Scheduled task was not found.");

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
        const auto scriptDir = StateManager::baseDir();
        const std::wstring scriptPath = (scriptDir / L"uninstall.cmd").wstring();
        const std::wstring exePath = exe.wstring();

        std::string script;
        script += "@echo off\r\n";
        script += "timeout /t 2 /nobreak > nul\r\n";
        script += "del \"" + std::string(exePath.begin(), exePath.end()) + "\" > nul 2>&1\r\n";
        script += "if not exist \"" + std::string(exePath.begin(), exePath.end()) + "\" (\r\n";
        script += "    rmdir /s /q \"" + std::string(scriptDir.wstring().begin(), scriptDir.wstring().end()) + "\" > nul 2>&1\r\n";
        script += ")\r\n";
        script += "del \"%~f0\" > nul 2>&1\r\n";

        std::ofstream out(scriptPath.c_str());
        out << script;
        out.close();

        std::wstring cmdLine = scriptPath;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        appendLog(L"Self-cleanup script launched.");
    }

    allowClose_ = true;
    running_ = false;
    DestroyWindow(hwnd_);
}

void App::worker() {
    logger_.status(L"Checking connection...");
    logger_.log(L"Single Update started.");

    for (int attempt = 0; attempt < 30 && running_; ++attempt) {
        if (ensureInternet()) {
            logger_.log(L"Internet connection OK.");
            break;
        }
        if (attempt == 0) {
            logger_.log(L"Waiting for Internet connection...");
        }
        if (attempt == 29) {
            auto* msg = heapCopy(L"No Internet connection available.");
            PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            return;
        }
        Sleep(2000);
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        auto* msg = heapCopy(L"Unable to resolve executable path.");
        PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
        return;
    }

    StateManager stateManager;
    const auto savedState = stateManager.load();
    const bool isContinuation = (savedState.phase == L"waiting_reboot");

    runNumber_ = savedState.rebootCount + 1;
    executionType_ = isContinuation ? L"Resume after reboot" : L"Fresh start";

    if (isContinuation) {
        logger_.log(L"Resuming after reboot (reboot #" +
                    std::to_wstring(savedState.rebootCount) + L").");
    } else {
        logger_.log(L"Fresh start.");
    }

    if (!isContinuation) {
        logger_.status(L"Preparing persistence...");
        logger_.log(L"Creating scheduled task for reboot resume...");
        Scheduler scheduler(exe);
        if (!scheduler.create()) {
            auto* msg = heapCopy(L"Failed to create the scheduled task.");
            PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            return;
        }
        logger_.log(L"Scheduled task ready.");
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

    engine.setLogCallback([this](std::wstring_view message) { logger_.log(message); });

    engine.setProgressCallback([this](std::wstring_view title, int percent) {
        const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        logger_.status(L"Working... " + std::to_wstring(clamped) + L"%");
        logger_.progress(std::wstring(title) + L"  [" + std::to_wstring(clamped) + L"%]");
    });

    while (running_) {
        const auto cycle = engine.runCycle();
        if (!cycle.success) {
            auto* msg = heapCopy(cycle.message.empty() ? L"Update cycle failed." : cycle.message);
            PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
            return;
        }

        totalUpdatesFound_ += cycle.updatesFound;
        totalUpdatesInstalled_ += cycle.updatesInstalled;
        totalUpdatesFailed_ += cycle.updatesFailed;
        finalVerificationDone_ = cycle.finalVerificationDone || finalVerificationDone_;

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
                auto* msg = heapCopy(
                    L"The update process could not be completed automatically.\r\n\r\n"
                    L"Windows Update appears to be stuck in a reboot cycle.\r\n\r\n"
                    L"Please complete the remaining updates manually.");
                PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(msg));
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
        Sleep(1000);
    }
}

void App::finishSuccess() {
    logSummary(L"Success");
    finished_ = true;
    allowClose_ = true;
    setStatus(L"This computer is ready.");
    appendLog(L"All available updates have been installed.");
    appendLog(L"The scheduled task has been removed automatically.");

    if (closeButton_) {
        ShowWindow(closeButton_, SW_SHOW);
        EnableWindow(closeButton_, TRUE);
    }
    if (uninstallButton_) {
        ShowWindow(uninstallButton_, SW_SHOW);
        EnableWindow(uninstallButton_, TRUE);
        SetWindowTextW(uninstallButton_, L"Uninstall");
    }
}

void App::finishError(const std::wstring& message) {
    allowClose_ = true;
    setStatus(L"Error");
    for (size_t pos = 0; pos < message.size();) {
        const size_t next = message.find(L'\r', pos);
        const std::wstring line = message.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
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
    if (closeButton_) {
        ShowWindow(closeButton_, SW_SHOW);
        EnableWindow(closeButton_, TRUE);
    }
    if (uninstallButton_) {
        ShowWindow(uninstallButton_, SW_SHOW);
        EnableWindow(uninstallButton_, TRUE);
        SetWindowTextW(uninstallButton_, L"Uninstall");
    }
}

void App::logSummary(const std::wstring& result) {
    const auto add = [this](const std::wstring& line) {
        logger_.log(line);
        appendLog(line);
    };
    add(L"");
    add(L"===== Single Update =====");
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
    logger_.status(L"Restarting...");
    logger_.log(L"Requesting system restart in 15 seconds...");
    Sleep(1500);

    if (Restart::now()) {
        logger_.log(L"Restart command sent.");
        allowClose_ = true;
        return;
    }

    allowClose_ = true;
    setStatus(L"Restart required");
    appendLog(L"Automatic restart failed. Please restart Windows manually.");
    appendLog(L"The scheduled task will resume Single Update after logon.");
    if (closeButton_) {
        ShowWindow(closeButton_, SW_SHOW);
        EnableWindow(closeButton_, TRUE);
    }
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
        case WM_CTLCOLOREDIT: {
            const auto child = reinterpret_cast<HWND>(lParam);
            if (child == terminal_) {
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(204, 204, 204));
                SetBkColor(reinterpret_cast<HDC>(wParam), RGB(30, 30, 30));
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
        case WM_APP_LOG: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                appendLog(*text);
                delete text;
            }
            return 0;
        }
        case WM_APP_PROGRESS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
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
        case WM_CLOSE:
            if (allowClose_) {
                running_ = false;
                DestroyWindow(hwnd_);
            }
            return 0;
        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
