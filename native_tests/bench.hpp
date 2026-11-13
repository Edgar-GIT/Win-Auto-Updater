#pragma once

#include "battery.hpp"
#include "cpu_burner.hpp"
#include "disk_test.hpp"
#include "gpu_test.hpp"
#include "ram_test.hpp"
#include "sensors.hpp"

// The bench: owns the tests and the sensors, times every run on the test clock
// (five minutes of the graphics test — see nt_core.hpp), runs the automatic
// session and produces the verdict and the report. The windows (dashboard, the
// separate test windows, the monitor bar) only draw what it publishes.
//
// Everything here runs on the UI thread; the tests have their own threads.

namespace nt {

enum class TestId { Ram = 0, Cpu, Disk, Gpu, Battery };
constexpr int kTestCount = 5;

struct SessionView {
    bool running = false;
    bool preparing = false;    // waiting for the graphics test to come up
    double testElapsed = 0.0;  // seconds on the test clock
    double realElapsed = 0.0;
    int verdictKind = 0;       // 0 none, 1 passed, 2 failed, 3 passed with warnings
    std::wstring verdict;
};

class Bench {
public:
    struct Config {
        bool automatic = false;        // one session of everything, started on launch
        bool keepDisplayMode = false;  // do not switch the screen to its highest resolution
    };

    explicit Bench(const Config& config);
    ~Bench();
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    const Config& config() const { return config_; }

    // ---- one test at a time (the separate test windows) ----
    void start(TestId id, HWND anchor);
    void stop(TestId id);
    bool active(TestId id) const;
    // Progress of the running test (or of the session it belongs to) through
    // its five test-minutes, 0..1; negative when it is not running.
    double runProgress(TestId id) const;
    // Test-clock seconds of the running test, or of its last run; negative = never ran.
    double runElapsed(TestId id) const;

    // ---- automatic session: every test at once ----
    void startSession(HWND anchor);
    void stopSession(bool finished);
    void stopAll();
    bool anyActive() const;

    // Called a few times per second by the bench's own timer.
    void tick();

    // ---- what the windows draw (refreshed by tick) ----
    RamStats ram;
    CpuStats cpu;
    DiskStats disk;
    GpuStats gpu;
    SensorSnapshot sensors;
    std::vector<float> ramHist;
    std::vector<float> cpuHist;
    std::vector<float> diskHist;
    std::vector<float> gpuHist;
    std::vector<float> cpuTempHist;
    std::vector<float> gpuTempHist;
    std::vector<float> cpuLoadHist;
    SessionView session;
    int frame = 0;  // animation counter
    // Pace of the test clock against real time (1 = real time; lower while the
    // graphics test renders below its nominal frame rate).
    double clockRate = 1.0;

    // Machine facts shown while idle.
    std::wstring cpuName;
    uint64_t totalRam = 0;
    uint64_t freeRam = 0;
    std::vector<DriveInfo> drives;
    size_t driveIndex = 0;
    GpuAdapter adapter;
    bool haveAdapter = false;
    void cycleDrive();

    uint64_t errorCount() const;
    std::wstring buildReport() const;
    void saveReport(HWND owner) const;

    // Windows repainted after every tick.
    void attach(HWND window);
    void detach(HWND window);

#ifdef SU_UI_SNAPSHOT
    // Developer aid: the snapshot renderer fills the views by hand and has the
    // tests reported as running without starting any.
    bool pretendActive = false;
#endif

private:
    struct Run {
        bool timed = false;
        double clockStart = 0.0;
        double realStart = 0.0;
        double lastElapsed = -1.0;
    };

    void refreshViews();
    void startTest(TestId id, HWND anchor);
    void stopTest(TestId id);
    void computeVerdict(bool finished);
    void updateKeepAwake();
    static LRESULT CALLBACK timerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    Config config_;
    RamTest ramTest_;
    CpuBurner cpuTest_;
    DiskTest diskTest_;
    GpuTest gpuTest_;
    Sensors sensors_;

    Run runs_[4];
    double sessionClockStart_ = 0.0;
    double sessionRealStart_ = 0.0;
    double prepareStart_ = 0.0;
    int batteryAtStart_ = -1;
    HWND sessionAnchor_ = nullptr;
    bool keepAwake_ = false;
    double lastClock_ = -1.0;
    double lastReal_ = 0.0;
    HWND timerWindow_ = nullptr;
    std::vector<HWND> windows_;
};

}  // namespace nt
