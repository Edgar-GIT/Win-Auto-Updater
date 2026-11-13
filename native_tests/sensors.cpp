#include "sensors.hpp"

#include "gpu_test.hpp"  // which adapter the graphics test uses

#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>

namespace nt {

namespace {

uint64_t fileTimeValue(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool plausibleTemp(double celsius) {
    return celsius > 1.0 && celsius < 130.0;
}

// ---------------------------------------------------------------------------
// Performance counters (PDH)
// ---------------------------------------------------------------------------

class CounterQuery {
public:
    CounterQuery() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
        }
    }
    ~CounterQuery() {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
        }
    }
    CounterQuery(const CounterQuery&) = delete;
    CounterQuery& operator=(const CounterQuery&) = delete;

    // English counter path, so it resolves on a Portuguese Windows too.
    // nullptr when this Windows does not have the counter.
    PDH_HCOUNTER add(const wchar_t* path) {
        PDH_HCOUNTER counter = nullptr;
        if (query_ == nullptr || PdhAddEnglishCounterW(query_, path, 0, &counter) != ERROR_SUCCESS) {
            return nullptr;
        }
        return counter;
    }

    bool collect() { return query_ != nullptr && PdhCollectQueryData(query_) == ERROR_SUCCESS; }

    static bool value(PDH_HCOUNTER counter, double* out) {
        if (counter == nullptr) {
            return false;
        }
        PDH_FMT_COUNTERVALUE formatted{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &formatted) != ERROR_SUCCESS ||
            (formatted.CStatus != PDH_CSTATUS_VALID_DATA && formatted.CStatus != PDH_CSTATUS_NEW_DATA)) {
            return false;
        }
        *out = formatted.doubleValue;
        return true;
    }

    // Calls fn(instanceName, value) for every instance of a wildcard counter.
    template <typename Fn>
    void forEach(PDH_HCOUNTER counter, Fn fn) {
        if (counter == nullptr) {
            return;
        }
        DWORD bytes = 0;
        DWORD count = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0) {
            return;
        }
        buffer_.resize(bytes + 64);
        bytes = static_cast<DWORD>(buffer_.size());
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
        if (status != ERROR_SUCCESS) {
            return;
        }
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].szName == nullptr ||
                (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                 items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) {
                continue;
            }
            fn(items[i].szName, items[i].FmtValue.doubleValue);
        }
    }

private:
    PDH_HQUERY query_ = nullptr;
    std::vector<uint8_t> buffer_;
};

// ---------------------------------------------------------------------------
// GPU: graphics kernel (WDDM 2.4+, Windows 10 1803 and later)
// ---------------------------------------------------------------------------
// The structures of d3dkmthk.h that are needed here (the header is not part of
// the MinGW SDK). This is the source Task Manager uses for the GPU temperature.

struct KmtOpenAdapterFromLuid {
    LUID luid;
    UINT handle;
};

struct KmtCloseAdapter {
    UINT handle;
};

struct KmtQueryAdapterInfo {
    UINT handle;
    UINT type;
    void* data;
    UINT size;
};

struct KmtAdapterPerfData {
    UINT32 physicalAdapterIndex;
    ULONGLONG memoryFrequency;
    ULONGLONG maxMemoryFrequency;
    ULONGLONG maxMemoryFrequencyOC;
    ULONGLONG memoryBandwidth;
    ULONGLONG pcieBandwidth;
    ULONG fanRpm;
    ULONG power;        // tenths of a percent
    ULONG temperature;  // tenths of a degree Celsius
    UCHAR powerStateOverride;
};

constexpr UINT kKmtAdapterPerfData = 62;  // KMTQAITYPE_ADAPTERPERFDATA

class KernelGpu {
public:
    ~KernelGpu() { close(); }

