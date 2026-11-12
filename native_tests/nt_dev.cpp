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

    std::ofstream file(reportFile, std::ios::binary);
    file << out.str();
    return 0;
}

int runGpuSnapshot(const std::filesystem::path& directory) {
    ui::startup();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::ostringstream out;

    std::wstring name;
    uint64_t vram = 0;
    bool software = false;
    if (GpuTest::queryAdapter(&name, &vram, &software)) {
        out << "adapter=" << narrow(name) << " vram=" << vram << " software=" << software << "\n";
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
