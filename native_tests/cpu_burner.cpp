#include "cpu_burner.hpp"

#include <immintrin.h>
#include <malloc.h>

#include <algorithm>
#include <cstring>

namespace nt {

namespace {

constexpr int kN = 96;             // matrix dimension (N×N doubles ≈ 72 KB: lives in L2)
constexpr int kMatMulsPerRound = 8;
constexpr int kIntWords = 8192;    // 64 KB of integer state
constexpr int kIntPasses = 6;
constexpr int kDataSets = 16;      // different inputs cycled through, each with its own reference

struct Workspace {
    double* a = nullptr;
    double* b = nullptr;
    double* c = nullptr;
    uint64_t* ints = nullptr;

    Workspace() {
        const size_t matrixBytes = sizeof(double) * kN * kN;
        a = static_cast<double*>(_aligned_malloc(matrixBytes, 64));
        b = static_cast<double*>(_aligned_malloc(matrixBytes, 64));
        c = static_cast<double*>(_aligned_malloc(matrixBytes, 64));
        ints = static_cast<uint64_t*>(_aligned_malloc(sizeof(uint64_t) * kIntWords, 64));
    }
    ~Workspace() {
        _aligned_free(a);
        _aligned_free(b);
        _aligned_free(c);
        _aligned_free(ints);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    bool ok() const { return a && b && c && ints; }
};

void matMulBase(const double* a, const double* b, double* c) {
    for (int i = 0; i < kN; ++i) {
        double* ci = c + i * kN;
        for (int j = 0; j < kN; ++j) {
            ci[j] = 0.0;
        }
        for (int k = 0; k < kN; ++k) {
            const double aik = a[i * kN + k];
            const double* bk = b + k * kN;
            for (int j = 0; j < kN; ++j) {
                ci[j] += aik * bk[j];
            }
        }
    }
}

// 256-bit FMA version: the heaviest (hottest) code a desktop CPU runs. Written
// with unaligned loads/stores on heap buffers and register-only temporaries.
__attribute__((target("avx2,fma"))) void matMulAvx2(const double* a, const double* b, double* c) {
    for (int i = 0; i < kN; ++i) {
        double* ci = c + i * kN;
        for (int j = 0; j < kN; j += 4) {
            _mm256_storeu_pd(ci + j, _mm256_setzero_pd());
        }
        for (int k = 0; k < kN; ++k) {
            const __m256d aik = _mm256_set1_pd(a[i * kN + k]);
            const double* bk = b + k * kN;
            for (int j = 0; j < kN; j += 4) {
                _mm256_storeu_pd(
                    ci + j, _mm256_fmadd_pd(aik, _mm256_loadu_pd(bk + j), _mm256_loadu_pd(ci + j)));
            }
        }
    }
}

inline uint64_t foldHash(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= 0x100000001B3ULL;
    return (hash << 23) | (hash >> 41);
}

// One unit of work: fills the inputs from the data-set seed, runs the FP and
// integer kernels and returns a checksum of every result bit.
uint64_t burnRound(Workspace& w, int dataSet, bool avx2) {
    uint64_t seed = 0xC0FFEE1234ULL + static_cast<uint64_t>(dataSet) * 0x9E3779B97F4A7C15ULL;
    uint64_t rng = splitMix64(seed) | 1ULL;
    for (int i = 0; i < kN * kN; ++i) {
        // Values in [-1, 1): products stay well inside double range.
        w.a[i] = static_cast<double>(static_cast<int64_t>(xorShift64(rng) >> 11)) / 4503599627370496.0 - 1.0;
        w.b[i] = static_cast<double>(static_cast<int64_t>(xorShift64(rng) >> 11)) / 4503599627370496.0 - 1.0;
    }

    uint64_t hash = 0xCBF29CE484222325ULL;
    for (int round = 0; round < kMatMulsPerRound; ++round) {
        if (avx2) {
            matMulAvx2(w.a, w.b, w.c);
        } else {
            matMulBase(w.a, w.b, w.c);
        }
        // Feed the product back so every multiplication sees new data.
        for (int i = 0; i < kN * kN; ++i) {
            uint64_t bits = 0;
            std::memcpy(&bits, &w.c[i], sizeof(bits));
            hash = foldHash(hash, bits);
            w.a[i] = w.c[i] * (1.0 / kN);
        }
    }

    for (int i = 0; i < kIntWords; ++i) {
        w.ints[i] = xorShift64(rng);
    }
    uint64_t previous = hash;
    for (int pass = 0; pass < kIntPasses; ++pass) {
        for (int i = 0; i < kIntWords; ++i) {
            uint64_t x = w.ints[i];
            x ^= x >> 29;
            x *= 0xBF58476D1CE4E5B9ULL;
            x ^= (previous << 17) | (previous >> 47);
            x += static_cast<uint64_t>(i) * 0x94D049BB133111EBULL;
            w.ints[i] = x;
            previous = x;
        }
    }
    for (int i = 0; i < kIntWords; ++i) {
        hash = foldHash(hash, w.ints[i]);
    }
    return hash;
}

bool cpuHasAvx2Fma() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
}

struct ProcessorPowerInformation {
    ULONG number;
    ULONG maxMhz;
    ULONG currentMhz;
    ULONG mhzLimit;
    ULONG maxIdleState;
    ULONG currentIdleState;
};

extern "C" LONG WINAPI CallNtPowerInformation(int level, PVOID in, ULONG inSize, PVOID out,
                                              ULONG outSize);

void readFrequency(int* currentMhz, int* maxMhz) {
    *currentMhz = 0;
    *maxMhz = 0;
    const int cpus = logicalProcessorCount();
    std::vector<ProcessorPowerInformation> info(static_cast<size_t>(cpus));
    constexpr int kProcessorInformation = 11;
    if (CallNtPowerInformation(kProcessorInformation, nullptr, 0, info.data(),
                               static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformation))) != 0) {
        return;
    }
    uint64_t sum = 0;
    ULONG maximum = 0;
    for (const auto& cpu : info) {
        sum += cpu.currentMhz;
        maximum = std::max(maximum, cpu.maxMhz);
    }
    *currentMhz = static_cast<int>(sum / info.size());
    *maxMhz = static_cast<int>(maximum);
}