    void open(const LUID& luid) {
        close();
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (gdi == nullptr) {
            return;
        }
        const auto openFn = reinterpret_cast<LONG(WINAPI*)(KmtOpenAdapterFromLuid*)>(
            reinterpret_cast<void (*)()>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")));
        query_ = reinterpret_cast<LONG(WINAPI*)(const KmtQueryAdapterInfo*)>(
            reinterpret_cast<void (*)()>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
        close_ = reinterpret_cast<LONG(WINAPI*)(const KmtCloseAdapter*)>(
            reinterpret_cast<void (*)()>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
        if (openFn == nullptr || query_ == nullptr || close_ == nullptr) {
            return;
        }
        KmtOpenAdapterFromLuid request{};
        request.luid = luid;
        if (openFn(&request) >= 0 && request.handle != 0) {
            handle_ = request.handle;
        }
    }

    void close() {
        if (handle_ != 0 && close_ != nullptr) {
            KmtCloseAdapter request{handle_};
            close_(&request);
        }
        handle_ = 0;
    }

    // Temperature in °C (or kNoReading), fan in rpm (or -1), power in % (or kNoReading).
    bool read(double* temperature, int* fanRpm, double* powerPct) {
        *temperature = kNoReading;
        *fanRpm = -1;
        *powerPct = kNoReading;
        if (handle_ == 0 || query_ == nullptr) {
            return false;
        }
        KmtAdapterPerfData data{};
        KmtQueryAdapterInfo request{};
        request.handle = handle_;
        request.type = kKmtAdapterPerfData;
        request.data = &data;
        request.size = sizeof(data);
        if (query_(&request) < 0) {
            return false;
        }
        const double celsius = static_cast<double>(data.temperature) / 10.0;
        if (plausibleTemp(celsius)) {
            *temperature = celsius;
        }
        if (data.fanRpm > 0 && data.fanRpm < 20000) {
            *fanRpm = static_cast<int>(data.fanRpm);
        }
        if (data.power > 0 && data.power <= 2000) {
            *powerPct = static_cast<double>(data.power) / 10.0;
        }
        return hasReading(*temperature);
    }

private:
    UINT handle_ = 0;
    LONG(WINAPI* query_)(const KmtQueryAdapterInfo*) = nullptr;
    LONG(WINAPI* close_)(const KmtCloseAdapter*) = nullptr;
};

// ---------------------------------------------------------------------------
// GPU: NVIDIA driver library (nvapi64.dll ships with every NVIDIA driver)
// ---------------------------------------------------------------------------

struct NvSensor {
    int controller;
    unsigned defaultMin;
    unsigned defaultMax;
    unsigned current;
    int target;
};

struct NvThermalSettings {
    unsigned version;
    unsigned count;
    NvSensor sensor[3];
};

class NvidiaGpu {
public:
    bool read(double* temperature) {
        if (!tried_) {
            tried_ = true;
            init();
        }
        if (thermal_ == nullptr || gpu_ == nullptr) {
            return false;
        }
        NvThermalSettings settings{};
        settings.version = static_cast<unsigned>(sizeof(NvThermalSettings)) | (1U << 16);
        settings.count = 3;
        constexpr int kAllTargets = 15;
        if (thermal_(gpu_, kAllTargets, &settings) != 0) {
            return false;
        }
        double best = kNoReading;
        for (unsigned i = 0; i < std::min(settings.count, 3U); ++i) {
            const double celsius = static_cast<double>(settings.sensor[i].current);
            constexpr int kTargetGpu = 1;
            if (plausibleTemp(celsius) && (settings.sensor[i].target == kTargetGpu || !hasReading(best))) {
                best = celsius;
            }
        }
        if (!hasReading(best)) {
            return false;
        }
        *temperature = best;
        return true;
    }

private:
    using QueryFn = void* (*)(unsigned);
    using InitFn = int (*)();
    using EnumFn = int (*)(void**, unsigned*);
    using ThermalFn = int (*)(void*, int, NvThermalSettings*);

    void init() {
        HMODULE module = LoadLibraryW(L"nvapi64.dll");
        if (module == nullptr) {
            return;
        }
        const auto query = reinterpret_cast<QueryFn>(
            reinterpret_cast<void (*)()>(GetProcAddress(module, "nvapi_QueryInterface")));
        if (query == nullptr) {
            return;
        }
        const auto initialize = reinterpret_cast<InitFn>(query(0x0150E828));
        const auto enumerate = reinterpret_cast<EnumFn>(query(0xE5AC921F));
        const auto thermal = reinterpret_cast<ThermalFn>(query(0xE3640A56));
        if (initialize == nullptr || enumerate == nullptr || thermal == nullptr || initialize() != 0) {
            return;
        }
        void* handles[64]{};
        unsigned count = 0;
        if (enumerate(handles, &count) != 0 || count == 0) {
            return;
        }
        gpu_ = handles[0];
        thermal_ = thermal;
    }

    bool tried_ = false;
    void* gpu_ = nullptr;
    ThermalFn thermal_ = nullptr;
};

// ---------------------------------------------------------------------------
// GPU: AMD driver library (atiadlxx.dll ships with every Radeon driver)
// ---------------------------------------------------------------------------

struct AdlTemperature {
    int size;
    int temperature;  // millidegrees
};

struct AdlSensor {
    int supported;
    int value;
};

struct AdlPmLogData {
    int size;
    AdlSensor sensors[256];
};

void* __stdcall adlAlloc(int size) {
    return std::malloc(static_cast<size_t>(std::max(0, size)));
}

class AmdGpu {
public:
    bool read(double* temperature) {
        if (!tried_) {
            tried_ = true;
            init();
        }
        if (context_ == nullptr || adapters_ <= 0) {
            return false;
        }
        // The first adapter index / interface that answers is kept.
        if (method_ != 0) {
            return query(method_, adapter_, temperature);
        }
        for (int adapter = 0; adapter < adapters_; ++adapter) {
            for (int method = 1; method <= 4; ++method) {
                if (query(method, adapter, temperature)) {
                    method_ = method;
                    adapter_ = adapter;
                    return true;
                }
            }
        }
        // Nothing answered: do not keep hammering the driver every second.
        adapters_ = 0;
        return false;
    }

private:
    using AllocFn = void* (__stdcall*)(int);
    using CreateFn = int (*)(AllocFn, int, void**);
    using CountFn = int (*)(void*, int*);
    using PmLogFn = int (*)(void*, int, AdlPmLogData*);
    using OdnFn = int (*)(void*, int, int, int*);
    using Od6Fn = int (*)(void*, int, int*);
    using Od5Fn = int (*)(void*, int, int, AdlTemperature*);

    template <typename Fn>
    static Fn symbol(HMODULE module, const char* name) {
        return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
    }

    void init() {
        HMODULE module = LoadLibraryW(L"atiadlxx.dll");
        if (module == nullptr) {
            return;
        }
        const auto create = symbol<CreateFn>(module, "ADL2_Main_Control_Create");
        const auto count = symbol<CountFn>(module, "ADL2_Adapter_NumberOfAdapters_Get");
        if (create == nullptr || count == nullptr) {
            return;
        }
        void* context = nullptr;
        if (create(adlAlloc, 1, &context) != 0 || context == nullptr) {
            return;
        }
        int adapters = 0;
        if (count(context, &adapters) != 0 || adapters <= 0) {
            return;
        }
        pmLog_ = symbol<PmLogFn>(module, "ADL2_New_QueryPMLogData_Get");
        odn_ = symbol<OdnFn>(module, "ADL2_OverdriveN_Temperature_Get");
        od6_ = symbol<Od6Fn>(module, "ADL2_Overdrive6_Temperature_Get");
        od5_ = symbol<Od5Fn>(module, "ADL2_Overdrive5_Temperature_Get");
        context_ = context;
        adapters_ = std::min(adapters, 32);
    }

    bool query(int method, int adapter, double* temperature) {
        double celsius = kNoReading;
        switch (method) {
            case 1:  // power-management log (Radeon RX 5000 and later)
                if (pmLog_ != nullptr) {
                    AdlPmLogData data{};
                    data.size = static_cast<int>(sizeof(data));
                    constexpr int kTemperatureEdge = 8;
                    if (pmLog_(context_, adapter, &data) == 0 && data.sensors[kTemperatureEdge].supported != 0) {
                        celsius = static_cast<double>(data.sensors[kTemperatureEdge].value);
                    }
                }
                break;
            case 2:  // Overdrive N
                if (odn_ != nullptr) {
                    int milli = 0;
                    constexpr int kEdge = 1;
                    if (odn_(context_, adapter, kEdge, &milli) == 0) {
                        celsius = static_cast<double>(milli) / 1000.0;
                    }
                }
                break;
            case 3:  // Overdrive 6
                if (od6_ != nullptr) {
                    int milli = 0;
                    if (od6_(context_, adapter, &milli) == 0) {
                        celsius = static_cast<double>(milli) / 1000.0;
                    }
                }
                break;
            case 4:  // Overdrive 5 (older cards)
                if (od5_ != nullptr) {
                    AdlTemperature data{};
                    data.size = static_cast<int>(sizeof(data));
                    if (od5_(context_, adapter, 0, &data) == 0) {
                        celsius = static_cast<double>(data.temperature) / 1000.0;
                    }
                }
                break;
            default:
                break;
        }
        if (!plausibleTemp(celsius)) {
            return false;
        }
        *temperature = celsius;
        return true;
    }

    bool tried_ = false;
    void* context_ = nullptr;
    int adapters_ = 0;
    int method_ = 0;
    int adapter_ = 0;
    PmLogFn pmLog_ = nullptr;
    OdnFn odn_ = nullptr;
    Od6Fn od6_ = nullptr;
    Od5Fn od5_ = nullptr;
};

// ---------------------------------------------------------------------------
// System drive temperature
// ---------------------------------------------------------------------------

double readSystemDiskTemperature() {
    wchar_t windows[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows, MAX_PATH) < 2 || windows[1] != L':') {
        return kNoReading;
    }
    const std::wstring volumePath = std::wstring(L"\\\\.\\") + windows[0] + L":";
    HANDLE volume = CreateFileW(volumePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        return kNoReading;
    }
    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0;
    const bool haveNumber = DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                                            sizeof(number), &returned, nullptr) != 0;
    CloseHandle(volume);
    if (!haveNumber) {
        return kNoReading;
    }

    const std::wstring diskPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(number.DeviceNumber);
    HANDLE disk = CreateFileW(diskPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (disk == INVALID_HANDLE_VALUE) {
        return kNoReading;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceTemperatureProperty;
    query.QueryType = PropertyStandardQuery;
    uint8_t buffer[512]{};
    double result = kNoReading;
    if (DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer),
                        &returned, nullptr) &&
        returned >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)) {
        const auto* descriptor = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buffer);
        const size_t available =
            (returned - offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo)) /
            sizeof(STORAGE_TEMPERATURE_INFO);
        const size_t count = std::min<size_t>(descriptor->InfoCount, available);
        for (size_t i = 0; i < count; ++i) {
            const double celsius = static_cast<double>(descriptor->TemperatureInfo[i].Temperature);
            if (plausibleTemp(celsius)) {
                result = std::max(result, celsius);
            }
        }
    }
    CloseHandle(disk);
    return result;
}

