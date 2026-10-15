#include "driverpolicy.hpp"

#include <windows.h>

#include <string>

namespace {

constexpr const wchar_t* kWuPolicyPath = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
constexpr const wchar_t* kDriverSearchPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching";

bool writeDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value,
                std::wstring* detail) {
    HKEY key = nullptr;
    const LONG created = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                         KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        if (detail) {
            *detail = L"Could not open the registry key " + std::wstring(subKey) + L".";
        }
        return false;
    }

    const LONG written = RegSetValueExW(key, valueName, 0, REG_DWORD,
                                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        if (detail) {
            *detail = L"Could not write the value " + std::wstring(valueName) + L".";
        }
        return false;
    }
    return true;
}

}  // namespace

namespace DriverPolicy {

bool excludeDriversFromWindowsUpdate(std::wstring* detail) {
    const bool policyOk =
        writeDword(HKEY_LOCAL_MACHINE, kWuPolicyPath, L"ExcludeWUDriversInQualityUpdate", 1, detail);
    const bool searchOk =
        writeDword(HKEY_LOCAL_MACHINE, kDriverSearchPath, L"SearchOrderConfig", 0, detail);
    const bool ok = policyOk && searchOk;
    if (ok && detail) {
        *detail = L"Windows Update is now blocked from installing hardware driver updates.";
    }
    return ok;
}

bool includeDriversInWindowsUpdate(std::wstring* detail) {
    const bool policyOk =
        writeDword(HKEY_LOCAL_MACHINE, kWuPolicyPath, L"ExcludeWUDriversInQualityUpdate", 0, detail);
    const bool searchOk =
        writeDword(HKEY_LOCAL_MACHINE, kDriverSearchPath, L"SearchOrderConfig", 3, detail);
    const bool ok = policyOk && searchOk;
    if (ok && detail) {
        *detail = L"Windows Update driver installation has been restored to the default.";
    }
    return ok;
}

}  // namespace DriverPolicy
