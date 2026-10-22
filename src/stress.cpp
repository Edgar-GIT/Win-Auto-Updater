#include "stress.hpp"

#include "macro.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

std::wstring trim(std::wstring value) {
    const auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool parseBool(const std::wstring& raw, bool fallback) {
    const std::wstring v = lower(trim(raw));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") {
        return true;
    }
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") {
        return false;
    }
    return fallback;
}

int parseInt(const std::wstring& raw, int fallback) {
    try {
        const std::wstring v = trim(raw);
        if (v.empty()) {
            return fallback;
        }
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::filesystem::path currentExecutable() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
}

std::filesystem::path tempDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    if (len == 0) {
        return {};
    }
    return std::filesystem::path(buffer) / L"WinAutoUpdater";
}

// Opens a GUI .exe the same way Explorer does (double-click): ShellExecute "open".
// FurMark / Prime95 / CrystalDiskMark often refuse CreateProcess from an elevated
// host; ShellExecute goes through the shell and matches a manual launch.
bool openGuiExe(const std::filesystem::path& exe, std::wstring* error = nullptr) {
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        if (error) {
            *error = L"not found";
        }
        return false;
    }
    const std::wstring file = exe.wstring();
    const std::wstring dir = exe.parent_path().wstring();

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

// Launches a process. When stdoutFile is set the child's stdout/stderr are
// redirected there; when wait is true the call blocks until the child exits.
// When exitCode is non-null and wait is true, receives the process exit code.
bool launchProcess(const std::wstring& exe, const std::wstring& args,
                   const std::filesystem::path& workDir, bool hidden, bool wait,
                   const std::filesystem::path& stdoutFile = {}, DWORD* exitCode = nullptr) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = hidden ? SW_HIDE : SW_SHOWNORMAL;

    HANDLE out = nullptr;
    if (!stdoutFile.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        out = CreateFileW(stdoutFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = out;
            si.hStdError = out;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        } else {
            out = nullptr;
        }
    }

    std::wstring command = quote(exe);
    if (!args.empty()) {
        command += L" " + args;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    const std::wstring dir = workDir.empty() ? std::wstring() : workDir.wstring();
    DWORD flags = 0;
    if (hidden) {
        flags |= CREATE_NO_WINDOW;
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok =
        CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, out ? TRUE : FALSE, flags, nullptr,
                       dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (out) {
        CloseHandle(out);
    }
    if (!ok) {
        return false;
    }

    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (exitCode != nullptr) {
            DWORD code = 1;
            if (GetExitCodeProcess(pi.hProcess, &code) == 0) {
                code = 1;
            }
            *exitCode = code;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring fromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(),
                        size);
    return result;
}

void forwardFileLines(const std::filesystem::path& file,
                      const std::function<void(const std::wstring&)>& log) {
    if (!log) {
        return;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return;
    }
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        if (raw.empty()) {
            continue;
        }
        log(fromUtf8(raw));
    }
}

bool usePythonMacroEngine(const Stress::Config& config) {
    const std::wstring engine = lower(trim(config.macroEngine));
    return engine.empty() || engine == L"python" || engine == L"py" || engine == L"helper";
}

// Editable default for the UI Automation helper. On the target machine run
// `powershell -File uia_stress.ps1 -Mode dump` to list the real control names.
const char kUiAutomationScript[] = R"PS(param(
    [ValidateSet('run','dump')][string]$Mode = 'run',
    [string]$Base = '',
    [string]$Log = '',
    [string]$Only = '',
    [string]$Ini = ''
)

$ErrorActionPreference = 'Continue'

function Write-Log([string]$m) {
    $line = "{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    if ($Log) { try { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 } catch {} }
    Write-Output $line
}

try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
} catch {
    Write-Log ("Failed to load UI Automation assemblies: " + $_.Exception.Message)
    exit 1
}

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

function Read-Ini([string]$path) {
    $map = @{}
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { return $map }
    $section = ''
    foreach ($raw in Get-Content -LiteralPath $path) {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        if ($line.StartsWith('[') -and $line.EndsWith(']')) {
            $section = $line.Substring(1, $line.Length - 2).Trim().ToLower()
            continue
        }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        $key = $line.Substring(0, $eq).Trim().ToLower()
        $val = $line.Substring($eq + 1).Trim()
        $map["$section.$key"] = $val
    }
    return $map
}

