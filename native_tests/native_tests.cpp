// Start-up of the native tests: the monitor bar plus either the automatic
// dashboard or the five separate test windows.

#include "native_tests.hpp"

#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace nt {

namespace {

constexpr const wchar_t* kInstanceMutex = L"WinAutoUpdaterNativeTestsInstance";
constexpr TestId kTools[kTestCount] = {TestId::Ram, TestId::Cpu, TestId::Disk, TestId::Gpu, TestId::Battery};

// A second launch brings the windows of the first one to the front.
void focusRunningInstance() {
    EnumWindows(
        [](HWND window, LPARAM) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(window, name, 64);
            if (std::wstring(name) == L"WinAutoUpdaterNativePanel" &&
                (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_APPWINDOW) != 0) {
                if (IsIconic(window)) {
                    ShowWindow(window, SW_RESTORE);
                }
                SetForegroundWindow(window);
            }
            return TRUE;
        },
        0);
}

// Lays the test windows out in a 3 × 2 grid in the area beside the monitor bar.
void tileTools(std::vector<std::unique_ptr<ToolWindow>>& tools, const RECT& area) {
    if (tools.empty()) {
        return;
    }
    const SIZE design = ToolWindow::design();
    const int gap = 8;
    const int cols = 3;
    const int rows = 2;
    const int areaW = area.right - area.left;
    const int areaH = area.bottom - area.top;
    const SIZE frame = tools.front()->outerSize(0, 0);
    float scale = static_cast<float>(ui::dpiForWindow(tools.front()->hwnd())) / 96.0f * 1.15f;
    scale = std::min(scale, static_cast<float>((areaW - gap * (cols + 1)) / cols - frame.cx) / design.cx);
    scale = std::min(scale, static_cast<float>((areaH - gap * (rows + 1)) / rows - frame.cy) / design.cy);
    scale = std::max(scale, 0.5f);
    const int clientW = static_cast<int>(std::lround(design.cx * scale));
    const int clientH = static_cast<int>(std::lround(design.cy * scale));
    const SIZE outer = tools.front()->outerSize(clientW, clientH);
    const int totalW = cols * outer.cx + (cols - 1) * gap;
    const int totalH = rows * outer.cy + (rows - 1) * gap;
    const int x0 = area.left + std::max(gap, (areaW - totalW) / 2);
    const int y0 = area.top + std::max(gap, (areaH - totalH) / 2);
    for (size_t i = 0; i < tools.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        tools[i]->placeClient(x0 + col * (outer.cx + gap), y0 + row * (outer.cy + gap), clientW, clientH, true);
    }
}

}  // namespace

int runNativeTests(HINSTANCE instance, const LaunchOptions& options) {
    // One bench at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        focusRunningInstance();
        CloseHandle(mutex);
        return 0;
    }

    ui::startup();
    setTestSeconds(options.testSeconds);
    logLine(L"");
    logLine(std::wstring(L"========== Testes nativos abertos (") + (options.automatic ? L"automático" : L"manual") +
            L") ==========");

    int exitCode = 0;
    {
        Bench::Config config;
        config.automatic = options.automatic;
        config.keepDisplayMode = options.keepDisplayMode;
        Bench bench(config);

        MonitorBar bar(bench);
        bar.onCloseAll = [] { PostQuitMessage(0); };
        bar.open(instance);
        RECT area = bar.workArea();
        area.left += bar.width();

        std::unique_ptr<DashWindow> dash;
        std::vector<std::unique_ptr<ToolWindow>> tools;
        int openWindows = 0;
        bool ready = true;

        if (options.automatic) {
            dash = std::make_unique<DashWindow>(bench);
            dash->onClosed = [] { PostQuitMessage(0); };
            ready = dash->open(instance, area);
            if (ready) {
                bench.startSession(dash->hwnd());
            }
        } else {
            for (const TestId id : kTools) {
                auto tool = std::make_unique<ToolWindow>(bench, id);
                tool->onClosed = [&openWindows] {
                    if (--openWindows <= 0) {
                        PostQuitMessage(0);
                    }
                };
                if (tool->open(instance)) {
                    ++openWindows;
                    tools.push_back(std::move(tool));
                }
            }
            ready = !tools.empty();
            tileTools(tools, area);
            if (ready) {
                SetForegroundWindow(tools.front()->hwnd());
            }
        }

        if (!ready) {
            MessageBoxW(nullptr, L"Não foi possível criar as janelas dos testes nativos.", kBenchTitle,
                        MB_OK | MB_ICONERROR);
            exitCode = 1;
        } else {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
        // Leaving this scope stops every test, closes the windows and the sensors.
    }

    logLine(L"========== Testes nativos fechados ==========");
    ui::shutdown();
    return exitCode;
}

