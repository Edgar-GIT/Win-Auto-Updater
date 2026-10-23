#pragma once

#include <filesystem>

class Cleanup {
public:
    // Temp/state under %LOCALAPPDATA% (or wherever StateManager keeps it).
    static void removeTempFiles();

    // Full uninstall: removes Desktop\helpers\ (tools, macro, MacroHelper, …)
    // and legacy loose files. Driver installers on the Desktop are kept.
    // Locked files are scheduled for deletion on the next reboot.
    static void removeBundle(const std::filesystem::path& executable);
};
