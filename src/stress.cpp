#include "stress.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
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

std::string toNarrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
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

// Launches a process. When stdoutFile is set the child's stdout/stderr are
// redirected there; when wait is true the call blocks until the child exits.
bool launchProcess(const std::wstring& exe, const std::wstring& args,
                   const std::filesystem::path& workDir, bool hidden, bool wait,
                   const std::filesystem::path& stdoutFile = {}) {
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
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring buildFurmarkArgs(const Stress::Config& config) {
    std::wstring args = L"--demo furmark-gl --gpu-index " + std::to_wstring(config.furmarkGpuIndex) +
                        L" --msaa " + std::to_wstring(config.furmarkMsaa) +
                        L" --max-time " + std::to_wstring(config.furmarkMinutes * 60000) +
                        L" --no-score-box --disable-logfile";
    if (config.furmarkFullscreen) {
        args += L" --fullscreen";
    }
    return args;
}

std::wstring buildDiskSpdArgs(const Stress::Config& config, const std::wstring& blockSize,
                              const std::wstring& queue, const std::wstring& threads,
                              const std::wstring& writePercent, bool random,
                              const std::filesystem::path& target) {
    if (!config.diskSpdArgs.empty()) {
        return config.diskSpdArgs + L" " + quote(target.wstring());
    }
    std::wstring args = L"-c" + std::to_wstring(config.diskSpdSizeMb) + L"M -b" + blockSize +
                        L" -d" + std::to_wstring(config.diskSpdSeconds) + L" -o" + queue + L" -t" +
                        threads + L" -w" + writePercent + L" -W1 -S -g";
    if (random) {
        args += L" -r";
    }
    args += L" " + quote(target.wstring());
    return args;
}

// Editable default for the UI Automation helper. On the target machine run
// `powershell -File uia_stress.ps1 -Mode dump` to list the real control names.
const char kUiAutomationScript[] = R"PS(param(
    [ValidateSet('run','dump')][string]$Mode = 'run',
    [string]$Base = '',
    [string]$Log = '',
    [string]$Only = ''
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

# Editable tool table. Tune Window / Buttons on the machine (run with -Mode dump
# to list the real control names) and adjust the values below.
$tools = @(
    [pscustomobject]@{ Name='powerMAX';        Exe='powerMAX\powerMAX.exe';              Window='powerMAX';        Buttons=@('Start','Test','Iniciar','Begin','Run','Go'); Instances=1 },
    [pscustomobject]@{ Name='MemTest';         Exe='memtest.exe';                         Window='MemTest';         Buttons=@('Start Testing','Start','Test');             Instances=1 },
    [pscustomobject]@{ Name='CrystalDiskMark'; Exe='CrystalDiskMark9\DiskMark64.exe';     Window='CrystalDiskMark'; Buttons=@('All','Start','Iniciar');                    Instances=1 }
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
            if ($name.ToLower().Contains($t.ToLower())) { return $b }
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

// Writes the embedded UI Automation helper next to the executable when absent
// so the user can edit and tune it on the target machine.
std::filesystem::path ensureUiScript() {
    const auto path = Stress::scriptPath(currentExecutable());
    std::error_code ec;
    if (!path.empty() && !std::filesystem::exists(path, ec)) {
        std::ofstream out(path, std::ios::binary);
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

std::filesystem::path toolsBaseDir() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) || path == nullptr) {
        if (path) {
            CoTaskMemFree(path);
        }
        return {};
    }
    std::filesystem::path desktop(path);
    CoTaskMemFree(path);
    return desktop / L"programas de teste";
}

std::filesystem::path configPath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"stress.ini";
}

std::filesystem::path scriptPath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"uia_stress.ps1";
}

