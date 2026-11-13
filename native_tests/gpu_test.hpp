#pragma once

#include "nt_core.hpp"

#include <filesystem>
#include <thread>

// Native GPU test (replaces FurMark / powerMAX GPU), on Direct3D 11.
//
// It always runs full screen at the highest resolution the display supports,
// rendered 1:1 (no internal down-scaling) with an uncapped frame rate, so the
// graphics card stays at 100 %. On hybrid laptops it picks the dedicated card.
// A driver reset under load (device removed / hung) is detected and counted as
// a failure — that is what an unstable graphics card does.
//
// While it renders, it also drives the test clock (see nt_core.hpp): the
// duration of every test is measured in frames of this scene.

namespace nt {

struct GpuAdapter {
    std::wstring name;
    uint64_t vramBytes = 0;    // dedicated video memory
    uint64_t sharedBytes = 0;  // shared system memory
    bool software = false;     // no real driver: Windows' software renderer
    LUID luid{};
    UINT vendorId = 0;
};

struct GpuStats {
    State state = State::Idle;
    std::wstring message;
    std::wstring adapter;
    uint64_t vramBytes = 0;
    bool softwareRenderer = false;
    bool fallbackShader = false;
    bool closedByUser = false;  // ESC / window closed
    int width = 0;              // resolution rendered and shown (1:1)
    int height = 0;
    double fps = 0.0;
    double avgFps = 0.0;
    double minFps = 0.0;
    double maxFps = 0.0;
    double frameMs = 0.0;
    uint64_t frames = 0;
    double elapsed = 0.0;
    int deviceResets = 0;
    int score = 0;  // average FPS normalised to 1920×1080, ×100
};

class GpuTest {
public:
    struct Options {
        HWND placeNear = nullptr;      // opens on the monitor of this window
        bool keepDisplayMode = false;  // do not switch the screen to its highest resolution
    };

    GpuTest() = default;
    ~GpuTest();
    GpuTest(const GpuTest&) = delete;
    GpuTest& operator=(const GpuTest&) = delete;

    bool start(const Options& options);
    void requestStop();
    void stop();
    bool active() const;

    GpuStats stats() const;
    std::vector<float> history() const { return history_.values(); }

    // The adapter the test renders on, without creating a device.
    static bool queryAdapter(GpuAdapter* adapter);

#ifdef SU_UI_SNAPSHOT
    // Developer aid: renders one frame of the scene off-screen into a PNG.
    static bool renderSnapshot(const std::filesystem::path& file, int width, int height, float time,
                               float quality, std::wstring* error);
#endif

private:
    void run(Options options);
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> resizePending_{false};
    std::atomic<bool> closedByUser_{false};

    mutable std::mutex mutex_;
    GpuStats stats_;
    History history_{150};
};

}  // namespace nt
