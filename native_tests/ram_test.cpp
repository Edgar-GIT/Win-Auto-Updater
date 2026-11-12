#include "ram_test.hpp"

#include <algorithm>

namespace nt {

namespace {

constexpr size_t kMaxChunkBytes = 128ULL * 1024 * 1024;
constexpr size_t kMinChunkBytes = 16ULL * 1024 * 1024;
constexpr size_t kBlockBytes = 1024 * 1024;  // unit of work between stop checks
constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr int kPatternCount = 8;
constexpr size_t kMaxRecordedErrors = 16;

}  // namespace

const wchar_t* RamTest::patternName(int pattern) {
    switch (pattern) {
        case 0:
            return L"aleatório";
        case 1:
            return L"aleatório invertido";
        case 2:
            return L"endereço";
        case 3:
            return L"xadrez 55/AA";
        case 4:
            return L"xadrez AA/55";
        case 5:
            return L"bit andante";
        case 6:
            return L"zeros";
        case 7:
            return L"uns";
        default:
            return L"—";
    }
}

RamTest::~RamTest() {
    stop();
}

bool RamTest::active() const {
    return active_.load();
}

RamStats RamTest::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool RamTest::start(uint64_t megabytes) {
    if (active_.exchange(true)) {
        return false;
    }
    if (controller_.joinable()) {
        controller_.join();
    }
    stopRequested_ = false;
    processedBytes_ = 0;
    errors_ = 0;
    currentPattern_ = 0;
    history_.clear();
    {
        std::lock_guard lock(mutex_);
        stats_ = RamStats{};
        stats_.state = State::Starting;
        stats_.message = L"A reservar memória...";
    }
    controller_ = std::thread([this, megabytes] { run(megabytes); });
    return true;
}

void RamTest::requestStop() {
    stopRequested_ = true;
}

void RamTest::stop() {
    stopRequested_ = true;
    if (controller_.joinable()) {
        controller_.join();
    }
}

void RamTest::report(uint64_t offset, uint64_t expected, uint64_t actual, int pattern) {
    errors_.fetch_add(1);
    std::lock_guard lock(mutex_);
    if (stats_.firstErrors.size() < kMaxRecordedErrors) {
        stats_.firstErrors.push_back(RamError{offset, expected, actual, pattern});
    }
}

// Writes (verify = false) or checks (verify = true) one block with the given
// pattern. The data of a block depends only on (pattern, seed, chunk, block),
// so the verify sweep can regenerate exactly what the write sweep stored.
uint64_t RamTest::sweepBlock(const Chunk& chunk, size_t blockIndex, int pattern, uint64_t seed,
                             bool verify) {
    const size_t blockOffset = blockIndex * kBlockBytes;
    const size_t bytes = std::min(kBlockBytes, chunk.bytes - blockOffset);
    const size_t count = bytes / sizeof(uint64_t);
    uint64_t* data = chunk.data + blockOffset / sizeof(uint64_t);
    const uint64_t baseOffset = chunk.offset + blockOffset;
    uint64_t bad = 0;

    const auto run = [&](auto&& next) {
        if (!verify) {
            for (size_t i = 0; i < count; ++i) {
                data[i] = next(i);
            }
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            const uint64_t expected = next(i);
            const uint64_t actual = data[i];
            if (actual != expected) {
                ++bad;
                report(baseOffset + i * sizeof(uint64_t), expected, actual, pattern);
            }
        }
    };

    uint64_t mix = seed ^ (baseOffset * 0x9E3779B97F4A7C15ULL) ^ 0xD1B54A32D192ED03ULL;
    uint64_t rng = splitMix64(mix) | 1ULL;

    switch (pattern) {
        case 0:
            run([&rng](size_t) { return xorShift64(rng); });
            break;
        case 1:
            run([&rng](size_t) { return ~xorShift64(rng); });
            break;
        case 2:
            // Each word holds its own address: catches address-line faults.
            run([baseOffset](size_t i) {
                return (baseOffset + i * sizeof(uint64_t)) ^ 0xA5A5A5A55A5A5A5AULL;
            });
            break;
        case 3:
            run([](size_t i) {
                return (i & 1) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
            });
            break;
        case 4:
            run([](size_t i) {
                return (i & 1) ? 0x5555555555555555ULL : 0xAAAAAAAAAAAAAAAAULL;
            });
            break;
        case 5: {
            const unsigned shift = static_cast<unsigned>(seed & 63U);
            run([shift](size_t i) { return 1ULL << ((i + shift) & 63U); });
            break;
        }
        case 6:
            run([](size_t) { return 0ULL; });
            break;
        default:
            run([](size_t) { return ~0ULL; });
            break;
    }
    return bad;
}

void RamTest::worker(int index, int threadCount) {
    lowerThreadPriority();
    uint64_t cycle = 0;
    while (!stopRequested_) {
        for (int pattern = 0; pattern < kPatternCount && !stopRequested_; ++pattern) {
            if (index == 0) {
                currentPattern_ = pattern;
            }
            const uint64_t seed = (cycle + 1) * 0x2545F4914F6CDD1DULL + static_cast<uint64_t>(pattern);
            // Write the whole share first, verify afterwards: the data has to
            // survive in RAM for a while, not just in the CPU cache.
            for (int phase = 0; phase < 2 && !stopRequested_; ++phase) {
                const bool verify = phase == 1;
                for (size_t c = static_cast<size_t>(index); c < chunks_.size() && !stopRequested_;
                     c += static_cast<size_t>(threadCount)) {
                    const Chunk& chunk = chunks_[c];
                    const size_t blocks = (chunk.bytes + kBlockBytes - 1) / kBlockBytes;
                    for (size_t b = 0; b < blocks && !stopRequested_; ++b) {
                        sweepBlock(chunk, b, pattern, seed, verify);
                        processedBytes_.fetch_add(std::min(kBlockBytes, chunk.bytes - b * kBlockBytes));
                    }
                }
            }
        }
        if (!stopRequested_) {
            ++cycle;
            std::lock_guard lock(mutex_);
            cycles_[static_cast<size_t>(index)] = cycle;
        }
    }
}

void RamTest::release() {
    for (Chunk& chunk : chunks_) {
        if (chunk.data != nullptr) {
            VirtualFree(chunk.data, 0, MEM_RELEASE);
        }
    }
    chunks_.clear();
    // Back to the default working-set limits.
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));
}

