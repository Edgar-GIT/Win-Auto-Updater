#pragma once

#include "nt_core.hpp"

#include <thread>

// Native CPU burner (replaces Prime95 / powerMAX CPU): one worker per logical
// processor running dense floating-point (AVX2+FMA when available) and integer
// work. Every round is checked against a reference result computed at start, so
// an unstable or overheating CPU shows up as errors, and thermal throttling
// shows up as throughput falling below its own peak.

namespace nt {

struct CpuStats {
    State state = State::Idle;
    std::wstring message;
    std::wstring cpuName;
    int threads = 0;
    bool avx2 = false;
    uint64_t iterations = 0;
    uint64_t errors = 0;
    double itersPerSec = 0.0;
    double peakItersPerSec = 0.0;
    double pctOfPeak = 100.0;  // current throughput vs. the best seen (throttling indicator)
    double loadPct = 0.0;      // whole-system CPU load
    int currentMhz = 0;
    int maxMhz = 0;
    double elapsed = 0.0;
};

class CpuBurner {
public:
    CpuBurner() = default;
    ~CpuBurner();
    CpuBurner(const CpuBurner&) = delete;
    CpuBurner& operator=(const CpuBurner&) = delete;

    // threads = 0 → one per logical processor.
    bool start(int threads = 0);
    void requestStop();
    void stop();
    bool active() const;

    CpuStats stats() const;
    std::vector<float> history() const { return history_.values(); }

    // Self-test aid: makes one worker report a wrong result once.
    void injectFault() { injectFault_ = true; }

    static std::wstring cpuName();

private:
    void run(int threads);
    void worker(int index);

    std::thread controller_;
    std::vector<std::thread> workers_;
    std::vector<uint64_t> reference_;  // expected checksum per data set

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> injectFault_{false};
    std::atomic<uint64_t> iterations_{0};
    std::atomic<uint64_t> errors_{0};
    bool useAvx2_ = false;

    mutable std::mutex mutex_;
    CpuStats stats_;
    History history_{150};
};

}  // namespace nt
