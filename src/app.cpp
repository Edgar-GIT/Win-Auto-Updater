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

#include <shellapi.h>
#include <windowsx.h>
#include <wininet.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using ui::Canvas;
using ui::Color;
using ui::RectF;
using ui::Weight;

constexpr const wchar_t* kAppTitle = L"Win Auto Updater";
constexpr const wchar_t* kHomeHint =
    L"Escolhe o modo de atualização ou usa a bancada de testes. O botão Guia explica cada opção.";

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

// Sets "Key=value" in an ini-style text file: rewrites the line when the key is
// already there (outside any [section]), appends it otherwise. Everything else
// in the file is left byte for byte as it was.
bool writeIniValue(const std::filesystem::path& file, const std::string& key, const std::string& value) {
    std::string content;
    {
        std::ifstream in(file, std::ios::binary);
        if (in.is_open()) {
            content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    // A UTF-16 file (Notepad's "Unicode") is not something this can edit safely.
    if (content.size() >= 2 && ((static_cast<unsigned char>(content[0]) == 0xFF &&
                                 static_cast<unsigned char>(content[1]) == 0xFE) ||
                                (static_cast<unsigned char>(content[0]) == 0xFE &&
                                 static_cast<unsigned char>(content[1]) == 0xFF))) {
        return false;
    }
    const std::string eol = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const auto lowered = [](std::string text) {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    };
    const std::string wanted = lowered(key);

    std::string out;
    bool replaced = false;
    bool inSection = false;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) {
            end = content.size();
        }
        std::string line = content.substr(pos, end - pos);
        const bool hadCr = !line.empty() && line.back() == '\r';
        if (hadCr) {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            inSection = true;
        }
        const size_t eq = line.find('=');
        if (!replaced && !inSection && first != std::string::npos && eq != std::string::npos &&
            line[first] != ';' && line[first] != '#') {
            std::string name = line.substr(first, eq - first);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.pop_back();
            }
            if (lowered(name) == wanted) {
                line = key + "=" + value;
                replaced = true;
            }
        }
        out += line;
        if (!last) {
            out += hadCr ? "\r\n" : "\n";
        }
        pos = last ? content.size() : end + 1;
    }
    if (!replaced) {
        // Top-level keys must stay above the first [section].
        const std::string entry = key + "=" + value + eol;
        const size_t section = out.find("\n[");
        if (out.rfind('[', 0) == 0) {
            out.insert(0, entry);
        } else if (section != std::string::npos) {
            out.insert(section + 1, entry);
        } else {
            if (!out.empty() && out.back() != '\n') {
                out += eol;
            }
            out += entry;
        }
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output.write(out.data(), static_cast<std::streamsize>(out.size()));
    return output.good();
}

const wchar_t* kGuideText =
    L"ATUALIZAÇÕES\r\n"
    L"  • Atualizar tudo — Windows + drivers + antivírus (respeita o filtro)\r\n"
    L"  • Sem drivers — só Windows Update\r\n"
    L"  • Só drivers — copia AMD/NVIDIA (sem Windows Update)\r\n"
    L"  • Lenovo/ThinkPad — copia também o System Update para o Ambiente de Trabalho\r\n"
    L"  • Filtro de updates — verde = instalar, vermelho = bloquear\r\n"
    L"  • Interruptores — testar após atualizar; e com que testes (nativos ou ferramentas + macro)\r\n"
    L"  • Se a atualização falhar, a ferramenta fecha e reabre sozinha uma vez\r\n"
    L"\r\n"
    L"BANCADA DE TESTES / MACRO\r\n"
    L"  Deploy: Ambiente de Trabalho\\SingleUpdate.exe + helpers\\\r\n"
    L"  Ferramentas: helpers\\programas de teste\r\n"
    L"  1. Gravar macro → Abrir ferramentas → clicar → F10\r\n"
    L"  2. Testes automáticos — stress + replay da macro\r\n"
    L"  • Nativos auto (beta) — RAM, CPU, disco, gráfica e bateria sozinhos, 5 min\r\n"
    L"  • Nativos manual (beta) — uma janela por teste, para clicar à mão\r\n"
    L"  • Sensores — OpenHardwareMonitor (+ BatteryCare)\r\n"
    L"\r\n"
    L"Filtro: helpers\\update_filter.txt (passa entre PCs com a pasta helpers).\r\n"
    L"Registo: %TEMP%\\WinAutoUpdater\\updater.log";

std::filesystem::path excludedTitlesPath() {
    return Stress::helpersDir(currentExecutable()) / L"update_filter.txt";
}

std::wstring* heapCopy(std::wstring_view text) {
    return new std::wstring(text);
}

