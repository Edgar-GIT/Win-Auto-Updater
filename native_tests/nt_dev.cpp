// Developer-only helpers (compiled with -DSU_UI_SNAPSHOT=ON, never shipped):
//   SingleUpdate.exe --native-selftest <report.txt>
// runs the RAM / CPU / disk tests for a few seconds each — once clean and once
// with a fault injected on purpose — and writes what they reported.

#ifdef SU_UI_SNAPSHOT

#include "native_tests.hpp"

#include "cpu_burner.hpp"
#include "disk_test.hpp"
#include "gpu_test.hpp"
#include "ram_test.hpp"
#include "sensors.hpp"

#include "ui.hpp"

#include <fstream>
#include <sstream>

namespace nt {

namespace {

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, size)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

const char* stateName(State s) {
    switch (s) {
        case State::Idle:
            return "Idle";
        case State::Starting:
            return "Starting";
        case State::Running:
            return "Running";
        case State::Passed:
            return "Passed";
        case State::Failed:
            return "Failed";
        case State::Stopped:
            return "Stopped";
        case State::Unavailable:
            return "Unavailable";
    }
    return "?";
}

}  // namespace

int runNativeSelfTest(const std::filesystem::path& reportFile) {
    std::ostringstream out;

    // ---- RAM ---------------------------------------------------------------
    for (int withFault = 0; withFault < 2; ++withFault) {
        RamTest ram;
        ram.start(256);
        const double begin = nowSeconds();
        while (nowSeconds() - begin < 4.0) {
            Sleep(100);
            if (withFault && nowSeconds() - begin > 1.0) {
                ram.injectFault();
            }
        }
        ram.stop();
        const RamStats s = ram.stats();
        out << "RAM" << (withFault ? " (fault injected)" : "") << ": state=" << stateName(s.state)
            << " bytes=" << s.bytesUnderTest << " threads=" << s.threads << " locked=" << s.locked
            << " coverage=" << s.coveragePct << "% passes=" << s.passes << " errors=" << s.errors
            << " msg=" << narrow(s.message) << "\n";
        for (const RamError& e : s.firstErrors) {
            out << "    offset=" << e.offset << " expected=" << std::hex << e.expected << " actual="
                << e.actual << std::dec << " pattern=" << narrow(RamTest::patternName(e.pattern))
                << "\n";
            if (&e - s.firstErrors.data() >= 2) {
                break;
            }
        }
    }

    // ---- CPU ---------------------------------------------------------------
    for (int withFault = 0; withFault < 2; ++withFault) {
        CpuBurner cpu;
        cpu.start();
        Sleep(1500);
        if (withFault) {
            cpu.injectFault();
        }
        Sleep(2500);
        const CpuStats live = cpu.stats();
        cpu.stop();
        const CpuStats s = cpu.stats();
        out << "CPU" << (withFault ? " (fault injected)" : "") << ": state=" << stateName(s.state)
            << " name=" << narrow(s.cpuName) << " threads=" << s.threads << " avx2=" << s.avx2
            << " iterations=" << s.iterations << " errors=" << s.errors
            << " rate=" << live.itersPerSec << "/s load=" << live.loadPct << "% mhz="
            << live.currentMhz << "/" << live.maxMhz << " msg=" << narrow(s.message) << "\n";
    }

    // ---- Disk --------------------------------------------------------------
    {
        for (const DriveInfo& d : DiskTest::listDrives()) {
            out << "Drive " << narrow(d.letter) << " model=" << narrow(d.model) << " kind="
                << narrow(d.kind) << " total=" << d.totalBytes << " free=" << d.freeBytes << "\n";
        }
        DiskTest disk;
        DiskTest::Options options;
        options.secondsPerTest = 1.0;
        options.fileMegabytes = 96;
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        options.directory = temp;
        disk.start(options);
        while (disk.active()) {
            Sleep(100);
        }
        const DiskStats s = disk.stats();
        out << "Disk: state=" << stateName(s.state) << " file=" << s.fileBytes << " verifyErrors="
            << s.verifyErrors << " ioErrors=" << s.ioErrors << " cycles=" << s.verifyCycles
            << " msg=" << narrow(s.message) << "\n";
        for (const DiskRow& row : s.rows) {
            out << "    " << narrow(row.name) << " read=" << row.readMBs << " MB/s (" << row.readIops
                << " iops) write=" << row.writeMBs << " MB/s (" << row.writeIops << " iops)\n";
        }
    }

    // ---- Sensors + battery ----------------------------------------------------
    {
        Sensors sensors;
        sensors.start();
        Sleep(3500);
        const SensorSnapshot s = sensors.snapshot();
        sensors.stop();
        out << "Sensors: cpuLoad=" << s.cpuLoadPct << "% cpuMhz=" << s.cpuMhz << "/" << s.cpuBaseMhz
            << " cpuTemp=" << s.cpuTempC << " gpu=" << narrow(s.gpuName) << " gpuTemp=" << s.gpuTempC << " ("
            << narrow(s.gpuTempSource) << ") gpuLoad=" << s.gpuLoadPct << " fan=" << s.gpuFanRpm << " vram="
            << s.vramUsed << "/" << s.vramTotal << " ram=" << s.ramUsed << "/" << s.ramTotal << " diskTemp="
            << s.diskTempC << "\n";
        const BatteryInfo b = readBattery();
        out << "Battery: present=" << b.present << " count=" << b.count << " percent=" << b.percent << " onAc="
            << b.onAc << " health=" << b.healthPct << " grade=" << narrow(batteryGradeText(gradeBattery(b)))
            << " status=" << narrow(batteryStatusText(b)) << "\n";
    }

    // ---- Test clock ---------------------------------------------------------------
    {
        TestClock& clock = TestClock::instance();
        const double a = clock.now();
        Sleep(500);
        const double realPart = clock.now() - a;  // real time: ~0.5
        clock.setGpuPaced(true);
        const double b = clock.now();
        Sleep(500);  // no frames: the clock must stand still
        const double stalled = clock.now() - b;
        for (int i = 0; i < 30; ++i) {
            clock.frame(0.2);  // 5 FPS: each frame only counts 1/10 s
        }
        const double slow = clock.now() - b;  // 30 / 10 = 3.0 instead of 6.0
        for (int i = 0; i < 60; ++i) {
            clock.frame(0.01);  // 100 FPS: real time, 0.6
        }
        const double fast = clock.now() - b - slow;
        clock.setGpuPaced(false);
        out << "TestClock: real=" << realPart << " stalled=" << stalled << " slow30frames=" << slow
            << " fast60frames=" << fast << " testSeconds=" << testSeconds() << " max=" << maxRealSeconds()
            << "\n";
    }

    std::ofstream file(reportFile, std::ios::binary);
    file << out.str();
    return 0;
}

