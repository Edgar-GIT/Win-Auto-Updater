#pragma once

#include <string>

// Detects the installed graphics adapter and downloads the matching driver
// installer (or a download-page shortcut) to the user's Desktop.
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
    bool downloaded = false;
    bool shortcutCreated = false;
    std::wstring savedPath;
    std::wstring message;
};

Vendor detectVendor();
std::wstring vendorName(Vendor vendor);
Result downloadForSystem();

}  // namespace Drivers