uint64_t fileTimeValue(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

std::wstring CpuBurner::cpuName() {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return L"CPU";
    }
    std::wstring name = buffer;
    // Collapse the padding some vendors put in the brand string.
    std::wstring out;
    bool space = false;
    for (wchar_t c : name) {
        if (c == L' ') {
            if (!space && !out.empty()) {
                out.push_back(c);
            }
            space = true;
        } else {
            out.push_back(c);
            space = false;
        }
    }
    while (!out.empty() && out.back() == L' ') {
        out.pop_back();
    }
    return out.empty() ? L"CPU" : out;
}

CpuBurner::~CpuBurner() {
    stop();
}

bool CpuBurner::active() const {
    return active_.load();
}

CpuStats CpuBurner::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool CpuBurner::start(int threads) {
    if (active_.exchange(true)) {
        return false;
    }
    if (controller_.joinable()) {
        controller_.join();
    }
    stopRequested_ = false;
    injectFault_ = false;
    iterations_ = 0;
    errors_ = 0;
    history_.clear();
    {
        std::lock_guard lock(mutex_);
        stats_ = CpuStats{};
        stats_.state = State::Starting;
        stats_.message = L"A calcular os resultados de referência...";
    }
    controller_ = std::thread([this, threads] { run(threads); });
    return true;
}

void CpuBurner::requestStop() {
    stopRequested_ = true;
}

void CpuBurner::stop() {
    stopRequested_ = true;
    if (controller_.joinable()) {
        controller_.join();
    }
}

void CpuBurner::worker(int index) {
    lowerThreadPriority();
    Workspace workspace;
    if (!workspace.ok()) {
        return;
    }
    int dataSet = index % kDataSets;
    while (!stopRequested_) {
        uint64_t result = burnRound(workspace, dataSet, useAvx2_);
        if (injectFault_.exchange(false)) {
            result ^= 1ULL;
        }
        if (result != reference_[static_cast<size_t>(dataSet)]) {
            errors_.fetch_add(1);
        }
        iterations_.fetch_add(1);
        dataSet = (dataSet + 1) % kDataSets;
    }
}