int runGpuSnapshot(const std::filesystem::path& directory) {
    ui::startup();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::ostringstream out;

    GpuAdapter adapter;
    if (GpuTest::queryAdapter(&adapter)) {
        out << "adapter=" << narrow(adapter.name) << " vram=" << adapter.vramBytes << " shared="
            << adapter.sharedBytes << " software=" << adapter.software << " vendor=" << std::hex
            << adapter.vendorId << std::dec << "\n";
    } else {
        out << "queryAdapter failed\n";
    }

    struct Shot {
        const wchar_t* file;
        float time;
        float quality;
        int width;
        int height;
    };
    const Shot shots[] = {
        {L"scene_t004.png", 4.0f, 0.75f, 960, 540},
        {L"scene_t021.png", 21.0f, 0.75f, 960, 540},
        {L"scene_t047.png", 47.0f, 0.75f, 960, 540},
        {L"scene_t075_flash.png", 82.75f, 0.75f, 960, 540},
        {L"scene_lowq.png", 12.0f, 0.0f, 640, 360},
    };
    for (const Shot& shot : shots) {
        std::wstring error;
        const double begin = nowSeconds();
        const bool ok = GpuTest::renderSnapshot(directory / shot.file, shot.width, shot.height, shot.time,
                                                shot.quality, &error);
        out << narrow(shot.file) << ": " << (ok ? "ok" : "FAILED") << " in " << (nowSeconds() - begin)
            << " s " << narrow(error) << "\n";
    }
    std::ofstream file(directory / L"gpu_snapshot.txt", std::ios::binary);
    file << out.str();
    ui::shutdown();
    return 0;
}

}  // namespace nt

#endif  // SU_UI_SNAPSHOT
