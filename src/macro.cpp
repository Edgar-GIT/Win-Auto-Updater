#include "macro.hpp"

#include <cwchar>
#include <fstream>
#include <sstream>

namespace {

std::wstring trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring processBaseName(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t buffer[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size) != 0) {
        name = std::filesystem::path(buffer).filename().wstring();
    }
    CloseHandle(process);
    return name;
}

std::wstring classNameOf(HWND window) {
    wchar_t buffer[256]{};
    GetClassNameW(window, buffer, 256);
    return buffer;
}

std::wstring titleOf(HWND window) {
    wchar_t buffer[512]{};
    GetWindowTextW(window, buffer, 512);
    return buffer;
}

std::wstring ownProcessName() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0) {
        return {};
    }
    return std::filesystem::path(buffer).filename().wstring();
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring fromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct FindContext {
    std::wstring proc;
    std::wstring cls;
    HWND found = nullptr;
};

BOOL CALLBACK enumWindowProc(HWND window, LPARAM param) {
    auto* context = reinterpret_cast<FindContext*>(param);
    if (IsWindowVisible(window) == 0) {
        return TRUE;
    }
    if (_wcsicmp(processBaseName(window).c_str(), context->proc.c_str()) != 0) {
        return TRUE;
    }
    if (!context->cls.empty() && _wcsicmp(classNameOf(window).c_str(), context->cls.c_str()) != 0) {
        return TRUE;
    }
    context->found = window;
    return FALSE;
}

HWND findWindow(const std::wstring& proc, const std::wstring& cls, int timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        FindContext context{proc, cls, nullptr};
        EnumWindows(enumWindowProc, reinterpret_cast<LPARAM>(&context));
        if (context.found != nullptr) {
            return context.found;
        }
        if (GetTickCount() - start > static_cast<DWORD>(timeoutMs)) {
            return nullptr;
        }
        Sleep(200);
    }
}

// SetForegroundWindow is often refused from a background thread, so attach our
// thread input to the current foreground thread while activating the target.
void activateWindow(HWND window) {
    const DWORD current = GetCurrentThreadId();
    const HWND foreground = GetForegroundWindow();
    const DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = other != 0 && other != current &&
                          AttachThreadInput(current, other, TRUE);
    ShowWindow(window, SW_RESTORE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    if (attached) {
        AttachThreadInput(current, other, FALSE);
    }
}

}  // namespace

namespace Macro {

Recorder* Recorder::instance_ = nullptr;

std::filesystem::path filePath(const std::filesystem::path& executable) {
    return executable.parent_path() / L"macro.txt";
}

bool save(const std::filesystem::path& file, const std::vector<Event>& events) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << toUtf8(L"# Win Auto Updater macro (clicks are relative to the target window)\n");
    for (const auto& event : events) {
        std::wostringstream line;
        if (event.kind == Event::Kind::Click) {
            line << L"C\t" << event.proc << L'\t' << event.cls << L'\t' << event.relX << L'\t'
                 << event.relY << L'\t' << (event.right ? L"R" : L"L") << L'\t' << event.title;
        } else {
            // K  proc  cls  vk  flags  title
            // flags: bits 1=ctrl 2=shift 4=alt 8=win
            unsigned flags = 0;
            if (event.ctrl) flags |= 1;
            if (event.shift) flags |= 2;
            if (event.alt) flags |= 4;
            if (event.win) flags |= 8;
            line << L"K\t" << event.proc << L'\t' << event.cls << L'\t' << event.vk << L'\t'
                 << flags << L'\t' << event.title;
        }
        out << toUtf8(line.str()) << '\n';
    }
    return true;
}

bool load(const std::filesystem::path& file, std::vector<Event>* events) {
    if (events == nullptr) {
        return false;
    }
    events->clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        std::wstring line = trim(fromUtf8(raw));
        if (line.empty() || line.front() == L'#') {
            continue;
        }
        std::wistringstream stream(line);
        std::vector<std::wstring> parts;
        std::wstring part;
        while (std::getline(stream, part, L'\t')) {
            parts.push_back(part);
        }
        if (parts.size() < 4) {
            continue;
        }
        Event event;
        if (parts[0] == L"C" && parts.size() >= 6) {
            event.kind = Event::Kind::Click;
            event.proc = parts[1];
            event.cls = parts[2];
            event.relX = _wtoi(parts[3].c_str());
            event.relY = _wtoi(parts[4].c_str());
            event.right = parts[5] == L"R";
            event.title = parts.size() >= 7 ? parts[6] : L"";
        } else if (parts[0] == L"K") {
            event.kind = Event::Kind::Key;
            event.proc = parts[1];
            event.cls = parts[2];
            event.vk = static_cast<unsigned>(_wtoi(parts[3].c_str()));
            // Support both new format (with flags) and old format (without)
            if (parts.size() >= 6) {
                unsigned flags = static_cast<unsigned>(_wtoi(parts[4].c_str()));
                event.ctrl = (flags & 1) != 0;
                event.shift = (flags & 2) != 0;
                event.alt = (flags & 4) != 0;
                event.win = (flags & 8) != 0;
                event.title = parts[5];
            } else {
                event.title = parts.size() >= 5 ? parts[4] : L"";
            }
        } else {
            continue;
        }
        events->push_back(event);
    }
    return true;
}

Recorder::~Recorder() {
    stop();
}

bool Recorder::start(HWND notify) {
    if (active_) {
        return true;
    }
    notify_ = notify;
    instance_ = this;
    events_.clear();
    ourProc_ = ownProcessName();
    ctrlDown_ = shiftDown_ = altDown_ = winDown_ = false;
    active_ = true;
    HINSTANCE module = GetModuleHandleW(nullptr);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, module, 0);
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, module, 0);
    if (mouseHook_ == nullptr || keyHook_ == nullptr) {
        stop();
        return false;
    }
    return true;
}

