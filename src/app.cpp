#include "app.hpp"

#include "cleanup.hpp"
#include "display.hpp"
#include "driverpolicy.hpp"
#include "drivers.hpp"
#include "restart.hpp"
#include "scheduler.hpp"
#include "state.hpp"
#include "stress.hpp"
#include "update.hpp"

#include "../resources/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wininet.h>

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Cozy, Windows 11 / Fluent inspired palette.
constexpr COLORREF kBgColor = RGB(243, 243, 243);
constexpr COLORREF kHeaderColor = RGB(0, 103, 192);
constexpr COLORREF kHeaderText = RGB(255, 255, 255);
constexpr COLORREF kHeaderSubText = RGB(213, 231, 250);
constexpr COLORREF kCardColor = RGB(255, 255, 255);
constexpr COLORREF kBorderColor = RGB(223, 223, 223);
constexpr COLORREF kTextPrimary = RGB(27, 27, 27);
constexpr COLORREF kTextSecondary = RGB(96, 96, 96);
constexpr COLORREF kAccent = RGB(0, 103, 192);
constexpr COLORREF kAccentDark = RGB(0, 78, 148);
constexpr COLORREF kTerminalBg = RGB(30, 30, 30);
constexpr COLORREF kTerminalFg = RGB(204, 204, 204);
constexpr COLORREF kGreen = RGB(16, 124, 16);
constexpr COLORREF kRed = RGB(196, 43, 28);
constexpr COLORREF kAmber = RGB(157, 93, 0);
constexpr COLORREF kSpinnerTrack = RGB(120, 174, 229);

std::filesystem::path currentExecutable() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
}

// Opens a GUI tool like Explorer (ShellExecute "open") — same as a double-click.
bool launchToolProcess(const std::filesystem::path& tool, std::wstring* error) {
    std::error_code ec;
    if (!std::filesystem::exists(tool, ec)) {
        if (error) {
            *error = L"not found";
        }
        return false;
    }
    const std::wstring file = tool.wstring();
    const std::wstring dir = tool.parent_path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) == FALSE) {
        if (error) {
            *error = L"ShellExecute failed (" + std::to_wstring(GetLastError()) + L")";
        }
        return false;
    }
    if (info.hProcess != nullptr) {
        CloseHandle(info.hProcess);
    }
    return true;
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

std::wstring systemErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                                   nullptr);
    std::wstring message = L"error " + std::to_wstring(code);
    if (n != 0 && buffer != nullptr) {
        message += L": ";
        message += buffer;
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                    message.back() == L' ')) {
            message.pop_back();
        }
        LocalFree(buffer);
    }
    return message;
}

