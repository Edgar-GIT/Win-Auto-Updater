#include "nt_core.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace nt {

double nowSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

std::wstring formatNumber(double value, int decimals) {
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%.*f", decimals, value);
    std::wstring text = buffer;
    for (wchar_t& c : text) {
        if (c == L'.') {
            c = L',';
        }
    }
    return text;
}

std::wstring formatCount(uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    std::wstring out;
    const size_t n = digits.size();
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[i]);
        const size_t remaining = n - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out.push_back(L' ');
        }
    }
    return out;
}

std::wstring formatBytes(uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0 * 1024.0) {
        return formatNumber(mb / (1024.0 * 1024.0), 2) + L" TB";
    }
    if (mb >= 1024.0) {
        const double gb = mb / 1024.0;
        return formatNumber(gb, gb >= 100.0 ? 0 : 1) + L" GB";
    }
    return formatNumber(mb, 0) + L" MB";
}

std::wstring formatDuration(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const unsigned long long total = static_cast<unsigned long long>(seconds);
    const unsigned long long h = total / 3600ULL;
    const unsigned long long m = (total / 60ULL) % 60ULL;
    const unsigned long long s = total % 60ULL;
    wchar_t buffer[32]{};
    if (h > 0) {
        swprintf(buffer, 32, L"%llu:%02llu:%02llu", h, m, s);
    } else {
        swprintf(buffer, 32, L"%02llu:%02llu", m, s);
    }
    return buffer;
}

void lowerThreadPriority() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

int logicalProcessorCount() {
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        count = info.dwNumberOfProcessors;
    }
    return count > 0 ? static_cast<int>(count) : 1;
}

std::wstring hresultText(HRESULT hr) {
    wchar_t buffer[32]{};
    swprintf(buffer, 32, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

void logLine(const std::wstring& message) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);

    wchar_t temp[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len > MAX_PATH) {
        return;
    }
    const std::filesystem::path dir = std::filesystem::path(temp) / L"WinAutoUpdater";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(dir / L"native_tests.log", std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[40]{};
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ", now.wYear, now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond);

    std::string utf8;
    if (!message.empty()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, message.data(),
                                             static_cast<int>(message.size()), nullptr, 0, nullptr,
                                             nullptr);
        if (size > 0) {
            utf8.resize(static_cast<size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                                utf8.data(), size, nullptr, nullptr);
        }
    }
    file << stamp << utf8 << "\r\n";
}

}  // namespace nt
