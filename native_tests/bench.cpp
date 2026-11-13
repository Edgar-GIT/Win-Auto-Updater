#include "bench.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace nt {

namespace {

constexpr const wchar_t* kTimerClass = L"WinAutoUpdaterNativeBenchTimer";
constexpr UINT_PTR kTimerId = 1;
constexpr int kTickMs = 250;
constexpr const wchar_t* kTitle = L"Win Auto Updater — Testes nativos (beta)";

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, size)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

const wchar_t* stateWord(State state) {
    switch (state) {
        case State::Idle:
            return L"Não correu";
        case State::Starting:
            return L"A iniciar";
        case State::Running:
            return L"A correr";
        case State::Passed:
            return L"OK";
        case State::Failed:
            return L"ERRO";
        case State::Stopped:
            return L"Interrompido";
        case State::Unavailable:
            return L"Indisponível";
    }
    return L"";
}

std::wstring formatMBs(double value) {
    if (value < 0.0) {
        return L"—";
    }
    return formatNumber(value, value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2));
}

std::wstring formatTemp(double celsius) {
    return hasReading(celsius) ? formatNumber(celsius, 0) + L" °C" : std::wstring(L"n/d");
}

}  // namespace

Bench::Bench(const Config& config) : config_(config) {
    cpuName = CpuBurner::cpuName();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        totalRam = memory.ullTotalPhys;
        freeRam = memory.ullAvailPhys;
    }
    drives = DiskTest::listDrives();
    haveAdapter = GpuTest::queryAdapter(&adapter);
    sensors_.start();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Bench::timerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTimerClass;
    RegisterClassExW(&wc);
    timerWindow_ = CreateWindowExW(0, kTimerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance,
                                   this);
    if (timerWindow_ != nullptr) {
        SetTimer(timerWindow_, kTimerId, kTickMs, nullptr);
    }
    refreshViews();
}

Bench::~Bench() {
    if (timerWindow_ != nullptr) {
        KillTimer(timerWindow_, kTimerId);
        DestroyWindow(timerWindow_);
        timerWindow_ = nullptr;
    }
    ramTest_.requestStop();
    cpuTest_.requestStop();
    diskTest_.requestStop();
    gpuTest_.requestStop();
    ramTest_.stop();
    cpuTest_.stop();
    diskTest_.stop();
    gpuTest_.stop();
    sensors_.stop();
    SetThreadExecutionState(ES_CONTINUOUS);
}