function Get-Ini($map, [string]$key, [string]$default) {
    if ($map.ContainsKey($key) -and $map[$key]) { return $map[$key] }
    return $default
}

$ini = Read-Ini $Ini

function Make-Tool([string]$name, [string]$prefix, $defaults) {
    $exe = Get-Ini $ini ("uia.$prefix.exe") $defaults.Exe
    $window = Get-Ini $ini ("uia.$prefix.window") $defaults.Window
    $buttons = Get-Ini $ini ("uia.$prefix.buttons") $defaults.Buttons
    $instances = [int](Get-Ini $ini ("uia.$prefix.instances") $defaults.Instances)
    return [pscustomobject]@{
        Name = $name
        Exe = $exe
        Window = $window
        Buttons = ($buttons -split '\|')
        Instances = $instances
    }
}

# Table read from the [UIA] section of stress.ini (defaults shown here).
$tools = @(
    Make-Tool 'powerMAX'        'powermax'        @{ Exe='powerMAX\powerMAX.exe';              Window='powerMAX';        Buttons='Start|Test|Iniciar|Begin|Run|Go'; Instances='1' },
    Make-Tool 'MemTest'         'memtest'         @{ Exe='memtest.exe';                        Window='MemTest';         Buttons='Start Testing|Start|Test';        Instances='1' },
    Make-Tool 'CrystalDiskMark' 'crystaldiskmark' @{ Exe='CrystalDiskMark9\DiskMark64.exe';    Window='CrystalDiskMark'; Buttons='All|Start|Iniciar';               Instances='1' }
)

function Get-TopWindows {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
    return $AE::RootElement.FindAll($TS::Children, $cond)
}

function Find-StartButton($window, $tokens) {
    $btnCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    $btns = $window.FindAll($TS::Descendants, $btnCond)
    foreach ($b in $btns) {
        $name = ''
        try { $name = $b.Current.Name } catch {}
        if (-not $name) { continue }
        foreach ($t in $tokens) {
            if ($t -and $name.ToLower().Contains($t.ToLower())) { return $b }
        }
    }
    return $null
}

function Invoke-Element($el) {
    $p = $null
    if ($el.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) {
        $p.Invoke()
        return $true
    }
    return $false
}

if ($Mode -eq 'dump') {
    Write-Log 'Dumping top-level windows and buttons...'
    foreach ($w in Get-TopWindows) {
        $name = ''; $procId = 0
        try { $name = $w.Current.Name } catch {}
        try { $procId = $w.Current.ProcessId } catch {}
        Write-Log ("WINDOW '{0}' pid={1}" -f $name, $procId)
        $btnCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
        $btns = $w.FindAll($TS::Descendants, $btnCond)
        foreach ($b in $btns) {
            $bn = ''; $bid = ''
            try { $bn = $b.Current.Name } catch {}
            try { $bid = $b.Current.AutomationId } catch {}
            Write-Log ("    BUTTON '{0}' id='{1}'" -f $bn, $bid)
        }
    }
    exit 0
}

$selected = $tools
if ($Only) {
    $names = $Only.Split(',') | ForEach-Object { $_.Trim().ToLower() }
    $selected = $tools | Where-Object { $names -contains $_.Name.ToLower() }
}

foreach ($tool in $selected) {
    $exe = Join-Path $Base $tool.Exe
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Log ("SKIP {0}: not found ({1})" -f $tool.Name, $exe)
        continue
    }
    for ($i = 1; $i -le $tool.Instances; $i++) {
        try {
            Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) | Out-Null
            Write-Log ("Started {0} (instance {1})" -f $tool.Name, $i)
        } catch {
            Write-Log ("FAILED to start {0}: {1}" -f $tool.Name, $_.Exception.Message)
            continue
        }
        $clicked = $false
        for ($t = 0; $t -lt 30 -and -not $clicked; $t++) {
            Start-Sleep -Milliseconds 1000
            foreach ($w in Get-TopWindows) {
                $wn = ''
                try { $wn = $w.Current.Name } catch {}
                if ($wn -and $wn.ToLower().Contains($tool.Window.ToLower())) {
                    $btn = Find-StartButton $w $tool.Buttons
                    if ($btn) {
                        if (Invoke-Element $btn) {
                            Write-Log ("Clicked '{0}' on {1}" -f $btn.Current.Name, $tool.Name)
                            $clicked = $true
                            break
                        }
                    }
                }
            }
        }
        if (-not $clicked) {
            Write-Log ("Could not find a start button for {0}; leaving it for manual start." -f $tool.Name)
        }
    }
}
)PS";

