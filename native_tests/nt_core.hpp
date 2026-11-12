#pragma once

// Shared plumbing for the native test bench (RAM / CPU / disk / GPU tests that
// replace the third-party tools driven by the macro). Everything in this folder
// is independent from the updater: it runs in its own process
// ("SingleUpdate.exe --native-tests") and only reuses the drawing toolkit.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nt {

enum class State {
    Idle,         // never started
    Starting,     // allocating / creating devices
    Running,
    Passed,       // finished (or stopped) without errors after doing real work
    Failed,       // hardware errors detected
    Stopped,      // stopped by the user before it could prove anything
    Unavailable,  // could not run on this machine (no memory, no GPU, no space...)
};

inline bool isActive(State s) {
    return s == State::Starting || s == State::Running;
}

// Fixed-capacity series for the sparklines. Written by the test threads, read
// by the UI thread.
class History {
public:
    explicit History(size_t capacity = 150) : capacity_(capacity) {}

    void push(float value) {
        std::lock_guard lock(mutex_);
        values_.push_back(value);
        if (values_.size() > capacity_) {
            values_.erase(values_.begin(), values_.begin() + (values_.size() - capacity_));
        }
    }

    void clear() {
        std::lock_guard lock(mutex_);
        values_.clear();
    }

    std::vector<float> values() const {
        std::lock_guard lock(mutex_);
        return values_;
    }

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<float> values_;
};

// Monotonic seconds.
double nowSeconds();

// "5,2 GB", "640 MB".
std::wstring formatBytes(uint64_t bytes);
// "12:34" or "1:02:03".
std::wstring formatDuration(double seconds);
// Decimal comma, fixed decimals: 1234.5 → "1234,5".
std::wstring formatNumber(double value, int decimals);
// Thousands separated integer: 12345 → "12 345".
std::wstring formatCount(uint64_t value);

// Test threads run below normal priority so the windows stay responsive while
// every core is busy; the load is the same (they still take all idle CPU).
void lowerThreadPriority();

int logicalProcessorCount();

// Appends a line to %TEMP%\WinAutoUpdater\native_tests.log (UTF-8, timestamped).
void logLine(const std::wstring& message);

std::wstring hresultText(HRESULT hr);

// SplitMix64: seeds the generators so runs are reproducible per (seed, index).
inline uint64_t splitMix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline uint64_t xorShift64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

}  // namespace nt
