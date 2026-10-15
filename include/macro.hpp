#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Records global mouse/keyboard input and replays it for the GUI-only test
// tools. Clicks are stored relative to the target top-level window so replay
// does not depend on the screen resolution (these tools have a fixed layout).
namespace Macro {

// Posted to the recorder's notify window when the stop hotkey (F10) is pressed.
constexpr UINT kStopMessage = WM_APP + 12;

struct Event {
    enum class Kind { Click, Key };
    Kind kind = Kind::Click;
    std::wstring proc;   // image name of the target window, e.g. "powerMAX.exe"
    std::wstring cls;    // window class name
    std::wstring title;  // window title at record time (informational)
    int relX = 0;        // X relative to the window's top-left
    int relY = 0;        // Y relative to the window's top-left
    unsigned vk = 0;     // virtual key for Kind::Key
    bool right = false;  // right mouse button for Kind::Click
};

std::filesystem::path filePath(const std::filesystem::path& executable);
bool save(const std::filesystem::path& file, const std::vector<Event>& events);
bool load(const std::filesystem::path& file, std::vector<Event>* events);

class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start(HWND notify);
    void stop();
    bool active() const {
        return active_;
    }

    void clear();
    std::vector<Event> events() const {
        return events_;
    }

private:
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK keyProc(int code, WPARAM wParam, LPARAM lParam);

    void captureTarget(POINT point, Event* event) const;
    void handleMouse(WPARAM message, const MSLLHOOKSTRUCT& info);
    void handleKey(WPARAM message, const KBDLLHOOKSTRUCT& info);

    static Recorder* instance_;
    HHOOK mouseHook_ = nullptr;
    HHOOK keyHook_ = nullptr;
    HWND notify_ = nullptr;
    std::wstring ourProc_;
    bool active_ = false;
    std::vector<Event> events_;
};

// Replays the events. Blocking; call from a worker thread.
void play(const std::vector<Event>& events, const std::function<void(const std::wstring&)>& log);

}  // namespace Macro
