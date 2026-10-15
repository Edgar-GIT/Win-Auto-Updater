#include "drivers.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <wininet.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kBrowserUserAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
constexpr const wchar_t* kIdentityHeader = L"Accept-Encoding: identity\r\n";

// How the newest installer is located for a given vendor. The page is scraped
// at runtime for the current download link, so we are not stuck on whatever
// version happened to be current when this tool was built. The hardcoded URLs
// are only used when the page cannot be fetched or parsed.
struct VendorPlan {
    const wchar_t* installerName;
    const wchar_t* pageUrl;
    const char* urlPrefix;
    const wchar_t* shortcutPage;
    std::vector<std::wstring> fallbacks;
};

VendorPlan planFor(Drivers::Vendor vendor) {
    if (vendor == Drivers::Vendor::Nvidia) {
        return VendorPlan{
            L"NVIDIA_app_setup.exe",
            L"https://www.nvidia.com/en-us/software/nvidia-app/",
            "https://us.download.nvidia.com/nvapp/client/",
            L"https://www.nvidia.com/en-us/drivers/",
            {L"https://us.download.nvidia.com/nvapp/client/11.0.9.565/"
             L"NVIDIA_app_v11.0.9.565.exe"}};
    }
    return VendorPlan{
        L"amd-software-adrenalin-setup.exe",
        L"https://www.amd.com/en/support/download/drivers.html",
        "https://drivers.amd.com/drivers/installer/",
        L"https://www.amd.com/en/support/download/drivers.html",
        {L"https://drivers.amd.com/drivers/installer/26.10/whql/"
         L"amd-software-adrenalin-edition-26.8.1-minimalsetup-260818_web.exe"}};
}

std::wstring widenAscii(const std::string& text) {
    std::wstring result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return result;
}

bool endsWithExe(const std::string& url) {
    if (url.size() < 4) {
        return false;
    }
    std::string tail = url.substr(url.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ".exe";
}

// Pulls the first "prefix ... .exe" URL out of a page. Also undoes the "\/"
// escaping that shows up inside embedded JSON blobs.
std::string findLatestUrl(const std::string& html, const std::string& prefix) {
    std::string normalized;
    normalized.reserve(html.size());
    for (std::size_t i = 0; i < html.size(); ++i) {
        if (html[i] == '\\' && i + 1 < html.size() && html[i + 1] == '/') {
            normalized.push_back('/');
            ++i;
        } else {
            normalized.push_back(html[i]);
        }
    }

    std::size_t searchFrom = 0;
    while (searchFrom < normalized.size()) {
        const std::size_t pos = normalized.find(prefix, searchFrom);
        if (pos == std::string::npos) {
            break;
        }
        std::size_t end = pos;
        while (end < normalized.size()) {
            const char c = normalized[end];
            if (c == '"' || c == '\'' || c == '<' || c == '>' || c == ' ' || c == '\r' ||
                c == '\n' || c == '\t' || c == '\\' || c == '(' || c == ')' || c == '}') {
                break;
            }
            ++end;
        }
        const std::string candidate = normalized.substr(pos, end - pos);
        if (endsWithExe(candidate)) {
            return candidate;
        }
        searchFrom = (end > pos) ? end : pos + prefix.size();
    }
    return {};
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

HINTERNET openSession() {
    HINTERNET session =
        InternetOpenW(kBrowserUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session) {
        return nullptr;
    }
    DWORD timeout = 20000;
    InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    return session;
}

constexpr DWORD kOpenFlags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                             INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI;

// Downloads a URL into memory. Used to read the vendor download pages.
bool fetchText(const std::wstring& url, std::string* out, std::wstring* error) {
    HINTERNET session = openSession();
    if (!session) {
        if (error) {
            *error = L"could not open a network session";
        }
        return false;
    }

    HINTERNET request = InternetOpenUrlW(session, url.c_str(), kIdentityHeader,
                                         static_cast<DWORD>(-1), kOpenFlags, 0);
    if (!request) {
        InternetCloseHandle(session);
        if (error) {
            *error = L"could not open the vendor download page";
        }
        return false;
    }

    constexpr std::size_t kMaxPageBytes = 8u * 1024u * 1024u;
    std::string data;
    char buffer[16384];
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!InternetReadFile(request, buffer, sizeof(buffer), &read)) {
            ok = false;
            if (error) {
                *error = L"the vendor page download was interrupted";
            }
            break;
        }
        if (read == 0) {
            break;
        }
        data.append(buffer, read);
        if (data.size() >= kMaxPageBytes) {
            break;
        }
    }

    InternetCloseHandle(request);
    InternetCloseHandle(session);

    if (ok && out) {
        *out = std::move(data);
    }
    return ok;
}

bool downloadFileTo(const std::wstring& url, const std::filesystem::path& destination,
                    std::wstring* error) {
    HINTERNET session = openSession();
    if (!session) {
        if (error) {
            *error = L"could not open a network session";
        }
        return false;
    }

    HINTERNET request = InternetOpenUrlW(session, url.c_str(), kIdentityHeader,
                                         static_cast<DWORD>(-1), kOpenFlags, 0);
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

    const VendorPlan plan = planFor(result.vendor);

    // Prefer the newest link found on the vendor's own page; fall back to the
    // last-known URLs when the page cannot be scraped.
    std::vector<std::wstring> candidates;
    std::string page;
    std::wstring pageError;
    if (fetchText(plan.pageUrl, &page, &pageError)) {
        const std::string latest = findLatestUrl(page, plan.urlPrefix);
        if (!latest.empty()) {
            candidates.push_back(widenAscii(latest));
        }
    }
    for (const std::wstring& fallback : plan.fallbacks) {
        if (std::find(candidates.begin(), candidates.end(), fallback) == candidates.end()) {
            candidates.push_back(fallback);
        }
    }

    const std::filesystem::path target = desktop / plan.installerName;
    std::wstring lastError;
    for (const std::wstring& url : candidates) {
        if (downloadFileTo(url, target, &lastError)) {
            result.downloaded = true;
            result.savedPath = target.wstring();
            result.message = result.vendorName + L" driver saved to the Desktop as " +
                             plan.installerName + L".";
            return result;
        }
    }

    const std::filesystem::path shortcut = desktop / (result.vendorName + L" drivers.url");
    if (writeInternetShortcut(shortcut, plan.shortcutPage)) {
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