Config loadConfig(const std::filesystem::path& executable) {
    Config config;
    const auto path = configPath(executable);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::wofstream out(path);
        if (out.is_open()) {
            out << L"; Win Auto Updater stress-test configuration\n";
            out << L"; Values: 1/0 for booleans. Times are in minutes unless noted.\n";
            out << L"; Tools are expected in Desktop\\programas de teste.\n\n";
            out << L"OpenHardwareMonitor=1\n";
            out << L"BatteryCare=1\n";
            out << L"FurMark=1\n";
            out << L"FurMarkMinutes=15\n";
            out << L"FurMarkMsaa=1\n";
            out << L"FurMarkFullscreen=1\n";
            out << L"FurMarkGpuIndex=0\n";
            out << L"Prime95=1\n";
            out << L"Prime95Minutes=120\n";
            out << L"DiskSpd=1\n";
            out << L"DiskSpdDrive=C:\n";
            out << L"DiskSpdSeconds=60\n";
            out << L"DiskSpdSizeMb=256\n";
            out << L"DiskSpdArgs=\n";
            out << L"MemTest=1\n";
            out << L"MemTestInstances=1\n";
            out << L"PowerMax=1\n";
            out << L"CrystalDiskMark=0\n";
            out << L"RunGuiAutomation=1\n";
        }
        return config;
    }

    std::wifstream in(path);
    if (!in.is_open()) {
        return config;
    }

    std::wstring line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        const std::wstring key = lower(trim(line.substr(0, eq)));
        const std::wstring value = trim(line.substr(eq + 1));

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
        else if (key == L"memtestinstances") config.memTestInstances = parseInt(value, config.memTestInstances);
        else if (key == L"powermax") config.powerMax = parseBool(value, config.powerMax);
        else if (key == L"crystaldiskmark") config.crystalDiskMarkGui = parseBool(value, config.crystalDiskMarkGui);
        else if (key == L"runguiautomation") config.runGuiAutomation = parseBool(value, config.runGuiAutomation);
    }
    return config;
}

