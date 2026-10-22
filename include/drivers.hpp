#pragma once

#include <string>

// Detects the installed graphics adapter and copies the matching driver
// installer from Desktop\programas de teste onto the Desktop
// (nvidia_app.exe / amd_drivers.exe).
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

}  // namespace Drivers
