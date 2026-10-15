#include "drivers.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <wininet.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Candidate {
    const wchar_t* url;
    const wchar_t* fileName;
    const wchar_t* pageUrl;
};

// Best-known direct installers. Vendors rotate these filenames every release,
// so each vendor lists a couple of candidates and falls back to a download
// page shortcut when none of them can be fetched.
const std::vector<Candidate>& nvidiaCandidates() {
    static const std::vector<Candidate> candidates = {
        {L"https://us.download.nvidia.com/nvapp/client/11.0.9.565/NVIDIA_app_v11.0.9.565.exe",
         L"NVIDIA_app_setup.exe", L"https://www.nvidia.com/en-us/drivers/"},
        {L"https://us.download.nvidia.com/nvapp/client/11.0.8.299/NVIDIA_app_v11.0.8.299.exe",
         L"NVIDIA_app_setup.exe", L"https://www.nvidia.com/en-us/drivers/"},
    };
    return candidates;
}

const std::vector<Candidate>& amdCandidates() {
    static const std::vector<Candidate> candidates = {
        {L"https://drivers.amd.com/drivers/installer/26.10/whql/"
         L"amd-software-adrenalin-edition-26.8.1-minimalsetup-260818_web.exe",
         L"amd-software-adrenalin-setup.exe",
         L"https://www.amd.com/en/support/download/drivers.html"},
    };
    return candidates;
}

std::wstring toUpper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return value;
}

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

bool downloadFileTo(const std::wstring& url, const std::filesystem::path& destination,
                    std::wstring* error) {
    HINTERNET session =
        InternetOpenW(L"WinAutoUpdater/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session) {
        if (error) {
            *error = L"could not open a network session";
        }
        return false;
    }

    DWORD timeout = 20000;
    InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET request = InternetOpenUrlW(
        session, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE |
            INTERNET_FLAG_NO_UI,
        0);
    if (!request) {
        InternetCloseHandle(session);
        if (error) {
            *error = L"could not reach the download server";
        }
        return false;
    }

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        InternetCloseHandle(request);
        InternetCloseHandle(session);
        if (error) {
            *error = L"could not create the file on the Desktop";
        }
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    std::uint64_t total = 0;
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!InternetReadFile(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            ok = false;
            if (error) {
                *error = L"the download was interrupted";
            }
            break;
        }
        if (read == 0) {
            break;
        }
        out.write(buffer.data(), read);
        if (!out) {
            ok = false;
            if (error) {
                *error = L"failed to write the downloaded file";
            }
            break;
        }
        total += read;
    }

    InternetCloseHandle(request);
    InternetCloseHandle(session);
    out.close();

    if (!ok) {
        return false;
    }

    constexpr std::uint64_t kMinimumInstallerSize = 512 * 1024;
    if (total < kMinimumInstallerSize) {
        if (error) {
            *error = L"the server returned an unexpected (too small) file";
        }
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        return false;
    }
    return true;
}

bool writeInternetShortcut(const std::filesystem::path& destination, const std::wstring& url) {
    std::wofstream out(destination);
    if (!out) {
        return false;
    }
    out << L"[InternetShortcut]\r\n";
    out << L"URL=" << url << L"\r\n";
    out.flush();
    const bool ok = out.good();
    out.close();
    return ok;
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

        const std::wstring id = toUpper(device.DeviceID);
        const std::wstring name = toUpper(device.DeviceString);

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
            return L"other";
        case Vendor::None:
            return L"none";
    }
    return L"none";
}

Result downloadForSystem() {
    Result result;
    result.vendor = detectVendor();
    result.vendorName = vendorName(result.vendor);
    result.detected = result.vendor != Vendor::None;

    if (result.vendor == Vendor::None) {
        result.message = L"No discrete AMD or NVIDIA graphics adapter was detected.";
        return result;
    }
    if (result.vendor == Vendor::Intel || result.vendor == Vendor::Other) {
        result.message = L"Graphics adapter detected (" + result.vendorName +
                         L"); automatic driver download is only provided for AMD and NVIDIA.";
        return result;
    }

    const std::filesystem::path desktop = desktopPath();
    if (desktop.empty()) {
        result.message = L"Could not locate the Desktop folder.";
        return result;
    }

    const std::vector<Candidate>& candidates =
        (result.vendor == Vendor::Nvidia) ? nvidiaCandidates() : amdCandidates();

    std::wstring lastError;
    for (const Candidate& candidate : candidates) {
        const std::filesystem::path target = desktop / candidate.fileName;
        if (downloadFileTo(candidate.url, target, &lastError)) {
            result.downloaded = true;
            result.savedPath = target.wstring();
            result.message = result.vendorName + L" driver saved to the Desktop as " +
                             candidate.fileName + L".";
            return result;
        }
    }

    const Candidate& fallback = candidates.front();
    const std::filesystem::path shortcut = desktop / (result.vendorName + L" drivers.url");
    if (writeInternetShortcut(shortcut, fallback.pageUrl)) {
        result.shortcutCreated = true;
        result.savedPath = shortcut.wstring();
        result.message = L"Saved a " + result.vendorName +
                         L" download shortcut to the Desktop (direct download unavailable: " +
                         lastError + L").";
        return result;
    }

    result.message =
        L"Could not download " + result.vendorName + L" drivers (" + lastError + L").";
    return result;
}

}  // namespace Drivers
