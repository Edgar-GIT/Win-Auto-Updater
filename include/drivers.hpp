#pragma once

#include <string>

// Detects the installed graphics adapter and copies the matching driver
// installer from helpers\programas de teste onto the Desktop
// (nvidia_app.exe / amd_drivers.exe). Lenovo/ThinkPad PCs also get
// system_update_*.exe copied the same way.
namespace Drivers {

enum class Vendor {
    None,
    Nvidia,
    Amd,
    Intel,
    Other
};

struct Result {
    Vendor vendor = Vendor::None;
    std::wstring vendorName;
    bool detected = false;
    bool downloaded = false;  // true when the installer was copied to the Desktop
    bool shortcutCreated = false;
    std::wstring savedPath;
    std::wstring message;
};

Vendor detectVendor();
std::wstring vendorName(Vendor vendor);
Result downloadForSystem();

// True when SystemManufacturer / product name looks like Lenovo or ThinkPad.
bool isLenovoMachine();
// Copies system_update_5.08.03.59.exe to the Desktop when this is a Lenovo PC.
Result copyLenovoSystemUpdate();

}  // namespace Drivers