void RamTest::injectFault() {
    if (chunks_.empty() || !active_) {
        return;
    }
    static uint64_t state = 0x1234567ULL;
    const Chunk& chunk = chunks_[splitMix64(state) % chunks_.size()];
    const size_t words = chunk.bytes / sizeof(uint64_t);
    chunk.data[splitMix64(state) % words] ^= 1ULL << (splitMix64(state) & 63U);
}

void RamTest::run(uint64_t megabytes) {
    const auto fail = [this](State state, const std::wstring& message) {
        {
            std::lock_guard lock(mutex_);
            stats_.state = state;
            stats_.message = message;
        }
        logLine(L"[RAM] " + message);
        active_ = false;
    };

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) {
        fail(State::Unavailable, L"Não foi possível ler o estado da memória.");
        return;
    }

    // Leave room for Windows, the other tests and the GPU driver.
    const uint64_t reserve = std::max<uint64_t>(1024 * kMiB, memory.ullTotalPhys * 15 / 100);
    uint64_t target = megabytes > 0 ? megabytes * kMiB
                                    : (memory.ullAvailPhys > reserve ? memory.ullAvailPhys - reserve : 0);
    target -= target % kBlockBytes;
    if (target < 64 * kMiB) {
        fail(State::Unavailable, L"Memória livre insuficiente para testar (" +
                                     formatBytes(memory.ullAvailPhys) + L" livres).");
        return;
    }

    // Several chunks per core: a failed allocation only costs one chunk, and
    // every thread gets its own share even when little memory is free.
    const int cores = std::clamp(logicalProcessorCount(), 1, 32);
    size_t chunkBytes = static_cast<size_t>(target / static_cast<uint64_t>(cores));
    chunkBytes = std::clamp(chunkBytes - chunkBytes % kBlockBytes, kMinChunkBytes, kMaxChunkBytes);

    uint64_t allocated = 0;
    while (allocated < target && !stopRequested_) {
        const size_t bytes = static_cast<size_t>(std::min<uint64_t>(chunkBytes, target - allocated));
        void* memoryBlock = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (memoryBlock == nullptr) {
            break;
        }
        chunks_.push_back(Chunk{static_cast<uint64_t*>(memoryBlock), bytes, allocated});
        allocated += bytes;
    }
    if (allocated < 64 * kMiB) {
        release();
        fail(State::Unavailable, L"O Windows não cedeu memória suficiente para o teste.");
        return;
    }

    // Pin the pages so the test exercises physical RAM instead of the pagefile.
    bool locked = false;
    if (SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(allocated + 256 * kMiB),
                                 static_cast<SIZE_T>(allocated + 768 * kMiB))) {
        locked = true;
        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (stopRequested_ || !VirtualLock(chunks_[i].data, chunks_[i].bytes)) {
                for (size_t j = 0; j < i; ++j) {
                    VirtualUnlock(chunks_[j].data, chunks_[j].bytes);
                }
                locked = false;
                break;
            }
        }
    }

    const int threadCount = static_cast<int>(std::min<size_t>(chunks_.size(), static_cast<size_t>(cores)));
    {
        std::lock_guard lock(mutex_);
        cycles_.assign(static_cast<size_t>(threadCount), 0);
        stats_.state = State::Running;
        stats_.message.clear();
        stats_.bytesUnderTest = allocated;
        stats_.totalPhysical = memory.ullTotalPhys;
        stats_.threads = threadCount;
        stats_.locked = locked;
    }
    logLine(L"[RAM] A testar " + formatBytes(allocated) + L" com " + std::to_wstring(threadCount) +
            L" threads" + (locked ? L" (memória fixada)." : L" (memória não fixada)."));

    const double started = nowSeconds();
    for (int i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i, threadCount] { worker(i, threadCount); });
    }

    uint64_t lastBytes = 0;
    double lastTime = started;
    double smoothed = 0.0;
    while (!stopRequested_) {
        Sleep(250);
        const double now = nowSeconds();
        const uint64_t bytes = processedBytes_.load();
        const double rate = static_cast<double>(bytes - lastBytes) / static_cast<double>(kMiB) /
                            std::max(0.001, now - lastTime);
        smoothed = smoothed <= 0.0 ? rate : smoothed * 0.6 + rate * 0.4;
        lastBytes = bytes;
        lastTime = now;
        history_.push(static_cast<float>(smoothed));

        std::lock_guard lock(mutex_);
        stats_.elapsed = now - started;
        stats_.mbPerSec = smoothed;
        stats_.coveragePct = static_cast<double>(bytes) / (2.0 * static_cast<double>(allocated)) * 100.0;
        stats_.bytesVerified = bytes / 2;
        stats_.errors = errors_.load();
        stats_.passes = cycles_.empty() ? 0 : *std::min_element(cycles_.begin(), cycles_.end());
        stats_.pattern = patternName(currentPattern_.load());
    }

    for (std::thread& thread : workers_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workers_.clear();
    release();

    const uint64_t errors = errors_.load();
    const double coverage =
        static_cast<double>(processedBytes_.load()) / (2.0 * static_cast<double>(allocated)) * 100.0;
    {
        std::lock_guard lock(mutex_);
        stats_.elapsed = nowSeconds() - started;
        stats_.errors = errors;
        stats_.coveragePct = coverage;
        stats_.bytesVerified = processedBytes_.load() / 2;
        stats_.mbPerSec = 0.0;
        if (errors > 0) {
            stats_.state = State::Failed;
            stats_.message = std::to_wstring(errors) + L" erro(s) de memória detetado(s).";
        } else if (coverage >= 100.0) {
            stats_.state = State::Passed;
            stats_.message = L"Sem erros — cobertura " + formatNumber(coverage, 0) + L"%.";
        } else {
            stats_.state = State::Stopped;
            stats_.message = L"Interrompido antes de cobrir toda a memória (" +
                             formatNumber(coverage, 0) + L"%).";
        }
    }
    logLine(L"[RAM] Fim: cobertura " + formatNumber(coverage, 0) + L"%, " + std::to_wstring(errors) +
            L" erro(s).");
    active_ = false;
}

}  // namespace nt
