#pragma once

#include <filesystem>

class Cleanup {
public:
    // Temp/state under %LOCALAPPDATA% (or wherever StateManager keeps it).
    static void removeTempFiles();

    // Full uninstall: Desktop tools folder + macro files + companions next to
    // SingleUpdate.exe (MacroHelper, stress.ini, uia_stress.ps1, …).
    // Files that are locked are scheduled for deletion on the next reboot.
    static void removeBundle(const std::filesystem::path& executable);
};
