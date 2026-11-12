#include "disk_test.hpp"

#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace nt {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint32_t kUnit = 4096;                   // every write is a multiple of this
constexpr uint32_t kPoolBlocks = 4096;             // 16 MiB of random payload
constexpr uint64_t kPoolBytes = static_cast<uint64_t>(kPoolBlocks) * kUnit;
constexpr uint64_t kMagic = 0x3154534B44554157ULL;  // "WAUDKST1"
constexpr uint32_t kHeaderBytes = 24;
constexpr uint64_t kMaxIoErrors = 64;

// Layout of the first bytes of every 4 KiB unit on disk.
struct UnitHeader {
    uint64_t magic;
    uint64_t offset;      // where in the file this unit was written
    uint32_t poolIndex;   // which pool block the payload was copied from
    uint32_t poolCheck;   // ~poolIndex
};

struct Slot {
    OVERLAPPED ov;  // must stay first: completions hand back a pointer to it
    uint8_t* buffer = nullptr;
    uint64_t offset = 0;
};

std::wstring trimCopy(const char* text) {
    std::string value = text ? text : "";
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    size_t first = 0;
    while (first < value.size() && value[first] == ' ') {
        ++first;
    }
    value = value.substr(first);
    return std::wstring(value.begin(), value.end());
}

const wchar_t* busName(int bus) {
    switch (bus) {
        case 17:
            return L"NVMe";
        case 11:
        case 3:
            return L"SATA";
        case 7:
            return L"USB";
        case 12:
        case 13:
            return L"SD/eMMC";
        case 8:
            return L"RAID";
        case 10:
            return L"SAS";
        default:
            return L"";
    }
}

std::wstring systemDriveLetter() {
    wchar_t windows[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows, MAX_PATH) >= 2 && windows[1] == L':') {
        return std::wstring(windows, 2);
    }
    return L"C:";
}

}  // namespace

struct DiskTest::IoSpec {
    bool write = false;
    uint32_t blockBytes = kUnit;
    int queueDepth = 1;
    bool random = false;
    double seconds = 3.0;    // time-limited when byteLimit == 0
    uint64_t byteLimit = 0;  // > 0: one sequential pass over this many bytes
    bool verify = false;     // check every unit read
};

struct DiskTest::IoResult {
    uint64_t bytes = 0;
    uint64_t ops = 0;
    double seconds = 0.0;
    uint64_t ioErrors = 0;
    uint64_t verifyErrors = 0;

    double mbPerSec() const {
        return seconds > 0.0 ? static_cast<double>(bytes) / static_cast<double>(kMiB) / seconds : 0.0;
    }
    double iops() const { return seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0; }
};

// ---------------------------------------------------------------------------
// Drive information
// ---------------------------------------------------------------------------

DriveInfo DiskTest::describeDrive(const std::wstring& letter) {
    DriveInfo info;
    info.letter = letter.size() >= 2 ? letter.substr(0, 2) : systemDriveLetter();

    const std::wstring root = info.letter + L"\\";
    ULARGE_INTEGER freeForCaller{}, total{}, totalFree{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeForCaller, &total, &totalFree)) {
        info.totalBytes = total.QuadPart;
        info.freeBytes = freeForCaller.QuadPart;
    }

    const std::wstring device = L"\\\\.\\" + info.letter;
    HANDLE volume = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        return info;
    }

    int bus = 0;
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        uint8_t buffer[1024]{};
        DWORD returned = 0;
        if (DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                            sizeof(buffer) - 1, &returned, nullptr) &&
            returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
            bus = static_cast<int>(descriptor->BusType);
            std::wstring vendor;
            std::wstring product;
            if (descriptor->VendorIdOffset != 0 && descriptor->VendorIdOffset < returned) {
                vendor = trimCopy(reinterpret_cast<const char*>(buffer) + descriptor->VendorIdOffset);
            }
            if (descriptor->ProductIdOffset != 0 && descriptor->ProductIdOffset < returned) {
                product = trimCopy(reinterpret_cast<const char*>(buffer) + descriptor->ProductIdOffset);
            }
            info.model = vendor.empty() ? product : (product.empty() ? vendor : vendor + L" " + product);
        }
    }

    bool known = false;
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
        DWORD returned = 0;
        if (DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty,
                            sizeof(penalty), &returned, nullptr) &&
            returned >= sizeof(penalty)) {
            info.hdd = penalty.IncursSeekPenalty != FALSE;
            known = true;
        }
    }
    CloseHandle(volume);

    if (bus == 17) {
        info.hdd = false;
        known = true;
    }
    if (known) {
        info.kind = info.hdd ? L"HDD" : L"SSD";
    }
    const std::wstring busText = busName(bus);
    if (!busText.empty()) {
        info.kind += info.kind.empty() ? busText : L" " + busText;
    }
    return info;
}

