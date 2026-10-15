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
    bool crystalDiskMarkGui = false;
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

    int memTestInstances = 1;

    bool runGuiAutomation = true;
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

// Launches the whole bench. Blocking; call from a worker thread.
Result run(const Config& config, const Callbacks& callbacks);

bool detectPortable();
std::filesystem::path toolsBaseDir();
std::filesystem::path configPath(const std::filesystem::path& executable);
std::filesystem::path scriptPath(const std::filesystem::path& executable);

}  // namespace Stress