LRESULT CALLBACK Bench::timerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (msg == WM_TIMER && wParam == kTimerId) {
        if (auto* self = reinterpret_cast<Bench*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            self->tick();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Bench::attach(HWND window) {
    if (window != nullptr && std::find(windows_.begin(), windows_.end(), window) == windows_.end()) {
        windows_.push_back(window);
    }
}

void Bench::detach(HWND window) {
    windows_.erase(std::remove(windows_.begin(), windows_.end(), window), windows_.end());
}

void Bench::cycleDrive() {
    if (!diskTest_.active() && !drives.empty()) {
        driveIndex = (driveIndex + 1) % drives.size();
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool Bench::active(TestId id) const {
#ifdef SU_UI_SNAPSHOT
    if (pretendActive) {
        return id != TestId::Battery;
    }
#endif
    switch (id) {
        case TestId::Ram:
            return ramTest_.active();
        case TestId::Cpu:
            return cpuTest_.active();
        case TestId::Disk:
            return diskTest_.active();
        case TestId::Gpu:
            return gpuTest_.active();
        case TestId::Battery:
            return false;
    }
    return false;
}

bool Bench::anyActive() const {
#ifdef SU_UI_SNAPSHOT
    if (pretendActive) {
        return true;
    }
#endif
    return ramTest_.active() || cpuTest_.active() || diskTest_.active() || gpuTest_.active();
}

void Bench::startTest(TestId id, HWND anchor) {
    switch (id) {
        case TestId::Ram:
            ramTest_.start(0);
            break;
        case TestId::Cpu:
            cpuTest_.start(0);
            break;
        case TestId::Disk: {
            DiskTest::Options options;
            if (driveIndex < drives.size()) {
                options.driveLetter = drives[driveIndex].letter;
            }
            // After the measurement it keeps reading and verifying until the
            // five minutes are over (reads only: no extra wear on an SSD).
            options.loopReads = true;
            diskTest_.start(options);
            break;
        }
        case TestId::Gpu: {
            GpuTest::Options options;
            options.placeNear = anchor;
            options.keepDisplayMode = config_.keepDisplayMode;
            gpuTest_.start(options);
            break;
        }
        case TestId::Battery:
            break;
    }
}

void Bench::stopTest(TestId id) {
    switch (id) {
        case TestId::Ram:
            ramTest_.stop();
            break;
        case TestId::Cpu:
            cpuTest_.stop();
            break;
        case TestId::Disk:
            diskTest_.stop();
            break;
        case TestId::Gpu:
            gpuTest_.stop();
            break;
        case TestId::Battery:
            break;
    }
}

void Bench::start(TestId id, HWND anchor) {
    if (id == TestId::Battery || session.running || active(id)) {
        return;
    }
    startTest(id, anchor);
    Run& run = runs_[static_cast<int>(id)];
    run.timed = true;
    run.clockStart = TestClock::instance().now();
    run.realStart = nowSeconds();
    run.lastElapsed = 0.0;
    tick();
}

void Bench::stop(TestId id) {
    if (id == TestId::Battery) {
        return;
    }
    if (session.running) {
        stopSession(false);
        return;
    }
    stopTest(id);
    tick();
}

void Bench::stopAll() {
    if (session.running) {
        stopSession(false);
        return;
    }
    ramTest_.requestStop();
    cpuTest_.requestStop();
    diskTest_.requestStop();
    gpuTest_.requestStop();
    ramTest_.stop();
    cpuTest_.stop();
    diskTest_.stop();
    gpuTest_.stop();
    tick();
}

double Bench::runElapsed(TestId id) const {
    if (id == TestId::Battery) {
        return -1.0;
    }
    if (session.running) {
        return session.preparing ? 0.0 : session.testElapsed;
    }
    const Run& run = runs_[static_cast<int>(id)];
    if (run.timed) {
        return std::max(0.0, TestClock::instance().now() - run.clockStart);
    }
    return run.lastElapsed;
}

double Bench::runProgress(TestId id) const {
    if (id == TestId::Battery || !active(id)) {
        return -1.0;
    }
    return std::clamp(runElapsed(id) / testSeconds(), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Automatic session
// ---------------------------------------------------------------------------

void Bench::startSession(HWND anchor) {
    if (session.running || anyActive()) {
        return;
    }
    session = SessionView{};
    session.running = true;
    session.preparing = true;
    sessionAnchor_ = anchor;
    prepareStart_ = nowSeconds();
    for (Run& run : runs_) {
        run = Run{};
    }
    sensors_.resetPeaks();
    batteryAtStart_ = sensors.battery.present ? sensors.battery.percent : -1;
    logLine(L"===== Sessão automática: " + formatDuration(testSeconds()) + L" no relógio do teste gráfico =====");
    // The graphics test first (it needs memory for its device, and a first-time
    // shader compile needs CPU); the disk starts measuring meanwhile. CPU and
    // RAM follow from tick() once the scene is on screen, RAM taking whatever
    // memory is left at that point.
    startTest(TestId::Gpu, anchor);
    startTest(TestId::Disk, anchor);
    tick();
}

void Bench::stopSession(bool finished) {
    if (!session.running) {
        return;
    }
    // Ask everything to wind down first, then wait: the tests stop in parallel.
    ramTest_.requestStop();
    cpuTest_.requestStop();
    diskTest_.requestStop();
    gpuTest_.requestStop();
    ramTest_.stop();
    cpuTest_.stop();
    diskTest_.stop();
    gpuTest_.stop();
    if (!session.preparing) {
        session.realElapsed = nowSeconds() - sessionRealStart_;
        if (finished) {
            session.testElapsed = std::min(session.testElapsed, testSeconds());
        }
    }
    session.running = false;
    session.preparing = false;
    for (Run& run : runs_) {
        run.lastElapsed = session.testElapsed;
    }
    refreshViews();
    computeVerdict(finished);
    if (sessionAnchor_ != nullptr && IsWindow(sessionAnchor_)) {
        if (IsIconic(sessionAnchor_)) {
            ShowWindow(sessionAnchor_, SW_RESTORE);
        }
        SetForegroundWindow(sessionAnchor_);
    }
    for (HWND window : windows_) {
        InvalidateRect(window, nullptr, FALSE);
    }
}

uint64_t Bench::errorCount() const {
    return ram.errors + cpu.errors + disk.verifyErrors + disk.ioErrors +
           static_cast<uint64_t>(std::max(0, gpu.deviceResets));
}

void Bench::computeVerdict(bool finished) {
    std::wstring problems;
    const auto add = [](std::wstring* list, const std::wstring& text) {
        if (!list->empty()) {
            *list += L" · ";
        }
        *list += text;
    };
    if (ram.errors > 0) {
        add(&problems, L"RAM: " + formatCount(ram.errors) + L" erro(s)");
    }
    if (cpu.errors > 0) {
        add(&problems, L"CPU: " + formatCount(cpu.errors) + L" cálculo(s) errado(s)");
    }
    if (disk.verifyErrors > 0 || disk.ioErrors > 0) {
        add(&problems, L"Disco: " + formatCount(disk.verifyErrors) + L" bloco(s) corrompido(s), " +
                           formatCount(disk.ioErrors) + L" erro(s) de E/S");
    }
    if (gpu.deviceResets > 0) {
        add(&problems, L"GPU: " + std::to_wstring(gpu.deviceResets) + L" reset(s) do controlador");
    }

    // Things worth a look that are not hardware errors.
    std::wstring warnings;
    if (gradeBattery(sensors.battery) == BatteryGrade::Poor) {
        add(&warnings, L"bateria gasta (" + formatNumber(sensors.battery.healthPct, 0) + L"% de saúde)");
    }
    if (sensors.cpuTempMaxC >= 95.0) {
        add(&warnings, L"CPU chegou a " + formatNumber(sensors.cpuTempMaxC, 0) + L" °C");
    }
    if (sensors.gpuTempMaxC >= 92.0) {
        add(&warnings, L"gráfica chegou a " + formatNumber(sensors.gpuTempMaxC, 0) + L" °C");
    }

    std::wstring time = formatDuration(session.testElapsed);
    if (std::abs(session.realElapsed - session.testElapsed) >= 5.0) {
        time += L" de teste (" + formatDuration(session.realElapsed) + L" reais)";
    }

    if (!problems.empty()) {
        session.verdictKind = 2;
        session.verdict = L"REPROVADO em " + time + L" — " + problems;
    } else {
        std::wstring notRun;
        std::wstring open;
        const auto note = [&](const wchar_t* name, State state) {
            if (state == State::Unavailable) {
                notRun += std::wstring(notRun.empty() ? L" Não correu: " : L", ") + name;
            } else if (state != State::Passed) {
                open += std::wstring(open.empty() ? L" Sem conclusão: " : L", ") + name;
            }
        };
        note(L"RAM", ram.state);
        note(L"CPU", cpu.state);
        note(L"disco", disk.state);
        note(L"gráfica", gpu.state);
        session.verdictKind = warnings.empty() ? 1 : 3;
        if (finished) {
            session.verdict = std::wstring(warnings.empty() ? L"APROVADO" : L"APROVADO COM AVISOS") +
                              L" — sem erros em " + time + L".";
        } else {
            session.verdict = L"Sem erros até à paragem manual (" + time + L").";
        }
        session.verdict += (notRun.empty() ? L"" : notRun + L".") + (open.empty() ? L"" : open + L".");
    }
    if (!warnings.empty()) {
        session.verdict += L" Avisos: " + warnings + L".";
    }
    logLine(L"Veredicto: " + session.verdict);
}

// The PC must not sleep or blank the screen in the middle of a run.
void Bench::updateKeepAwake() {
    const bool wanted = anyActive();
    if (wanted == keepAwake_) {
        return;
    }
    keepAwake_ = wanted;
    SetThreadExecutionState(wanted ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
                                   : ES_CONTINUOUS);
}

void Bench::refreshViews() {
    ram = ramTest_.stats();
    cpu = cpuTest_.stats();
    disk = diskTest_.stats();
    gpu = gpuTest_.stats();
    ramHist = ramTest_.history();
    cpuHist = cpuTest_.history();
    diskHist = diskTest_.history();
    gpuHist = gpuTest_.history();
    sensors = sensors_.snapshot();
    cpuTempHist = sensors_.cpuTempHistory();
    gpuTempHist = sensors_.gpuTempHistory();
    cpuLoadHist = sensors_.cpuLoadHistory();
    if (sensors.ramTotal > 0) {
        totalRam = sensors.ramTotal;
        if (!ramTest_.active()) {
            freeRam = sensors.ramTotal - sensors.ramUsed;
        }
    }
}

void Bench::tick() {
    ++frame;
    refreshViews();

    const double clock = TestClock::instance().now();
    const double real = nowSeconds();
    if (lastClock_ >= 0.0 && real - lastReal_ >= 0.9) {
        const double rate = std::clamp((clock - lastClock_) / (real - lastReal_), 0.0, 1.0);
        clockRate = clockRate * 0.6 + rate * 0.4;
        lastClock_ = clock;
        lastReal_ = real;
    } else if (lastClock_ < 0.0) {
        lastClock_ = clock;
        lastReal_ = real;
    }

    // Tests started one by one: five minutes on the test clock each.
    if (!session.running) {
        static const TestId kTimed[4] = {TestId::Ram, TestId::Cpu, TestId::Disk, TestId::Gpu};
        bool stoppedOne = false;
        for (const TestId id : kTimed) {
            Run& run = runs_[static_cast<int>(id)];
            if (!run.timed) {
                continue;
            }
            const double elapsed = std::max(0.0, clock - run.clockStart);
            run.lastElapsed = elapsed;
            if (!active(id)) {
                run.timed = false;
            } else if (elapsed >= testSeconds() || real - run.realStart >= maxRealSeconds()) {
                stopTest(id);
                run.timed = false;
                run.lastElapsed = std::min(elapsed, testSeconds());
                stoppedOne = true;
            }
        }
        if (stoppedOne) {
            refreshViews();
        }
    }

    if (session.running) {
        const bool gpuUp = gpu.state != State::Starting;
        if (session.preparing && (gpuUp || real - prepareStart_ > 90.0)) {
            session.preparing = false;
            sessionClockStart_ = TestClock::instance().now();
            sessionRealStart_ = nowSeconds();
            startTest(TestId::Cpu, sessionAnchor_);
            startTest(TestId::Ram, sessionAnchor_);
        }
        if (!session.preparing) {
            session.testElapsed = std::max(0.0, TestClock::instance().now() - sessionClockStart_);
            session.realElapsed = nowSeconds() - sessionRealStart_;
        }
        if (gpu.closedByUser && !gpuTest_.active()) {
            // ESC on the full-screen scene: the user wants out of the whole run.
            stopSession(false);
            return;
        }
        if (!session.preparing) {
            if (session.testElapsed >= testSeconds() || session.realElapsed >= maxRealSeconds()) {
                stopSession(true);
                return;
            }
            // Everything ended by itself (e.g. nothing could run): close the session.
            if (!anyActive() && session.realElapsed > 3.0) {
                stopSession(false);
                return;
            }
        }
    }

    updateKeepAwake();
    for (HWND window : windows_) {
        InvalidateRect(window, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

std::wstring Bench::buildReport() const {
    std::wstring r;
    const auto line = [&r](const std::wstring& text) { r += text + L"\r\n"; };

    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computer, &size);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[64]{};
    swprintf(stamp, 64, L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);

    line(L"Win Auto Updater — relatório dos testes nativos (beta)");
    line(L"Computador: " + std::wstring(computer) + L"    Data: " + stamp);
    line(L"");
    if (!session.verdict.empty()) {
        line(L"VEREDICTO: " + session.verdict);
        line(L"Tempo de teste: " + formatDuration(session.testElapsed) + L" (relógio do teste gráfico)    " +
             L"Tempo real: " + formatDuration(session.realElapsed));
        line(L"");
    }

    line(L"[Temperaturas máximas]  CPU: " + formatTemp(sensors.cpuTempMaxC) + L"   Gráfica: " +
         formatTemp(sensors.gpuTempMaxC) + L"   Disco: " + formatTemp(sensors.diskTempMaxC));
    line(L"");

    line(L"[RAM] " + std::wstring(stateWord(ram.state)) + L" — total " + formatBytes(totalRam));
    if (ram.state != State::Idle) {
        line(L"  Testado: " + formatBytes(ram.bytesUnderTest) + L" com " + std::to_wstring(ram.threads) +
             L" threads" + (ram.locked ? L" (fixada)" : L""));
        line(L"  Cobertura: " + formatNumber(ram.coveragePct, 0) + L"%   Passagens: " + formatCount(ram.passes) +
             L"   Verificado: " + formatBytes(ram.bytesVerified));
        line(L"  Erros: " + formatCount(ram.errors) + L"   Tempo: " + formatDuration(ram.elapsed));
        for (const RamError& e : ram.firstErrors) {
            wchar_t detail[160]{};
            swprintf(detail, 160, L"    offset 0x%llX  esperado %016llX  lido %016llX  (%ls)",
                     static_cast<unsigned long long>(e.offset), static_cast<unsigned long long>(e.expected),
                     static_cast<unsigned long long>(e.actual), RamTest::patternName(e.pattern));
            line(detail);
        }
        if (!ram.message.empty()) {
            line(L"  " + ram.message);
        }
    }
    line(L"");

    line(L"[CPU] " + std::wstring(stateWord(cpu.state)) + L" — " + cpuName);
    if (cpu.state != State::Idle) {
        line(L"  Threads: " + std::to_wstring(cpu.threads) + (cpu.avx2 ? L" (AVX2+FMA)" : L" (SSE2)") +
             L"   Iterações: " + formatCount(cpu.iterations) + L"   Erros: " + formatCount(cpu.errors));
        line(L"  Pico: " + formatNumber(cpu.peakItersPerSec, 0) + L" iter/s   Tempo: " +
             formatDuration(cpu.elapsed));
        if (!cpu.message.empty()) {
            line(L"  " + cpu.message);
        }
    }
    line(L"");

    const DriveInfo& drive =
        disk.state != State::Idle || drives.empty() ? disk.drive : drives[std::min(driveIndex, drives.size() - 1)];
    line(L"[Disco] " + std::wstring(stateWord(disk.state)) + L" — " + drive.letter + L" " + drive.model +
         (drive.kind.empty() ? L"" : L" (" + drive.kind + L")"));
    if (disk.state != State::Idle) {
        line(L"  Ficheiro de teste: " + formatBytes(disk.fileBytes) +
             L"                 Leitura MB/s   Escrita MB/s");
        for (const DiskRow& row : disk.rows) {
            wchar_t text[160]{};
            swprintf(text, 160, L"  %-14ls %14ls %14ls", row.name, formatMBs(row.readMBs).c_str(),
                     formatMBs(row.writeMBs).c_str());
            line(text);
        }
        line(L"  Verificações completas: " + formatCount(disk.verifyCycles) + L"   Blocos corrompidos: " +
             formatCount(disk.verifyErrors) + L"   Erros de E/S: " + formatCount(disk.ioErrors));
        if (!disk.message.empty()) {
            line(L"  " + disk.message);
        }
    }
    line(L"");

    line(L"[Gráfica] " + std::wstring(stateWord(gpu.state)) + L" — " +
         (gpu.adapter.empty() ? adapter.name : gpu.adapter));
    if (gpu.state != State::Idle) {
        line(L"  Resolução: " + std::to_wstring(gpu.width) + L"×" + std::to_wstring(gpu.height) +
             L" (ecrã inteiro)   Frames: " + formatCount(gpu.frames) + L"   Tempo: " +
             formatDuration(gpu.elapsed));
        line(L"  FPS médio: " + formatNumber(gpu.avgFps, 1) + L"   mín: " + formatNumber(gpu.minFps, 1) +
             L"   máx: " + formatNumber(gpu.maxFps, 1));
        if (gpu.score > 0) {
            line(L"  Pontuação: " + formatCount(static_cast<uint64_t>(gpu.score)) +
                 L" (FPS médio normalizado a 1920×1080, ×100)");
        }
        line(L"  Resets do controlador: " + std::to_wstring(gpu.deviceResets));
        if (!gpu.message.empty()) {
            line(L"  " + gpu.message);
        }
    }
    line(L"");

    const BatteryInfo& b = sensors.battery;
    line(L"[Bateria] " + std::wstring(batteryGradeText(gradeBattery(b))));
    if (b.present) {
        line(L"  Carga: " + (b.percent >= 0 ? std::to_wstring(b.percent) + L"%" : std::wstring(L"n/d")) + L" — " +
             batteryStatusText(b) +
             (batteryAtStart_ >= 0 && !session.verdict.empty()
                  ? L" (no início da sessão: " + std::to_wstring(batteryAtStart_) + L"%)"
                  : L""));
        if (b.detailed) {
            const wchar_t* unit = b.relativeUnits ? L" (unidades relativas)" : L" mWh";
            line(L"  Capacidade de fábrica: " + formatCount(b.designMwh) + unit + L"   Capacidade atual: " +
                 formatCount(b.fullMwh) + unit);
            line(L"  Saúde: " + formatNumber(b.healthPct, 0) + L"%   Desgaste: " +
                 formatNumber(100.0 - b.healthPct, 0) + L"%" +
                 (b.cycles > 0 ? L"   Ciclos: " + std::to_wstring(b.cycles) : L""));
        }
        if (b.voltageMv > 0 || b.rateMw != 0) {
            line(L"  Tensão: " + formatNumber(b.voltageMv / 1000.0, 2) + L" V   Potência: " +
                 formatNumber(b.rateMw / 1000.0, 1) + L" W");
        }
        if (!b.name.empty() || !b.manufacturer.empty() || !b.chemistry.empty()) {
            line(L"  " + b.manufacturer + (b.manufacturer.empty() ? L"" : L" ") + b.name +
                 (b.chemistry.empty() ? L"" : L" · " + b.chemistry));
        }
    }
    return r;
}

void Bench::saveReport(HWND owner) const {
    PWSTR desktopRaw = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopRaw)) && desktopRaw) {
        folder = desktopRaw;
    }
    if (desktopRaw) {
        CoTaskMemFree(desktopRaw);
    }
    if (folder.empty()) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        folder = temp;
    }
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computer, &size);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[160]{};
    swprintf(name, 160, L"Testes_%ls_%04u%02u%02u_%02u%02u.txt", computer, now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute);
    const std::filesystem::path file = folder / name;

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        MessageBoxW(owner, (L"Não foi possível gravar o relatório em:\r\n" + file.wstring()).c_str(), kTitle,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    out << "\xEF\xBB\xBF" << toUtf8(buildReport());
    out.close();
    ShellExecuteW(owner, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace nt
