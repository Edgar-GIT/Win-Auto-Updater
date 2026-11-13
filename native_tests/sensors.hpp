#pragma once

#include "battery.hpp"
#include "nt_core.hpp"

#include <thread>

// System sensors for the monitor bar (the job HWMonitor / the FurMark overlay
// do in the macro workflow), read once a second on a background thread with
// what Windows and the graphics drivers expose to a normal process:
//
//   CPU   load (GetSystemTimes), effective frequency (performance counter),
//         temperature from the ACPI thermal zones.
//   GPU   temperature / fan / power from the graphics kernel (WDDM 2.4+), with
//         the NVIDIA and AMD driver libraries as fallback; load and video
//         memory from the "GPU Engine" / "GPU Adapter Memory" counters.
//   Disk  temperature of the system drive (storage temperature query).
//   RAM   physical memory in use.  Battery: see battery.hpp.
//
// Per-core CPU temperatures need a kernel driver (that is what HWMonitor
// installs), so the CPU figure is the hottest ACPI thermal zone — on some
// desktop boards there is none and the value stays "n/d".

namespace nt {

constexpr double kNoReading = -1.0;

inline bool hasReading(double value) {
    return value >= 0.0;
}

struct SensorSnapshot {
    // CPU
    double cpuLoadPct = 0.0;
    int cpuMhz = 0;      // effective frequency now
    int cpuBaseMhz = 0;  // nominal frequency
    double cpuTempC = kNoReading;
    double cpuTempMaxC = kNoReading;  // since resetPeaks()

    // GPU (the adapter the graphics test runs on)
    std::wstring gpuName;
    const wchar_t* gpuTempSource = L"";  // which interface answered
    double gpuTempC = kNoReading;
    double gpuTempMaxC = kNoReading;
    double gpuLoadPct = kNoReading;
    double gpuPowerPct = kNoReading;
    int gpuFanRpm = -1;
    uint64_t vramUsed = 0;
    uint64_t vramTotal = 0;

    // Memory
    uint64_t ramTotal = 0;
    uint64_t ramUsed = 0;

    // System drive
    double diskTempC = kNoReading;
    double diskTempMaxC = kNoReading;

    BatteryInfo battery;
};

class Sensors {
public:
    Sensors() = default;
    ~Sensors();
    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;

    void start();
    void stop();

    SensorSnapshot snapshot() const;
    // Forget the maxima (called when a test session starts).
    void resetPeaks();

    std::vector<float> cpuTempHistory() const { return cpuTemp_.values(); }
    std::vector<float> gpuTempHistory() const { return gpuTemp_.values(); }
    std::vector<float> cpuLoadHistory() const { return cpuLoad_.values(); }

private:
    void run();

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> resetPeaks_{false};
    mutable std::mutex mutex_;
    SensorSnapshot snapshot_;
    History cpuTemp_{120};
    History gpuTemp_{120};
    History cpuLoad_{120};
};

}  // namespace nt