bool postText(HWND hwnd, UINT msg, std::wstring_view text, WPARAM wParam = 0) {
    auto* copy = heapCopy(text);
    if (!PostMessageW(hwnd, msg, wParam, reinterpret_cast<LPARAM>(copy))) {
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

std::string toUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

// Low-level COM chatter is useful in updater.log but only noise in the small
// on-screen activity block.
bool isNoisyLogLine(const std::wstring& line) {
    if (line.find_first_not_of(L" \t") == std::wstring::npos) {
        return true;
    }
    static const wchar_t* kNoise[] = {
        L"HRESULT",     L"UpdateCollection", L"ReadOnly",        L"Adding update",
        L"ISystemInformation", L"IUpdateInstaller", L"=====", L"Copy item count",
        L"After removal", L"Removing all items",
    };
    for (const wchar_t* token : kNoise) {
        if (line.find(token) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring trimLeft(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t");
    return first == std::wstring::npos ? std::wstring() : value.substr(first);
}

std::wstring firstLine(const std::wstring& value) {
    const auto end = value.find_first_of(L"\r\n");
    return end == std::wstring::npos ? value : value.substr(0, end);
}

std::wstring formatElapsed(unsigned long long ms) {
    const unsigned long long total = ms / 1000ULL;
    const unsigned long long hours = total / 3600ULL;
    const unsigned long long minutes = (total / 60ULL) % 60ULL;
    const unsigned long long seconds = total % 60ULL;
    wchar_t buffer[32]{};
    if (hours > 0) {
        swprintf(buffer, 32, L"%llu:%02llu:%02llu", hours, minutes, seconds);
    } else {
        swprintf(buffer, 32, L"%02llu:%02llu", minutes, seconds);
    }
    return buffer;
}

bool intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

RECT inflated(RECT r, int amount) {
    InflateRect(&r, amount, amount);
    return r;
}

bool isEmptyRect(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

struct StressMessage {
    std::wstring text;
    bool final = false;
};

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {}

// ---------------------------------------------------------------------------
// Startup / shutdown
// ---------------------------------------------------------------------------

void App::parseCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return;
    }
    const auto valueOf = [](const std::wstring& arg, const wchar_t* prefix, int fallback) {
        const std::wstring p(prefix);
        if (arg.size() <= p.size()) {
            return fallback;
        }
        try {
            return std::stoi(arg.substr(p.size()));
        } catch (...) {
            return fallback;
        }
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--retried") {
            retried_ = true;
        } else if (arg.rfind(L"--auto-retry=", 0) == 0) {
            const int mode = valueOf(arg, L"--auto-retry=", -1);
            if (mode >= 0 && mode <= 2) {
                autoRetryMode_ = mode;
            }
        } else if (arg.rfind(L"--auto-stress=", 0) == 0) {
            autoStressOverride_ = valueOf(arg, L"--auto-stress=", -1);
        }
    }
    LocalFree(argv);
}

int App::run() {
    if (!elevateIfNeeded()) {
        return 0;
    }
    parseCommandLine();

    HANDLE singleInstance =
        CreateMutexW(nullptr, TRUE, L"Local\\WinAutoUpdaterSingleInstanceMutex");
    if (!singleInstance) {
        MessageBoxW(nullptr, L"Failed to create the single-instance lock.", kAppTitle,
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

    ui::startup();

    if (!createUi()) {
        MessageBoxW(nullptr, L"Failed to create the application window.", kAppTitle,
                    MB_OK | MB_ICONERROR);
        ui::shutdown();
        releaseSingleInstance();
        return 1;
    }

    // Ensure Desktop\helpers\ + stress.ini / uia_stress.ps1 exist.
    Stress::ensureAssets(currentExecutable());
    // Default the chooser's toggles to the AutoRunStress / NativeTests settings.
    {
        const Stress::Config stressConfig = Stress::loadConfig(currentExecutable());
        autoStressAfterUpdate_ = stressConfig.autoRunStress;
        nativeAfterUpdate_ = stressConfig.nativeTests;
    }
    if (autoStressOverride_ >= 0) {
        autoStressAfterUpdate_ = autoStressOverride_ != 0;
    }
    refreshHelperStatus();

    openLogFile();

    logger_.setStatusSink([this](std::wstring_view message) {
        if (!uiDoneMode_) {
            postText(hwnd_, WM_APP_STATUS, message);
        }
    });

    logger_.setLogSink([this](std::wstring_view message) {
        writeLogLine(message);
        if (!uiDoneMode_) {
            postText(hwnd_, WM_APP_LOG, message);
        }
    });

    // Progress sink is unused — progress goes only to the ring via postProgress.
    logger_.setProgressSink([](std::wstring_view) {});

    loadExcludedTitles();
    refreshFilterCaption();
    appendLog(L"Win Auto Updater iniciado.");
    appendLog(L"Pasta helpers: " + Stress::helpersDir(currentExecutable()).wstring());
    appendLog(L"Filtro updates: helpers\\update_filter.txt (" +
              std::to_wstring(excludedTitles_.size()) + L" bloqueada(s)).");

    // After a reboot mid-update, state.json has phase=waiting_reboot — jump
    // straight back into the update worker (same mode as before the restart).
    // The same happens when this process is the automatic retry after an error.
    // Fresh launches still open on the chooser.
    {
        StateManager stateManager;
        const auto savedState = stateManager.load();
        if (savedState.phase == L"waiting_reboot") {
            appendLog(retried_ ? L"Reabertura automática após erro — a retomar as atualizações."
                               : L"Reinício detetado — a retomar as atualizações automaticamente.");
            UpdateMode mode = UpdateMode::Full;
            if (savedState.updateMode == 1 || savedState.withoutDrivers) {
                mode = UpdateMode::NoDrivers;
            } else if (savedState.updateMode == 2) {
                mode = UpdateMode::DriversOnly;
            }
            beginUpdate(mode);
            setPhase(retried_ ? L"Nova tentativa automática" : L"A retomar após reinício");
            setCurrent(mode == UpdateMode::NoDrivers
                           ? L"A continuar (Windows sem drivers)..."
                           : L"A continuar as atualizações Windows...");
        } else if (autoRetryMode_ >= 0) {
            appendLog(L"Reabertura automática após erro — nova tentativa de atualização.");
            beginUpdate(static_cast<UpdateMode>(autoRetryMode_));
            setPhase(L"Nova tentativa automática");
            setCurrent(L"A ferramenta foi reaberta após um erro — a tentar de novo...");
        }
    }

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

    releaseSurfaces();
    if (appIcon_) {
        DestroyIcon(appIcon_);
        appIcon_ = nullptr;
    }
    ui::shutdown();

    releaseSingleInstance();
    return static_cast<int>(msg.wParam);
}

bool App::elevateIfNeeded() {
    if (isElevated()) {
        return true;
    }

    const auto exe = currentExecutable();
    if (exe.empty()) {
        MessageBoxW(nullptr, L"Unable to locate the executable path.", kAppTitle,
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
            MessageBoxW(nullptr, L"Administrator privileges are required.", kAppTitle,
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

void App::openLogFile() {
    const auto tempDir = StateManager::baseDir();
    if (tempDir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    const auto logPath = tempDir / L"updater.log";

    // The log is appended to (not truncated) so the cause of a failure survives
    // the automatic close-and-reopen. It is only reset once it grows large.
    const auto size = std::filesystem::file_size(logPath, ec);
    const bool fresh = ec || size == 0 || size > 2 * 1024 * 1024;
    {
        std::lock_guard lock(logMutex_);
        logFile_.open(logPath, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
        if (logFile_.is_open() && fresh) {
            logFile_ << "\xEF\xBB\xBF";
        }
    }
    writeLogLine(L"");
    writeLogLine(L"========== Nova sessão ==========");
}

void App::writeLogLine(std::wstring_view message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[40]{};
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::string utf8 = toUtf8(message);

    std::lock_guard lock(logMutex_);
    if (!logFile_.is_open()) {
        return;
    }
    logFile_ << stamp << utf8 << "\r\n";
    logFile_.flush();
}

// ---------------------------------------------------------------------------
// Window creation and sizing
// ---------------------------------------------------------------------------

void App::initWidgets() {
    const auto add = [this](int id, const wchar_t* text, const wchar_t* sub, ui::ButtonStyle style,
                            ui::Icon icon, bool toggle = false) {
        Widget w;
        w.id = id;
        w.text = text;
        w.sub = sub;
        w.style = style;
        w.icon = icon;
        w.toggle = toggle;
        widgets_.push_back(std::move(w));
    };
    using ui::ButtonStyle;
    using ui::Icon;
    widgets_.clear();
    add(IDC_UPDATE, L"Atualizar tudo", L"Windows Update + drivers + antivírus", ButtonStyle::Primary,
        Icon::Download);
    add(IDC_UPDATE_NO_DRIVERS, L"Sem drivers", L"Só Windows Update", ButtonStyle::Glass,
        Icon::Window);
    add(IDC_UPDATE_DRIVERS_ONLY, L"Só drivers", L"AMD · NVIDIA · Lenovo", ButtonStyle::Glass,
        Icon::Chip);
    add(IDC_CONFIGURE_UPDATES, L"Filtro de updates", L"Escolher o que instalar", ButtonStyle::Glass,
        Icon::Filter);
    add(IDC_STRESS_TOGGLE, L"Testes automáticos após atualizar", L"", ButtonStyle::Glass,
        Icon::None, true);
    add(IDC_RUN_TESTS, L"Testes automáticos", L"Abre as ferramentas e reproduz a macro",
        ButtonStyle::Accent, Icon::Play);
    add(IDC_OPEN_TOOLS, L"Abrir ferramentas", L"Sem macro", ButtonStyle::Glass, Icon::Grid);
    add(IDC_MONITOR, L"Sensores", L"Temperaturas", ButtonStyle::Glass, Icon::Pulse);
    add(IDC_RECORD_MACRO, L"Gravar macro", L"MacroHelper · F10", ButtonStyle::Glass, Icon::Record);
    // Native tests (beta): our own RAM / CPU / disk / GPU / battery tests, in their
    // own process. Automatic = everything by itself; manual = one window per test.
    add(IDC_NATIVE_TESTS, L"Nativos auto", L"Beta · tudo sozinho, 5 min", ButtonStyle::Glass,
        Icon::Chip);
    add(IDC_NATIVE_MANUAL, L"Nativos manual", L"Beta · uma janela por teste", ButtonStyle::Glass,
        Icon::Grid);
    add(IDC_NATIVE_TOGGLE, L"Usar os testes nativos após atualizar", L"", ButtonStyle::Glass,
        Icon::None, true);
    add(IDC_REOPEN_TOGGLE, L"Reabrir após reinício", L"", ButtonStyle::Glass, Icon::None, true);
    add(IDC_RETRY, L"Tentar novamente", L"", ButtonStyle::Primary, Icon::Retry);
    add(IDC_GUIDE, L"Guia", L"", ButtonStyle::Ghost, Icon::Info);
    add(IDC_UNINSTALL, L"Desinstalar", L"", ButtonStyle::Danger, Icon::None);
    add(IDC_CLOSE, L"Fechar", L"", ButtonStyle::Glass, Icon::None);
}

bool App::createUi() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // the whole client area is painted by render()
    wc.lpszClassName = L"WinAutoUpdaterWindow";
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    initWidgets();

    // Resizable + maximizable: the layout scales with the client area, so the
    // window works from 800x600 panels up to 4K at any Windows scaling.
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, kAppTitle,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            kDesignW, kDesignH, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }

    ui::applyDarkFrame(hwnd_);
    allowClose_ = true;
    lastCurrent_ = kHomeHint;

    fitWindowToScreen();
    layout();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    SetTimer(hwnd_, kAnimTimerId, kAnimIntervalMs, nullptr);
    return true;
}

// Sizes the window for the current monitor: the design size at the monitor's
// DPI, shrunk as needed to fit the work area (small or low-resolution screens),
// then centred. Called at startup and whenever the display mode changes.
void App::fitWindowToScreen() {
    if (!hwnd_ || IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        return;
    }
    const UINT dpi = ui::dpiForWindow(hwnd_);
    const RECT work = ui::workAreaFor(hwnd_);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT frame{0, 0, 0, 0};
    ui::adjustWindowRect(&frame, style, exStyle, dpi);
    const int frameW = frame.right - frame.left;
    const int frameH = frame.bottom - frame.top;

    const int breathing = 12;  // keep a sliver of desktop visible around the window
    const float availW = static_cast<float>(std::max(200, workW - frameW - breathing));
    const float availH = static_cast<float>(std::max(150, workH - frameH - breathing));

    float scale = static_cast<float>(dpi) / 96.0f;
    scale = std::min(scale, availW / static_cast<float>(kDesignW));
    scale = std::min(scale, availH / static_cast<float>(kDesignH));
    scale = std::max(scale, 0.4f);
    fitScale_ = scale;

    const int clientW = static_cast<int>(std::lround(kDesignW * scale));
    const int clientH = static_cast<int>(std::lround(kDesignH * scale));
    const int winW = clientW + frameW;
    const int winH = clientH + frameH;
    const int x = work.left + std::max(0, (workW - winW) / 2);
    const int y = work.top + std::max(0, (workH - winH) / 2);
    SetWindowPos(hwnd_, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

int App::px(float du) const {
    return static_cast<int>(std::lround(du * scale_));
}

App::Screen App::screen() const {
    if (choosing_) {
        return Screen::Home;
    }
    return uiDoneMode_ ? Screen::Done : Screen::Working;
}

App::Widget* App::widget(int id) {
    for (auto& w : widgets_) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

void App::setWidgetText(int id, const std::wstring& text) {
    if (Widget* w = widget(id)) {
        if (w->text != text) {
            w->text = text;
            invalidateWidget(id);
        }
    }
}

void App::setWidgetEnabled(int id, bool enabled) {
    if (Widget* w = widget(id)) {
        if (w->enabled != enabled) {
            w->enabled = enabled;
            invalidateWidget(id);
        }
    }
}

void App::invalidate(const RECT* rect) {
    if (hwnd_) {
        InvalidateRect(hwnd_, rect, FALSE);
    }
}

void App::invalidateWidget(int id) {
    if (Widget* w = widget(id)) {
        if (w->visible) {
            // Accent buttons paint a glow outside their bounds.
            const RECT area = inflated(w->rc, px(14.0f));
            invalidate(&area);
        }
    }
}

void App::place(int id, int x, int y, int w, int h) {
    if (Widget* item = widget(id)) {
        item->rc = RECT{x, y, x + w, y + h};
        item->visible = true;
    }
}

void App::layout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        return;
    }
    layout(rc.right, rc.bottom);
    invalidate();
}

void App::layout(int width, int height) {
    clientW_ = std::max(1, width);
    clientH_ = std::max(1, height);
    scale_ = std::clamp(std::min(static_cast<float>(clientW_) / static_cast<float>(kDesignW),
                                 static_cast<float>(clientH_) / static_cast<float>(kDesignH)),
                        0.35f, 5.0f);

    for (auto& w : widgets_) {
        w.visible = false;
    }
    cardLeft_ = cardRight_ = hintRect_ = chipsRect_ = RECT{};
    heroRect_ = ringRect_ = statsRect_ = activityRect_ = detailRect_ = RECT{};

    const int margin = px(26.0f);
    const int iconSize = px(38.0f);
    iconRect_ = RECT{margin, px(20.0f), margin + iconSize, px(20.0f) + iconSize};
    pillRect_ = RECT{clientW_ - margin - px(210.0f), px(24.0f), clientW_ - margin,
                     px(24.0f) + px(30.0f)};

    if (iconSize != appIconSize_) {
        if (appIcon_) {
            DestroyIcon(appIcon_);
        }
        appIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
        appIconSize_ = iconSize;
    }

    // Footer: Desinstalar + Fechar on the right on every screen.
    const int footerH = px(40.0f);
    const int footerY = clientH_ - margin - footerH;
    const int closeW = px(124.0f);
    const int uninstallW = px(150.0f);
    const int gap = px(10.0f);
    place(IDC_CLOSE, clientW_ - margin - closeW, footerY, closeW, footerH);
    place(IDC_UNINSTALL, clientW_ - margin - closeW - gap - uninstallW, footerY, uninstallW,
          footerH);
    footerTextRect_ = RECT{margin, footerY, clientW_ - margin - closeW - gap - uninstallW - px(16.0f),
                           footerY + footerH};

    const int top = px(78.0f) + px(16.0f);
    const int bottom = footerY - px(16.0f);
    switch (screen()) {
        case Screen::Home:
            layoutHome(top, bottom);
            break;
        case Screen::Working:
            layoutWorking(top, bottom);
            break;
        case Screen::Done:
            layoutDone(top, bottom);
            break;
    }
}

// When the window is taller than the design aspect (4:3 / 5:4 screens, a
// maximised window), the rows grow a little instead of leaving a big empty band.
static float verticalStretch(int available, int natural) {
    if (natural <= 0 || available <= natural) {
        return 1.0f;
    }
    return std::min(1.32f, static_cast<float>(available) / static_cast<float>(natural));
}

void App::layoutHome(int top, int bottom) {
    const int margin = px(26.0f);
    const float v = verticalStretch(bottom - top, px(384.0f + 14.0f + 46.0f));
    const auto vpx = [&](float du) { return static_cast<int>(std::lround(du * scale_ * v)); };

    const int headerH = px(46.0f);  // card caption strip (not stretched)
    const int pad = px(20.0f);
    const int rowGap = vpx(12.0f);
    const int bigH = vpx(64.0f);
    const int midH = vpx(56.0f);
    const int smallH = vpx(50.0f);
    const int cardH = headerH + bigH + midH + smallH * 3 + rowGap * 4 + pad;
    const int hintGap = vpx(14.0f);
    const int hintH = vpx(46.0f);
    const int blockH = cardH + hintGap + hintH;
    const int y0 = top + std::max(0, (bottom - top - blockH) / 2);

    const int gap = px(18.0f);
    const int colW = (clientW_ - margin * 2 - gap) / 2;
    cardLeft_ = RECT{margin, y0, margin + colW, y0 + cardH};
    cardRight_ = RECT{margin + colW + gap, y0, clientW_ - margin, y0 + cardH};

    const int colGap = px(12.0f);
    {
        const int x = cardLeft_.left + pad;
        const int w = (cardLeft_.right - cardLeft_.left) - pad * 2;
        const int half = (w - colGap) / 2;
        int y = y0 + headerH;
        place(IDC_UPDATE, x, y, w, bigH);
        y += bigH + rowGap;
        place(IDC_UPDATE_NO_DRIVERS, x, y, half, midH);
        place(IDC_UPDATE_DRIVERS_ONLY, x + half + colGap, y, w - half - colGap, midH);
        y += midH + rowGap;
        place(IDC_CONFIGURE_UPDATES, x, y, w, smallH);
        y += smallH + rowGap;
        place(IDC_STRESS_TOGGLE, x, y, w, smallH);
        y += smallH + rowGap;
        place(IDC_NATIVE_TOGGLE, x, y, w, smallH);
    }
    {
        const int x = cardRight_.left + pad;
        const int w = (cardRight_.right - cardRight_.left) - pad * 2;
        const int half = (w - colGap) / 2;
        int y = y0 + headerH;
        place(IDC_RUN_TESTS, x, y, w, bigH);
        y += bigH + rowGap;
        place(IDC_OPEN_TOOLS, x, y, half, midH);
        place(IDC_MONITOR, x + half + colGap, y, w - half - colGap, midH);
        y += midH + rowGap;
        place(IDC_NATIVE_TESTS, x, y, half, smallH);
        place(IDC_NATIVE_MANUAL, x + half + colGap, y, w - half - colGap, smallH);
        y += smallH + rowGap;
        place(IDC_RECORD_MACRO, x, y, w, smallH);
        y += smallH + rowGap;
        chipsRect_ = RECT{x, y, x + w, y + smallH};
    }

    hintRect_ = RECT{margin, y0 + cardH + hintGap, clientW_ - margin, y0 + cardH + hintGap + hintH};

    const Widget* close = widget(IDC_CLOSE);
    if (close != nullptr) {
        place(IDC_GUIDE, margin, close->rc.top, px(104.0f), close->rc.bottom - close->rc.top);
    }
}

void App::layoutWorking(int top, int bottom) {
    const int margin = px(26.0f);
    const float v = verticalStretch(bottom - top, px(178.0f + 14.0f + 74.0f + 14.0f + 98.0f));
    const auto vpx = [&](float du) { return static_cast<int>(std::lround(du * scale_ * v)); };

    const int heroH = vpx(178.0f);
    const int statsH = vpx(74.0f);
    const int activityH = vpx(98.0f);
    const int gap = vpx(14.0f);
    const int blockH = heroH + gap + statsH + gap + activityH;
    const int y0 = top + std::max(0, (bottom - top - blockH) / 2);

    heroRect_ = RECT{margin, y0, clientW_ - margin, y0 + heroH};
    const int ringD = vpx(124.0f);
    const int ringX = margin + px(30.0f);
    const int ringY = y0 + (heroH - ringD) / 2;
    ringRect_ = RECT{ringX, ringY, ringX + ringD, ringY + ringD};
    statsRect_ = RECT{margin, y0 + heroH + gap, clientW_ - margin, y0 + heroH + gap + statsH};
    activityRect_ = RECT{margin, statsRect_.bottom + gap, clientW_ - margin,
                         statsRect_.bottom + gap + activityH};
}

void App::layoutDone(int top, int bottom) {
    const int margin = px(26.0f);
    const int gap = px(12.0f);
    int y = top;

    heroRect_ = RECT{margin, y, clientW_ - margin, y + px(104.0f)};
    y = heroRect_.bottom + gap;

    if (showDoneStats_) {
        statsRect_ = RECT{margin, y, clientW_ - margin, y + px(62.0f)};
        y = statsRect_.bottom + gap;
    }

    const bool actions = uninstallState_ == 0;
    const int actionsH = px(44.0f);
    const int detailBottom = actions ? bottom - actionsH - gap : bottom;
    detailRect_ = RECT{margin, y, clientW_ - margin, std::max(y + px(40.0f), detailBottom)};

    if (!actions) {
        return;
    }
    const int actionsY = bottom - actionsH;
    const int fullW = clientW_ - margin * 2;
    if (doneSuccess_) {
        // Five actions; the widths follow the length of the labels.
        const int ids[5] = {IDC_OPEN_TOOLS, IDC_RUN_TESTS, IDC_NATIVE_TESTS, IDC_NATIVE_MANUAL, IDC_MONITOR};
        const float weights[5] = {1.12f, 1.17f, 0.92f, 0.98f, 0.81f};
        const int smallGap = px(10.0f);
        const float unit = static_cast<float>(fullW - smallGap * 4) / 5.0f;
        int x = margin;
        for (int i = 0; i < 5; ++i) {
            const int w = i == 4 ? margin + fullW - x : static_cast<int>(std::lround(unit * weights[i]));
            place(ids[i], x, actionsY, w, actionsH);
            x += w + smallGap;
        }
        const Widget* close = widget(IDC_CLOSE);
        if (close != nullptr) {
            place(IDC_REOPEN_TOGGLE, margin, close->rc.top, px(290.0f),
                  close->rc.bottom - close->rc.top);
        }
    } else {
        place(IDC_RETRY, margin, actionsY, px(230.0f), actionsH);
    }
}

// ---------------------------------------------------------------------------
// Text / state setters
// ---------------------------------------------------------------------------

void App::setStatus(std::wstring message) {
    lastStatus_ = std::move(message);
    setPhase(lastStatus_);
}

void App::setPhase(std::wstring message) {
    lastPhase_ = std::move(message);
    if (screen() == Screen::Working) {
        invalidate(&heroRect_);
    }
}

void App::setCurrent(std::wstring message) {
    lastCurrent_ = std::move(message);
    switch (screen()) {
        case Screen::Home:
            invalidate(&hintRect_);
            break;
        case Screen::Working:
            invalidate(&heroRect_);
            break;
        case Screen::Done:
            break;
    }
}

void App::setProgressPercent(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (percent == lastPercent_) {
        return;
    }
    lastPercent_ = percent;
    if (screen() == Screen::Working) {
        const RECT area = inflated(ringRect_, px(12.0f));
        invalidate(&area);
    }
}

void App::setRingPhase(RingPhase phase) {
    ringPhase_ = phase;
    lastPercent_ = 0;
    if (screen() == Screen::Working) {
        invalidate(&heroRect_);
    }
}

void App::pushRecentLog(std::wstring message) {
    message = sanitizeLogLine(std::move(message));
    if (isNoisyLogLine(message)) {
        return;
    }
    recentLog_.push_back(trimLeft(message));
    while (recentLog_.size() > kMaxRecentLog) {
        recentLog_.pop_front();
    }
    if (screen() == Screen::Working) {
        invalidate(&activityRect_);
    }
}

// UI-thread log line: goes to updater.log and to the small activity block.
void App::appendLog(std::wstring message) {
    writeLogLine(message);
    pushRecentLog(std::move(message));
}

void App::applyHint(const std::wstring& hint) {
    if (screen() == Screen::Done) {
        doneHint_ = hint;
        detailStickBottom_ = true;
        invalidate(&detailRect_);
    } else {
        setCurrent(hint);
    }
}

void App::refreshHelperStatus() {
    const auto exe = currentExecutable();
    std::error_code ec;
    const bool tools = std::filesystem::is_directory(Stress::toolsBaseDir(exe), ec);
    const bool helper = std::filesystem::exists(Stress::macroHelperPath(exe), ec);
    const bool macro =
        std::filesystem::exists(Stress::macroFilePath(exe, Stress::loadConfig(exe)), ec);
    if (tools != toolsPresent_ || helper != helperPresent_ || macro != macroPresent_) {
        toolsPresent_ = tools;
        helperPresent_ = helper;
        macroPresent_ = macro;
        if (!isEmptyRect(chipsRect_)) {
            invalidate(&chipsRect_);
        }
    }
}

void App::refreshFilterCaption() {
    if (Widget* w = widget(IDC_CONFIGURE_UPDATES)) {
        w->sub = excludedTitles_.empty()
                     ? std::wstring(L"Nenhuma atualização bloqueada")
                     : std::to_wstring(excludedTitles_.size()) + L" bloqueada(s) no ficheiro";
        invalidateWidget(IDC_CONFIGURE_UPDATES);
    }
}

void App::showGuide() {
    MessageBoxW(hwnd_, kGuideText, L"Guia — Win Auto Updater", MB_OK | MB_ICONINFORMATION);
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
    // Much tighter throttle than before so the ring visibly tracks reality.
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

// ---------------------------------------------------------------------------
// Finished / uninstall
// ---------------------------------------------------------------------------

void App::showFinishedScreen(bool success, const std::wstring& detail) {
    uiDoneMode_ = true;
    finished_ = true;
    allowClose_ = true;
    choosing_ = false;
    doneSuccess_ = success;
    uninstallState_ = 0;
    KillTimer(hwnd_, kRetryTimerId);

    doneTitle_ = success ? L"Concluído — este PC está pronto" : L"Interrompido — ocorreu um erro";
    showDoneStats_ = !(updateMode_ == UpdateMode::DriversOnly && success);

    std::wstring summary;
    if (updateMode_ == UpdateMode::DriversOnly && success) {
        doneSubtitle_ = L"Modo só drivers — o Windows Update não foi executado. Resolução e "
                        L"instaladores gráficos preparados no Ambiente de Trabalho.";
        if (!postSummary_.empty()) {
            summary += L"Configuração:\r\n" + postSummary_ + L"\r\n";
        }
    } else {
        if (success) {
            doneSubtitle_ = uiStats_.failed > 0
                                ? L"As atualizações disponíveis foram processadas — algumas "
                                  L"falharam (ver contadores)."
                                : L"Todas as atualizações Windows disponíveis foram instaladas.";
        } else {
            const std::wstring headline =
                firstLine(detail.empty() ? L"Ocorreu um erro durante o processo de atualização."
                                         : detail);
            doneSubtitle_ = headline;
            summary += (detail.empty() ? headline : detail) + L"\r\n\r\n";
            summary += retried_ ? L"A ferramenta já foi reaberta automaticamente uma vez e o erro "
                                  L"repetiu-se. Usa \"Tentar novamente\" ou conclui em Definições "
                                  L"→ Windows Update.\r\n\r\n"
                                : L"Usa \"Tentar novamente\" para fechar e reabrir a ferramenta.\r\n\r\n";
        }

        if (uiStats_.avUpdated > 0) {
            summary += L"Antivírus: Microsoft Defender atualizado.\r\n\r\n";
        } else if (uiStats_.avFailed > 0) {
            summary += L"Antivírus: não foi possível atualizar o Microsoft Defender "
                       L"automaticamente (detalhes em updater.log). Atualiza em Segurança do "
                       L"Windows → Proteção contra vírus e ameaças.\r\n\r\n";
        }

        if (!postSummary_.empty()) {
            summary += L"Configuração pós-atualização:\r\n" + postSummary_ + L"\r\n";
        }

        if (updateWithoutDrivers_) {
            summary += L"O Windows Update foi configurado para não instalar drivers de hardware "
                       L"nesta execução.\r\n\r\n";
        }
    }

    if (success) {
        summary += L"Podes fechar esta janela, correr os testes ou desinstalar a ferramenta.";
    }

    doneSummary_ = summary;
    doneHint_.clear();
    stressLog_.clear();
    detailScroll_ = 0;
    detailStickBottom_ = false;

    setWidgetText(IDC_RUN_TESTS, L"Testes automáticos");
    setWidgetText(IDC_OPEN_TOOLS, L"Abrir ferramentas");
    setWidgetText(IDC_CLOSE, L"Fechar");
    setWidgetText(IDC_UNINSTALL, L"Desinstalar");
    setWidgetEnabled(IDC_RUN_TESTS, true);
    setWidgetEnabled(IDC_OPEN_TOOLS, true);
    setWidgetEnabled(IDC_MONITOR, true);
    setWidgetEnabled(IDC_CLOSE, true);
    setWidgetEnabled(IDC_UNINSTALL, true);
    setWidgetEnabled(IDC_REOPEN_TOGGLE, true);

    if (success) {
        // Reflect the real state of the logon task (it is not stored in memory
        // across launches): ON only if the tool will actually reopen.
        reopenAfterRestart_ = Scheduler(currentExecutable()).exists();
    }

    layout();

    // "After update: run stress tests" — kick off the bench once the update ends.
    if (success && autoStressAfterUpdate_ && !autoStressStarted_) {
        autoStressStarted_ = true;
        if (nativeAfterUpdate_) {
            launchNativeTests(true);
        } else {
            runStressTests();
        }
    }
}

void App::performUninstall() {
    const int confirm = MessageBoxW(
        hwnd_,
        L"Desinstalar o Win Auto Updater?\r\n\r\n"
        L"Vai apagar:\r\n"
        L"• Tarefa de reabertura automática\r\n"
        L"• Pasta helpers\\ (programas de teste, macro, MacroHelper, …)\r\n"
        L"• Estado temporário\r\n"
        L"• SingleUpdate.exe (apagado no próximo reinício)\r\n\r\n"
        L"(Os instaladores NVIDIA/AMD no Ambiente de Trabalho mantêm-se.)\r\n\r\n"
        L"Isto não se pode anular. Continuar?",
        L"Desinstalar", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (confirm != IDOK) {
        return;
    }

    // Immediate on-screen feedback (before the background work).
    choosing_ = false;
    uiDoneMode_ = true;
    uninstallState_ = 1;
    doneSuccess_ = true;
    showDoneStats_ = false;
    allowClose_ = false;
    doneTitle_ = L"A desinstalar…";
    doneSubtitle_ = L"A remover a tarefa agendada e a pasta helpers\\. Aguarda um momento.";
    doneSummary_ = L"A remover:\r\n"
                   L"• Tarefa de reabertura automática\r\n"
                   L"• Pasta helpers\\ (ferramentas, macro, MacroHelper, …)\r\n"
                   L"• Estado temporário";
    doneHint_.clear();
    stressLog_.clear();
    detailScroll_ = 0;
    setWidgetEnabled(IDC_UNINSTALL, false);
    setWidgetText(IDC_UNINSTALL, L"A desinstalar...");
    setWidgetEnabled(IDC_CLOSE, false);
    layout();
    if (hwnd_) {
        UpdateWindow(hwnd_);
    }

    // The log lives in the temp folder that is about to be removed.
    {
        std::lock_guard lock(logMutex_);
        if (logFile_.is_open()) {
            logFile_.close();
        }
    }

    // Task Scheduler COM can block for seconds on the UI thread — do it off-thread.
    const auto exe = currentExecutable();
    std::thread([this, exe] {
        Scheduler scheduler(exe);
        scheduler.remove();
        StateManager stateManager;
        stateManager.remove();
        Cleanup::removeTempFiles();
        Cleanup::removeBundle(exe);
        if (!exe.empty()) {
            // Self-delete on reboot (file is locked while we run).
            MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        PostMessageW(hwnd_, WM_APP_UNINSTALL_DONE, 0, 0);
    }).detach();
}

// ---------------------------------------------------------------------------
// Automatic close-and-reopen after a failure
// ---------------------------------------------------------------------------

// Starts a fresh copy of the tool that waits for this process to exit and then
// re-runs the same update mode. Returns false when the new process could not
// be started (the caller then shows the normal error screen).
bool App::relaunchSelf(bool markRetried) {
    const auto exe = currentExecutable();
    if (exe.empty()) {
        return false;
    }
    std::wstring args = L"--wait-pid=" + std::to_wstring(GetCurrentProcessId()) +
                        L" --auto-retry=" + std::to_wstring(static_cast<int>(updateMode_)) +
                        L" --auto-stress=" + std::to_wstring(autoStressAfterUpdate_ ? 1 : 0);
    if (markRetried) {
        args += L" --retried";
    }
    std::wstring error;
    if (!launchDetached(exe, args, &error)) {
        writeLogLine(L"Reabertura automática falhou: " + error);
        return false;
    }
    writeLogLine(L"A fechar para reabrir automaticamente (" + args + L").");
    allowClose_ = true;
    running_ = false;
    DestroyWindow(hwnd_);
    return true;
}

bool App::scheduleAutoRetry(const std::wstring& message) {
    if (retried_ || screen() != Screen::Working) {
        return false;
    }
    pendingRetryError_ = message;
    retryCountdown_ = kAutoRetrySeconds;
    writeLogLine(L"ERRO: " + sanitizeLogLine(message));
    writeLogLine(L"A ferramenta vai fechar e reabrir sozinha (tentativa automática 1/1).");
    ringPhase_ = RingPhase::Checking;
    lastPercent_ = 0;
    setPhase(L"Falhou — a reabrir automaticamente");
    setCurrent(L"Fechar e reabrir costuma resolver. Nova tentativa em " +
               std::to_wstring(retryCountdown_) + L" s… (" + firstLine(message) + L")");
    SetTimer(hwnd_, kRetryTimerId, 1000, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Update flow
// ---------------------------------------------------------------------------

void App::startWorker() {
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
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

void App::syncExcludedFromChoices() {
    // Merge titles shown in the dialog. Keep blocks for updates not present on this PC.
    // When enabling/disabling, also drop older aliases that normalize to the same family
    // (so KB504… → KB505… replaces the old line instead of stacking forever).
    for (const auto& choice : updateChoices_) {
        const std::wstring norm = UpdateEngine::normalizeTitle(choice.title);
        if (choice.enabled) {
            excludedTitles_.erase(choice.title);
            if (!norm.empty()) {
                for (auto it = excludedTitles_.begin(); it != excludedTitles_.end();) {
                    if (UpdateEngine::normalizeTitle(*it) == norm) {
                        it = excludedTitles_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        } else {
            if (!norm.empty()) {
                for (auto it = excludedTitles_.begin(); it != excludedTitles_.end();) {
                    if (UpdateEngine::normalizeTitle(*it) == norm) {
                        it = excludedTitles_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            excludedTitles_.insert(choice.title);
        }
    }
}

void App::saveExcludedTitles() const {
    const auto path = excludedTitlesPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::wofstream file(path);
    if (!file.is_open()) {
        return;
    }
    for (const auto& title : excludedTitles_) {
        file << title << L'\n';
    }
}

void App::loadExcludedTitles() {
    excludedTitles_.clear();
    const auto path = excludedTitlesPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    std::wifstream file(path);
    if (!file.is_open()) {
        return;
    }
    std::wstring line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            excludedTitles_.insert(line);
        }
    }
}

void App::configureUpdates() {
    if (!choosing_) {
        return;
    }

    loadExcludedTitles();

    setCurrent(L"A pesquisar atualizações pendentes no Windows Update...");
    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const int busy[] = {IDC_CONFIGURE_UPDATES, IDC_UPDATE, IDC_UPDATE_NO_DRIVERS,
                        IDC_UPDATE_DRIVERS_ONLY};
    for (int id : busy) {
        setWidgetEnabled(id, false);
    }
    if (hwnd_) {
        UpdateWindow(hwnd_);
    }

    std::wstring error;
    const auto titles = UpdateEngine::listPendingTitles(&error);

    SetCursor(prev);
    for (int id : busy) {
        setWidgetEnabled(id, true);
    }

    if (titles.empty()) {
        const std::wstring msg =
            error.empty() ? L"Não há atualizações pendentes neste momento."
                          : (L"Não foi possível listar as atualizações:\r\n" + error);
        MessageBoxW(hwnd_, msg.c_str(), L"Filtro de updates", MB_OK | MB_ICONINFORMATION);
        setCurrent(kHomeHint);
        return;
    }

    std::vector<UpdateChoice> next;
    next.reserve(titles.size());
    for (const auto& title : titles) {
        const bool enabled = !UpdateEngine::isTitleBlocked(title, excludedTitles_);
        next.push_back(UpdateChoice{title, enabled});
    }

    if (!showUpdatePickerDialog(hwnd_, instance_, &next)) {
        setCurrent(kHomeHint);
        return;
    }

    updateChoices_ = std::move(next);
    syncExcludedFromChoices();
    saveExcludedTitles();
    refreshFilterCaption();

    size_t blockedHere = 0;
    for (const auto& c : updateChoices_) {
        if (!c.enabled) {
            ++blockedHere;
        }
    }
    setCurrent(L"Filtro guardado em helpers\\update_filter.txt — " +
               std::to_wstring(updateChoices_.size() - blockedHere) + L" a instalar, " +
               std::to_wstring(blockedHere) + L" bloqueada(s) neste PC (" +
               std::to_wstring(excludedTitles_.size()) + L" no ficheiro).");
    appendLog(L"Filtro updates guardado: " + std::to_wstring(excludedTitles_.size()) +
              L" título(s) bloqueado(s) em helpers\\update_filter.txt.");
}

void App::beginUpdate(UpdateMode mode) {
    if (!choosing_) {
        return;
    }
    choosing_ = false;
    updateMode_ = mode;
    updateWithoutDrivers_ = (mode == UpdateMode::NoDrivers);

    if (mode == UpdateMode::Full) {
        loadExcludedTitles();
    }

    allowClose_ = false;
    setWidgetEnabled(IDC_CLOSE, false);
    setWidgetEnabled(IDC_UNINSTALL, false);

    workStartMs_ = nowMs();
    ringPhase_ = RingPhase::Preparing;
    lastPercent_ = 0;
    uiStats_ = UpdateStats{};
    recentLog_.clear();

    if (mode == UpdateMode::DriversOnly) {
        lastPhase_ = L"A preparar drivers";
        lastCurrent_ = L"A copiar o instalador gráfico (sem Windows Update)...";
    } else {
        lastPhase_ = L"A iniciar";
        lastCurrent_ = L"A preparar o Win Auto Updater...";
    }
    layout();

    startWorker();
}

void App::runPostUpdateTasks() {
    postSummary_.clear();

    logger_.status(L"A finalizar este computador...");
    postText(hwnd_, WM_APP_PHASE, L"", static_cast<WPARAM>(RingPhase::Checking));
    postText(hwnd_, WM_APP_CURRENT, L"A definir a resolução recomendada do ecrã...");
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

    if (Drivers::isLenovoMachine()) {
        logger_.status(L"Lenovo/ThinkPad detetado...");
        postText(hwnd_, WM_APP_CURRENT,
                 L"A copiar o Lenovo System Update para o Ambiente de Trabalho...");
        const Drivers::Result lenovo = Drivers::copyLenovoSystemUpdate();
        logger_.log(L"Lenovo: " + lenovo.message);
        postSummary_ += L"Lenovo: " + lenovo.message + L"\r\n";
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
        setWidgetText(IDC_RECORD_MACRO, L"Gravar macro");
    }
    stressRunning_ = true;
    stressLog_.clear();

    setWidgetEnabled(IDC_RUN_TESTS, false);
    setWidgetText(IDC_RUN_TESTS, L"Testes a correr...");
    setWidgetEnabled(IDC_OPEN_TOOLS, false);
    applyHint(L"A iniciar a bancada de testes + macro...");
    invalidate(&pillRect_);

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
    // Recording keeps running: opening the tools is part of the capture workflow.
    stressRunning_ = true;
    stressLog_.clear();

    setWidgetEnabled(IDC_OPEN_TOOLS, false);
    setWidgetText(IDC_OPEN_TOOLS, L"A abrir...");
    setWidgetEnabled(IDC_RUN_TESTS, false);
    applyHint(L"A abrir as ferramentas de teste (MemTest por último, em cima). Sem replay.");
    invalidate(&pillRect_);

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
            post(L"Concluído. " + result.message, true);
            if (!result.ok) {
                const std::wstring alert =
                    L"Nao foi possivel abrir as ferramentas de teste.\r\n\r\n"
                    L"Pasta usada (Ambiente de Trabalho):\r\n" +
                    result.baseDir +
                    L"\r\n\r\n" + result.message +
                    L"\r\n\r\nConfirma que helpers\\programas de teste existe e tem:\r\n"
                    L"  FurMark2_x64\\FurMark_GUI.exe\r\n"
                    L"  Prime95\\prime95.exe\r\n"
                    L"  CrystalDiskMark9\\DiskMark64.exe\r\n"
                    L"  powerMAX\\powerMAX.exe  (ou powerMAX_x64.exe)\r\n"
                    L"  memtest.exe";
                MessageBoxW(hwnd_, alert.c_str(), L"Abrir ferramentas", MB_OK | MB_ICONWARNING);
            }
        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            post(L"Falha ao abrir ferramentas: " + std::wstring(what.begin(), what.end()), true);
        } catch (...) {
            post(L"Falha ao abrir ferramentas (erro desconhecido).", true);
        }
    });
}

// "Sensores" — OpenHardwareMonitor, plus BatteryCare on laptops. These are not
// part of the automated bench; they only run when the user asks for them.
void App::launchMonitorTools() {
    const std::filesystem::path base = Stress::toolsBaseDir(currentExecutable());
    std::wstring summary;
    const auto launch = [&](const std::wstring& label, const std::filesystem::path& tool) {
        std::wstring error;
        if (launchToolProcess(tool, &error)) {
            appendLog(L"Iniciado " + label + L".");
            summary += label + L" aberto. ";
        } else {
            appendLog(L"Não foi possível iniciar " + label + L": " + error + L" (" +
                      tool.wstring() + L")");
            summary += label + L": " + error + L". ";
        }
    };
    launch(L"OpenHardwareMonitor", base / L"OpenHardwareMonitor" / L"OpenHardwareMonitor.exe");
    if (Stress::detectPortable()) {
        launch(L"BatteryCare", base / L"batterycare" / L"BatteryCare.exe");
    }
    applyHint(summary);
}

// Native tests (beta): they live in native_tests/ and run as a second process of
// this same executable, so nothing they do (a GPU driver reset, most of the RAM
// under test) can disturb the updater or the macro workflow.
//   automatic — every test at once, by itself, five minutes on the graphics clock
//   manual    — one window per test, started by hand
void App::launchNativeTests(bool automatic) {
    std::wstring error;
    if (!launchDetached(currentExecutable(), automatic ? L"--native-tests --auto" : L"--native-tests --manual",
                        &error)) {
        MessageBoxW(hwnd_, (L"Não foi possível abrir os testes nativos.\r\n\r\n" + error).c_str(),
                    kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    if (automatic) {
        appendLog(L"Testes nativos automáticos (beta) iniciados.");
        applyHint(L"Testes nativos automáticos a correr — RAM, CPU, disco, gráfica e bateria; o veredicto "
                  L"aparece no fim.");
    } else {
        appendLog(L"Testes nativos manuais (beta) abertos.");
        applyHint(L"Testes nativos manuais abertos — uma janela por teste, com a barra de monitorização à "
                  L"esquerda.");
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
                        L"MacroHelper.exe não foi encontrado em helpers\\.\r\n"
                        L"Copia a pasta helpers\\ para o Ambiente de Trabalho "
                        L"(junto ao SingleUpdate.exe).",
                        kAppTitle, MB_OK | MB_ICONWARNING);
            return;
        }
        const std::filesystem::path macroFile = Stress::macroFilePath(exe, config);
        const std::wstring args = L"--record --macro \"" + macroFile.wstring() + L"\"";
        std::wstring launchError;
        if (!launchDetached(helper, args, &launchError)) {
            MessageBoxW(hwnd_,
                        (L"Não foi possível iniciar o MacroHelper.exe.\r\n\r\n" + launchError +
                         L"\r\n\r\nCaminho: " + helper.wstring())
                            .c_str(),
                        kAppTitle, MB_OK | MB_ICONERROR);
            return;
        }
        applyHint(L"MacroHelper: Iniciar gravação → Abrir ferramentas → clicar → F10. "
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
                    kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    recording_ = true;
    setWidgetText(IDC_RECORD_MACRO, L"Parar (F10)");
    applyHint(L"A gravar macro (C++): faz todos os cliques e teclas, depois F10 para parar.");
}

void App::finishRecording() {
    if (!recording_) {
        return;
    }
    recorder_.stop();
    recording_ = false;
    setWidgetText(IDC_RECORD_MACRO, L"Gravar macro");

    const auto events = recorder_.events();
    if (events.empty()) {
        applyHint(L"Gravação parada. Nada foi capturado.");
        MessageBoxW(hwnd_, L"Nada foi gravado.", kAppTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto file = Macro::filePath(currentExecutable());
    if (!Macro::save(file, events)) {
        applyHint(L"Gravação parada. Não foi possível escrever o ficheiro da macro.");
        MessageBoxW(hwnd_, L"Não foi possível escrever o ficheiro da macro junto ao executável.",
                    kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    applyHint(L"Macro guardada: " + file.wstring() + L" (" + std::to_wstring(events.size()) +
              L" passos). \"Testes automáticos\" vai reproduzi-la.");
    ShellExecuteW(nullptr, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::worker() {
    logger_.log(L"Worker do Win Auto Updater iniciado.");

    if (updateMode_ == UpdateMode::DriversOnly) {
        logger_.status(L"Modo só drivers");
        postText(hwnd_, WM_APP_CURRENT, L"A preparar resolução e instaladores gráficos...");
        logger_.log(L"Modo: atualizar só drivers (sem Windows Update).");
        runPostUpdateTasks();
        cleanupDone_ = true;
        {
            StateManager stateManager;
            stateManager.remove();
        }
        logSummary(L"Success (drivers only)");
        PostMessageW(hwnd_, WM_APP_DONE, 0, 0);
        return;
    }

    logger_.status(L"A verificar a ligação à Internet...");
    postText(hwnd_, WM_APP_CURRENT, L"À espera de ligação de rede...");

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
        if (!excludedTitles_.empty()) {
            logger_.log(L"Filtro updates: " + std::to_wstring(excludedTitles_.size()) +
                        L" atualização(ões) bloqueada(s) em helpers\\update_filter.txt.");
        }
    }

    for (int attempt = 0; attempt < 30 && running_; ++attempt) {
        if (ensureInternet()) {
            logger_.log(L"Ligação à Internet OK.");
            break;
        }
        if (attempt == 0) {
            logger_.log(L"À espera de ligação à Internet...");
        }
        if (attempt == 29) {
            postText(hwnd_, WM_APP_ERROR,
                     L"Sem ligação à Internet.\r\n\r\n"
                     L"Liga o PC à rede e volta a correr o Win Auto Updater.");
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
                 L"A retomar as atualizações após o reinício #" +
                     std::to_wstring(savedState.rebootCount) + L"...");
    } else {
        logger_.log(L"Fresh start.");
    }

    logger_.status(L"A preparar a retoma automática...");
    logger_.log(L"Registering automatic resume after reboot (Task Scheduler + RunOnce)...");
    postText(hwnd_, WM_APP_CURRENT,
             L"A registar a retoma automática para a ferramenta reabrir após reiniciar...");
    {
        Scheduler scheduler(exe);
        std::wstring scheduleError;
        if (!scheduler.create(&scheduleError)) {
            postText(hwnd_, WM_APP_ERROR,
                     L"Não foi possível registar a retoma automática.\r\n\r\n" + scheduleError +
                         L"\r\n\r\nSem ela, a ferramenta não reabre após um reinício.");
            return;
        }
        if (!scheduleError.empty()) {
            logger_.log(L"Resume registration note: " + scheduleError);
        }
        logger_.log(L"Automatic resume is ready (scheduled task and/or RunOnce).");
    }

    UpdateEngine engine;
    engine.setStatusCallback([this](UpdateEngine::Phase phase, std::wstring_view detail) {
        const auto announce = [&](RingPhase ring, const wchar_t* status, const wchar_t* fallback) {
            logger_.status(status);
            postText(hwnd_, WM_APP_PHASE, L"", static_cast<WPARAM>(ring));
            postText(hwnd_, WM_APP_CURRENT, detail.empty() ? std::wstring(fallback)
                                                           : std::wstring(detail));
        };
        switch (phase) {
            case UpdateEngine::Phase::Searching:
                announce(RingPhase::Searching, L"A procurar atualizações",
                         L"A contactar o Windows Update...");
                break;
            case UpdateEngine::Phase::Downloading:
                announce(RingPhase::Downloading, L"A transferir atualizações",
                         L"A transferir os ficheiros da atualização...");
                break;
            case UpdateEngine::Phase::Installing:
                announce(RingPhase::Installing, L"A instalar atualizações",
                         L"A instalar a atualização...");
                break;
            case UpdateEngine::Phase::CheckingAgain:
                announce(RingPhase::Checking, L"A verificar de novo",
                         L"A procurar mais atualizações...");
                break;
            case UpdateEngine::Phase::UpToDate:
                announce(RingPhase::Checking, L"Sistema atualizado",
                         L"Não há mais atualizações necessárias.");
                break;
            case UpdateEngine::Phase::RebootRequired:
                announce(RingPhase::Rebooting, L"Reinício necessário",
                         L"É necessário reiniciar para continuar.");
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

    if (updateMode_ == UpdateMode::Full && !excludedTitles_.empty()) {
        engine.setExcludedTitles(excludedTitles_);
    }

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
                logger_.status(L"A tentar de novo após uma falha temporária");
                postText(hwnd_, WM_APP_PHASE, L"", static_cast<WPARAM>(RingPhase::Checking));
                postText(hwnd_, WM_APP_CURRENT,
                         L"Falha temporária — nova tentativa dentro de segundos...");
                Sleep(5000);
                continue;
            }
            postText(hwnd_, WM_APP_ERROR,
                     cycle.message.empty() ? L"O ciclo de atualização falhou." : cycle.message);
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
                // wParam = 1: not retryable — reopening would only restart the loop.
                postText(hwnd_, WM_APP_ERROR,
                         L"Não foi possível concluir as atualizações automaticamente.\r\n\r\n"
                         L"O Windows Update parece estar preso num ciclo de reinícios.\r\n\r\n"
                         L"Conclui as atualizações em falta em Definições → Windows Update.",
                         1);
                return;
            }
            StateManager::State state;
            state.phase = L"waiting_reboot";
            state.rebootCount = newRebootCount;
            state.withoutDrivers = updateWithoutDrivers_;
            state.updateMode = static_cast<int>(updateMode_);
            stateManager.save(state);
            if (updateMode_ == UpdateMode::Full) {
                saveExcludedTitles();
            }

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

        logger_.status(L"A verificar de novo");
        postText(hwnd_, WM_APP_PHASE, L"", static_cast<WPARAM>(RingPhase::Checking));
        postText(hwnd_, WM_APP_CURRENT, L"A procurar mais atualizações após a última instalação...");
        Sleep(1000);
    }
}

void App::finishSuccess() {
    showFinishedScreen(true, L"");
}

void App::finishError(const std::wstring& message) {
    writeLogLine(L"ERRO: " + sanitizeLogLine(message));
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
    add(L"Antivirus updated: " + std::to_wstring(stats_.avUpdated) + L" (not applied: " +
        std::to_wstring(stats_.avFailed) + L")");
    add(L"Pending restart: " + std::wstring(hadPendingRestart_ ? L"Yes" : L"No"));
    add(L"Final verification: " + std::wstring(finalVerificationDone_ ? L"Passed" : L"Skipped"));
    add(L"Cleanup: " + std::wstring(cleanupDone_ ? L"Completed" : L"Skipped"));
    add(L"Result: " + result);
    add(L"");
}

void App::requestReboot() {
    ringPhase_ = RingPhase::Rebooting;
    setPhase(L"Reinício necessário");
    logger_.log(L"A pedir reinício do sistema...");
    appendLog(L"É necessário reiniciar para continuar a instalar atualizações.");
    appendLog(L"O Windows vai reiniciar dentro de 15 segundos. Guarda o teu trabalho.");

    rebootCountdown_ = 15;
    setCurrent(L"O Windows vai reiniciar em " + std::to_wstring(rebootCountdown_) +
               L" segundos para continuar as atualizações. Guarda o teu trabalho.");
    SetTimer(hwnd_, kRebootTimerId, 1000, nullptr);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void App::releaseSurfaces() {
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteDC(backDc_);
        backDc_ = nullptr;
    }
    if (backBitmap_) {
        DeleteObject(backBitmap_);
        backBitmap_ = nullptr;
    }
    if (bgDc_) {
        SelectObject(bgDc_, bgOld_);
        DeleteDC(bgDc_);
        bgDc_ = nullptr;
    }
    if (bgBitmap_) {
        DeleteObject(bgBitmap_);
        bgBitmap_ = nullptr;
    }
    backW_ = backH_ = bgW_ = bgH_ = 0;
}

// (Re)creates the cached background for the given client size.
void App::ensureSurfaces(HDC reference, int width, int height) {
    if (bgBitmap_ != nullptr && bgW_ == width && bgH_ == height) {
        return;
    }
    if (bgDc_) {
        SelectObject(bgDc_, bgOld_);
        DeleteDC(bgDc_);
        bgDc_ = nullptr;
    }
    if (bgBitmap_) {
        DeleteObject(bgBitmap_);
        bgBitmap_ = nullptr;
    }
    bgBitmap_ = ui::renderBackground(reference, width, height);
    bgDc_ = CreateCompatibleDC(reference);
    if (bgDc_ && bgBitmap_) {
        bgOld_ = SelectObject(bgDc_, bgBitmap_);
    }
    bgW_ = width;
    bgH_ = height;
}

void App::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd_, &ps);
        return;
    }

    // Persistent back buffer: only the dirty rectangle is redrawn and copied,
    // which keeps the 20 fps ring animation cheap on old machines.
    if (backBitmap_ == nullptr || backW_ != w || backH_ != h) {
        if (backDc_) {
            SelectObject(backDc_, backOld_);
            DeleteDC(backDc_);
        }
        if (backBitmap_) {
            DeleteObject(backBitmap_);
        }
        backDc_ = CreateCompatibleDC(hdc);
        backBitmap_ = CreateCompatibleBitmap(hdc, w, h);
        backOld_ = SelectObject(backDc_, backBitmap_);
        backW_ = w;
        backH_ = h;
    }

    RECT clip = ps.rcPaint;
    if (isEmptyRect(clip)) {
        clip = rc;
    }
    render(backDc_, w, h, clip);
    BitBlt(hdc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, backDc_,
           clip.left, clip.top, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

void App::render(HDC dc, int width, int height, const RECT& clip) {
    ensureSurfaces(dc, width, height);
    if (bgDc_) {
        BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, bgDc_,
               clip.left, clip.top, SRCCOPY);
    }
    if (appIcon_ && intersects(clip, iconRect_)) {
        DrawIconEx(dc, iconRect_.left, iconRect_.top, appIcon_, iconRect_.right - iconRect_.left,
                   iconRect_.bottom - iconRect_.top, 0, nullptr, DI_NORMAL);
    }
    GdiFlush();

    Canvas canvas(dc, scale_);
    canvas.g().SetClip(Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left,
                                     clip.bottom - clip.top));
    drawHeader(canvas, clip);
    switch (screen()) {
        case Screen::Home:
            drawHome(canvas, clip);
            break;
        case Screen::Working:
            drawWorking(canvas, clip);
            break;
        case Screen::Done:
            drawDone(canvas, clip);
            break;
    }
    drawWidgets(canvas, clip);
    canvas.g().Flush(Gdiplus::FlushIntentionSync);
}

std::wstring App::statusPillText(Color* dot, bool* pulse) const {
    *pulse = false;
    switch (screen()) {
        case Screen::Home:
            if (stressRunning_) {
                *dot = ui::color::violet();
                *pulse = true;
                return L"Testes a correr";
            }
            if (recording_) {
                *dot = ui::color::red();
                *pulse = true;
                return L"A gravar macro";
            }
            *dot = ui::color::green();
            return L"Pronto";
        case Screen::Working:
            *pulse = true;
            if (retryCountdown_ > 0) {
                *dot = ui::color::amber();
                return L"A reabrir";
            }
            if (ringPhase_ == RingPhase::Rebooting) {
                *dot = ui::color::amber();
                return L"A reiniciar";
            }
            *dot = ui::color::cyan();
            return L"A atualizar";
        case Screen::Done:
            if (uninstallState_ == 1) {
                *dot = ui::color::amber();
                *pulse = true;
                return L"A desinstalar";
            }
            if (uninstallState_ == 2) {
                *dot = ui::color::muted();
                return L"Desinstalado";
            }
            if (stressRunning_) {
                *dot = ui::color::violet();
                *pulse = true;
                return L"Testes a correr";
            }
            *dot = doneSuccess_ ? ui::color::green() : ui::color::red();
            return doneSuccess_ ? L"Concluído" : L"Erro";
    }
    *dot = ui::color::muted();
    return L"";
}

void App::drawHeader(Canvas& canvas, const RECT& clip) {
    const RECT header{0, 0, clientW_, px(80.0f)};
    if (!intersects(clip, header)) {
        return;
    }
    const float margin = canvas.s(26.0f);
    const float textX = static_cast<float>(iconRect_.right) + canvas.s(14.0f);
    const float textW = static_cast<float>(pillRect_.left) - canvas.s(12.0f) - textX;
    canvas.text(kAppTitle, RectF(textX, canvas.s(15.0f), textW, canvas.s(30.0f)), 21.0f,
                Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
    canvas.text(L"Atualizações automáticas do Windows · bancada de testes",
                RectF(textX, canvas.s(45.0f), textW, canvas.s(20.0f)), 12.5f, Weight::Regular,
                ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);

    Color dot = ui::color::muted();
    bool pulse = false;
    const std::wstring label = statusPillText(&dot, &pulse);
    if (!label.empty()) {
        const float labelW = canvas.measure(label, 12.0f, Weight::Semibold).Width;
        const float pillW = std::min(static_cast<float>(pillRect_.right - pillRect_.left),
                                     labelW + canvas.s(44.0f));
        const RectF pill(static_cast<float>(pillRect_.right) - pillW,
                         static_cast<float>(pillRect_.top), pillW,
                         static_cast<float>(pillRect_.bottom - pillRect_.top));
        const float phase = static_cast<float>(animFrame_ % kAnimFramesPerTurn) /
                            static_cast<float>(kAnimFramesPerTurn);
        canvas.pill(pill, label, dot, pulse, phase);
    }

    canvas.divider(margin, static_cast<float>(clientW_) - margin, canvas.s(78.0f));
}

void App::drawHome(Canvas& canvas, const RECT& clip) {
    const auto card = [&](const RECT& rc, const wchar_t* caption, Color accent) {
        if (isEmptyRect(rc) || !intersects(clip, rc)) {
            return;
        }
        const RectF r = ui::toRectF(rc);
        canvas.glass(r, 20.0f);
        const float pad = canvas.s(20.0f);
        const float dotSize = canvas.s(7.0f);
        const float captionY = r.Y + canvas.s(18.0f);
        Gdiplus::SolidBrush brush(accent);
        canvas.g().FillEllipse(&brush, r.X + pad, captionY + (canvas.s(16.0f) - dotSize) / 2.0f,
                               dotSize, dotSize);
        canvas.text(ui::spaced(caption),
                    RectF(r.X + pad + dotSize + canvas.s(9.0f), captionY,
                          r.Width - pad * 2.0f - dotSize, canvas.s(16.0f)),
                    10.0f, Weight::Semibold, ui::withAlpha(accent, 235),
                    ui::kLeft | ui::kVCenter | ui::kEllipsis);
    };
    card(cardLeft_, L"Atualizações", ui::color::cyan());
    card(cardRight_, L"Bancada de testes", ui::color::violet());

    // Presence of the bench pieces under helpers\ — tells at a glance whether
    // the pen was copied completely and a macro exists.
    if (!isEmptyRect(chipsRect_) && intersects(clip, chipsRect_)) {
        struct Chip {
            const wchar_t* label;
            bool ok;
        };
        const Chip chips[3] = {{L"Ferramentas", toolsPresent_},
                               {L"MacroHelper", helperPresent_},
                               {L"Macro gravada", macroPresent_}};
        const RectF area = ui::toRectF(chipsRect_);
        const float gap = canvas.s(10.0f);
        const float w = (area.Width - gap * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const RectF r(area.X + (w + gap) * static_cast<float>(i), area.Y, w, area.Height);
            canvas.fillRound(r, 12.0f, Color(12, 255, 255, 255));
            canvas.strokeRound(r, 12.0f, Color(30, 255, 255, 255));
            const Color state = chips[i].ok ? ui::color::green() : ui::color::red();
            const float iconSize = canvas.s(12.0f);
            const float chipPad = canvas.s(10.0f);
            canvas.icon(chips[i].ok ? ui::Icon::Check : ui::Icon::Cross,
                        RectF(r.X + chipPad, r.Y + (r.Height - iconSize) / 2.0f, iconSize, iconSize),
                        state);
            const float tx = r.X + chipPad + iconSize + canvas.s(7.0f);
            const float tw = r.GetRight() - canvas.s(6.0f) - tx;
            canvas.text(chips[i].label, RectF(tx, r.Y, tw, r.Height),
                        canvas.fitSize(chips[i].label, 11.5f, 9.0f, Weight::Semibold, tw),
                        Weight::Semibold, chips[i].ok ? ui::color::text() : ui::color::muted(),
                        ui::kLeft | ui::kVCenter | ui::kEllipsis);
        }
    }

    if (!isEmptyRect(hintRect_) && intersects(clip, hintRect_)) {
        const RectF r = ui::toRectF(hintRect_);
        canvas.glass(r, 14.0f, 0.72f);
        const float iconSize = canvas.s(16.0f);
        canvas.icon(ui::Icon::Info,
                    RectF(r.X + canvas.s(16.0f), r.Y + (r.Height - iconSize) / 2.0f, iconSize,
                          iconSize),
                    ui::color::cyan());
        const float tx = r.X + canvas.s(16.0f) + iconSize + canvas.s(12.0f);
        canvas.text(lastCurrent_,
                    RectF(tx, r.Y + canvas.s(5.0f), r.GetRight() - canvas.s(16.0f) - tx,
                          r.Height - canvas.s(10.0f)),
                    12.5f, Weight::Regular, ui::color::muted(),
                    ui::kLeft | ui::kVCenter | ui::kWrap | ui::kEllipsis);
    }
}

void App::drawStats(Canvas& canvas, const RECT& clip) {
    if (isEmptyRect(statsRect_) || !intersects(clip, statsRect_)) {
        return;
    }
    struct StatDef {
        const wchar_t* caption;
        int value;
        Color color;
    };
    const StatDef defs[6] = {
        {L"ENCONTRADAS", uiStats_.found, ui::color::text()},
        {L"TRANSFERIDAS", uiStats_.downloaded, ui::color::cyan()},
        {L"INSTALADAS", uiStats_.installed, ui::color::green()},
        {L"CONCLUÍDAS", uiStats_.finished, ui::color::text()},
        {L"IGNORADAS", uiStats_.skipped, ui::color::amber()},
        {L"FALHADAS", uiStats_.failed, ui::color::red()},
    };
    const RectF area = ui::toRectF(statsRect_);
    const float gap = canvas.s(12.0f);
    const float w = (area.Width - gap * 5.0f) / 6.0f;
    for (int i = 0; i < 6; ++i) {
        const RectF chip(area.X + (w + gap) * static_cast<float>(i), area.Y, w, area.Height);
        const Color valueColor =
            defs[i].value == 0 && i != 0 ? ui::withAlpha(defs[i].color, 150) : defs[i].color;
        canvas.statChip(chip, defs[i].caption, defs[i].value, valueColor);
    }
}

void App::drawWorking(Canvas& canvas, const RECT& clip) {
    const float phase = static_cast<float>(animFrame_ % kAnimFramesPerTurn) /
                        static_cast<float>(kAnimFramesPerTurn);

    if (!isEmptyRect(heroRect_) && intersects(clip, inflated(heroRect_, px(12.0f)))) {
        const RectF hero = ui::toRectF(heroRect_);
        canvas.glass(hero, 22.0f);

        // Progress ring.
        const RectF ring = ui::toRectF(ringRect_);
        const float cx = ring.X + ring.Width / 2.0f;
        const float cy = ring.Y + ring.Height / 2.0f;
        const float radius = ring.Width / 2.0f - canvas.s(8.0f);
        const bool determinate =
            ringPhase_ == RingPhase::Downloading || ringPhase_ == RingPhase::Installing;
        canvas.ring(cx, cy, radius, 9.0f,
                    determinate ? static_cast<float>(lastPercent_) / 100.0f : -1.0f, phase);

        const wchar_t* ringCaption = L"A PREPARAR";
        switch (ringPhase_) {
            case RingPhase::Preparing:
                ringCaption = L"A PREPARAR";
                break;
            case RingPhase::Searching:
                ringCaption = L"A PROCURAR";
                break;
            case RingPhase::Downloading:
                ringCaption = L"TRANSFERIR";
                break;
            case RingPhase::Installing:
                ringCaption = L"INSTALAR";
                break;
            case RingPhase::Checking:
                ringCaption = L"A VERIFICAR";
                break;
            case RingPhase::Rebooting:
                ringCaption = L"REINÍCIO";
                break;
        }
        const bool countdown = ringPhase_ == RingPhase::Rebooting && rebootCountdown_ > 0;
        if (determinate || countdown) {
            const std::wstring big = countdown ? std::to_wstring(rebootCountdown_)
                                               : std::to_wstring(lastPercent_) + L"%";
            canvas.text(big, RectF(ring.X, cy - canvas.s(24.0f), ring.Width, canvas.s(34.0f)),
                        27.0f, Weight::Semibold, ui::color::text(), ui::kCenter | ui::kVCenter);
            canvas.text(ringCaption,
                        RectF(ring.X, cy + canvas.s(10.0f), ring.Width, canvas.s(15.0f)), 8.5f,
                        Weight::Semibold, ui::color::muted(), ui::kCenter | ui::kVCenter);
        } else {
            canvas.text(ringCaption, RectF(ring.X, cy - canvas.s(9.0f), ring.Width, canvas.s(18.0f)),
                        9.5f, Weight::Semibold, ui::color::muted(), ui::kCenter | ui::kVCenter);
        }

        // Status text beside the ring.
        const float tx = ring.GetRight() + canvas.s(30.0f);
        const float tw = hero.GetRight() - canvas.s(26.0f) - tx;
        const wchar_t* modeLabel = updateMode_ == UpdateMode::NoDrivers
                                       ? L"Windows sem drivers"
                                       : (updateMode_ == UpdateMode::DriversOnly
                                              ? L"Só drivers"
                                              : L"Windows + drivers");
        canvas.text(ui::spaced(L"Estado") + L"   ·   " + ui::spaced(modeLabel),
                    RectF(tx, hero.Y + canvas.s(24.0f), tw, canvas.s(16.0f)), 9.5f,
                    Weight::Semibold, ui::withAlpha(ui::color::cyan(), 225),
                    ui::kLeft | ui::kVCenter | ui::kEllipsis);
        canvas.text(lastPhase_, RectF(tx, hero.Y + canvas.s(44.0f), tw, canvas.s(34.0f)), 23.0f,
                    Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        // Wrapped; takes whatever height the card leaves above the meta line.
        const float currentY = hero.Y + canvas.s(82.0f);
        const float currentH =
            std::max(canvas.s(20.0f), hero.GetBottom() - canvas.s(48.0f) - currentY);
        canvas.text(lastCurrent_, RectF(tx, currentY, tw, currentH), 13.5f, Weight::Regular,
                    ui::color::muted(), ui::kLeft | ui::kTop | ui::kWrap | ui::kEllipsis);

        std::wstring meta = L"Tempo decorrido  " + formatElapsed(nowMs() - workStartMs_);
        if (runNumber_ > 1) {
            meta += L"     ·     Execução #" + std::to_wstring(runNumber_);
        }
        if (retried_) {
            meta += L"     ·     Tentativa automática";
        }
        canvas.text(meta, RectF(tx, hero.GetBottom() - canvas.s(40.0f), tw, canvas.s(18.0f)), 12.0f,
                    Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }

    drawStats(canvas, clip);

    // The only "log" on screen: a tiny block with the last few actions.
    if (!isEmptyRect(activityRect_) && intersects(clip, activityRect_)) {
        const RectF r = ui::toRectF(activityRect_);
        canvas.glass(r, 16.0f, 0.78f);
        const float pad = canvas.s(18.0f);
        const float dot = canvas.s(7.0f);
        const float wave = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
        Gdiplus::SolidBrush dotBrush(
            ui::withAlpha(ui::color::cyan(), 110 + static_cast<int>(145.0f * wave)));
        canvas.g().FillEllipse(&dotBrush, r.X + pad, r.Y + canvas.s(15.0f), dot, dot);
        canvas.text(ui::spaced(L"Atividade"),
                    RectF(r.X + pad + dot + canvas.s(9.0f), r.Y + canvas.s(10.0f),
                          r.Width - pad * 2.0f, canvas.s(17.0f)),
                    9.5f, Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);

        const float lineH = canvas.s(19.0f);
        const float firstY = r.Y + canvas.s(33.0f);
        const int capacity =
            std::max(1, static_cast<int>((r.GetBottom() - canvas.s(8.0f) - firstY) / lineH));
        const int count = std::min<int>(capacity, static_cast<int>(recentLog_.size()));
        const int start = static_cast<int>(recentLog_.size()) - count;
        for (int i = 0; i < count; ++i) {
            const bool newest = i == count - 1;
            canvas.text(recentLog_[static_cast<size_t>(start + i)],
                        RectF(r.X + pad, firstY + lineH * static_cast<float>(i),
                              r.Width - pad * 2.0f, lineH),
                        11.5f, Weight::Regular,
                        newest ? ui::withAlpha(ui::color::text(), 235) : ui::color::dim(),
                        ui::kLeft | ui::kVCenter | ui::kEllipsis | ui::kMono);
        }
        if (count == 0) {
            canvas.text(L"À espera da primeira ação...",
                        RectF(r.X + pad, firstY, r.Width - pad * 2.0f, lineH), 11.5f,
                        Weight::Regular, ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kMono);
        }
    }

    if (!isEmptyRect(footerTextRect_) && intersects(clip, footerTextRect_)) {
        canvas.text(L"Registo completo em %TEMP%\\WinAutoUpdater\\updater.log",
                    ui::toRectF(footerTextRect_), 11.5f, Weight::Regular, ui::color::dim(),
                    ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }
}

std::wstring App::detailText() const {
    std::wstring text = doneSummary_;
    if (!stressLog_.empty()) {
        if (!text.empty()) {
            text += L"\r\n\r\n";
        }
        text += L"Testes de stress:\r\n" + stressLog_;
    }
    if (!doneHint_.empty()) {
        if (!text.empty()) {
            text += L"\r\n\r\n";
        }
        text += doneHint_;
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

void App::drawDone(Canvas& canvas, const RECT& clip) {
    const float phase = static_cast<float>(animFrame_ % kAnimFramesPerTurn) /
                        static_cast<float>(kAnimFramesPerTurn);

    if (!isEmptyRect(heroRect_) && intersects(clip, inflated(heroRect_, px(12.0f)))) {
        const RectF hero = ui::toRectF(heroRect_);
        canvas.glass(hero, 22.0f);

        Color accent = doneSuccess_ ? ui::color::green() : ui::color::red();
        ui::Icon icon = doneSuccess_ ? ui::Icon::Check : ui::Icon::Cross;
        if (uninstallState_ == 1) {
            accent = ui::color::amber();
            icon = ui::Icon::Trash;
        } else if (uninstallState_ == 2) {
            accent = ui::color::green();
            icon = ui::Icon::Check;
        }

        const float d = canvas.s(56.0f);
        const float cx = hero.X + canvas.s(24.0f);
        const float cy = hero.Y + (hero.Height - d) / 2.0f;
        const RectF circle(cx, cy, d, d);
        canvas.glow(circle, 28.0f, ui::withAlpha(accent, 120), 9.0f);
        canvas.fillRound(circle, 28.0f, ui::withAlpha(accent, 46));
        canvas.strokeRound(circle, 28.0f, ui::withAlpha(accent, 220), 1.5f);
        if (uninstallState_ == 1) {
            canvas.ring(cx + d / 2.0f, cy + d / 2.0f, d / 2.0f - canvas.s(3.0f), 3.0f, -1.0f, phase);
        }
        const float iconSize = canvas.s(24.0f);
        canvas.icon(icon, RectF(cx + (d - iconSize) / 2.0f, cy + (d - iconSize) / 2.0f, iconSize,
                                iconSize),
                    accent);

        const float tx = circle.GetRight() + canvas.s(20.0f);
        const float tw = hero.GetRight() - canvas.s(24.0f) - tx;
        canvas.text(doneTitle_, RectF(tx, hero.Y + canvas.s(18.0f), tw, canvas.s(32.0f)), 22.0f,
                    Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        canvas.text(doneSubtitle_, RectF(tx, hero.Y + canvas.s(52.0f), tw, canvas.s(38.0f)), 13.0f,
                    Weight::Regular, ui::color::muted(),
                    ui::kLeft | ui::kTop | ui::kWrap | ui::kEllipsis);
    }

    drawStats(canvas, clip);

    if (!isEmptyRect(detailRect_) && intersects(clip, detailRect_)) {
        const RectF r = ui::toRectF(detailRect_);
        canvas.glass(r, 16.0f, 0.78f);

        const float padX = canvas.s(20.0f);
        const float padY = canvas.s(14.0f);
        const RectF inner(r.X + padX, r.Y + padY, r.Width - padX * 2.0f - canvas.s(8.0f),
                          r.Height - padY * 2.0f);
        const std::wstring text = detailText();
        const float contentH =
            text.empty() ? 0.0f : canvas.measure(text, 13.0f, Weight::Regular, inner.Width).Height;
        detailContentH_ = static_cast<int>(std::ceil(contentH));
        const int maxScroll =
            std::max(0, detailContentH_ - static_cast<int>(std::floor(inner.Height)));
        if (detailStickBottom_) {
            detailScroll_ = maxScroll;
        }
        detailScroll_ = std::clamp(detailScroll_, 0, maxScroll);

        if (!text.empty() && inner.Width > 4.0f && inner.Height > 4.0f) {
            const Gdiplus::GraphicsState saved = canvas.g().Save();
            canvas.g().SetClip(inner, Gdiplus::CombineModeIntersect);
            canvas.text(text,
                        RectF(inner.X, inner.Y - static_cast<float>(detailScroll_), inner.Width,
                              contentH + canvas.s(40.0f)),
                        13.0f, Weight::Regular, ui::withAlpha(ui::color::text(), 222),
                        ui::kLeft | ui::kTop | ui::kWrap);
            canvas.g().Restore(saved);
        }

        if (maxScroll > 0) {
            // Slim scroll indicator (mouse wheel scrolls the text).
            const float trackH = inner.Height;
            const float thumbH =
                std::max(canvas.s(24.0f), trackH * inner.Height / std::max(1.0f, contentH));
            const float thumbY = inner.Y + (trackH - thumbH) * static_cast<float>(detailScroll_) /
                                               static_cast<float>(maxScroll);
            canvas.fillRound(RectF(r.GetRight() - canvas.s(11.0f), thumbY, canvas.s(4.0f), thumbH),
                             2.0f, Color(90, 255, 255, 255));
        }
    }
}

void App::drawWidgets(Canvas& canvas, const RECT& clip) {
    const int glowMargin = px(14.0f);
    for (const Widget& w : widgets_) {
        if (!w.visible || !intersects(clip, inflated(w.rc, glowMargin))) {
            continue;
        }
        ui::ButtonState state;
        state.disabled = !w.enabled;
        state.hover = hoverId_ == w.id;
        state.pressed = pressedId_ == w.id && hoverId_ == w.id;
        const RectF r = ui::toRectF(w.rc);
        if (w.toggle) {
            bool on = reopenAfterRestart_;
            std::wstring sub;
            if (w.id == IDC_STRESS_TOGGLE) {
                on = autoStressAfterUpdate_;
                sub = on ? L"Ligado — a bancada corre no fim da atualização"
                         : L"Desligado — só atualiza";
            } else if (w.id == IDC_NATIVE_TOGGLE) {
                on = nativeAfterUpdate_;
                sub = on ? L"Ligado — os nossos testes automáticos (5 min)"
                         : L"Desligado — ferramentas + macro (predefinido)";
            }
            canvas.toggleRow(r, w.text, sub, on, state);
        } else {
            canvas.button(r, w.text, w.sub, w.style, state, w.icon);
        }
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

int App::widgetAt(POINT pt) const {
    for (const Widget& w : widgets_) {
        if (w.visible && w.enabled && PtInRect(&w.rc, pt)) {
            return w.id;
        }
    }
    return 0;
}

void App::onMouseMove(POINT pt) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        trackingMouse_ = true;
    }
    const int id = widgetAt(pt);
    if (id != hoverId_) {
        const int previous = hoverId_;
        hoverId_ = id;
        invalidateWidget(previous);
        invalidateWidget(id);
    }
}

void App::onMouseLeave() {
    trackingMouse_ = false;
    if (hoverId_ != 0) {
        const int previous = hoverId_;
        hoverId_ = 0;
        invalidateWidget(previous);
    }
}

void App::onLeftDown(POINT pt) {
    const int id = widgetAt(pt);
    if (id == 0) {
        return;
    }
    pressedId_ = id;
    hoverId_ = id;
    SetCapture(hwnd_);
    invalidateWidget(id);
}

void App::onLeftUp(POINT pt) {
    if (pressedId_ == 0) {
        return;
    }
    const int id = pressedId_;
    pressedId_ = 0;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    invalidateWidget(id);
    if (widgetAt(pt) == id) {
        onCommand(id);
    }
}

void App::onMouseWheel(int delta) {
    if (screen() != Screen::Done || isEmptyRect(detailRect_)) {
        return;
    }
    detailStickBottom_ = false;
    detailScroll_ -= static_cast<int>(std::lround(static_cast<float>(delta) / 120.0f * scale_ * 48.0f));
    if (detailScroll_ < 0) {
        detailScroll_ = 0;
    }
    invalidate(&detailRect_);
}

void App::onCommand(int id) {
    switch (id) {
        case IDC_UPDATE:
            beginUpdate(UpdateMode::Full);
            break;
        case IDC_UPDATE_DRIVERS_ONLY:
            beginUpdate(UpdateMode::DriversOnly);
            break;
        case IDC_UPDATE_NO_DRIVERS:
            beginUpdate(UpdateMode::NoDrivers);
            break;
        case IDC_CONFIGURE_UPDATES:
            configureUpdates();
            break;
        case IDC_STRESS_TOGGLE:
            if (choosing_) {
                autoStressAfterUpdate_ = !autoStressAfterUpdate_;
                invalidateWidget(IDC_STRESS_TOGGLE);
            }
            break;
        case IDC_NATIVE_TOGGLE:
            if (choosing_) {
                nativeAfterUpdate_ = !nativeAfterUpdate_;
                invalidateWidget(IDC_NATIVE_TOGGLE);
                // Remembered in stress.ini: the update restarts the PC (and this
                // tool) several times before the tests run.
                if (!writeIniValue(Stress::configPath(currentExecutable()), "NativeTests",
                                   nativeAfterUpdate_ ? "1" : "0")) {
                    appendLog(L"Não foi possível gravar NativeTests em helpers\\stress.ini.");
                }
            }
            break;
        case IDC_REOPEN_TOGGLE:
            if (uiDoneMode_) {
                reopenAfterRestart_ = !reopenAfterRestart_;
                invalidateWidget(IDC_REOPEN_TOGGLE);
                Scheduler scheduler(currentExecutable());
                if (reopenAfterRestart_) {
                    std::wstring error;
                    if (!scheduler.create(&error)) {
                        MessageBoxW(hwnd_,
                                    (L"Não foi possível agendar a reabertura após reinício:\r\n" +
                                     error)
                                        .c_str(),
                                    kAppTitle, MB_OK | MB_ICONWARNING);
                    }
                } else {
                    scheduler.remove();
                }
            }
            break;
        case IDC_UNINSTALL:
            // Available on the home screen and on the final screen.
            if ((uiDoneMode_ || choosing_) && uninstallState_ == 0) {
                performUninstall();
            }
            break;
        case IDC_RUN_TESTS:
            runStressTests();
            break;
        case IDC_RECORD_MACRO:
            toggleRecording();
            break;
        case IDC_MONITOR:
            launchMonitorTools();
            break;
        case IDC_NATIVE_TESTS:
            launchNativeTests(true);
            break;
        case IDC_NATIVE_MANUAL:
            launchNativeTests(false);
            break;
        case IDC_OPEN_TOOLS:
            openTestTools();
            break;
        case IDC_GUIDE:
            showGuide();
            break;
        case IDC_RETRY:
            if (uiDoneMode_ && !doneSuccess_) {
                // Manual retry: same close-and-reopen, and the fresh process may
                // still use its own automatic retry once.
                if (!relaunchSelf(false)) {
                    MessageBoxW(hwnd_, L"Não foi possível reabrir a ferramenta automaticamente.\r\n"
                                       L"Fecha e abre o SingleUpdate.exe manualmente.",
                                kAppTitle, MB_OK | MB_ICONWARNING);
                }
            }
            break;
        case IDC_CLOSE:
            if (allowClose_) {
                running_ = false;
                DestroyWindow(hwnd_);
            }
            break;
        default:
            break;
    }
}

void App::onAnimTick() {
    ++animFrame_;
    const Screen current = screen();
    if (current == Screen::Working) {
        const RECT ring = inflated(ringRect_, px(12.0f));
        invalidate(&ring);
        invalidate(&pillRect_);
        // Activity dot.
        RECT dot = activityRect_;
        dot.right = dot.left + px(40.0f);
        dot.bottom = dot.top + px(30.0f);
        invalidate(&dot);
        if (animFrame_ % 20 == 0) {
            invalidate(&heroRect_);  // elapsed time, once per second
        }
    } else if (stressRunning_ || recording_ || uninstallState_ == 1) {
        invalidate(&pillRect_);
        if (uninstallState_ == 1) {
            invalidate(&heroRect_);
        }
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

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
            if (wParam != SIZE_MINIMIZED) {
                layout();
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // Never let the window shrink below a legible layout.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = ui::dpiForWindow(hwnd);
            const float dpiScale = static_cast<float>(dpi) / 96.0f;
            const float minScale = std::min(fitScale_, std::max(0.5f, 0.62f * dpiScale));
            RECT frame{0, 0, 0, 0};
            ui::adjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                                 static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
            info->ptMinTrackSize.x =
                static_cast<LONG>(std::lround(kDesignW * minScale)) + (frame.right - frame.left);
            info->ptMinTrackSize.y =
                static_cast<LONG>(std::lround(kDesignH * minScale)) + (frame.bottom - frame.top);
            return 0;
        }
        case WM_DPICHANGED: {
            // Moved to a monitor with different scaling: take Windows' suggested rect.
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            // Resolution changed (this tool sets the maximum resolution itself, and
            // GPU drivers get installed mid-run): re-fit and re-centre the window.
            fitWindowToScreen();
            layout();
            return 0;
        case WM_ERASEBKGND:
            // The whole client area is painted in WM_PAINT (double buffered).
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSELEAVE:
            onMouseLeave();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            onLeftDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_LBUTTONUP:
            onLeftUp(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_CAPTURECHANGED:
            if (pressedId_ != 0) {
                const int id = pressedId_;
                pressedId_ = 0;
                invalidateWidget(id);
            }
            return 0;
        case WM_MOUSEWHEEL:
            onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hoverId_ != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE && screen() == Screen::Home) {
                // Coming back from MacroHelper / Explorer: refresh the bench chips.
                refreshHelperStatus();
            }
            break;
        case WM_APP_STATUS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_ && retryCountdown_ == 0) {
                    setStatus(*text);
                }
                delete text;
            }
            return 0;
        }
        case WM_APP_PHASE: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            delete text;
            if (!uiDoneMode_ && retryCountdown_ == 0) {
                setRingPhase(static_cast<RingPhase>(static_cast<int>(wParam)));
            }
            return 0;
        }
        case WM_APP_CURRENT: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_ && retryCountdown_ == 0) {
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
                    invalidate(&statsRect_);
                }
                delete snapshot;
            }
            return 0;
        }
        case WM_APP_LOG: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                if (!uiDoneMode_) {
                    pushRecentLog(*text);
                }
                delete text;
            }
            return 0;
        }
        case WM_APP_STRESS: {
            auto* message = reinterpret_cast<StressMessage*>(lParam);
            if (message) {
                stressLog_ += message->text + L"\r\n";
                writeLogLine(L"[stress] " + sanitizeLogLine(message->text));
                if (screen() == Screen::Done) {
                    doneHint_.clear();
                    detailStickBottom_ = true;
                    invalidate(&detailRect_);
                } else if (screen() == Screen::Home) {
                    setCurrent(message->text);
                }
                if (message->final) {
                    stressRunning_ = false;
                    setWidgetEnabled(IDC_RUN_TESTS, true);
                    setWidgetText(IDC_RUN_TESTS, L"Testes automáticos");
                    setWidgetEnabled(IDC_OPEN_TOOLS, true);
                    setWidgetText(IDC_OPEN_TOOLS, L"Abrir ferramentas");
                    invalidate(&pillRect_);
                    refreshHelperStatus();
                }
                delete message;
            }
            return 0;
        }
        case WM_APP_UNINSTALL_DONE: {
            choosing_ = false;
            uiDoneMode_ = true;
            uninstallState_ = 2;
            doneSuccess_ = true;
            showDoneStats_ = false;
            doneTitle_ = L"Desinstalado — reinicia para concluir";
            doneSubtitle_ = L"Clica em Fechar e reinicia o PC para completar a remoção.";
            doneSummary_ = L"DESINSTALAÇÃO CONCLUÍDA\r\n\r\n"
                           L"• Tarefa de reabertura removida.\r\n"
                           L"• Pasta helpers\\ removida (ferramentas, macro, …).\r\n"
                           L"• O SingleUpdate.exe é apagado no próximo reinício.";
            doneHint_.clear();
            stressLog_.clear();
            setWidgetEnabled(IDC_UNINSTALL, false);
            setWidgetText(IDC_UNINSTALL, L"Desinstalado");
            allowClose_ = true;
            setWidgetEnabled(IDC_CLOSE, true);
            setWidgetText(IDC_CLOSE, L"Fechar");
            layout();
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
            if (!uiDoneMode_ && retryCountdown_ == 0) {
                if (percentPtr) {
                    setProgressPercent(*percentPtr);
                }
                if (text && !text->empty() && *text != lastCurrent_) {
                    // The percentage lives in the ring; the label shows what is
                    // being transferred / installed.
                    setCurrent(*text);
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
            // wParam = 0: transient failure — close and reopen once on our own
            // before bothering the user with the error screen.
            if (wParam == 0 && scheduleAutoRetry(message)) {
                return 0;
            }
            finishError(message);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kRebootTimerId) {
                --rebootCountdown_;
                if (rebootCountdown_ > 0) {
                    setCurrent(L"O Windows vai reiniciar em " + std::to_wstring(rebootCountdown_) +
                               L" segundos para continuar as atualizações. Guarda o teu trabalho.");
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
            } else if (wParam == kRetryTimerId) {
                --retryCountdown_;
                if (retryCountdown_ > 0) {
                    lastCurrent_ = L"Fechar e reabrir costuma resolver. Nova tentativa em " +
                                   std::to_wstring(retryCountdown_) + L" s… (" +
                                   firstLine(pendingRetryError_) + L")";
                    invalidate(&heroRect_);
                } else {
                    KillTimer(hwnd_, kRetryTimerId);
                    retryCountdown_ = 0;
                    if (!relaunchSelf(true)) {
                        finishError(pendingRetryError_);
                    }
                }
            } else if (wParam == kAnimTimerId) {
                onAnimTick();
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
            KillTimer(hwnd_, kRetryTimerId);
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

#ifdef SU_UI_SNAPSHOT
// ---------------------------------------------------------------------------
// Developer aid (only in -DSU_UI_SNAPSHOT=ON builds): render each screen to PNG
// at several client sizes, without a window, to review the layout offline.
// ---------------------------------------------------------------------------
int App::runSnapshot(const std::filesystem::path& directory) {
    ui::startup();
    initWidgets();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    CLSID png{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);

    const auto shot = [&](const std::wstring& name, int w, int h) {
        layout(w, h);
        HDC screenDc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screenDc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        const RECT clip{0, 0, w, h};
        render(mem, w, h, clip);
        GdiFlush();
        {
            Gdiplus::Bitmap image(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
            const auto file = directory / (name + L"_" + std::to_wstring(w) + L"x" +
                                           std::to_wstring(h) + L".png");
            image.Save(file.c_str(), &png, nullptr);
        }
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screenDc);
    };

    const int sizes[][2] = {{940, 632}, {780, 524}, {1340, 760}, {1264, 950}};
    const auto all = [&](const std::wstring& name) {
        for (const auto& size : sizes) {
            shot(name, size[0], size[1]);
        }
    };

    // Home
    nativeAfterUpdate_ = true;
    lastCurrent_ = kHomeHint;
    toolsPresent_ = true;
    helperPresent_ = true;
    macroPresent_ = false;
    excludedTitles_.insert(L"x");
    refreshFilterCaption();
    hoverId_ = IDC_UPDATE_NO_DRIVERS;
    all(L"1_home");
    hoverId_ = 0;

    // Working — downloading
    choosing_ = false;
    setWidgetEnabled(IDC_CLOSE, false);
    setWidgetEnabled(IDC_UNINSTALL, false);
    workStartMs_ = nowMs() - 754000;
    ringPhase_ = RingPhase::Downloading;
    lastPercent_ = 43;
    lastPhase_ = L"A transferir atualizações";
    lastCurrent_ = L"2026-11 Atualização Cumulativa para Windows 11 Version 24H2 para sistemas "
                   L"baseados em x64 (KB5046617)";
    uiStats_.found = 14;
    uiStats_.downloaded = 6;
    uiStats_.installed = 5;
    uiStats_.finished = 6;
    uiStats_.skipped = 1;
    uiStats_.failed = 0;
    recentLog_ = {L"Installed: Intel - System - 10.1.45.9", L"Download OK.",
                  L"Installing: Security Intelligence Update for Microsoft Defender Antivirus",
                  L"Downloading: 2026-11 Cumulative Update for Windows 11 (KB5046617)"};
    all(L"2_working");

    // Working — searching (indeterminate)
    ringPhase_ = RingPhase::Searching;
    lastPhase_ = L"A procurar atualizações";
    lastCurrent_ = L"A procurar atualizações no Windows Update...";
    animFrame_ = 13;
    shot(L"3_searching", 940, 632);

    // Done — success with stress log
    uiDoneMode_ = true;
    doneSuccess_ = true;
    showDoneStats_ = true;
    doneTitle_ = L"Concluído — este PC está pronto";
    doneSubtitle_ = L"Todas as atualizações Windows disponíveis foram instaladas.";
    doneSummary_ = L"Antivírus: Microsoft Defender atualizado.\r\n\r\n"
                   L"Configuração pós-atualização:\r\n"
                   L"Ecrã: Resolution set to 1920 x 1080 @ 32-bit.\r\n"
                   L"Gráficos (NVIDIA): nvidia_app.exe copiado para o Ambiente de Trabalho.\r\n\r\n"
                   L"Podes fechar esta janela, correr os testes ou desinstalar a ferramenta.";
    stressLog_ = L"• Opened FurMark\r\n• Opened Prime95\r\n• Opened CrystalDiskMark\r\n"
                 L"• Opened powerMAX\r\n• Opened MemTest\r\nReplaying the recorded macro...\r\n";
    setWidgetEnabled(IDC_CLOSE, true);
    setWidgetEnabled(IDC_UNINSTALL, true);
    all(L"4_done");

    // Done — error
    doneSuccess_ = false;
    stressLog_.clear();
    doneTitle_ = L"Interrompido — ocorreu um erro";
    doneSubtitle_ = L"All updates failed.";
    doneSummary_ = L"All updates failed.\r\n\r\n- 2026-11 Cumulative Update (KB5046617): install — "
                   L"Install failed: Failed (0x80070643 — Fatal error during installation).\r\n\r\n"
                   L"A ferramenta já foi reaberta automaticamente uma vez e o erro repetiu-se.";
    uiStats_.failed = 1;
    shot(L"5_error", 940, 632);
    shot(L"5_error", 780, 524);

    releaseSurfaces();
    ui::shutdown();
    return 0;
}
#endif