bool launchDetached(const std::filesystem::path& exe, const std::wstring& args,
                    std::wstring* error) {
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    if (!args.empty()) {
        command += L" ";
        command += args;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe.parent_path().wstring();
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       dir.empty() ? nullptr : dir.c_str(), &si, &pi) == 0) {
        if (error) {
            *error = systemErrorMessage(GetLastError());
        }
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

const wchar_t* kChooserHelp =
    L"ATUALIZAÇÕES WINDOWS\r\n"
    L"  • Instalar todas as atualizações — Windows + drivers\r\n"
    L"  • Atualizar sem drivers — só Windows\r\n"
    L"  • Interruptor abaixo — testes de stress após a atualização\r\n"
    L"\r\n"
    L"STRESS / MACRO\r\n"
    L"  Ferramentas: SEMPRE Ambiente de Trabalho\\programas de teste\r\n"
    L"  1. Gravar macro — abre o gravador (Iniciar gravação)\r\n"
    L"  2. Abrir só as ferramentas — abre as 5 apps (MemTest por cima)\r\n"
    L"  3. Clica manualmente nas apps enquanto grava\r\n"
    L"  4. F10 — guarda Ambiente de Trabalho\\macro.json\r\n"
    L"  5. Correr testes + macro — testa o replay automaticamente\r\n"
    L"  • Monitorizar sensores — OpenHardwareMonitor (+ BatteryCare)";

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

struct StressMessage {
    std::wstring text;
    bool final = false;
};

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

    // Write stress.ini and uia_stress.ps1 next to the executable so they can be
    // tuned on the target machine without a rebuild.
    Stress::ensureAssets(currentExecutable());
    // Default the chooser's "auto stress" toggle to the AutoRunStress setting.
    autoStressAfterUpdate_ = Stress::loadConfig(currentExecutable()).autoRunStress;
    updateStressToggleText();

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

    // The mode chooser is always shown, even when a previous run was interrupted
    // by a restart, so the user always has the "Install all updates" button.
    {
        StateManager stateManager;
        const auto savedState = stateManager.load();
        if (savedState.phase == L"waiting_reboot") {
            appendLog(L"An interrupted update was found — click an update button to continue.");
        }
    }

    // Otherwise the worker is only launched once the user picks an update mode,
    // so the window opens on the mode chooser instead of starting work immediately.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    if (stressThread_.joinable()) {
        stressThread_.join();
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
    if (numberFont_) {
        DeleteObject(numberFont_);
    }
    if (captionFont_) {
        DeleteObject(captionFont_);
    }
    if (buttonFont_) {
        DeleteObject(buttonFont_);
    }
    if (borderPen_) {
        DeleteObject(borderPen_);
    }
    if (appIcon_) {
        DestroyIcon(appIcon_);
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
    if (cardBrush_) {
        DeleteObject(cardBrush_);
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

void App::applyWindowChrome(HWND hwnd) {
    // Rounded corners + custom caption colours (Windows 11+). Older systems
    // simply ignore the calls, so no feature detection is required.
    constexpr DWORD kCornerAttribute = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD kCornerRound = 2;       // DWMWCP_ROUND
    DWORD corner = kCornerRound;
    DwmSetWindowAttribute(hwnd, kCornerAttribute, &corner, sizeof(corner));

    constexpr DWORD kCaptionAttribute = 35;  // DWMWA_CAPTION_COLOR
    COLORREF caption = kHeaderColor;
    DwmSetWindowAttribute(hwnd, kCaptionAttribute, &caption, sizeof(caption));

    constexpr DWORD kTextAttribute = 36;  // DWMWA_TEXT_COLOR
    COLORREF captionText = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, kTextAttribute, &captionText, sizeof(captionText));
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

    // Account for the drop shadow/border so the client area matches kWidth/kHeight.
    RECT desired{0, 0, kWidth, kHeight};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    const int winW = desired.right - desired.left;
    const int winH = desired.bottom - desired.top;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenW - winW) / 2;
    const int y = (screenH - winH) / 2;

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Win Auto Updater",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                WS_CLIPCHILDREN,
                            x, y, winW, winH, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }

    applyWindowChrome(hwnd_);

    titleFont_ = CreateFontW(28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    uiFont_ = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    smallFont_ = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    terminalFont_ = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
    numberFont_ = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    captionFont_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    buttonFont_ = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    terminalBrush_ = CreateSolidBrush(kTerminalBg);
    headerBrush_ = CreateSolidBrush(kHeaderColor);
    cardBrush_ = CreateSolidBrush(kCardColor);
    borderPen_ = CreatePen(PS_SOLID, 1, kBorderColor);
    appIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             44, 44, LR_DEFAULTCOLOR));

    headerTitle_ = CreateWindowExW(0, L"STATIC", L"Win Auto Updater", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);

    headerSubtitle_ = CreateWindowExW(0, L"STATIC", L"Instalador automático de atualizações Windows",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd_, nullptr,
                                      instance_, nullptr);

    phaseLabel_ = CreateWindowExW(0, L"STATIC", lastPhase_.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PHASE)), instance_,
                                  nullptr);

    currentLabel_ = CreateWindowExW(0, L"STATIC", lastCurrent_.c_str(),
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0,
                                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CURRENT)),
                                    instance_, nullptr);

    progressBar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                   0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)),
                                   instance_, nullptr);
    SendMessageW(progressBar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(progressBar_, PBM_SETPOS, 0, 0);
    SendMessageW(progressBar_, PBM_SETBARCOLOR, 0, kAccent);
    SendMessageW(progressBar_, PBM_SETBKCOLOR, 0, RGB(220, 226, 234));

    terminal_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TERMINAL)), instance_,
        nullptr);
    // Raise the default ~32K limit so long sessions don't corrupt the control.
    SendMessageW(terminal_, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);

    summaryLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd_,
                                    nullptr, instance_, nullptr);

    uninstallButton_ = CreateWindowExW(
        0, L"BUTTON", L"Desinstalar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UNINSTALL)), instance_, nullptr);

    closeButton_ = CreateWindowExW(
        0, L"BUTTON", L"Fechar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLOSE)), instance_, nullptr);

    updateButton_ = CreateWindowExW(
        0, L"BUTTON", L"Instalar todas as atualizações", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
        0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UPDATE)), instance_, nullptr);

    updateNoDriversButton_ = CreateWindowExW(
        0, L"BUTTON", L"Atualizar sem drivers", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UPDATE_NO_DRIVERS)), instance_,
        nullptr);

    runTestsButton_ = CreateWindowExW(
        0, L"BUTTON", L"Correr testes + macro", WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RUN_TESTS)), instance_, nullptr);

    closeReopenButton_ = CreateWindowExW(
        0, L"BUTTON", L"Fechar e reabrir", WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLOSE_REOPEN)), instance_, nullptr);

    dumpButton_ = nullptr;  // removido — botão de debug antigo

    recordButton_ = CreateWindowExW(
        0, L"BUTTON", L"Gravar macro", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RECORD_MACRO)), instance_, nullptr);

    monitorButton_ = CreateWindowExW(
        0, L"BUTTON", L"Monitorizar sensores", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MONITOR)), instance_, nullptr);

    openToolsButton_ = CreateWindowExW(
        0, L"BUTTON", L"Abrir só as ferramentas", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPEN_TOOLS)), instance_, nullptr);

    stressToggleButton_ = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STRESS_TOGGLE)), instance_, nullptr);

    reopenToggleButton_ = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REOPEN_TOGGLE)), instance_, nullptr);

    if (titleFont_) {
        SendMessageW(headerTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    }
    if (smallFont_) {
        SendMessageW(headerSubtitle_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (uiFont_) {
        SendMessageW(phaseLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
    if (smallFont_) {
        SendMessageW(currentLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (terminalFont_) {
        SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), TRUE);
    }
    if (smallFont_) {
        SendMessageW(summaryLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (buttonFont_) {
        SendMessageW(uninstallButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(closeButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(updateButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(updateNoDriversButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_),
                     TRUE);
        SendMessageW(runTestsButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(closeReopenButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(recordButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(monitorButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(openToolsButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(stressToggleButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
        SendMessageW(reopenToggleButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
    }
    updateReopenToggleText();

    // Close stays available on the chooser, and Uninstall is enabled too so the
    // tool can be removed right after a reopen (it is disabled again while an
    // update is running, in beginUpdate). Uninstall always asks for confirmation.
    EnableWindow(uninstallButton_, TRUE);
    allowClose_ = true;

    if (terminal_) {
        ShowWindow(terminal_, SW_HIDE);
    }
    if (progressBar_) {
        ShowWindow(progressBar_, SW_HIDE);
    }

    layoutControls();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    SetTimer(hwnd_, kAnimTimerId, kAnimIntervalMs, nullptr);

    setStatus(L"Pronto");
    setPhase(L"Início — atualizações e testes de stress");
    setCurrent(L"Escolhe o modo de atualização, ou usa os botões de stress/macro. Vê o guia.");
    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_, kChooserHelp);
        ShowWindow(summaryLabel_, SW_SHOW);
    }
    appendLog(L"Win Auto Updater iniciado.");
    appendLog(L"Pasta das ferramentas: Ambiente de Trabalho\\programas de teste (sempre)");
    appendLog(L"Macro: Gravar macro → Abrir só as ferramentas → clicar nas apps → F10 → "
              L"Correr testes + macro.");
    return true;
}

void App::layoutControls() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int margin = 24;
    const int contentW = width - margin * 2;
    const int gap = 14;
    const int buttonY = height - margin - kButtonHeight;

    if (headerTitle_) {
        MoveWindow(headerTitle_, margin + 58, 22, contentW - 130, 36, TRUE);
    }
    if (headerSubtitle_) {
        MoveWindow(headerSubtitle_, margin + 58, 62, contentW - 130, 24, TRUE);
    }
    spinnerRect_ = {width - 66, 34, width - 30, 70};

    if (choosing_) {
        const int chooseW = 420;
        const int centerX = (width - chooseW) / 2;
        const int leftCol = margin;
        const int helpW = contentW;
        int y = 118;

        if (phaseLabel_) {
            MoveWindow(phaseLabel_, leftCol, y, contentW, 28, TRUE);
        }
        y += 30;
        if (currentLabel_) {
            MoveWindow(currentLabel_, leftCol, y, contentW, 22, TRUE);
        }
        y += 30;

        // Update actions
        if (updateButton_) {
            MoveWindow(updateButton_, centerX, y, chooseW, 48, TRUE);
        }
        y += 48 + 10;
        if (updateNoDriversButton_) {
            MoveWindow(updateNoDriversButton_, centerX, y, chooseW, 40, TRUE);
        }
        y += 40 + 10;
        if (stressToggleButton_) {
            MoveWindow(stressToggleButton_, centerX, y, chooseW, 36, TRUE);
        }
        y += 36 + 14;

        // Stress / macro — two clear rows
        const int halfW = (chooseW - gap) / 2;
        if (recordButton_) {
            ShowWindow(recordButton_, SW_SHOW);
            MoveWindow(recordButton_, centerX, y, halfW, 40, TRUE);
        }
        if (openToolsButton_) {
            ShowWindow(openToolsButton_, SW_SHOW);
            MoveWindow(openToolsButton_, centerX + halfW + gap, y, halfW, 40, TRUE);
        }
        y += 40 + 8;
        if (runTestsButton_) {
            ShowWindow(runTestsButton_, SW_SHOW);
            EnableWindow(runTestsButton_, TRUE);
            MoveWindow(runTestsButton_, centerX, y, halfW, 40, TRUE);
        }
        if (monitorButton_) {
            ShowWindow(monitorButton_, SW_SHOW);
            MoveWindow(monitorButton_, centerX + halfW + gap, y, halfW, 40, TRUE);
        }
        y += 40 + 12;

        if (summaryLabel_) {
            ShowWindow(summaryLabel_, SW_SHOW);
            summaryCardRect_ = {margin, y, margin + helpW, buttonY - 12};
            MoveWindow(summaryLabel_, summaryCardRect_.left + 18, summaryCardRect_.top + 14,
                       summaryCardRect_.right - summaryCardRect_.left - 36,
                       summaryCardRect_.bottom - summaryCardRect_.top - 28, TRUE);
        }
        const int bottomW = 200;
        const int bottomGap = 12;
        const int bottomTotal = bottomW * 2 + bottomGap;
        const int bottomX = (width - bottomTotal) / 2;
        if (uninstallButton_) {
            MoveWindow(uninstallButton_, bottomX, buttonY, bottomW, kButtonHeight, TRUE);
        }
        if (closeButton_) {
            MoveWindow(closeButton_, bottomX + bottomW + bottomGap, buttonY, bottomW, kButtonHeight,
                       TRUE);
        }
        return;
    }

    if (uiDoneMode_) {
        // Ecrã final — 2 filas limpas (sem Dump UI):
        //   [Abrir ferramentas] [Correr testes + macro] [Sensores]
        //   [Gravar macro] [Fechar e reabrir] [Desinstalar] [Fechar]
        const int actionH = 44;
        const int bottomY = buttonY;
        const int actionY = bottomY - actionH - 12;
        const int reopenH = 38;
        const int reopenW = 340;
        const int reopenY = actionY - reopenH - 10;
        const int actionW = (contentW - gap * 2) / 3;
        const int actionStartX = margin;
        const int bottomCount = 4;
        const int bottomW = (contentW - gap * (bottomCount - 1)) / bottomCount;
        const int bottomStartX = margin;

        if (phaseLabel_) {
            MoveWindow(phaseLabel_, margin, 120, contentW, 30, TRUE);
        }
        if (summaryLabel_) {
            summaryCardRect_ = {margin, 162, margin + contentW, reopenY - 14};
            MoveWindow(summaryLabel_, summaryCardRect_.left + 26, summaryCardRect_.top + 24,
                       summaryCardRect_.right - summaryCardRect_.left - 52,
                       summaryCardRect_.bottom - summaryCardRect_.top - 48, TRUE);
        }
        if (reopenToggleButton_) {
            MoveWindow(reopenToggleButton_, (width - reopenW) / 2, reopenY, reopenW, reopenH, TRUE);
        }
        if (openToolsButton_) {
            MoveWindow(openToolsButton_, actionStartX, actionY, actionW, actionH, TRUE);
        }
        if (runTestsButton_) {
            MoveWindow(runTestsButton_, actionStartX + actionW + gap, actionY, actionW, actionH,
                       TRUE);
        }
        if (monitorButton_) {
            MoveWindow(monitorButton_, actionStartX + (actionW + gap) * 2, actionY, actionW, actionH,
                       TRUE);
        }
        if (recordButton_) {
            ShowWindow(recordButton_, SW_SHOW);
            MoveWindow(recordButton_, bottomStartX, bottomY, bottomW, kButtonHeight, TRUE);
        }
        if (closeReopenButton_) {
            MoveWindow(closeReopenButton_, bottomStartX + bottomW + gap, bottomY, bottomW,
                       kButtonHeight, TRUE);
        }
        if (uninstallButton_) {
            MoveWindow(uninstallButton_, bottomStartX + (bottomW + gap) * 2, bottomY, bottomW,
                       kButtonHeight, TRUE);
        }
        if (closeButton_) {
            MoveWindow(closeButton_, bottomStartX + (bottomW + gap) * 3, bottomY, bottomW,
                       kButtonHeight, TRUE);
        }
        return;
    }

    if (phaseLabel_) {
        MoveWindow(phaseLabel_, margin, 118, contentW, 30, TRUE);
    }
    if (currentLabel_) {
        MoveWindow(currentLabel_, margin, 152, contentW, 24, TRUE);
    }
    if (progressBar_) {
        MoveWindow(progressBar_, margin, 186, contentW, 12, TRUE);
    }

    statsRect_ = {margin, 210, margin + contentW, 210 + 78};

    const int terminalTop = 304;
    terminalCardRect_ = {margin, terminalTop, margin + contentW, buttonY - 18};
    if (terminal_) {
        MoveWindow(terminal_, terminalCardRect_.left + 12, terminalCardRect_.top + 12,
                   terminalCardRect_.right - terminalCardRect_.left - 24,
                   terminalCardRect_.bottom - terminalCardRect_.top - 24, TRUE);
    }

    // Durante a atualização: Desinstalar (desativado) + Fechar (desativado).
    const int midW = 200;
    const int midGap = 12;
    const int midTotal = midW * 2 + midGap;
    const int midX = (width - midTotal) / 2;
    if (uninstallButton_) {
        MoveWindow(uninstallButton_, midX, buttonY, midW, kButtonHeight, TRUE);
    }
    if (closeButton_) {
        MoveWindow(closeButton_, midX + midW + midGap, buttonY, midW, kButtonHeight, TRUE);
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

void App::showActionButtons(bool enabled) {
    if (closeButton_) {
        ShowWindow(closeButton_, SW_SHOW);
        EnableWindow(closeButton_, enabled ? TRUE : FALSE);
        SetWindowTextW(closeButton_, L"Fechar");
    }
    if (uninstallButton_) {
        ShowWindow(uninstallButton_, SW_SHOW);
        EnableWindow(uninstallButton_, enabled ? TRUE : FALSE);
        SetWindowTextW(uninstallButton_, L"Desinstalar");
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
    const int lineIndex = static_cast<int>(SendMessageW(terminal_, EM_LINEFROMCHAR, keepFrom, 0));
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
    if (percent == lastPct && (now - lastMs) < 120) {
        return false;
    }
    // Much tighter throttle than before so the bar/label visibly track reality.
    if ((now - lastMs) < 60 && percent != 100) {
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

    KillTimer(hwnd_, kAnimTimerId);

    if (terminal_) {
        ShowWindow(terminal_, SW_HIDE);
    }
    if (progressBar_) {
        ShowWindow(progressBar_, SW_HIDE);
    }
    if (currentLabel_) {
        ShowWindow(currentLabel_, SW_HIDE);
    }
    if (stressToggleButton_) {
        ShowWindow(stressToggleButton_, SW_HIDE);
    }

    setPhase(success ? L"Concluído — este PC está pronto"
                     : L"Interrompido — ocorreu um erro");

    std::wstring summary;
    if (success) {
        summary = L"Todas as atualizações Windows disponíveis foram instaladas.\r\n\r\n";
    } else {
        summary = detail.empty() ? L"Ocorreu um erro durante o processo de atualização." : detail;
        summary += L"\r\n\r\n";
    }

    summary += L"Encontradas: " + std::to_wstring(uiStats_.found) + L"     Transferidas: " +
               std::to_wstring(uiStats_.downloaded) + L"     Instaladas: " +
               std::to_wstring(uiStats_.installed) + L"\r\n";
    summary += L"Concluídas: " + std::to_wstring(uiStats_.finished) + L"     Ignoradas: " +
               std::to_wstring(uiStats_.skipped) + L"     Falhadas: " +
               std::to_wstring(uiStats_.failed) + L"\r\n\r\n";

    if (uiStats_.skipped > 0) {
        summary +=
            L"A atualização do antivírus é ignorada de propósito — atualiza as definições do "
            L"Microsoft Defender manualmente em Segurança do Windows.\r\n\r\n";
    }

    if (!postSummary_.empty()) {
        summary += L"Configuração pós-atualização:\r\n" + postSummary_ + L"\r\n";
    }

    if (updateWithoutDrivers_) {
        summary +=
            L"O Windows Update foi configurado para não instalar drivers de hardware nesta "
            L"execução.\r\n\r\n";
    }

    if (success) {
        summary += L"Podes fechar esta janela, ou desinstalar a ferramenta.";
    }

    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_, summary.c_str());
        ShowWindow(summaryLabel_, SW_SHOW);
    }
    doneSummary_ = summary;
    stressLog_.clear();

    if (runTestsButton_) {
        if (success) {
            ShowWindow(runTestsButton_, SW_SHOW);
            EnableWindow(runTestsButton_, TRUE);
            SetWindowTextW(runTestsButton_, L"Correr testes + macro");
        } else {
            ShowWindow(runTestsButton_, SW_HIDE);
        }
    }
    if (openToolsButton_) {
        if (success) {
            ShowWindow(openToolsButton_, SW_SHOW);
            EnableWindow(openToolsButton_, TRUE);
            SetWindowTextW(openToolsButton_, L"Abrir só as ferramentas");
        } else {
            ShowWindow(openToolsButton_, SW_HIDE);
        }
    }
    if (monitorButton_) {
        if (success) {
            ShowWindow(monitorButton_, SW_SHOW);
            EnableWindow(monitorButton_, TRUE);
            SetWindowTextW(monitorButton_, L"Monitorizar sensores");
        } else {
            ShowWindow(monitorButton_, SW_HIDE);
        }
    }
    if (recordButton_ && success) {
        ShowWindow(recordButton_, SW_SHOW);
        EnableWindow(recordButton_, TRUE);
        SetWindowTextW(recordButton_, L"Gravar macro");
    } else if (recordButton_) {
        ShowWindow(recordButton_, SW_HIDE);
    }
    if (reopenToggleButton_) {
        if (success) {
            // Reflect the real state of the logon task (it is not stored in memory
            // across launches): ON only if the tool will actually reopen.
            reopenAfterRestart_ = Scheduler(currentExecutable()).exists();
            updateReopenToggleText();
            ShowWindow(reopenToggleButton_, SW_SHOW);
            EnableWindow(reopenToggleButton_, TRUE);
        } else {
            ShowWindow(reopenToggleButton_, SW_HIDE);
        }
    }
    if (closeReopenButton_) {
        ShowWindow(closeReopenButton_, SW_SHOW);
        EnableWindow(closeReopenButton_, TRUE);
    }

    showActionButtons(true);
    layoutControls();
    InvalidateRect(hwnd_, nullptr, TRUE);

    // "After update: run stress tests" — kick off the bench once the update ends.
    if (success && autoStressAfterUpdate_ && !autoStressStarted_) {
        autoStressStarted_ = true;
        runStressTests();
    }
}

void App::performUninstall() {
    const int confirm =
        MessageBoxW(hwnd_,
                    L"Remove the scheduled reopen task and uninstall this tool?\r\n\r\n"
                    L"SingleUpdate.exe on the USB is kept until the next restart.",
                    L"Win Auto Updater", MB_OKCANCEL | MB_ICONQUESTION);
    if (confirm != IDOK) {
        return;
    }

    if (uninstallButton_) {
        EnableWindow(uninstallButton_, FALSE);
        SetWindowTextW(uninstallButton_, L"A desinstalar...");
    }
    applyHint(L"A desinstalar — a remover a tarefa agendada...");

    // Task Scheduler COM can block for seconds on the UI thread — do it off-thread.
    const auto exe = currentExecutable();
    std::thread([this, exe] {
        Scheduler scheduler(exe);
        scheduler.remove();
        StateManager stateManager;
        stateManager.remove();
        Cleanup::removeTempFiles();
        if (!exe.empty()) {
            MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        PostMessageW(hwnd_, WM_APP_UNINSTALL_DONE, 0, 0);
    }).detach();
}

void App::startWorker() {
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
}

void App::beginUpdate(bool withoutDrivers) {
    if (!choosing_) {
        return;
    }
    choosing_ = false;
    updateWithoutDrivers_ = withoutDrivers;

    if (updateButton_) {
        ShowWindow(updateButton_, SW_HIDE);
    }
    if (updateNoDriversButton_) {
        ShowWindow(updateNoDriversButton_, SW_HIDE);
    }
    if (stressToggleButton_) {
        ShowWindow(stressToggleButton_, SW_HIDE);
    }
    if (monitorButton_) {
        ShowWindow(monitorButton_, SW_HIDE);
    }
    if (openToolsButton_) {
        ShowWindow(openToolsButton_, SW_HIDE);
    }
    if (runTestsButton_) {
        ShowWindow(runTestsButton_, SW_HIDE);
    }
    if (recordButton_) {
        ShowWindow(recordButton_, SW_HIDE);
    }
    if (summaryLabel_) {
        ShowWindow(summaryLabel_, SW_HIDE);
    }
    if (terminal_) {
        ShowWindow(terminal_, SW_SHOW);
    }
    if (progressBar_) {
        ShowWindow(progressBar_, SW_SHOW);
    }
    if (currentLabel_) {
        ShowWindow(currentLabel_, SW_SHOW);
    }

    allowClose_ = false;
    EnableWindow(closeButton_, FALSE);
    EnableWindow(uninstallButton_, FALSE);

    setPhase(L"A iniciar");
    setCurrent(L"A preparar o Win Auto Updater...");
    layoutControls();
    InvalidateRect(hwnd_, nullptr, TRUE);

    startWorker();
}

void App::runPostUpdateTasks() {
    postSummary_.clear();

    logger_.status(L"Finalizing this computer...");
    postText(hwnd_, WM_APP_CURRENT, L"Setting the recommended display resolution...");
    {
        std::wstring detail;
        Display::setMaxResolution(&detail);
        logger_.log(L"Ecrã: " + detail);
        postSummary_ += L"Ecrã: " + detail + L"\r\n";
    }

    logger_.status(L"A preparar drivers gráficos...");
    postText(hwnd_, WM_APP_CURRENT, L"A copiar o instalador AMD / NVIDIA para o Ambiente de Trabalho...");
    {
        const Drivers::Result driver = Drivers::downloadForSystem();
        logger_.log(L"Gráficos: " + driver.message);
        const std::wstring label =
            driver.detected ? driver.vendorName : std::wstring(L"não detetado");
        postSummary_ += L"Gráficos (" + label + L"): " + driver.message + L"\r\n";
    }
}

void App::runStressTests() {
    if (stressRunning_ || dumping_) {
        return;
    }
    if (recording_) {
        // Stop silently: the user should have pressed F10 first; don't open files here.
        recorder_.stop();
        recording_ = false;
        if (recordButton_) {
            SetWindowTextW(recordButton_, L"Gravar macro");
        }
    }
    stressRunning_ = true;
    stressLog_.clear();

    if (runTestsButton_) {
        EnableWindow(runTestsButton_, FALSE);
        SetWindowTextW(runTestsButton_, L"Testes a correr...");
    }
    if (openToolsButton_) {
        EnableWindow(openToolsButton_, FALSE);
    }
    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_,
                       (doneSummary_ + L"\r\n\r\nTestes de stress:\r\nA iniciar a bancada "
                                        L"de testes + macro...").c_str());
    }

    if (stressThread_.joinable()) {
        stressThread_.join();
    }
    stressThread_ = std::thread([this] {
        const auto post = [this](std::wstring text, bool final) {
            auto* msg = new StressMessage{std::move(text), final};
            if (!PostMessageW(hwnd_, WM_APP_STRESS, 0, reinterpret_cast<LPARAM>(msg))) {
                delete msg;
            }
        };

        Stress::Callbacks callbacks;
        callbacks.log = [&](const std::wstring& m) { post(L"• " + m, false); };
        callbacks.status = [&](const std::wstring& m) { post(m, false); };

        try {
            // Set the maximum display resolution first so fullscreen tools
            // (FurMark, etc.) and the recorded macro coordinates line up.
            post(L"• A definir a resolução máxima do ecrã...", false);
            std::wstring displayDetail;
            Display::setMaxResolution(&displayDetail);
            post(L"• Ecrã: " + displayDetail, false);

            const auto config = Stress::loadConfig(currentExecutable());
            const Stress::Result result = Stress::run(config, callbacks, true);
            post(L"Concluído. " + result.message, true);
        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            post(L"Testes de stress falharam: " + std::wstring(what.begin(), what.end()), true);
        } catch (...) {
            post(L"Testes de stress falharam (erro desconhecido).", true);
        }
    });
}

void App::openTestTools() {
    if (stressRunning_ || dumping_) {
        return;
    }
    if (recording_) {
        // Keep recording — opening tools is part of the capture workflow.
    }
    stressRunning_ = true;
    stressLog_.clear();

    if (openToolsButton_) {
        EnableWindow(openToolsButton_, FALSE);
        SetWindowTextW(openToolsButton_, L"A abrir...");
    }
    if (runTestsButton_) {
        EnableWindow(runTestsButton_, FALSE);
    }
    if (summaryLabel_) {
        SetWindowTextW(summaryLabel_,
                       (doneSummary_ + L"\r\n\r\nA abrir as ferramentas de teste (sem macro)...").c_str());
    }
    applyHint(L"A abrir as ferramentas de teste (MemTest por último, em cima). Sem replay.");

    if (stressThread_.joinable()) {
        stressThread_.join();
    }
    stressThread_ = std::thread([this] {
        const auto post = [this](std::wstring text, bool final) {
            auto* msg = new StressMessage{std::move(text), final};
            if (!PostMessageW(hwnd_, WM_APP_STRESS, 0, reinterpret_cast<LPARAM>(msg))) {
                delete msg;
            }
        };

        Stress::Callbacks callbacks;
        callbacks.log = [&](const std::wstring& m) { post(L"• " + m, false); };
        callbacks.status = [&](const std::wstring& m) { post(m, false); };

        try {
            const auto config = Stress::loadConfig(currentExecutable());
            const Stress::Result result = Stress::launchTools(config, callbacks);
            post(L"Done. " + result.message, true);
            if (!result.ok) {
                const std::wstring alert =
                    L"Nao foi possivel abrir as ferramentas de teste.\r\n\r\n"
                    L"Pasta usada (Ambiente de Trabalho):\r\n" +
                    result.baseDir +
                    L"\r\n\r\n" + result.message +
                    L"\r\n\r\nConfirma que Desktop\\programas de teste existe e tem:\r\n"
                    L"  FurMark2_x64\\FurMark_GUI.exe\r\n"
                    L"  Prime95\\prime95.exe\r\n"
                    L"  CrystalDiskMark9\\DiskMark64.exe\r\n"
                    L"  powerMAX\\powerMAX.exe  (ou powerMAX_x64.exe)\r\n"
                    L"  memtest.exe";
                MessageBoxW(hwnd_, alert.c_str(), L"Abrir só as ferramentas", MB_OK | MB_ICONWARNING);
            }
        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            post(L"Falha ao abrir ferramentas: " + std::wstring(what.begin(), what.end()), true);
        } catch (...) {
            post(L"Falha ao abrir ferramentas (erro desconhecido).", true);
        }
    });
}

void App::runUiDump() {
    // Botão removido da UI — mantido vazio por compatibilidade.
}

// "Monitor" — OpenHardwareMonitor, plus BatteryCare on laptops. These are not
// part of the automated bench; they only run when the user asks for them.
void App::launchMonitorTools() {
    const std::filesystem::path base = Stress::toolsBaseDir();
    const auto launch = [this](const std::wstring& label, const std::filesystem::path& tool) {
        std::wstring error;
        if (launchToolProcess(tool, &error)) {
            appendLog(L"Started " + label + L".");
        } else {
            appendLog(L"Não foi possível iniciar " + label + L": " + error + L" (" +
                      tool.wstring() + L")");
        }
    };
    launch(L"OpenHardwareMonitor", base / L"OpenHardwareMonitor" / L"OpenHardwareMonitor.exe");
    if (Stress::detectPortable()) {
        launch(L"BatteryCare", base / L"batterycare" / L"BatteryCare.exe");
    }
}

void App::updateStressToggleText() {
    if (stressToggleButton_) {
        SetWindowTextW(stressToggleButton_,
                       autoStressAfterUpdate_ ? L"Após atualizar: correr testes de stress"
                                              : L"Após atualizar: parar");
    }
}

void App::updateReopenToggleText() {
    if (reopenToggleButton_) {
        SetWindowTextW(reopenToggleButton_, L"Reabrir após reinício");
    }
}

void App::applyHint(const std::wstring& hint) {
    if (uiDoneMode_) {
        if (summaryLabel_) {
            SetWindowTextW(summaryLabel_, (doneSummary_ + L"\r\n\r\n" + hint).c_str());
        }
    } else if (currentLabel_) {
        SetWindowTextW(currentLabel_, hint.c_str());
    }
}

void App::toggleRecording() {
    if (stressRunning_ || dumping_) {
        return;
    }

    const auto exe = currentExecutable();
    const auto config = Stress::loadConfig(exe);
    const std::wstring engine = config.macroEngine;
    const bool python =
        engine.empty() || _wcsicmp(engine.c_str(), L"python") == 0 ||
        _wcsicmp(engine.c_str(), L"py") == 0 || _wcsicmp(engine.c_str(), L"helper") == 0;

    // Default: launch MacroHelper.exe recording GUI (Python).
    if (python) {
        if (recording_) {
            finishRecording();
        }
        const auto helper = Stress::macroHelperPath(exe);
        std::error_code ec;
        if (!std::filesystem::exists(helper, ec)) {
            MessageBoxW(hwnd_,
                        L"MacroHelper.exe não foi encontrado junto ao SingleUpdate.exe.\r\n"
                        L"Copia o MacroHelper.exe para a pen (mesma pasta).",
                        L"Win Auto Updater", MB_OK | MB_ICONWARNING);
            return;
        }
        std::filesystem::path macroFile = Stress::desktopDir() / L"macro.json";
        if (macroFile.parent_path().empty()) {
            macroFile = Stress::macroFilePath(exe, config);
        }
        const std::wstring args = L"--record --macro \"" + macroFile.wstring() + L"\"";
        std::wstring launchError;
        if (!launchDetached(helper, args, &launchError)) {
            MessageBoxW(hwnd_,
                        (L"Não foi possível iniciar o MacroHelper.exe.\r\n\r\n" + launchError +
                         L"\r\n\r\nCaminho: " + helper.wstring())
                            .c_str(),
                        L"Win Auto Updater", MB_OK | MB_ICONERROR);
            return;
        }
        applyHint(L"MacroHelper: Iniciar gravação → Abrir só as ferramentas → clicar → F10. "
                  L"Guarda: " +
                  macroFile.wstring());
        appendLog(L"MacroHelper iniciado para gravação → " + macroFile.wstring());
        return;
    }

    // Fallback: built-in C++ recorder (macro.txt).
    if (recording_) {
        finishRecording();
        return;
    }

    recorder_.clear();
    if (!recorder_.start(hwnd_)) {
        MessageBoxW(hwnd_,
                    L"Não foi possível iniciar a gravação: os hooks de input foram recusados. "
                    L"Há outra ferramenta a bloquear hooks globais?",
                    L"Win Auto Updater", MB_OK | MB_ICONWARNING);
        return;
    }

    recording_ = true;
    if (recordButton_) {
        SetWindowTextW(recordButton_, L"Parar (F10)");
    }
    applyHint(L"A gravar macro (C++): faz todos os cliques e teclas, depois F10 para parar.");
}

void App::finishRecording() {
    if (!recording_) {
        return;
    }
    recorder_.stop();
    recording_ = false;

    if (recordButton_) {
        SetWindowTextW(recordButton_, L"Gravar macro");
    }

    const auto events = recorder_.events();
    if (events.empty()) {
        applyHint(L"Gravação parada. Nada foi capturado.");
        MessageBoxW(hwnd_, L"Nada foi gravado.", L"Win Auto Updater", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto file = Macro::filePath(currentExecutable());
    if (!Macro::save(file, events)) {
        applyHint(L"Gravação parada. Não foi possível escrever o ficheiro da macro.");
        MessageBoxW(hwnd_, L"Não foi possível escrever o ficheiro da macro junto ao executável.",
                    L"Win Auto Updater", MB_OK | MB_ICONWARNING);
        return;
    }

    applyHint(L"Macro guardada: " + file.wstring() + L" (" + std::to_wstring(events.size()) +
              L" passos). \"Correr testes + macro\" vai reproduzi-la.");
    ShellExecuteW(nullptr, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::closeAndReopen() {
    const auto exe = currentExecutable();
    Scheduler scheduler(exe);
    std::wstring error;
    if (!scheduler.armReopen(&error)) {
        const std::wstring message =
            L"A ferramenta vai fechar, mas não foi possível agendar a reabertura no "
            L"próximo reinício:\r\n" +
            error;
        MessageBoxW(hwnd_, message.c_str(), L"Win Auto Updater", MB_OK | MB_ICONWARNING);
    }

    allowClose_ = true;
    running_ = false;
    DestroyWindow(hwnd_);
}

void App::worker() {
    logger_.status(L"A verificar a ligação à Internet...");
    postText(hwnd_, WM_APP_CURRENT, L"À espera de ligação de rede...");
    logger_.log(L"Worker do Win Auto Updater iniciado.");

    if (updateWithoutDrivers_) {
        std::wstring policyDetail;
        if (DriverPolicy::excludeDriversFromWindowsUpdate(&policyDetail)) {
            logger_.log(L"Política de drivers: " + policyDetail);
            postText(hwnd_, WM_APP_CURRENT, L"O Windows Update vai ignorar atualizações de drivers.");
        } else {
            logger_.log(L"Aviso de política de drivers: " + policyDetail);
        }
    } else {
        logger_.log(L"Os drivers de hardware podem ser atualizados pelo Windows Update.");
    }

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

    engine.setStatsCallback([this](const UpdateStats& snapshot) {
        stats_ = snapshot;
        auto* copy = new UpdateStats(snapshot);
        if (!PostMessageW(hwnd_, WM_APP_STATS, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
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
        finalVerificationDone_ = cycle.finalVerificationDone || finalVerificationDone_;

        if (cycle.upToDate) {
            runPostUpdateTasks();
            cleanupDone_ = true;
            logSummary(L"Success");
            logger_.log(L"System is up to date. Cleaning up...");
            // Keep the logon task so the tool reopens after a restart until it is
            // uninstalled (or the "Reopen after restart" toggle is turned off).
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
            state.withoutDrivers = updateWithoutDrivers_;
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
    add(L"Updates found: " + std::to_wstring(stats_.found));
    add(L"Updates downloaded: " + std::to_wstring(stats_.downloaded));
    add(L"Updates installed: " + std::to_wstring(stats_.installed));
    add(L"Updates skipped: " + std::to_wstring(stats_.skipped));
    add(L"Updates failed: " + std::to_wstring(stats_.failed));
    add(L"Updates finished: " + std::to_wstring(stats_.finished));
    add(L"Pending restart: " + std::wstring(hadPendingRestart_ ? L"Yes" : L"No"));
    add(L"Final verification: " + std::wstring(finalVerificationDone_ ? L"Passed" : L"Skipped"));
    add(L"Cleanup: " + std::wstring(cleanupDone_ ? L"Completed" : L"Skipped"));
    add(L"Result: " + result);
    add(L"");
}

void App::requestReboot() {
    setPhase(L"Reinício necessário");
    setCurrent(L"O Windows vai reiniciar automaticamente para continuar as atualizações.");
    setProgressPercent(100);
    logger_.status(L"A reiniciar...");
    logger_.log(L"A pedir reinício do sistema...");
    appendLog(L"É necessário reiniciar para continuar a instalar atualizações.");
    appendLog(L"O Windows vai reiniciar dentro de 15 segundos. Guarda o teu trabalho.");

    rebootCountdown_ = 15;
    setCurrent(L"A reiniciar em " + std::to_wstring(rebootCountdown_) +
               L" segundos... Guarda o teu trabalho.");
    SetTimer(hwnd_, kRebootTimerId, 1000, nullptr);
}

void App::paintRoundRect(HDC hdc, const RECT& rect, int radius, HBRUSH fill) {
    HGDIOBJ oldBrush =
        SelectObject(hdc, fill ? fill : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HGDIOBJ oldPen =
        SelectObject(hdc, borderPen_ ? borderPen_ : static_cast<HPEN>(GetStockObject(NULL_PEN)));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}

void App::paintSpinner(HDC hdc) {
    if (spinnerRect_.right <= spinnerRect_.left) {
        return;
    }
    const int cx = (spinnerRect_.left + spinnerRect_.right) / 2;
    const int cy = (spinnerRect_.top + spinnerRect_.bottom) / 2;
    const int radius = 13;

    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    HPEN trackPen = CreatePen(PS_SOLID, 3, kSpinnerTrack);
    HGDIOBJ oldPen = SelectObject(hdc, trackPen);
    Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);

    HPEN arcPen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    SelectObject(hdc, arcPen);
    const int start = (animFrame_ * 22) % 360;
    const int arcRadius = radius - 3;
    const double startRad = static_cast<double>(start) * 3.14159265358979323846 / 180.0;
    // Position the pen exactly on the arc start so AngleArc does not draw a
    // radial connector line back to the centre.
    MoveToEx(hdc, cx + static_cast<int>(arcRadius * std::cos(startRad)),
             cy - static_cast<int>(arcRadius * std::sin(startRad)), nullptr);
    AngleArc(hdc, cx, cy, static_cast<FLOAT>(arcRadius), static_cast<FLOAT>(start), 110.0f);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(trackPen);
    DeleteObject(arcPen);
}

void App::paintStats(HDC hdc) {
    struct StatDef {
        const wchar_t* caption;
        int value;
        COLORREF color;
    };
    const StatDef defs[kStatCardCount] = {
        {L"Encontradas", uiStats_.found, kTextPrimary},
        {L"Transferidas", uiStats_.downloaded, kAccent},
        {L"Instaladas", uiStats_.installed, kGreen},
        {L"Concluídas", uiStats_.finished, kTextPrimary},
        {L"Ignoradas", uiStats_.skipped, kAmber},
        {L"Falhadas", uiStats_.failed, kRed},
    };

    const int gap = 12;
    const int totalW = statsRect_.right - statsRect_.left;
    const int cardW = (totalW - gap * (kStatCardCount - 1)) / kStatCardCount;
    if (cardW <= 0) {
        return;
    }

    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < kStatCardCount; ++i) {
        const int left = statsRect_.left + i * (cardW + gap);
        RECT card{left, statsRect_.top, left + cardW, statsRect_.bottom};
        paintRoundRect(hdc, card, 12, cardBrush_);

        RECT capRect = card;
        capRect.top += 10;
        capRect.bottom = capRect.top + 22;
        SelectObject(hdc, captionFont_ ? captionFont_ : smallFont_);
        SetTextColor(hdc, kTextSecondary);
        DrawTextW(hdc, defs[i].caption, -1, &capRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT valRect = card;
        valRect.top = card.top + 30;
        valRect.bottom = card.bottom - 6;
        SelectObject(hdc, numberFont_ ? numberFont_ : uiFont_);
        SetTextColor(hdc, defs[i].color);
        const std::wstring value = std::to_wstring(defs[i].value);
        DrawTextW(hdc, value.c_str(), -1, &valRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

void App::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    FillRect(mem, &rc, bgBrush_ ? bgBrush_ : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    RECT header{0, 0, w, kHeaderHeight};
    FillRect(mem, &header, headerBrush_ ? headerBrush_ : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    if (appIcon_) {
        DrawIconEx(mem, 24, 28, appIcon_, 44, 44, 0, nullptr, DI_NORMAL);
    }

    if (uiDoneMode_ || choosing_) {
        if (summaryCardRect_.right > summaryCardRect_.left) {
            paintRoundRect(mem, summaryCardRect_, 14, cardBrush_);
        }
    } else {
        paintStats(mem);
        if (terminalCardRect_.right > terminalCardRect_.left) {
            paintRoundRect(mem, terminalCardRect_, 14, terminalBrush_);
        }
        paintSpinner(mem);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void App::drawButton(const DRAWITEMSTRUCT* item) {
    if (!item) {
        return;
    }
    HDC hdc = item->hDC;
    RECT r = item->rcItem;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;

    // Toggles are drawn as a labelled switch: [ label ............ (ON)  o ].
    if (item->CtlID == IDC_STRESS_TOGGLE || item->CtlID == IDC_REOPEN_TOGGLE) {
        const bool on = item->CtlID == IDC_STRESS_TOGGLE ? autoStressAfterUpdate_
                                                         : reopenAfterRestart_;
        FillRect(hdc, &r, bgBrush_ ? bgBrush_ : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        InflateRect(&r, -1, -1);
        SetBkMode(hdc, TRANSPARENT);

        // Card background + border so it reads as a control.
        HBRUSH fillBrush = CreateSolidBrush(disabled ? RGB(245, 245, 245) : kCardColor);
        HPEN borderPen = CreatePen(PS_SOLID, 1, on ? kAccent : RGB(200, 200, 200));
        HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(fillBrush);
        DeleteObject(borderPen);

        // Right-aligned switch layout: [ label ......  ON/OFF  ( o ) ].
        const int rightPad = 12;
        const int trackH = 22;
        const int trackW = 48;
        const int trackTop = (r.top + r.bottom - trackH) / 2;
        const int trackRight = r.right - rightPad;
        const int trackLeft = trackRight - trackW;

        const int badgeW = 42;
        const int badgeRight = trackLeft - 8;
        const int badgeLeft = badgeRight - badgeW;

        // Label on the left, ending before the badge.
        wchar_t label[128]{};
        GetWindowTextW(item->hwndItem, label, 128);
        RECT textRect = r;
        textRect.left += 14;
        textRect.right = badgeLeft - 8;
        SelectObject(hdc, smallFont_ ? smallFont_ : buttonFont_);
        SetTextColor(hdc, disabled ? RGB(152, 152, 152) : kTextPrimary);
        DrawTextW(hdc, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Switch track.
        RECT track{trackLeft, trackTop, trackRight, trackTop + trackH};
        HBRUSH trackBrush = CreateSolidBrush(on ? kGreen : RGB(209, 209, 209));
        HGDIOBJ oldTrackBrush = SelectObject(hdc, trackBrush);
        HPEN nullPen = CreatePen(PS_SOLID, 1, on ? kGreen : RGB(209, 209, 209));
        HGDIOBJ oldTrackPen = SelectObject(hdc, nullPen);
        RoundRect(hdc, track.left, track.top, track.right, track.bottom, trackH / 2, trackH / 2);
        SelectObject(hdc, oldTrackBrush);
        SelectObject(hdc, oldTrackPen);
        DeleteObject(trackBrush);
        DeleteObject(nullPen);

        // Knob.
        const int knobSize = 18;
        const int knobY = trackTop + (trackH - knobSize) / 2;
        const int knobX = on ? (trackRight - knobSize - 2) : (trackLeft + 2);
        RECT knob{knobX, knobY, knobX + knobSize, knobY + knobSize};
        HBRUSH knobBrush = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ oldKnobBrush = SelectObject(hdc, knobBrush);
        HGDIOBJ oldKnobPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, knob.left, knob.top, knob.right, knob.bottom);
        SelectObject(hdc, oldKnobBrush);
        SelectObject(hdc, oldKnobPen);
        DeleteObject(knobBrush);

        // ON/OFF text badge to the left of the track.
        RECT badge{badgeLeft, r.top, badgeRight, r.bottom};
        SetTextColor(hdc, on ? kGreen : RGB(120, 120, 120));
        DrawTextW(hdc, on ? L"ON" : L"OFF", -1, &badge, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    bool primary = item->CtlID == IDC_CLOSE || item->CtlID == IDC_UPDATE ||
                   item->CtlID == IDC_RUN_TESTS;

    FillRect(hdc, &r, bgBrush_ ? bgBrush_ : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    InflateRect(&r, -1, -1);
    SetBkMode(hdc, TRANSPARENT);

    COLORREF fill = kCardColor;
    COLORREF border = RGB(200, 200, 200);
    COLORREF text = kTextPrimary;

    if (primary) {
        fill = disabled ? RGB(196, 196, 196) : (pressed ? kAccentDark : kAccent);
        border = fill;
        text = RGB(255, 255, 255);
    } else {
        fill = pressed ? RGB(237, 237, 237) : (disabled ? RGB(245, 245, 245) : kCardColor);
        border = disabled ? RGB(228, 228, 228) : RGB(200, 200, 200);
        text = disabled ? RGB(152, 152, 152) : kTextPrimary;
    }

    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 10, 10);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);

    wchar_t label[128]{};
    GetWindowTextW(item->hwndItem, label, 128);
    SelectObject(hdc, buttonFont_ ? buttonFont_ : smallFont_);
    SetTextColor(hdc, text);
    DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        case WM_ERASEBKGND:
            // We paint the whole background ourselves (double buffered).
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_DRAWITEM:
            drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_CTLCOLORSTATIC: {
            const auto child = reinterpret_cast<HWND>(lParam);
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            if (child == headerTitle_) {
                SetTextColor(hdc, kHeaderText);
                return reinterpret_cast<LRESULT>(headerBrush_);
            }
            if (child == headerSubtitle_) {
                SetTextColor(hdc, kHeaderSubText);
                return reinterpret_cast<LRESULT>(headerBrush_);
            }
            if (child == summaryLabel_) {
                SetTextColor(hdc, kTextPrimary);
                return reinterpret_cast<LRESULT>(cardBrush_);
            }
            if (child == currentLabel_) {
                SetTextColor(hdc, kTextSecondary);
            } else {
                SetTextColor(hdc, kTextPrimary);
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
                case IDC_UPDATE:
                    beginUpdate(false);
                    return 0;
                case IDC_STRESS_TOGGLE:
                    if (choosing_) {
                        autoStressAfterUpdate_ = !autoStressAfterUpdate_;
                        updateStressToggleText();
                        InvalidateRect(stressToggleButton_, nullptr, TRUE);
                    }
                    return 0;
                case IDC_REOPEN_TOGGLE:
                    if (uiDoneMode_) {
                        reopenAfterRestart_ = !reopenAfterRestart_;
                        updateReopenToggleText();
                        InvalidateRect(reopenToggleButton_, nullptr, TRUE);
                        Scheduler scheduler(currentExecutable());
                        if (reopenAfterRestart_) {
                            std::wstring error;
                            if (!scheduler.create(&error)) {
                                MessageBoxW(hwnd_,
                                            (L"Não foi possível agendar a reabertura após "
                                             L"reinício:\r\n" +
                                             error)
                                                .c_str(),
                                            L"Win Auto Updater", MB_OK | MB_ICONWARNING);
                            }
                        } else {
                            scheduler.remove();
                        }
                    }
                    return 0;
                case IDC_UPDATE_NO_DRIVERS:
                    beginUpdate(true);
                    return 0;
                case IDC_UNINSTALL:
                    if (uiDoneMode_) {
                        performUninstall();
                    }
                    return 0;
                case IDC_RUN_TESTS:
                    runStressTests();
                    return 0;
                case IDC_DUMP_UI:
                    runUiDump();
                    return 0;
                case IDC_RECORD_MACRO:
                    toggleRecording();
                    return 0;
                case IDC_MONITOR:
                    launchMonitorTools();
                    return 0;
                case IDC_OPEN_TOOLS:
                    openTestTools();
                    return 0;
                case IDC_CLOSE_REOPEN:
                    if (allowClose_) {
                        closeAndReopen();
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
            auto* snapshot = reinterpret_cast<UpdateStats*>(lParam);
            if (snapshot) {
                if (!uiDoneMode_) {
                    uiStats_ = *snapshot;
                    InvalidateRect(hwnd_, &statsRect_, FALSE);
                }
                delete snapshot;
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
        case WM_APP_STRESS: {
            auto* msg = reinterpret_cast<StressMessage*>(lParam);
            if (msg) {
                stressLog_ += msg->text + L"\r\n";
                if (summaryLabel_) {
                    SetWindowTextW(
                        summaryLabel_,
                        (doneSummary_ + L"\r\n\r\nTestes de stress:\r\n" + stressLog_).c_str());
                }
                if (!uiDoneMode_ && currentLabel_) {
                    SetWindowTextW(currentLabel_, msg->text.c_str());
                }
                if (msg->final) {
                    stressRunning_ = false;
                    if (runTestsButton_) {
                        EnableWindow(runTestsButton_, TRUE);
                        SetWindowTextW(runTestsButton_, L"Correr testes + macro");
                    }
                    if (openToolsButton_) {
                        EnableWindow(openToolsButton_, TRUE);
                        SetWindowTextW(openToolsButton_, L"Abrir só as ferramentas");
                    }
                }
                delete msg;
            }
            return 0;
        }
        case WM_APP_UNINSTALL_DONE: {
            if (uninstallButton_) {
                EnableWindow(uninstallButton_, FALSE);
                SetWindowTextW(uninstallButton_, L"Desinstalado");
            }
            if (runTestsButton_) {
                ShowWindow(runTestsButton_, SW_HIDE);
            }
            if (openToolsButton_) {
                ShowWindow(openToolsButton_, SW_HIDE);
            }
            if (recordButton_) {
                ShowWindow(recordButton_, SW_HIDE);
            }
            if (monitorButton_) {
                ShowWindow(monitorButton_, SW_HIDE);
            }
            if (closeReopenButton_) {
                ShowWindow(closeReopenButton_, SW_HIDE);
            }
            if (reopenToggleButton_) {
                ShowWindow(reopenToggleButton_, SW_HIDE);
            }
            if (summaryLabel_) {
                SetWindowTextW(summaryLabel_,
                               L"O Win Auto Updater foi desinstalado.\r\n\r\n"
                               L"A tarefa agendada de reabertura foi removida.\r\n"
                               L"O SingleUpdate.exe na pen desaparece no próximo reinício.\r\n\r\n"
                               L"Clica em Fechar para sair.");
                ShowWindow(summaryLabel_, SW_SHOW);
            }
            allowClose_ = true;
            if (closeButton_) {
                EnableWindow(closeButton_, TRUE);
                ShowWindow(closeButton_, SW_SHOW);
            }
            applyHint(L"Desinstalação concluída. Clica em Fechar.");
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
        case WM_APP_DUMP: {
            dumping_ = false;
            return 0;
        }
        case Macro::kStopMessage:
            finishRecording();
            return 0;
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
                    setCurrent(L"A reiniciar em " + std::to_wstring(rebootCountdown_) +
                               L" segundos... Guarda o teu trabalho.");
                } else {
                    KillTimer(hwnd_, kRebootTimerId);
                    setCurrent(L"A enviar o comando de reinício...");
                    if (Restart::now()) {
                        appendLog(L"Comando de reinício enviado.");
                        allowClose_ = true;
                    } else {
                        showFinishedScreen(
                            false,
                            L"O reinício automático falhou. Reinicia o Windows manualmente.\r\n\r\n"
                            L"A tarefa agendada retoma o Win Auto Updater após o início de sessão.");
                    }
                }
            } else if (wParam == kAnimTimerId) {
                if (!uiDoneMode_) {
                    ++animFrame_;
                    InvalidateRect(hwnd_, &spinnerRect_, FALSE);
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
            KillTimer(hwnd_, kAnimTimerId);
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
