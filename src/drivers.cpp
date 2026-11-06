#include "drivers.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace {

constexpr const wchar_t* kLenovoSystemUpdate = L"system_update_5.08.03.59.exe";

std::filesystem::path desktopPath() {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        result = raw;
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    if (result.empty()) {
        wchar_t buffer[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH) > 0) {
            result = std::filesystem::path(buffer) / L"Desktop";
        }
    }
    return result;
}

std::filesystem::path thisExeDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

// Bundled installers live in helpers\programas de teste next to SingleUpdate.
std::filesystem::path toolsDir() {
    return thisExeDir() / L"helpers" / L"programas de teste";
}

const wchar_t* bundledInstallerName(Drivers::Vendor vendor) {
    if (vendor == Drivers::Vendor::Nvidia) {
        return L"nvidia_app.exe";
    }
    if (vendor == Drivers::Vendor::Amd) {
        return L"amd_drivers.exe";
    }
    return nullptr;
}

std::wstring toUpper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return text;
}

std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[512]{};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                                     &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    return buffer;
}

Drivers::Result copyBundledFileToDesktop(const wchar_t* fileName, const std::wstring& label) {
    Drivers::Result result;
    result.vendorName = label;

    const std::filesystem::path desktop = desktopPath();
    if (desktop.empty()) {
        result.message = L"Não foi possível localizar a pasta Ambiente de Trabalho.";
        return result;
    }

    const std::filesystem::path tools = toolsDir();
    const std::filesystem::path source = tools / fileName;
    const std::filesystem::path target = desktop / fileName;

    std::error_code ec;
    if (tools.empty() || !std::filesystem::is_directory(tools, ec)) {
        result.message =
            L"Pasta helpers\\programas de teste não encontrada "
            L"(precisa de " +
            std::wstring(fileName) + L").";
        return result;
    }
    if (!std::filesystem::is_regular_file(source, ec)) {
        result.message = L"Ficheiro em falta: " + source.wstring();
        return result;
    }

    if (!CopyFileW(source.wstring().c_str(), target.wstring().c_str(), FALSE)) {
        result.message = L"Falha ao copiar " + std::wstring(fileName) +
                         L" para o Ambiente de Trabalho (erro " +
                         std::to_wstring(GetLastError()) + L").";
        return result;
    }

    result.downloaded = true;
    result.savedPath = target.wstring();
    result.message = label + L" — " + fileName + L" copiado para o Ambiente de Trabalho.";
    return result;
}

}  // namespace

namespace Drivers {

Vendor detectVendor() {
    bool intelSeen = false;
    bool otherSeen = false;

    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) {
            break;
        }

        std::wstring id = device.DeviceID;
        std::wstring name = device.DeviceString;
        const auto upper = [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); };
        std::transform(id.begin(), id.end(), id.begin(), upper);
        std::transform(name.begin(), name.end(), name.begin(), upper);

        if (id.find(L"VEN_10DE") != std::wstring::npos ||
            name.find(L"NVIDIA") != std::wstring::npos) {
            return Vendor::Nvidia;
        }
        if (id.find(L"VEN_1002") != std::wstring::npos ||
            name.find(L"AMD") != std::wstring::npos ||
            name.find(L"RADEON") != std::wstring::npos) {
            return Vendor::Amd;
        }
        if (id.find(L"VEN_8086") != std::wstring::npos ||
            name.find(L"INTEL") != std::wstring::npos) {
            intelSeen = true;
        } else if (!id.empty()) {
            otherSeen = true;
        }
    }

    if (intelSeen) {
        return Vendor::Intel;
    }
    if (otherSeen) {
        return Vendor::Other;
    }
    return Vendor::None;
}

std::wstring vendorName(Vendor vendor) {
    switch (vendor) {
        case Vendor::Nvidia:
            return L"NVIDIA";
        case Vendor::Amd:
            return L"AMD";
        case Vendor::Intel:
            return L"Intel";
        case Vendor::Other:
            return L"outro";
        case Vendor::None:
            return L"nenhum";
    }
    return L"nenhum";
}

bool isLenovoMachine() {
    const std::wstring manufacturer = toUpper(readRegString(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer"));
    const std::wstring product = toUpper(readRegString(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName"));
    const std::wstring family = toUpper(
        readRegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemFamily"));
    const std::wstring alt = toUpper(readRegString(
        HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SystemInformation",
        L"SystemManufacturer"));

    const auto has = [](const std::wstring& hay, const wchar_t* needle) {
        return !hay.empty() && hay.find(needle) != std::wstring::npos;
    };

    return has(manufacturer, L"LENOVO") || has(alt, L"LENOVO") || has(product, L"THINKPAD") ||
           has(family, L"THINKPAD") || has(product, L"LENOVO") || has(family, L"LENOVO");
}

Result copyLenovoSystemUpdate() {
    Result result;
    result.vendorName = L"Lenovo";
    if (!isLenovoMachine()) {
        result.message = L"PC não é Lenovo/ThinkPad — System Update não copiado.";
        return result;
    }
    result.detected = true;
    result = copyBundledFileToDesktop(kLenovoSystemUpdate, L"Lenovo");
    result.detected = true;
    result.vendorName = L"Lenovo";
    return result;
}

Result downloadForSystem() {
    Result result;
    result.vendor = detectVendor();
    result.vendorName = vendorName(result.vendor);
    result.detected = result.vendor != Vendor::None;

    if (result.vendor == Vendor::None) {
        result.message = L"Nenhuma placa gráfica AMD ou NVIDIA foi detetada.";
        return result;
    }
    if (result.vendor == Vendor::Intel || result.vendor == Vendor::Other) {
        result.message = L"Adaptador gráfico detetado (" + result.vendorName +
                         L"); a cópia automática de drivers só está disponível para AMD e NVIDIA.";
        return result;
    }

    const wchar_t* fileName = bundledInstallerName(result.vendor);
    if (!fileName) {
        result.message = L"Fornecedor de gráficos não suportado.";
        return result;
    }

    Result copied = copyBundledFileToDesktop(fileName, result.vendorName);
    copied.vendor = result.vendor;
    copied.vendorName = result.vendorName;
    copied.detected = true;
    return copied;
}

}  // namespace Drivers