std::vector<DriveInfo> DiskTest::listDrives() {
    std::vector<DriveInfo> drives;
    const std::wstring system = systemDriveLetter();
    drives.push_back(describeDrive(system));

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1U << i)) == 0) {
            continue;
        }
        const std::wstring letter{static_cast<wchar_t>(L'A' + i), L':'};
        if (_wcsicmp(letter.c_str(), system.c_str()) == 0) {
            continue;
        }
        if (GetDriveTypeW((letter + L"\\").c_str()) != DRIVE_FIXED) {
            continue;
        }
        DriveInfo info = describeDrive(letter);
        if (info.totalBytes > 0) {
            drives.push_back(std::move(info));
        }
    }
    return drives;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

DiskTest::~DiskTest() {
    stop();
}

bool DiskTest::active() const {
    return active_.load();
}

DiskStats DiskTest::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool DiskTest::start(const Options& options) {
    if (active_.exchange(true)) {
        return false;
    }
    if (controller_.joinable()) {
        controller_.join();
    }
    stopRequested_ = false;
    history_.clear();
    {
        std::lock_guard lock(mutex_);
        stats_ = DiskStats{};
        stats_.state = State::Starting;
        stats_.message = L"A preparar...";
        stats_.rows[0].name = L"SEQ1M Q8T1";
        stats_.rows[1].name = L"SEQ1M Q1T1";
        stats_.rows[2].name = L"RND4K Q32T1";
        stats_.rows[3].name = L"RND4K Q1T1";
    }
    controller_ = std::thread([this, options] { run(options); });
    return true;
}

void DiskTest::requestStop() {
    stopRequested_ = true;
}

void DiskTest::stop() {
    stopRequested_ = true;
    if (controller_.joinable()) {
        controller_.join();
    }
}

// ---------------------------------------------------------------------------
// Data: self-describing 4 KiB units
// ---------------------------------------------------------------------------

void DiskTest::prepareWrite(uint8_t* buffer, uint32_t bytes, uint64_t offset, uint64_t& rng) const {
    const uint32_t units = bytes / kUnit;
    const uint32_t first = static_cast<uint32_t>(xorShift64(rng) % (kPoolBlocks - units + 1));
    std::memcpy(buffer, pool_ + static_cast<size_t>(first) * kUnit, bytes);
    for (uint32_t u = 0; u < units; ++u) {
        const uint64_t unitOffset = offset + static_cast<uint64_t>(u) * kUnit;
        std::memcpy(buffer + static_cast<size_t>(u) * kUnit + offsetof(UnitHeader, offset), &unitOffset,
                    sizeof(unitOffset));
    }
}