// Writes the embedded UI Automation helper next to the executable. It is
// rewritten on every launch so the app always ships the current version; all
// per-machine tuning lives in stress.ini, not in this script.
std::filesystem::path ensureUiScript(const std::filesystem::path& executable) {
    const auto path = Stress::scriptPath(executable);
    if (!path.empty()) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << kUiAutomationScript;
        }
    }
    return path;
}

}  // namespace

namespace Stress {

bool detectPortable() {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        return false;
    }
    // BatteryFlag 128 = "no system battery", 255 = unknown: both mean desktop.
    return status.BatteryFlag != 128 && status.BatteryFlag != 255;
}

std::filesystem::path desktopDir() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) || path == nullptr) {
        if (path) {
            CoTaskMemFree(path);
        }
        return {};
    }
    std::filesystem::path desktop(path);
    CoTaskMemFree(path);
    return desktop;
}

std::filesystem::path toolsBaseDir() {
    // Always Desktop\programas de teste — never the USB copy next to the exe.
    const auto desktop = desktopDir();
    if (desktop.empty()) {
        return {};
    }
    return desktop / L"programas de teste";
}

std::filesystem::path configPath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"stress.ini";
}

std::filesystem::path scriptPath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"uia_stress.ps1";
}

std::filesystem::path macroHelperPath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"MacroHelper.exe";
}

std::filesystem::path macroFilePath(const std::filesystem::path& executable,
                                    const Config& config) {
    if (!usePythonMacroEngine(config)) {
        return Macro::filePath(executable);
    }
    std::filesystem::path name = config.macroFile.empty() ? L"macro.json" : config.macroFile;
    if (name.is_absolute()) {
        return name;
    }
    // Default bare "macro.json" → Desktop (visible after F10). Any other relative
    // name stays next to the exe so a USB deploy can pin MacroFile=macro.json
    // beside SingleUpdate by using an absolute path or copying the Desktop file.
    // Prefer: if a copy already exists next to the exe (pen), use that for replay.
    const auto besideExe = executable.parent_path() / name;
    std::error_code ec;
    if (std::filesystem::exists(besideExe, ec)) {
        return besideExe;
    }
    const auto desktop = desktopDir();
    if (!desktop.empty() && (name == L"macro.json" || name.filename() == L"macro.json")) {
        return desktop / name.filename();
    }
    return besideExe;
}

