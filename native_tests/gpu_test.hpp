#pragma once

#include "nt_core.hpp"

#include <filesystem>
#include <thread>

// Native GPU test (replaces FurMark / powerMAX GPU), on Direct3D 11:
//   • Burn       — runs until stopped, uncapped frame rate (the GPU stays at
//                  100 %), internal resolution adapts so even weak integrated
//                  graphics keep a responsive frame time.
//   • Benchmark  — fixed 1280×720 internal resolution and quality for a fixed
//                  time; the score is comparable between machines.
// A driver reset under load (device removed / hung) is detected and counted as
// a failure — that is what an unstable graphics card does.

namespace nt {

struct GpuStats {
    State state = State::Idle;
    std::wstring message;
    std::wstring adapter;
    uint64_t vramBytes = 0;
    bool softwareRenderer = false;  // no real GPU driver installed
    bool fallbackShader = false;
    bool benchmark = false;
    bool fullscreen = false;
    int width = 0;         // output window
    int height = 0;
    int renderWidth = 0;   // internal scene resolution
    int renderHeight = 0;
    double fps = 0.0;
    double avgFps = 0.0;
    double minFps = 0.0;
    double maxFps = 0.0;
    double frameMs = 0.0;
    uint64_t frames = 0;
    double elapsed = 0.0;
    double duration = 0.0;  // benchmark length (s); 0 for burn
    int deviceResets = 0;
    int score = 0;          // benchmark result
};

class GpuTest {
public:
    enum class Mode { Burn, Benchmark };

    struct Options {
        Mode mode = Mode::Burn;
        bool fullscreen = true;
        HWND placeNear = nullptr;  // opens on the monitor of this window
        double benchmarkSeconds = 60.0;
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

    // HUD overlay drawn over the scene (premultiplied BGRA, anchored top-left).
    // Rendered by the bench window on the UI thread; an empty buffer hides it.
    void setHud(int width, int height, std::vector<uint8_t> pixels);

    // Primary graphics adapter, without creating a device (for the idle card).
    static bool queryAdapter(std::wstring* name, uint64_t* vramBytes, bool* software);

#ifdef SU_UI_SNAPSHOT
    // Developer aid: renders one frame of the scene off-screen into a PNG.
    // With a HUD bitmap it renders the scene at renderScale and runs the same
    // upscale + overlay pass as the on-screen path.
    static bool renderSnapshot(const std::filesystem::path& file, int width, int height, float time,
                               float quality, std::wstring* error,
                               const std::vector<uint8_t>* hud = nullptr, int hudWidth = 0,
                               int hudHeight = 0, float renderScale = 1.0f);
#endif

private:
    void run(Options options);
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> resizePending_{false};
    std::atomic<bool> toggleFullscreen_{false};
    std::atomic<bool> hudVisible_{true};
    std::atomic<bool> fullscreen_{false};

    mutable std::mutex mutex_;
    GpuStats stats_;
    History history_{150};

    std::mutex hudMutex_;
    std::vector<uint8_t> hudPixels_;
    int hudWidth_ = 0;
    int hudHeight_ = 0;
    uint64_t hudVersion_ = 0;
};

}  // namespace nt