void CpuBurner::run(int threads) {
    const int threadCount = threads > 0 ? threads : logicalProcessorCount();
    useAvx2_ = cpuHasAvx2Fma();
    const std::wstring name = cpuName();

    // Reference results: computed twice here; they must already agree.
    {
        Workspace workspace;
        if (!workspace.ok()) {
            std::lock_guard lock(mutex_);
            stats_.state = State::Unavailable;
            stats_.message = L"Sem memória para o teste de CPU.";
            active_ = false;
            return;
        }
        reference_.assign(kDataSets, 0);
        bool consistent = true;
        for (int set = 0; set < kDataSets; ++set) {
            reference_[static_cast<size_t>(set)] = burnRound(workspace, set, useAvx2_);
            if (burnRound(workspace, set, useAvx2_) != reference_[static_cast<size_t>(set)]) {
                consistent = false;
            }
        }
        if (!consistent) {
            // The CPU cannot even repeat a calculation: that is a failure by itself.
            errors_.fetch_add(1);
        }
    }

    {
        std::lock_guard lock(mutex_);
        stats_.state = State::Running;
        stats_.message.clear();
        stats_.cpuName = name;
        stats_.threads = threadCount;
        stats_.avx2 = useAvx2_;
    }
    logLine(L"[CPU] " + name + L" — " + std::to_wstring(threadCount) + L" threads, " +
            (useAvx2_ ? L"AVX2+FMA." : L"SSE2."));

    const double started = nowSeconds();
    for (int i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i] { worker(i); });
    }

    FILETIME idle0{}, kernel0{}, user0{};
    GetSystemTimes(&idle0, &kernel0, &user0);
    uint64_t lastIterations = 0;
    double lastTime = started;
    double smoothed = 0.0;
    double peak = 0.0;
    int tick = 0;
    while (!stopRequested_) {
        Sleep(250);
        ++tick;
        const double now = nowSeconds();
        const uint64_t iterations = iterations_.load();
        const double rate =
            static_cast<double>(iterations - lastIterations) / std::max(0.001, now - lastTime);
        smoothed = smoothed <= 0.0 ? rate : smoothed * 0.7 + rate * 0.3;
        lastIterations = iterations;
        lastTime = now;
        // Ignore the first seconds (threads spinning up, turbo ramping).
        if (now - started > 4.0) {
            peak = std::max(peak, smoothed);
        }
        history_.push(static_cast<float>(smoothed));

        double load = 0.0;
        FILETIME idle1{}, kernel1{}, user1{};
        if (GetSystemTimes(&idle1, &kernel1, &user1)) {
            const uint64_t idle = fileTimeValue(idle1) - fileTimeValue(idle0);
            const uint64_t total = (fileTimeValue(kernel1) - fileTimeValue(kernel0)) +
                                   (fileTimeValue(user1) - fileTimeValue(user0));
            if (total > 0) {
                load = 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(total));
            }
            idle0 = idle1;
            kernel0 = kernel1;
            user0 = user1;
        }

        int currentMhz = 0;
        int maxMhz = 0;
        if (tick % 4 == 0) {
            readFrequency(&currentMhz, &maxMhz);
        }

        std::lock_guard lock(mutex_);
        stats_.elapsed = now - started;
        stats_.iterations = iterations;
        stats_.errors = errors_.load();
        stats_.itersPerSec = smoothed;
        stats_.peakItersPerSec = peak;
        stats_.pctOfPeak = peak > 0.0 ? std::min(100.0, 100.0 * smoothed / peak) : 100.0;
        stats_.loadPct = std::clamp(load, 0.0, 100.0);
        if (tick % 4 == 0) {
            stats_.currentMhz = currentMhz;
            stats_.maxMhz = maxMhz;
        }
    }

    for (std::thread& thread : workers_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workers_.clear();

    const uint64_t errors = errors_.load();
    const uint64_t iterations = iterations_.load();
    {
        std::lock_guard lock(mutex_);
        stats_.elapsed = nowSeconds() - started;
        stats_.iterations = iterations;
        stats_.errors = errors;
        stats_.itersPerSec = 0.0;
        stats_.loadPct = 0.0;
        if (errors > 0) {
            stats_.state = State::Failed;
            stats_.message = std::to_wstring(errors) + L" cálculo(s) errado(s) — CPU instável.";
        } else if (iterations > 0) {
            stats_.state = State::Passed;
            stats_.message = L"Sem erros em " + formatCount(iterations) + L" iterações.";
        } else {
            stats_.state = State::Stopped;
            stats_.message = L"Interrompido.";
        }
    }
    logLine(L"[CPU] Fim: " + std::to_wstring(iterations) + L" iterações, " + std::to_wstring(errors) +
            L" erro(s).");
    active_ = false;
}

}  // namespace nt
