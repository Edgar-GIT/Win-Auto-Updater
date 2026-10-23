#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Launches the hardware test bench from Desktop\helpers\programas de teste
// (SingleUpdate.exe stays on the Desktop; everything else lives under helpers\).
namespace Stress {

struct Config {
    bool openHardwareMonitor = true;
    bool furmark = true;
    bool prime95 = true;
    bool diskSpd = true;
    bool memTest = true;
    bool powerMax = true;
    bool crystalDiskMarkGui = true;
    bool batteryCare = true;

    int furmarkMinutes = 15;
    int furmarkMsaa = 1;
    bool furmarkFullscreen = true;
    int furmarkGpuIndex = 0;

    int primeMinutes = 120;

    std::wstring diskDrive = L"C:";
    std::wstring diskSpdArgs;
    int diskSpdSeconds = 60;
    int diskSpdSizeMb = 256;

    bool runGuiAutomation = true;

    // When a recorded macro exists and UseMacro=1, the GUI-only tools are
    // launched directly and driven by replaying the macro instead of the
    // single-button UI Automation heuristic.
    bool useMacro = true;
    // "python" (default) → MacroHelper.exe + macro.json; "cpp" → legacy Macro::play
    // + macro.txt. Recording follows the same switch.
    std::wstring macroEngine = L"python";
    // Macro file for the python engine. Default: helpers\macro.json next to
    // SingleUpdate.exe. Absolute paths are honored.
    std::wstring macroFile = L"macro.json";
    // Starts the bench automatically right after a successful update.
    // Deploy: Desktop\SingleUpdate.exe + Desktop\helpers\ (tools, macro, …).
    bool autoRunStress = true;
    std::wstring powerMaxExe = L"powerMAX\\powerMAX.exe";
    int powerMaxInstances = 1;
    std::wstring memTestExe = L"memtest.exe";
    int memTestInstances = 1;
    std::wstring cdmExe = L"CrystalDiskMark9\\DiskMark64.exe";
    int cdmInstances = 1;
};

struct Callbacks {
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::wstring&)> status;
};

struct Result {
    bool ok = false;
    bool portable = false;
    int launched = 0;
    int failed = 0;
    std::wstring baseDir;
    std::wstring message;
};

// Reads helpers\stress.ini; falls back to defaults and writes a template if missing.
Config loadConfig(const std::filesystem::path& executable);

// Launches the whole bench (tools + optional macro replay). Blocking; worker thread.
Result run(const Config& config, const Callbacks& callbacks, bool playMacro = true);

// Opens the five GUI stress tools only (no macro). MemTest is started last and
// brought to the foreground so it is usable without hunting the taskbar.
Result launchTools(const Config& config, const Callbacks& callbacks);

bool detectPortable();
// Desktop\helpers\ next to SingleUpdate.exe (created if needed by ensureAssets).
std::filesystem::path helpersDir(const std::filesystem::path& executable);
std::filesystem::path toolsBaseDir(const std::filesystem::path& executable);
std::filesystem::path toolsBaseDir();  // uses current executable
std::filesystem::path desktopDir();
std::filesystem::path configPath(const std::filesystem::path& executable);
std::filesystem::path scriptPath(const std::filesystem::path& executable);
std::filesystem::path macroHelperPath(const std::filesystem::path& executable);
std::filesystem::path macroFilePath(const std::filesystem::path& executable,
                                    const Config& config);

// Ensures helpers\ exists and writes stress.ini / uia_stress.ps1 when missing.
void ensureAssets(const std::filesystem::path& executable);

// Runs the UI Automation helper in dump mode (blocking). On success returns the
// path of the dump file in dumpLog.
bool dumpUiTree(const std::filesystem::path& executable, std::wstring* dumpLog);

}  // namespace Stress