Config loadConfig(const std::filesystem::path& executable) {
    Config config;
    const auto path = configPath(executable);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::wofstream out(path);
        if (out.is_open()) {
            out << L"; Win Auto Updater stress-test configuration\n\n";
            out << L"; When UseMacro=1 and a macro was recorded next to the app, the\n";
            out << L"; stress run opens the same programs as the manual .bat (FurMark,\n";
            out << L"; Prime95, CrystalDiskMark, powerMAX, MemTest) and replays the macro.\n";
            out << L"; OpenHardwareMonitor and BatteryCare are started from the Monitor\n";
            out << L"; button only. Tools folder is ALWAYS Desktop\\programas de teste.\n\n";
            out << L"; Run the test bench automatically after a successful update.\n";
            out << L"AutoRunStress=1\n\n";
            out << L"; Replay the recorded macro (MacroHelper / Macro::play) after launch.\n";
            out << L"UseMacro=1\n";
            out << L"; Macro engine: python (MacroHelper.exe + macro.json) or cpp (legacy).\n";
            out << L"MacroEngine=python\n";
            out << L"; Macro file: default macro.json is recorded on the Desktop. If the same\n";
            out << L"; file also exists next to SingleUpdate.exe (USB), replay prefers that copy.\n";
            out << L"MacroFile=macro.json\n";
        }
        return config;
    }

    std::wifstream in(path);
    if (!in.is_open()) {
        return config;
    }

    std::wstring line;
    std::wstring section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }
        if (line.front() == L'[' && line.back() == L']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        const std::wstring key = lower(trim(line.substr(0, eq)));
        const std::wstring value = trim(line.substr(eq + 1));

        if (section == L"uia") {
            if (key == L"powermax.exe") config.powerMaxExe = value;
            else if (key == L"powermax.instances") config.powerMaxInstances = parseInt(value, config.powerMaxInstances);
            else if (key == L"memtest.exe") config.memTestExe = value;
            else if (key == L"memtest.instances") config.memTestInstances = parseInt(value, config.memTestInstances);
            else if (key == L"crystaldiskmark.exe") config.cdmExe = value;
            else if (key == L"crystaldiskmark.instances") config.cdmInstances = parseInt(value, config.cdmInstances);
            continue;
        }

        if (key == L"openhardwaremonitor") config.openHardwareMonitor = parseBool(value, config.openHardwareMonitor);
        else if (key == L"batterycare") config.batteryCare = parseBool(value, config.batteryCare);
        else if (key == L"furmark") config.furmark = parseBool(value, config.furmark);
        else if (key == L"furmarkminutes") config.furmarkMinutes = parseInt(value, config.furmarkMinutes);
        else if (key == L"furmarkmsaa") config.furmarkMsaa = parseInt(value, config.furmarkMsaa);
        else if (key == L"furmarkfullscreen") config.furmarkFullscreen = parseBool(value, config.furmarkFullscreen);
        else if (key == L"furmarkgpuindex") config.furmarkGpuIndex = parseInt(value, config.furmarkGpuIndex);
        else if (key == L"prime95") config.prime95 = parseBool(value, config.prime95);
        else if (key == L"prime95minutes") config.primeMinutes = parseInt(value, config.primeMinutes);
        else if (key == L"diskspd") config.diskSpd = parseBool(value, config.diskSpd);
        else if (key == L"diskspddrive") config.diskDrive = value;
        else if (key == L"diskspdseconds") config.diskSpdSeconds = parseInt(value, config.diskSpdSeconds);
        else if (key == L"diskspdsizemb") config.diskSpdSizeMb = parseInt(value, config.diskSpdSizeMb);
        else if (key == L"diskspdargs") config.diskSpdArgs = value;
        else if (key == L"memtest") config.memTest = parseBool(value, config.memTest);
        else if (key == L"powermax") config.powerMax = parseBool(value, config.powerMax);
        else if (key == L"crystaldiskmark") config.crystalDiskMarkGui = parseBool(value, config.crystalDiskMarkGui);
        else if (key == L"runguiautomation") config.runGuiAutomation = parseBool(value, config.runGuiAutomation);
        else if (key == L"usemacro") config.useMacro = parseBool(value, config.useMacro);
        else if (key == L"macroengine") config.macroEngine = value;
        else if (key == L"macrofile") config.macroFile = value;
        else if (key == L"autorunstress") config.autoRunStress = parseBool(value, config.autoRunStress);
    }
    return config;
}

namespace {

// MemTest has no taskbar button on some builds — bring its top-level window forward
// after everything else has started so the user (or the macro) can reach it.
void focusProcessWindows(const std::wstring& processName) {
    struct Ctx {
        std::wstring want;
        HWND best = nullptr;
    } ctx{processName, nullptr};

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (IsWindowVisible(window) == 0) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == 0) {
                return TRUE;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process == nullptr) {
                return TRUE;
            }
            wchar_t buffer[MAX_PATH]{};
            DWORD size = MAX_PATH;
            std::wstring name;
            if (QueryFullProcessImageNameW(process, 0, buffer, &size) != 0) {
                name = std::filesystem::path(buffer).filename().wstring();
            }
            CloseHandle(process);
            if (_wcsicmp(name.c_str(), c->want.c_str()) != 0) {
                return TRUE;
            }
            c->best = window;
            return TRUE;  // keep going — last match wins (most recently created often last)
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.best == nullptr) {
        return;
    }
    ShowWindow(ctx.best, SW_RESTORE);
    BringWindowToTop(ctx.best);
    SetForegroundWindow(ctx.best);
    SetWindowPos(ctx.best, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

}  // namespace

