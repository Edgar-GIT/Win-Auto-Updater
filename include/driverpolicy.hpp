#pragma once

#include <string>

// Controls whether Windows Update is allowed to install hardware driver
// updates. This is the registry equivalent of the Group Policy setting
// "Do not include drivers with Windows Updates".
namespace DriverPolicy {

bool excludeDriversFromWindowsUpdate(std::wstring* detail);
bool includeDriversInWindowsUpdate(std::wstring* detail);

}  // namespace DriverPolicy