void Recorder::stop() {
    active_ = false;
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (keyHook_ != nullptr) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = nullptr;
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

void Recorder::clear() {
    events_.clear();
}

void Recorder::captureTarget(POINT point, Event* event) const {
    HWND window = WindowFromPoint(point);
    if (window == nullptr) {
        return;
    }
    HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) {
        root = window;
    }
    event->proc = processBaseName(root);
    event->cls = classNameOf(root);
    event->title = titleOf(root);
    RECT rect{};
    GetWindowRect(root, &rect);
    event->relX = point.x - rect.left;
    event->relY = point.y - rect.top;
}

void Recorder::handleMouse(WPARAM message, const MSLLHOOKSTRUCT& info) {
    if ((info.flags & LLMHF_INJECTED) != 0) {
        return;
    }
    if (message != WM_LBUTTONDOWN && message != WM_RBUTTONDOWN) {
        return;
    }
    Event event;
    event.kind = Event::Kind::Click;
    event.right = message == WM_RBUTTONDOWN;
    captureTarget(info.pt, &event);
    if (event.proc.empty() || _wcsicmp(event.proc.c_str(), ourProc_.c_str()) == 0) {
        return;
    }
    events_.push_back(event);
}

void Recorder::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& info) {
    if ((info.flags & LLKHF_INJECTED) != 0) {
        return;
    }
    // We need both keydown and keyup to track modifier state correctly.
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
        message != WM_KEYUP && message != WM_SYSKEYUP) {
        return;
    }
    const bool isDown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);

    // Update modifier state on every key event.
    switch (info.vkCode) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            shiftDown_ = isDown;
            return;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            ctrlDown_ = isDown;
            return;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            altDown_ = isDown;
            return;
        case VK_LWIN:
        case VK_RWIN:
            winDown_ = isDown;
            return;
        default:
            break;
    }
    // Only record non-modifier keys on keydown (not keyup).
    if (!isDown) {
        return;
    }
    // F10 is handled in keyProc before calling this function; skip here as safety.
    if (info.vkCode == VK_F10) {
        return;
    }
    Event event;
    event.kind = Event::Kind::Key;
    event.vk = info.vkCode;
    // Store modifier state at time of keypress.
    event.ctrl = ctrlDown_;
    event.shift = shiftDown_;
    event.alt = altDown_;
    event.win = winDown_;
    HWND window = GetForegroundWindow();
    if (window != nullptr) {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root == nullptr) {
            root = window;
        }
        event.proc = processBaseName(root);
        event.cls = classNameOf(root);
        event.title = titleOf(root);
    }
    if (event.proc.empty() || _wcsicmp(event.proc.c_str(), ourProc_.c_str()) == 0) {
        return;
    }
    events_.push_back(event);
}

LRESULT CALLBACK Recorder::mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && instance_ != nullptr && instance_->active_) {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        instance_->handleMouse(wParam, *info);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK Recorder::keyProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && instance_ != nullptr && instance_->active_) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if ((info->flags & LLKHF_INJECTED) == 0 &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && info->vkCode == VK_F10) {
            instance_->active_ = false;
            if (instance_->notify_ != nullptr) {
                PostMessageW(instance_->notify_, kStopMessage, 0, 0);
            }
            return 1;
        }
        instance_->handleKey(wParam, *info);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void play(const std::vector<Event>& events, const std::function<void(const std::wstring&)>& log) {
    for (const auto& event : events) {
        HWND window = findWindow(event.proc, event.cls, 20000);
        if (window == nullptr) {
            if (log) {
                log(L"Macro: janela nao encontrada (" + event.proc + L"), a abortar.");
            }
            return;
        }
        if (GetForegroundWindow() != window) {
            activateWindow(window);
            Sleep(250);
        }
        if (event.kind == Event::Kind::Click) {
            RECT rect{};
            GetWindowRect(window, &rect);
            const int x = rect.left + event.relX;
            const int y = rect.top + event.relY;
            SetCursorPos(x, y);
            Sleep(60);
            INPUT inputs[2]{};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = event.right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = event.right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
            SendInput(2, inputs, sizeof(INPUT));
        } else {
            // Key event: press modifiers first, then the key, then release in reverse
            INPUT inputs[10]{};
            int n = 0;
            // Press modifiers (reverse order for proper Windows behavior)
            if (event.win) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_LWIN;
                n++;
            }
            if (event.alt) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_MENU;
                n++;
            }
            if (event.shift) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_SHIFT;
                n++;
            }
            if (event.ctrl) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_CONTROL;
                n++;
            }
            // The key itself
            inputs[n].type = INPUT_KEYBOARD;
            inputs[n].ki.wVk = static_cast<WORD>(event.vk);
            n++;
            inputs[n].type = INPUT_KEYBOARD;
            inputs[n].ki.wVk = static_cast<WORD>(event.vk);
            inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
            n++;
            // Release modifiers (reverse of press)
            if (event.ctrl) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_CONTROL;
                inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
                n++;
            }
            if (event.shift) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_SHIFT;
                inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
                n++;
            }
            if (event.alt) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_MENU;
                inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
                n++;
            }
            if (event.win) {
                inputs[n].type = INPUT_KEYBOARD;
                inputs[n].ki.wVk = VK_LWIN;
                inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
                n++;
            }
            SendInput(n, inputs, sizeof(INPUT));
        }
        Sleep(300);
    }
}

}  // namespace Macro