Result run(const Config& config, const Callbacks& callbacks) {
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
    log(L"Stress base folder: " + base.wstring());

    std::error_code ec;
    if (base.empty() || !std::filesystem::is_directory(base, ec)) {
        result.message = L"Folder \"programas de teste\" was not found on the Desktop.";
        log(result.message);
        return result;
    }

    const auto start = [&](const std::wstring& label, const std::filesystem::path& exe,
                           const std::wstring& args, bool hidden, bool wait,
                           const std::filesystem::path& out = {}) {
        if (!std::filesystem::exists(exe, ec)) {
            log(L"Missing " + label + L": " + exe.wstring());
            ++result.failed;
            return false;
        }
        const bool ok = launchProcess(exe.wstring(), args, exe.parent_path(), hidden, wait, out);
        if (ok) {
            log(L"Launched " + label + L": " + exe.wstring());
            ++result.launched;
        } else {
            log(L"Failed to launch " + label + L": " + exe.wstring());
            ++result.failed;
        }
        return ok;
    };

    status(L"Starting hardware monitoring...");
    if (config.openHardwareMonitor) {
        start(L"OpenHardwareMonitor", base / L"OpenHardwareMonitor" / L"OpenHardwareMonitor.exe",
              L"", false, false);
    }
    if (config.batteryCare && result.portable) {
        start(L"BatteryCare", base / L"batterycare" / L"BatteryCare.exe", L"", false, false);
    }

    status(L"Starting FurMark (GPU stress)...");
    if (config.furmark) {
        start(L"FurMark", base / L"FurMark2_x64" / L"furmark.exe", buildFurmarkArgs(config), false,
              false);
    }

    status(L"Starting Prime95 (CPU stress)...");
    if (config.prime95) {
        const auto primeExe = base / L"Prime95" / L"prime95.exe";
        if (start(L"Prime95", primeExe, L"-t", true, false)) {
            // Prime95 has no built-in total runtime; stop it after the configured time.
            const int seconds = std::max(1, config.primeMinutes) * 60;
            launchProcess(L"cmd.exe",
                          L"/c timeout /t " + std::to_wstring(seconds) +
                              L" /nobreak > nul & taskkill /IM prime95.exe /F",
                          {}, true, false);
            log(L"Prime95 will be stopped after " + std::to_wstring(config.primeMinutes) +
                L" minutes.");
        }
    }

    status(L"Running disk benchmark (DiskSpd)...");
    if (config.diskSpd) {
        const auto diskSpd =
            base / L"CrystalDiskMark9" / L"CdmResource" / L"DiskSpd" / L"DiskSpd64.exe";
        if (std::filesystem::exists(diskSpd, ec)) {
            const std::filesystem::path target =
                config.diskDrive + L"\\WinAutoUpdater_diskspd.dat";
            const auto outDir = tempDir();
            std::filesystem::create_directories(outDir, ec);

            const std::vector<std::wstring> labels = {
                L"SEQ1M Q8T1 read", L"SEQ1M Q8T1 write", L"RND4K Q32T1 read", L"RND4K Q32T1 write"};
            const std::vector<std::wstring> blocks = {L"1M", L"1M", L"4K", L"4K"};
            const std::vector<std::wstring> queues = {L"8", L"8", L"32", L"32"};
            const std::vector<std::wstring> writes = {L"0", L"100", L"0", L"100"};
            const std::vector<bool> randoms = {false, false, true, true};

            // Serialize the four runs in a detached batch file so this thread
            // does not block for minutes and so the runs never overlap.
            const auto batch = outDir / L"diskspd_run.bat";
            std::ofstream out(batch, std::ios::binary);
            if (out.is_open()) {
                out << "@echo off\r\n";
                for (size_t i = 0; i < labels.size(); ++i) {
                    const std::wstring args = buildDiskSpdArgs(config, blocks[i], queues[i], L"1",
                                                               writes[i], randoms[i], target);
                    const auto logFile = outDir / (L"diskspd_" + std::to_wstring(i + 1) + L".txt");
                    out << "\"" << toNarrow(diskSpd.wstring()) << "\" " << toNarrow(args) << " > \""
                        << toNarrow(logFile.wstring()) << "\" 2>&1\r\n";
                }
                out << "del /f /q \"" << toNarrow(target.wstring()) << "\" > nul 2>&1\r\n";
                out.close();

                if (launchProcess(L"cmd.exe", L"/c " + quote(batch.wstring()), {}, true, false)) {
                    log(L"DiskSpd started in the background. Results: " + outDir.wstring());
                    ++result.launched;
                } else {
                    log(L"Could not start the DiskSpd batch.");
                    ++result.failed;
                }
            } else {
                log(L"Could not write the DiskSpd batch file.");
                ++result.failed;
            }
        } else {
            log(L"Missing DiskSpd: " + diskSpd.wstring());
            ++result.failed;
        }
    }

    std::wstring guiOnly;
    if (config.powerMax) {
        guiOnly += L"powerMAX,";
    }
    if (config.memTest) {
        guiOnly += L"MemTest,";
    }
    if (config.crystalDiskMarkGui) {
        guiOnly += L"CrystalDiskMark,";
    }
    if (!guiOnly.empty()) {
        guiOnly.pop_back();
    }

    if (config.runGuiAutomation && !guiOnly.empty()) {
        status(L"Automating powerMAX / MemTest / CrystalDiskMark...");
        const auto script = ensureUiScript();
        const auto outDir = tempDir();
        std::filesystem::create_directories(outDir, ec);
        const auto psLog = outDir / L"uia_stress.log";
        const std::wstring psArgs =
            L"-NoProfile -ExecutionPolicy Bypass -File " + quote(script.wstring()) +
            L" -Mode run -Base " + quote(base.wstring()) + L" -Log " + quote(psLog.wstring()) +
            L" -Only " + quote(guiOnly);
        if (launchProcess(L"powershell.exe", psArgs, {}, true, false)) {
            log(L"UI Automation helper started (log: " + psLog.wstring() + L").");
            ++result.launched;
        } else {
            log(L"Could not start the UI Automation helper.");
            ++result.failed;
        }
    }

    result.ok = result.launched > 0;
    result.message = L"Launched " + std::to_wstring(result.launched) + L" test(s), " +
                     std::to_wstring(result.failed) + L" failed.";
    log(result.message);
    return result;
}

}  // namespace Stress
