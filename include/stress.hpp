#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Launches the hardware test bench ("programas de teste") that lives on the
// user's Desktop. Tools that expose a command line are started directly;
// GUI-only tools are driven through a PowerShell UI Automation helper.
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
    // Macro file for the python engine. Default name is macro.json on the Desktop
    // (easy to find after recording). Absolute paths are honored; relative paths
    // other than the bare default resolve next to SingleUpdate.exe (USB deploy).
    std::wstring macroFile = L"macro.json";
    // Starts the bench automatically right after a successful update, so the
    // caller only has to deploy SingleUpdate.exe + MacroHelper.exe + macro.json.
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

// Reads stress.ini next to the executable; falls back to built-in defaults and
// writes a commented template when the file does not exist yet.
Config loadConfig(const std::filesystem::path& executable);

// Launches the whole bench (tools + optional macro replay). Blocking; worker thread.
Result run(const Config& config, const Callbacks& callbacks, bool playMacro = true);

// Opens the five GUI stress tools only (no macro). MemTest is started last and
// brought to the foreground so it is usable without hunting the taskbar.
Result launchTools(const Config& config, const Callbacks& callbacks);

bool detectPortable();
std::filesystem::path toolsBaseDir();
std::filesystem::path desktopDir();
std::filesystem::path configPath(const std::filesystem::path& executable);
std::filesystem::path scriptPath(const std::filesystem::path& executable);
std::filesystem::path macroHelperPath(const std::filesystem::path& executable);
std::filesystem::path macroFilePath(const std::filesystem::path& executable,
                                    const Config& config);

// Writes stress.ini and uia_stress.ps1 next to the executable when missing so
// the user can deploy the app and tune it without a rebuild.
void ensureAssets(const std::filesystem::path& executable);

// Runs the UI Automation helper in dump mode (blocking). On success returns the
// path of the dump file in dumpLog.
bool dumpUiTree(const std::filesystem::path& executable, std::wstring* dumpLog);

}  // namespace Stress
