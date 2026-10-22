#include "drivers.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace {

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

std::filesystem::path toolsDir() {
    const auto desktop = desktopPath();
    if (desktop.empty()) {
        return {};
    }
    return desktop / L"programas de teste";
}

// Bundled installers live next to the stress tools (Desktop\programas de teste).
const wchar_t* bundledInstallerName(Drivers::Vendor vendor) {
    if (vendor == Drivers::Vendor::Nvidia) {
        return L"nvidia_app.exe";
    }
    if (vendor == Drivers::Vendor::Amd) {
        return L"amd_drivers.exe";
    }
    return nullptr;
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

    const std::filesystem::path desktop = desktopPath();
    if (desktop.empty()) {
        result.message = L"Não foi possível localizar a pasta Ambiente de Trabalho.";
        return result;
    }

    const wchar_t* fileName = bundledInstallerName(result.vendor);
    if (!fileName) {
        result.message = L"Fornecedor de gráficos não suportado.";
        return result;
    }

    const std::filesystem::path tools = toolsDir();
    const std::filesystem::path source = tools / fileName;
    const std::filesystem::path target = desktop / fileName;

    std::error_code ec;
    if (tools.empty() || !std::filesystem::is_directory(tools, ec)) {
        result.message =
            L"Pasta \"programas de teste\" não encontrada no Ambiente de Trabalho "
            L"(precisa de " +
            std::wstring(fileName) + L").";
        return result;
    }
    if (!std::filesystem::is_regular_file(source, ec)) {
        result.message = L"Ficheiro em falta: " + source.wstring();
        return result;
    }

    // Overwrite any previous copy on the Desktop.
    if (!CopyFileW(source.wstring().c_str(), target.wstring().c_str(), FALSE)) {
        result.message = L"Falha ao copiar " + std::wstring(fileName) +
                         L" para o Ambiente de Trabalho (erro " +
                         std::to_wstring(GetLastError()) + L").";
        return result;
    }

    result.downloaded = true;
    result.savedPath = target.wstring();
    result.message = result.vendorName + L" — " + fileName +
                     L" copiado para o Ambiente de Trabalho.";
    return result;
}

}  // namespace Drivers