int readBaseMhz() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, L"~MHz", nullptr, &type, reinterpret_cast<BYTE*>(&mhz), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD ? static_cast<int>(mhz) : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------

Sensors::~Sensors() {
    stop();
}

void Sensors::start() {
    if (thread_.joinable()) {
        return;
    }
    stopRequested_ = false;
    thread_ = std::thread([this] { run(); });
}

void Sensors::stop() {
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

SensorSnapshot Sensors::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void Sensors::resetPeaks() {
    resetPeaks_ = true;
}

void Sensors::run() {
    SensorSnapshot current;
    current.cpuBaseMhz = readBaseMhz();

    GpuAdapter adapter;
    const bool haveAdapter = GpuTest::queryAdapter(&adapter);
    if (haveAdapter) {
        current.gpuName = adapter.name;
    }
    // Integrated graphics have next to no dedicated memory: what they use is shared.
    const bool sharedMemory = haveAdapter && adapter.vramBytes < (768ULL << 20);
    current.vramTotal = haveAdapter ? (sharedMemory ? adapter.sharedBytes : adapter.vramBytes) : 0;

    CounterQuery counters;
    PDH_HCOUNTER zonePrecise = counters.add(L"\\Thermal Zone Information(*)\\High Precision Temperature");
    PDH_HCOUNTER zone = counters.add(L"\\Thermal Zone Information(*)\\Temperature");
    PDH_HCOUNTER performance = counters.add(L"\\Processor Information(_Total)\\% Processor Performance");
    PDH_HCOUNTER gpuEngine = counters.add(L"\\GPU Engine(*)\\Utilization Percentage");
    PDH_HCOUNTER gpuMemory = counters.add(sharedMemory ? L"\\GPU Adapter Memory(*)\\Shared Usage"
                                                       : L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    counters.collect();

    wchar_t luidText[48]{};
    if (haveAdapter) {
        swprintf(luidText, 48, L"luid_0x%08X_0x%08X", static_cast<unsigned>(adapter.luid.HighPart),
                 static_cast<unsigned>(adapter.luid.LowPart));
    }

    KernelGpu kernelGpu;
    if (haveAdapter && !adapter.software) {
        kernelGpu.open(adapter.luid);
    }
    NvidiaGpu nvidia;
    AmdGpu amd;

    FILETIME idle{}, kernel{}, user{};
    GetSystemTimes(&idle, &kernel, &user);
    uint64_t lastIdle = fileTimeValue(idle);
    uint64_t lastTotal = fileTimeValue(kernel) + fileTimeValue(user);

    int tick = 0;
    while (!stopRequested_) {
        // 1 s cadence, but react to stop() quickly.
        for (int i = 0; i < 10 && !stopRequested_; ++i) {
            Sleep(tick == 0 ? 20 : 100);
        }
        if (stopRequested_) {
            break;
        }
        if (resetPeaks_.exchange(false)) {
            current.cpuTempMaxC = current.gpuTempMaxC = current.diskTempMaxC = kNoReading;
        }

        // ---- CPU ----
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const uint64_t idleNow = fileTimeValue(idle);
            const uint64_t totalNow = fileTimeValue(kernel) + fileTimeValue(user);
            const uint64_t total = totalNow - lastTotal;
            const uint64_t idled = idleNow - lastIdle;
            if (total > 0) {
                current.cpuLoadPct =
                    std::clamp(100.0 * (1.0 - static_cast<double>(idled) / static_cast<double>(total)), 0.0,
                               100.0);
            }
            lastIdle = idleNow;
            lastTotal = totalNow;
        }

        counters.collect();

        double perfPct = 0.0;
        if (current.cpuBaseMhz > 0 && CounterQuery::value(performance, &perfPct) && perfPct > 1.0) {
            current.cpuMhz = static_cast<int>(current.cpuBaseMhz * perfPct / 100.0);
        } else {
            current.cpuMhz = 0;
        }

        // Hottest ACPI thermal zone (kelvin → °C).
        double zoneTemp = kNoReading;
        counters.forEach(zonePrecise, [&](const wchar_t*, double tenthsKelvin) {
            const double celsius = tenthsKelvin / 10.0 - 273.15;
            if (plausibleTemp(celsius)) {
                zoneTemp = std::max(zoneTemp, celsius);
            }
        });
        if (!hasReading(zoneTemp)) {
            counters.forEach(zone, [&](const wchar_t*, double kelvin) {
                const double celsius = kelvin - 273.15;
                if (plausibleTemp(celsius)) {
                    zoneTemp = std::max(zoneTemp, celsius);
                }
            });
        }
        current.cpuTempC = zoneTemp;

        // ---- GPU ----
        double gpuTemp = kNoReading;
        double gpuPower = kNoReading;
        int fan = -1;
        current.gpuTempSource = L"";
        if (kernelGpu.read(&gpuTemp, &fan, &gpuPower)) {
            current.gpuTempSource = L"WDDM";
        } else if (haveAdapter && adapter.vendorId == 0x10DE && nvidia.read(&gpuTemp)) {
            current.gpuTempSource = L"NVAPI";
        } else if (haveAdapter && adapter.vendorId == 0x1002 && amd.read(&gpuTemp)) {
            current.gpuTempSource = L"ADL";
        }
        current.gpuTempC = gpuTemp;
        current.gpuFanRpm = fan;
        current.gpuPowerPct = gpuPower;

        // Busiest engine of the adapter, summed over every process using it.
        if (gpuEngine != nullptr) {
            std::map<std::wstring, double> engines;
            bool matchedAdapter = false;
            for (int pass = 0; pass < 2 && engines.empty(); ++pass) {
                counters.forEach(gpuEngine, [&](const wchar_t* name, double value) {
                    const wchar_t* luid = std::wcsstr(name, L"luid_");
                    if (luid == nullptr) {
                        return;
                    }
                    if (pass == 0 && (luidText[0] == L'\0' || _wcsnicmp(luid, luidText, std::wcslen(luidText)) != 0)) {
                        return;
                    }
                    const wchar_t* type = std::wcsstr(luid, L"_engtype");
                    const std::wstring key = type != nullptr ? std::wstring(luid, type) : std::wstring(luid);
                    engines[key] += value;
                    matchedAdapter = true;
                });
            }
            double busiest = kNoReading;
            for (const auto& [key, value] : engines) {
                busiest = std::max(busiest, value);
            }
            current.gpuLoadPct = matchedAdapter ? std::clamp(busiest, 0.0, 100.0) : kNoReading;
        }
        if (gpuMemory != nullptr) {
            double used = 0.0;
            double any = 0.0;
            counters.forEach(gpuMemory, [&](const wchar_t* name, double value) {
                any = std::max(any, value);
                if (luidText[0] != L'\0' && _wcsnicmp(name, luidText, std::wcslen(luidText)) == 0) {
                    used = std::max(used, value);
                }
            });
            current.vramUsed = static_cast<uint64_t>(used > 0.0 ? used : any);
        }

        // ---- Memory ----
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            current.ramTotal = memory.ullTotalPhys;
            current.ramUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        }

        // ---- Slower readings ----
        if (tick % 5 == 0) {
            current.diskTempC = readSystemDiskTemperature();
        }
        if (tick % 2 == 0) {
            current.battery = readBattery();
        }

        const auto peak = [](double value, double* maximum) {
            if (hasReading(value)) {
                *maximum = std::max(*maximum, value);
            }
        };
        peak(current.cpuTempC, &current.cpuTempMaxC);
        peak(current.gpuTempC, &current.gpuTempMaxC);
        peak(current.diskTempC, &current.diskTempMaxC);

        cpuLoad_.push(static_cast<float>(current.cpuLoadPct));
        if (hasReading(current.cpuTempC)) {
            cpuTemp_.push(static_cast<float>(current.cpuTempC));
        }
        if (hasReading(current.gpuTempC)) {
            gpuTemp_.push(static_cast<float>(current.gpuTempC));
        }
        {
            std::lock_guard lock(mutex_);
            snapshot_ = current;
        }
        ++tick;
    }
}

}  // namespace nt
