#pragma once

#include "nt_core.hpp"

#include <thread>

// Native disk test (replaces CrystalDiskMark): the classic four rows
// (SEQ1M Q8T1, SEQ1M Q1T1, RND4K Q32T1, RND4K Q1T1), read and write, measured
// with unbuffered overlapped I/O on a temporary file — plus something the
// original does not do: every block written is self-describing, and a final
// pass reads the whole file back and checks it, so corrupted data is reported.

namespace nt {

struct DiskRow {
    const wchar_t* name = L"";
    double readMBs = -1.0;   // < 0 = not measured yet
    double writeMBs = -1.0;
    double readIops = 0.0;
    double writeIops = 0.0;
};

struct DriveInfo {
    std::wstring letter;  // "C:"
    std::wstring model;   // "Samsung SSD 970 EVO 500GB"
    std::wstring kind;    // "SSD NVMe", "HDD SATA", ...
    bool hdd = false;
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
};

struct DiskStats {
    State state = State::Idle;
    std::wstring message;
    DriveInfo drive;
    uint64_t fileBytes = 0;
    DiskRow rows[4];
    std::wstring phase;
    int phaseIndex = 0;
    int phaseCount = 0;
    double phaseProgress = 0.0;  // 0..1 inside the current phase
    double liveMBs = 0.0;
    uint64_t verifyErrors = 0;   // blocks that read back different from what was written
    uint64_t ioErrors = 0;       // read/write calls that failed
    uint64_t verifyCycles = 0;   // completed full-file verifications
    double elapsed = 0.0;
};

class DiskTest {
public:
    struct Options {
        std::wstring driveLetter;     // "C:"; empty = system drive
        bool loopReads = false;       // after the benchmark keep reading + verifying until stopped
        double secondsPerTest = 3.0;
        uint64_t fileMegabytes = 0;   // 0 = automatic
        std::wstring directory;       // optional override for where the test file goes
    };

    DiskTest() = default;
    ~DiskTest();
    DiskTest(const DiskTest&) = delete;
    DiskTest& operator=(const DiskTest&) = delete;

    bool start(const Options& options);
    void requestStop();
    void stop();
    bool active() const;

    DiskStats stats() const;
    std::vector<float> history() const { return history_.values(); }

    // Fixed drives of this PC ("C:", "D:", ...), system drive first.
    static std::vector<DriveInfo> listDrives();
    static DriveInfo describeDrive(const std::wstring& letter);

private:
    struct IoSpec;
    struct IoResult;

    void run(Options options);
    IoResult runIo(HANDLE file, HANDLE port, bool skipOnSuccess, const IoSpec& spec);
    void prepareWrite(uint8_t* buffer, uint32_t bytes, uint64_t offset, uint64_t& rng) const;
    uint64_t verifyBuffer(const uint8_t* buffer, uint32_t bytes, uint64_t offset) const;

    std::thread controller_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};

    uint8_t* pool_ = nullptr;  // random payload the written blocks are cut from
    uint64_t fileBytes_ = 0;
    double startedAt_ = 0.0;

    mutable std::mutex mutex_;
    DiskStats stats_;
    History history_{150};
};

}  // namespace nt
