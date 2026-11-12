#pragma once

#include "nt_core.hpp"

#include <thread>

// Native RAM test (replaces MemTest): takes most of the free physical memory,
// keeps it resident, and loops write → verify sweeps with several data patterns
// from every core. Any word that reads back different is a hardware error.

namespace nt {

struct RamError {
    uint64_t offset = 0;  // byte offset inside the tested memory
    uint64_t expected = 0;
    uint64_t actual = 0;
    int pattern = 0;
};

struct RamStats {
    State state = State::Idle;
    std::wstring message;
    uint64_t bytesUnderTest = 0;
    uint64_t totalPhysical = 0;
    int threads = 0;
    bool locked = false;        // pages pinned in physical RAM (cannot be swapped out)
    double coveragePct = 0.0;   // 100 % = every byte written and verified once
    uint64_t bytesVerified = 0; // total bytes written and read back so far
    uint64_t passes = 0;        // complete cycles through all patterns
    uint64_t errors = 0;
    double mbPerSec = 0.0;
    double elapsed = 0.0;
    std::wstring pattern;
    std::vector<RamError> firstErrors;
};

class RamTest {
public:
    RamTest() = default;
    ~RamTest();
    RamTest(const RamTest&) = delete;
    RamTest& operator=(const RamTest&) = delete;

    // megabytes = 0 → automatic (free physical memory minus a safety reserve).
    bool start(uint64_t megabytes = 0);
    void requestStop();
    void stop();  // waits for the threads and releases the memory
    bool active() const;

    RamStats stats() const;
    std::vector<float> history() const { return history_.values(); }

    // Self-test aid: flips one bit somewhere in the tested memory.
    void injectFault();

    static const wchar_t* patternName(int pattern);

private:
    struct Chunk {
        uint64_t* data = nullptr;
        size_t bytes = 0;
        uint64_t offset = 0;  // offset of this chunk inside the whole tested area
    };

    void run(uint64_t megabytes);
    void worker(int index, int threadCount);
    uint64_t sweepBlock(const Chunk& chunk, size_t blockIndex, int pattern, uint64_t seed,
                        bool verify);
    void report(uint64_t offset, uint64_t expected, uint64_t actual, int pattern);
    void release();

    std::thread controller_;
    std::vector<std::thread> workers_;
    std::vector<Chunk> chunks_;
    std::vector<uint64_t> cycles_;  // per-thread completed pattern cycles (guarded by mutex_)

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> processedBytes_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<int> currentPattern_{0};

    mutable std::mutex mutex_;
    RamStats stats_;
    History history_{150};
};

}  // namespace nt