#ifdef SU_UI_SNAPSHOT
// Developer aid: renders every native-test window into PNG files with made-up
// (but plausible) readings, so the layouts can be reviewed without a test PC.
int runNativeSnapshot(HINSTANCE, const std::filesystem::path& directory) {
    ui::startup();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    {
        Bench bench(Bench::Config{});
        MonitorBar bar(bench);
        bar.addWidgets();
        DashWindow dash(bench);
        dash.addWidgets();
        std::vector<std::unique_ptr<ToolWindow>> tools;
        for (const TestId id : kTools) {
            tools.push_back(std::make_unique<ToolWindow>(bench, id));
            tools.back()->addWidgets();
        }
        static const wchar_t* kToolNames[kTestCount] = {L"ram", L"cpu", L"disk", L"gpu", L"battery"};
        const auto shootAll = [&](const std::wstring& tag) {
            dash.snapshot(directory / (L"dash_" + tag + L"_1040x584.png"), 1040, 584);
            bar.snapshot(directory / (L"bar_" + tag + L"_250x1040.png"), 250, 1040);
            for (size_t i = 0; i < tools.size(); ++i) {
                tools[i]->snapshot(directory / (L"tool_" + std::wstring(kToolNames[i]) + L"_" + tag + L".png"), 344,
                                   272);
            }
        };

        // ---- idle, with this machine's real facts ----
        Sleep(1500);  // one sensor reading
        bench.tick();
        shootAll(L"idle");

        // ---- a session in progress ----
        const auto wave = [](float base, float amp, float freq, int count = 120) {
            std::vector<float> v;
            for (int i = 0; i < count; ++i) {
                v.push_back(base + amp * std::sin(static_cast<float>(i) * freq) +
                            amp * 0.3f * std::sin(static_cast<float>(i) * freq * 3.7f));
            }
            return v;
        };
        const auto ramp = [](float from, float to, int count = 120) {
            std::vector<float> v;
            for (int i = 0; i < count; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                v.push_back(from + (to - from) * (1.0f - std::exp(-4.0f * t)) + 0.6f * std::sin(i * 0.9f));
            }
            return v;
        };
        bench.pretendActive = true;
        bench.frame = 3;
        bench.clockRate = 0.62;
        bench.cpuName = L"Intel(R) Core(TM) i5-6500 CPU @ 3.20GHz";
        bench.totalRam = 16ULL << 30;
        bench.freeRam = 11ULL << 30;
        DriveInfo drive;
        drive.letter = L"C:";
        drive.model = L"Samsung SSD 860 EVO 250GB";
        drive.kind = L"SSD SATA";
        drive.totalBytes = 250ULL * 1000 * 1000 * 1000;
        drive.freeBytes = 131ULL << 30;
        bench.drives = {drive, drive};
        bench.haveAdapter = true;
        bench.adapter.name = L"NVIDIA GeForce GTX 1050 Ti";
        bench.adapter.vramBytes = 4ULL << 30;
        bench.adapter.software = false;

        bench.session.running = true;
        bench.session.preparing = false;
        bench.session.testElapsed = 133.0;
        bench.session.realElapsed = 214.0;

        bench.ram.state = State::Running;
        bench.ram.bytesUnderTest = 9ULL << 30;
        bench.ram.coveragePct = 412.0;
        bench.ram.passes = 4;
        bench.ram.threads = 4;
        bench.ram.locked = true;
        bench.ram.mbPerSec = 8650.0;
        bench.ram.bytesVerified = 37ULL << 30;
        bench.ram.pattern = L"aleatório invertido";
        bench.ramHist = wave(8600.0f, 500.0f, 0.21f);

        bench.cpu.state = State::Running;
        bench.cpu.cpuName = bench.cpuName;
        bench.cpu.threads = 4;
        bench.cpu.avx2 = true;
        bench.cpu.loadPct = 100.0;
        bench.cpu.itersPerSec = 1284.0;
        bench.cpu.peakItersPerSec = 1420.0;
        bench.cpu.pctOfPeak = 90.0;
        bench.cpu.iterations = 168000;
        bench.cpuHist = wave(1300.0f, 90.0f, 0.13f);
        bench.cpuLoadHist = wave(98.0f, 2.0f, 0.3f);

        bench.disk.state = State::Running;
        bench.disk.drive = drive;
        bench.disk.fileBytes = 1ULL << 30;
        bench.disk.rows[0] = {L"SEQ1M Q8T1", 548.2, 512.7, 0, 0};
        bench.disk.rows[1] = {L"SEQ1M Q1T1", 531.9, 498.1, 0, 0};
        bench.disk.rows[2] = {L"RND4K Q32T1", 352.4, 311.0, 0, 0};
        bench.disk.rows[3] = {L"RND4K Q1T1", 41.37, 98.6, 0, 0};
        bench.disk.phase = L"A verificar os dados (ciclo)";
        bench.disk.phaseIndex = 3;
        bench.disk.phaseCount = 3;
        bench.disk.phaseProgress = 0.62;
        bench.disk.liveMBs = 540.0;
        bench.disk.verifyCycles = 7;
        bench.diskHist = wave(420.0f, 130.0f, 0.4f);

        bench.gpu.state = State::Running;
        bench.gpu.adapter = bench.adapter.name;
        bench.gpu.fps = 9.3;
        bench.gpu.avgFps = 9.1;
        bench.gpu.minFps = 8.0;
        bench.gpu.maxFps = 11.0;
        bench.gpu.frames = 1990;
        bench.gpu.elapsed = 214.0;
        bench.gpu.width = 1920;
        bench.gpu.height = 1080;
        bench.gpu.frameMs = 107.5;
        bench.gpuHist = wave(9.2f, 0.9f, 0.3f);

        SensorSnapshot& s = bench.sensors;
        s.cpuLoadPct = 100.0;
        s.cpuMhz = 3390;
        s.cpuBaseMhz = 3200;
        s.cpuTempC = 78.0;
        s.cpuTempMaxC = 81.0;
        s.gpuName = bench.adapter.name;
        s.gpuTempC = 67.0;
        s.gpuTempMaxC = 68.0;
        s.gpuLoadPct = 99.0;
        s.gpuFanRpm = 1840;
        s.gpuPowerPct = 71.0;
        s.vramUsed = 1300ULL << 20;
        s.vramTotal = 4ULL << 30;
        s.ramTotal = 16ULL << 30;
        s.ramUsed = 14200ULL << 20;
        s.diskTempC = 38.0;
        s.diskTempMaxC = 39.0;
        s.battery.present = true;
        s.battery.detailed = true;
        s.battery.onAc = true;
        s.battery.charging = true;
        s.battery.count = 1;
        s.battery.percent = 64;
        s.battery.designMwh = 47520;
        s.battery.fullMwh = 41230;
        s.battery.currentMwh = 26400;
        s.battery.rateMw = 23500;
        s.battery.voltageMv = 12110;
        s.battery.cycles = 312;
        s.battery.healthPct = 86.8;
        s.battery.chemistry = L"Li-ion";
        s.battery.manufacturer = L"LGC";
        s.battery.name = L"45N1049";
        bench.cpuTempHist = ramp(46.0f, 78.0f);
        bench.gpuTempHist = ramp(41.0f, 67.0f);
        shootAll(L"running");
        // The bar at the size it gets on a 1366 × 768 laptop.
        bar.snapshot(directory / L"bar_running_240x728.png", 240, 728);
        dash.snapshot(directory / L"dash_running_800x450.png", 800, 450);

        // ---- finished with a failure ----
        bench.pretendActive = false;
        bench.session.running = false;
        bench.session.testElapsed = 300.0;
        bench.session.realElapsed = 482.0;
        bench.session.verdictKind = 2;
        bench.session.verdict = L"REPROVADO em 05:00 de teste (08:02 reais) — RAM: 3 erro(s)";
        bench.ram.state = State::Failed;
        bench.ram.errors = 3;
        bench.ram.message = L"3 erro(s) de memória detetado(s).";
        bench.cpu.state = State::Passed;
        bench.cpu.message = L"Sem erros em 368 400 iterações.";
        bench.disk.state = State::Passed;
        bench.disk.message = L"Sem erros — dados verificados 14 vez(es).";
        bench.gpu.state = State::Passed;
        bench.gpu.score = 912;
        bench.gpu.message = L"Sem falhas do controlador em 08:02 — 9,1 FPS médios a 1920×1080.";
        s.battery.healthPct = 41.0;
        s.battery.fullMwh = 19480;
        s.battery.charging = false;
        s.battery.rateMw = 0;
        s.battery.percent = 100;
        shootAll(L"failed");
    }
    ui::shutdown();
    return 0;
}
#endif

}  // namespace nt