Result launchTools(const Config& /*config*/, const Callbacks& callbacks) {
    Result result;
    result.portable = detectPortable();

    const auto log = [&](const std::wstring& m) {
        if (callbacks.log) {
            callbacks.log(m);
        }
    };
    const auto status = [&](const std::wstring& m) {
        if (callbacks.status) {
            callbacks.status(m);
        }
    };

    const auto base = toolsBaseDir();
    result.baseDir = base.wstring();
    log(L"Stress base folder (Desktop): " + base.wstring());

    std::error_code ec;
    if (base.empty() || !std::filesystem::is_directory(base, ec)) {
        result.message =
            L"Pasta \"programas de teste\" nao encontrada no Ambiente de Trabalho.\r\n"
            L"Esperado: " +
            (base.empty() ? std::wstring(L"%USERPROFILE%\\Desktop\\programas de teste")
                          : base.wstring());
        log(result.message);
        return result;
    }

    // Same order as the manual .bat. MemTest is intentionally last and then
    // forced to the foreground (it often has no taskbar button).
    status(L"Opening the test tools...");

    // Each tool may exist under slightly different relative paths on shop PCs.
    const auto resolveTool = [&](std::initializer_list<const wchar_t*> candidates)
        -> std::filesystem::path {
        for (const wchar_t* rel : candidates) {
            const std::filesystem::path candidate = base / rel;
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
        return {};
    };

    const auto launchTool = [&](const std::wstring& label,
                                std::initializer_list<const wchar_t*> candidates) {
        const std::filesystem::path tool = resolveTool(candidates);
        if (tool.empty()) {
            std::wstring tried = L"Missing " + label + L" under " + base.wstring() + L" — tried:";
            for (const wchar_t* rel : candidates) {
                tried += L"\r\n  • ";
                tried += rel;
            }
            log(tried);
            ++result.failed;
            return;
        }
        std::wstring launchError;
        if (openGuiExe(tool, &launchError)) {
            log(L"Opened " + label + L": " + tool.wstring());
            ++result.launched;
        } else {
            log(L"Failed to open " + label + L": " + tool.wstring() + L" — " + launchError);
            ++result.failed;
        }
    };

    launchTool(L"FurMark", {L"FurMark2_x64\\FurMark_GUI.exe", L"FurMark\\FurMark_GUI.exe",
                            L"FurMark_GUI.exe"});
    Sleep(400);
    launchTool(L"Prime95", {L"Prime95\\prime95.exe", L"prime95.exe"});
    Sleep(400);
    launchTool(L"CrystalDiskMark", {L"CrystalDiskMark9\\DiskMark64.exe",
                                    L"CrystalDiskMark\\DiskMark64.exe", L"DiskMark64.exe"});
    Sleep(400);
    launchTool(L"powerMAX", {L"powerMAX\\powerMAX.exe", L"powerMAX_x64.exe", L"powerMAX.exe",
                             L"powerMAX_x32.exe"});
    Sleep(400);
    launchTool(L"MemTest", {L"memtest.exe", L"MemTest\\memtest.exe"});

    // Let MemTest's window appear, then stack it on top.
    Sleep(1500);
    focusProcessWindows(L"memtest.exe");
    if (result.launched > 0) {
        log(L"MemTest brought to foreground (started last).");
    }

    result.ok = result.launched > 0;
    result.message = L"Launched " + std::to_wstring(result.launched) + L" test(s), " +
                     std::to_wstring(result.failed) + L" failed. Base: " + base.wstring();
    log(result.message);
    return result;
}

Result run(const Config& config, const Callbacks& callbacks, bool playMacro) {
    Result result = launchTools(config, callbacks);

    const auto log = [&](const std::wstring& m) {
        if (callbacks.log) {
            callbacks.log(m);
        }
    };
    const auto status = [&](const std::wstring& m) {
        if (callbacks.status) {
            callbacks.status(m);
        }
    };

    if (!playMacro) {
        return result;
    }

    // Extra settle time before the macro starts clicking (FurMark can be slow).
    Sleep(2500);

    std::error_code ec;
    const auto exe = currentExecutable();
    if (!config.useMacro) {
        log(L"UseMacro=0 — tools launched, macro replay skipped.");
    } else if (usePythonMacroEngine(config)) {
        const auto helper = macroHelperPath(exe);
        const auto macroFile = macroFilePath(exe, config);
        if (!std::filesystem::exists(helper, ec)) {
            log(L"MacroHelper.exe not found next to SingleUpdate.exe (" + helper.wstring() +
                L"). Deploy MacroHelper.exe or set MacroEngine=cpp.");
        } else if (!std::filesystem::exists(macroFile, ec)) {
            log(L"No macro recorded yet (" + macroFile.wstring() +
                L"). Click \"Record macro\", capture the clicks, then press F10.");
        } else {
            status(L"Replaying the recorded macro (MacroHelper)...");
            log(L"Replaying macro via MacroHelper: " + macroFile.wstring());
            const auto outLog = tempDir() / L"macro_helper_stdout.txt";
            std::filesystem::create_directories(outLog.parent_path(), ec);
            DeleteFileW(outLog.wstring().c_str());
            // continue: MemTest OOM dialog / closed windows must not abort CDM/Prime/FurMark.
            const std::wstring args =
                L"--play " + quote(macroFile.wstring()) + L" --on-fail continue";
            DWORD exitCode = 1;
            if (!launchProcess(helper.wstring(), args, helper.parent_path(), true, true, outLog,
                               &exitCode)) {
                log(L"Failed to launch MacroHelper.exe.");
            } else {
                std::error_code sizeEc;
                const auto stdoutSize = std::filesystem::file_size(outLog, sizeEc);
                if (!sizeEc && stdoutSize > 0) {
                    forwardFileLines(outLog, log);
                } else {
                    const auto sidecar = std::filesystem::path(macroFile.wstring() + L".log");
                    if (std::filesystem::exists(sidecar, ec)) {
                        log(L"MacroHelper stdout empty — reading " + sidecar.wstring());
                        forwardFileLines(sidecar, log);
                    }
                }
                if (exitCode == 0) {
                    log(L"Macro replay finished successfully.");
                } else {
                    log(L"Macro replay finished with errors (exit " + std::to_wstring(exitCode) +
                        L").");
                }
            }
        }
    } else {
        const auto macroFile = Macro::filePath(exe);
        std::vector<Macro::Event> macro;
        if (std::filesystem::exists(macroFile, ec) && Macro::load(macroFile, &macro) &&
            !macro.empty()) {
            status(L"Replaying the recorded macro (C++)...");
            log(L"Replaying macro (" + std::to_wstring(macro.size()) + L" step(s)) from " +
                macroFile.wstring());
            Macro::play(macro, log);
            log(L"Macro replay finished.");
        } else {
            log(L"No macro recorded yet. The tools are open — click \"Record macro\" in the "
                L"app, do your clicks, then press F10.");
        }
    }

    return result;
}

void ensureAssets(const std::filesystem::path& executable) {
    // loadConfig writes the stress.ini template when the file is missing.
    (void)loadConfig(executable);
    (void)ensureUiScript(executable);
}

bool dumpUiTree(const std::filesystem::path& executable, std::wstring* dumpLog) {
    const auto script = ensureUiScript(executable);

    // Write the dump next to the executable so it is easy to find and send back.
    std::filesystem::path log = executable.parent_path() / L"uia_dump.txt";
    std::error_code ec;
    if (log.parent_path().empty()) {
        log = tempDir() / L"uia_dump.txt";
    }
    std::filesystem::create_directories(log.parent_path(), ec);
    DeleteFileW(log.wstring().c_str());
    if (dumpLog) {
        *dumpLog = log.wstring();
    }

    const std::wstring psArgs = L"-NoProfile -ExecutionPolicy Bypass -File " +
                                quote(script.wstring()) + L" -Mode dump -Base " +
                                quote(toolsBaseDir().wstring());
    // stdout+stderr are redirected into the dump file, so PowerShell errors show up too.
    return launchProcess(L"powershell.exe", psArgs, {}, true, true, log);
}

}  // namespace Stress