uint64_t DiskTest::verifyBuffer(const uint8_t* buffer, uint32_t bytes, uint64_t offset) const {
    uint64_t bad = 0;
    const uint32_t units = bytes / kUnit;
    for (uint32_t u = 0; u < units; ++u) {
        const uint8_t* unit = buffer + static_cast<size_t>(u) * kUnit;
        UnitHeader header{};
        std::memcpy(&header, unit, sizeof(header));
        const bool headerOk = header.magic == kMagic &&
                              header.offset == offset + static_cast<uint64_t>(u) * kUnit &&
                              header.poolIndex < kPoolBlocks && header.poolCheck == ~header.poolIndex;
        if (!headerOk ||
            std::memcmp(unit + kHeaderBytes,
                        pool_ + static_cast<size_t>(header.poolIndex) * kUnit + kHeaderBytes,
                        kUnit - kHeaderBytes) != 0) {
            ++bad;
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------
// I/O engine: `queueDepth` overlapped requests kept in flight on one thread
// ---------------------------------------------------------------------------

DiskTest::IoResult DiskTest::runIo(HANDLE file, HANDLE port, bool skipOnSuccess, const IoSpec& spec) {
    IoResult result;
    const int depth = std::clamp(spec.queueDepth, 1, 64);
    std::vector<Slot> slots(static_cast<size_t>(depth));
    for (Slot& slot : slots) {
        slot.buffer = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, spec.blockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (slot.buffer == nullptr) {
            ++result.ioErrors;
        }
    }

    const uint64_t blocksInFile = fileBytes_ / spec.blockBytes;
    uint64_t seed = 0xD15C7E57ULL ^ (static_cast<uint64_t>(spec.blockBytes) << 20) ^
                    static_cast<uint64_t>(spec.queueDepth) ^ GetTickCount64();
    uint64_t rng = splitMix64(seed) | 1ULL;
    uint64_t nextSequential = 0;
    int outstanding = 0;
    bool cancelled = false;
    const double started = nowSeconds();
    double lastReport = started;
    uint64_t lastReportBytes = 0;

    const auto shouldContinue = [&]() {
        if (stopRequested_ || result.ioErrors >= kMaxIoErrors || blocksInFile == 0) {
            return false;
        }
        if (spec.byteLimit > 0) {
            return nextSequential < spec.byteLimit;
        }
        return nowSeconds() - started < spec.seconds;
    };

    // 0 = failed, 1 = pending (a completion packet will arrive), 2 = done inline.
    const auto submit = [&](Slot& slot, DWORD* inlineBytes) {
        uint64_t offset = 0;
        if (spec.random) {
            offset = (xorShift64(rng) % blocksInFile) * spec.blockBytes;
        } else {
            offset = nextSequential % (blocksInFile * spec.blockBytes);
            nextSequential += spec.blockBytes;
        }
        slot.offset = offset;
        std::memset(&slot.ov, 0, sizeof(slot.ov));
        slot.ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        slot.ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        if (spec.write) {
            prepareWrite(slot.buffer, spec.blockBytes, offset, rng);
        }
        const BOOL ok = spec.write ? WriteFile(file, slot.buffer, spec.blockBytes, nullptr, &slot.ov)
                                   : ReadFile(file, slot.buffer, spec.blockBytes, nullptr, &slot.ov);
        if (!ok) {
            return GetLastError() == ERROR_IO_PENDING ? 1 : 0;
        }
        if (!skipOnSuccess) {
            return 1;  // completed already, but the packet is still queued to the port
        }
        *inlineBytes = static_cast<DWORD>(slot.ov.InternalHigh);
        return 2;
    };

    const auto complete = [&](Slot& slot, DWORD bytes) {
        if (bytes != spec.blockBytes) {
            ++result.ioErrors;
            return;
        }
        result.bytes += bytes;
        ++result.ops;
        if (spec.verify) {
            result.verifyErrors += verifyBuffer(slot.buffer, bytes, slot.offset);
        }
    };

    const auto reportProgress = [&]() {
        const double now = nowSeconds();
        if (now - lastReport < 0.2) {
            return;
        }
        const double live = static_cast<double>(result.bytes - lastReportBytes) /
                            static_cast<double>(kMiB) / std::max(0.001, now - lastReport);
        lastReport = now;
        lastReportBytes = result.bytes;
        history_.push(static_cast<float>(live));
        std::lock_guard lock(mutex_);
        stats_.liveMBs = live;
        stats_.elapsed = now - startedAt_;
        stats_.phaseProgress =
            spec.byteLimit > 0
                ? std::min(1.0, static_cast<double>(result.bytes) / static_cast<double>(spec.byteLimit))
                : std::min(1.0, (now - started) / std::max(0.1, spec.seconds));
    };

    // Keeps a slot busy until the phase is over or the request went asynchronous.
    const auto pump = [&](Slot& slot) {
        while (shouldContinue()) {
            DWORD bytes = 0;
            const int outcome = submit(slot, &bytes);
            if (outcome == 1) {
                ++outstanding;
                return;
            }
            if (outcome == 0) {
                ++result.ioErrors;
                return;
            }
            complete(slot, bytes);
            reportProgress();
        }
    };

    for (Slot& slot : slots) {
        if (slot.buffer != nullptr) {
            pump(slot);
        }
    }

    while (outstanding > 0) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &ov, 500);
        if (ov == nullptr) {
            // Timeout: nothing finished. On stop, cancel whatever is still queued.
            if (stopRequested_ && !cancelled) {
                CancelIoEx(file, nullptr);
                cancelled = true;
            }
            continue;
        }
        --outstanding;
        Slot& slot = *reinterpret_cast<Slot*>(ov);
        if (!ok) {
            if (GetLastError() != ERROR_OPERATION_ABORTED) {
                ++result.ioErrors;
            }
        } else {
            complete(slot, bytes);
        }
        reportProgress();
        pump(slot);
    }

    result.seconds = std::max(0.0001, nowSeconds() - started);
    for (Slot& slot : slots) {
        if (slot.buffer != nullptr) {
            VirtualFree(slot.buffer, 0, MEM_RELEASE);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Test sequence
// ---------------------------------------------------------------------------

void DiskTest::run(Options options) {
    startedAt_ = nowSeconds();
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE port = nullptr;

    const auto finish = [&](State state, const std::wstring& message) {
        if (port != nullptr) {
            CloseHandle(port);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);  // FILE_FLAG_DELETE_ON_CLOSE removes the test file
        }
        if (pool_ != nullptr) {
            VirtualFree(pool_, 0, MEM_RELEASE);
            pool_ = nullptr;
        }
        {
            std::lock_guard lock(mutex_);
            stats_.state = state;
            stats_.message = message;
            stats_.liveMBs = 0.0;
            stats_.elapsed = nowSeconds() - startedAt_;
        }
        logLine(L"[Disco] " + message);
        active_ = false;
    };

    DriveInfo drive = describeDrive(options.driveLetter);
    {
        std::lock_guard lock(mutex_);
        stats_.drive = drive;
    }

    // Test file size: big enough to defeat caches, small enough to be quick.
    uint64_t fileBytes = options.fileMegabytes > 0 ? options.fileMegabytes * kMiB
                                                   : (drive.hdd ? 512 * kMiB : 1024 * kMiB);
    if (drive.freeBytes > 0) {
        fileBytes = std::min(fileBytes, drive.freeBytes / 4);
    }
    fileBytes -= fileBytes % kMiB;
    if (fileBytes < 64 * kMiB) {
        finish(State::Unavailable, L"Espaço livre insuficiente em " + drive.letter + L" (" +
                                       formatBytes(drive.freeBytes) + L" livres).");
        return;
    }
    fileBytes_ = fileBytes;

    // Payload pool: random (incompressible) data, each unit tagged with its index.
    pool_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kPoolBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (pool_ == nullptr) {
        finish(State::Unavailable, L"Sem memória para o teste de disco.");
        return;
    }
    {
        uint64_t seed = 0x5EEDD15CULL ^ GetTickCount64();
        uint64_t rng = splitMix64(seed) | 1ULL;
        auto* words = reinterpret_cast<uint64_t*>(pool_);
        for (uint64_t i = 0; i < kPoolBytes / sizeof(uint64_t); ++i) {
            words[i] = xorShift64(rng);
        }
        for (uint32_t block = 0; block < kPoolBlocks; ++block) {
            UnitHeader header{kMagic, 0, block, ~block};
            std::memcpy(pool_ + static_cast<size_t>(block) * kUnit, &header, sizeof(header));
        }
    }

    const DWORD flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_NORMAL;
    std::vector<std::filesystem::path> candidates;
    if (!options.directory.empty()) {
        candidates.emplace_back(std::filesystem::path(options.directory) / L"WinAutoUpdater-disktest.tmp");
    }
    candidates.emplace_back(drive.letter + L"\\WinAutoUpdater-disktest.tmp");
    {
        wchar_t temp[MAX_PATH]{};
        const DWORD len = GetTempPathW(MAX_PATH, temp);
        if (len > 2 && len < MAX_PATH && _wcsnicmp(temp, drive.letter.c_str(), 2) == 0) {
            candidates.emplace_back(std::filesystem::path(temp) / L"WinAutoUpdater-disktest.tmp");
        }
    }
    DWORD openError = 0;
    for (const auto& path : candidates) {
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags,
                           nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        openError = GetLastError();
    }
    if (file == INVALID_HANDLE_VALUE) {
        finish(State::Unavailable, L"Não foi possível criar o ficheiro de teste em " + drive.letter +
                                       L" (erro " + std::to_wstring(openError) + L").");
        return;
    }

    // Reserve the space up front so a full disk is reported before any timing.
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(fileBytes);
    if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
        finish(State::Unavailable, L"Não foi possível reservar " + formatBytes(fileBytes) + L" em " +
                                       drive.letter + L" (erro " + std::to_wstring(GetLastError()) + L").");
        return;
    }

    port = CreateIoCompletionPort(file, nullptr, 0, 1);
    if (port == nullptr) {
        finish(State::Unavailable, L"Falha ao preparar a fila de E/S (erro " +
                                       std::to_wstring(GetLastError()) + L").");
        return;
    }
    // Requests that finish immediately are handled inline instead of via the port.
    const bool skipOnSuccess =
        SetFileCompletionNotificationModes(file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0;

    {
        std::lock_guard lock(mutex_);
        stats_.state = State::Running;
        stats_.message.clear();
        stats_.fileBytes = fileBytes;
    }
    logLine(L"[Disco] " + drive.letter + L" " + drive.model + L" (" + drive.kind + L"), ficheiro de " +
            formatBytes(fileBytes) + L".");

    struct Phase {
        const wchar_t* label;
        int row;  // -1 = no result row
        IoSpec spec;
    };
    const double seconds = std::clamp(options.secondsPerTest, 0.5, 30.0);
    const auto spec = [&](bool write, uint32_t block, int depth, bool random) {
        IoSpec s;
        s.write = write;
        s.blockBytes = block;
        s.queueDepth = depth;
        s.random = random;
        s.seconds = seconds;
        return s;
    };
    IoSpec fill = spec(true, 1024 * 1024, 1, false);
    fill.byteLimit = fileBytes;
    IoSpec verify = spec(false, 1024 * 1024, 4, false);
    verify.byteLimit = fileBytes;
    verify.verify = true;

    const std::vector<Phase> benchmark = {
        {L"A escrever o ficheiro de teste (SEQ1M Q1T1)", 1, fill},
        {L"Leitura SEQ1M Q8T1", 0, spec(false, 1024 * 1024, 8, false)},
        {L"Leitura SEQ1M Q1T1", 1, spec(false, 1024 * 1024, 1, false)},
        {L"Leitura RND4K Q32T1", 2, spec(false, kUnit, 32, true)},
        {L"Leitura RND4K Q1T1", 3, spec(false, kUnit, 1, true)},
        {L"Escrita SEQ1M Q8T1", 0, spec(true, 1024 * 1024, 8, false)},
        {L"Escrita RND4K Q32T1", 2, spec(true, kUnit, 32, true)},
        {L"Escrita RND4K Q1T1", 3, spec(true, kUnit, 1, true)},
        {L"A verificar os dados gravados", -1, verify},
    };
    // Endurance loop (bench mode): reads only, so it adds no wear to an SSD.
    const std::vector<Phase> endurance = {
        {L"Leitura SEQ1M Q8T1 (ciclo)", 0, spec(false, 1024 * 1024, 8, false)},
        {L"Leitura RND4K Q32T1 (ciclo)", 2, spec(false, kUnit, 32, true)},
        {L"A verificar os dados gravados (ciclo)", -1, verify},
    };

    uint64_t ioErrors = 0;
    uint64_t verifyErrors = 0;
    uint64_t cycles = 0;
    bool completed = false;

    const auto runPhases = [&](const std::vector<Phase>& phases) {
        for (size_t i = 0; i < phases.size() && !stopRequested_; ++i) {
            const Phase& phase = phases[i];
            {
                std::lock_guard lock(mutex_);
                stats_.phase = phase.label;
                stats_.phaseIndex = static_cast<int>(i) + 1;
                stats_.phaseCount = static_cast<int>(phases.size());
                stats_.phaseProgress = 0.0;
            }
            const IoResult r = runIo(file, port, skipOnSuccess, phase.spec);
            ioErrors += r.ioErrors;
            verifyErrors += r.verifyErrors;
            const bool whole = phase.spec.byteLimit == 0 || r.bytes >= phase.spec.byteLimit;
            if (phase.spec.verify && whole) {
                ++cycles;
            }
            std::lock_guard lock(mutex_);
            stats_.ioErrors = ioErrors;
            stats_.verifyErrors = verifyErrors;
            stats_.verifyCycles = cycles;
            // A phase cut short by "stop" has no meaningful rate.
            if (phase.row >= 0 && (whole || !stopRequested_) && r.ops > 0) {
                DiskRow& row = stats_.rows[phase.row];
                if (phase.spec.write) {
                    row.writeMBs = r.mbPerSec();
                    row.writeIops = r.iops();
                } else {
                    row.readMBs = r.mbPerSec();
                    row.readIops = r.iops();
                }
            }
            if (ioErrors >= kMaxIoErrors) {
                break;
            }
        }
    };

    runPhases(benchmark);
    completed = !stopRequested_ && ioErrors < kMaxIoErrors;
    while (options.loopReads && !stopRequested_ && ioErrors < kMaxIoErrors) {
        runPhases(endurance);
    }

    if (verifyErrors > 0 || ioErrors > 0) {
        finish(State::Failed, std::to_wstring(verifyErrors) + L" bloco(s) corrompido(s), " +
                                  std::to_wstring(ioErrors) + L" erro(s) de E/S em " + drive.letter + L".");
    } else if (completed) {
        finish(State::Passed, L"Sem erros — dados verificados " + std::to_wstring(cycles) + L" vez(es).");
    } else {
        finish(State::Stopped, L"Interrompido antes de terminar a medição.");
    }
}

}  // namespace nt
