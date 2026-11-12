// The native test bench window: one glass dashboard with a card per test
// (RAM, CPU, disk, GPU), a "run everything for N minutes" control, a verdict and
// a text report. Drawn with the same toolkit as the updater (ui::Canvas).

#include "native_tests.hpp"

#include "cpu_burner.hpp"
#include "disk_test.hpp"
#include "gpu_test.hpp"
#include "ram_test.hpp"

#include "ui.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace nt {

namespace {

using ui::Canvas;
using ui::Color;
using ui::RectF;
using ui::Weight;

constexpr int kDesignW = 1040;
constexpr int kDesignH = 700;
constexpr UINT_PTR kPollTimer = 1;
constexpr int kPollMs = 200;
constexpr const wchar_t* kWindowClass = L"WinAutoUpdaterNativeBench";
constexpr const wchar_t* kTitle = L"Win Auto Updater — Testes nativos (beta)";

enum Id : int {
    kStartAll = 1,
    kDuration0 = 10,  // .. kDuration0 + 4
    kRamToggle = 20,
    kCpuToggle,
    kDiskToggle,
    kDiskDrive,
    kGpuBurn,
    kGpuBench,
    kGpuStop,
    kReport,
    kClose,
};

struct DurationChoice {
    const wchar_t* label;
    int minutes;  // 0 = until stopped
};
constexpr DurationChoice kDurations[5] = {
    {L"5 min", 5}, {L"15 min", 15}, {L"30 min", 30}, {L"1 h", 60}, {L"Contínuo", 0}};

Color colorRam() { return ui::color::cyan(); }
Color colorCpu() { return ui::color::violet(); }
Color colorDisk() { return Color(255, 45, 212, 191); }
Color colorGpu() { return Color(255, 232, 121, 249); }

struct Widget {
    int id = 0;
    RECT rc{};
    std::wstring text;
    ui::ButtonStyle style = ui::ButtonStyle::Glass;
    ui::Icon icon = ui::Icon::None;
    bool visible = false;
    bool enabled = true;
};

bool intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

RECT inflated(RECT r, int amount) {
    InflateRect(&r, amount, amount);
    return r;
}

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

const wchar_t* stateLabel(State state) {
    switch (state) {
        case State::Idle:
            return L"Parado";
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

Color stateColor(State state, Color accent) {
    switch (state) {
        case State::Starting:
            return ui::color::amber();
        case State::Running:
            return accent;
        case State::Passed:
            return ui::color::green();
        case State::Failed:
            return ui::color::red();
        case State::Unavailable:
            return ui::color::amber();
        default:
            return ui::color::muted();
    }
}

std::wstring formatRate(double mbPerSec) {
    if (mbPerSec >= 1000.0) {
        return formatNumber(mbPerSec / 1024.0, 1) + L" GB/s";
    }
    return formatNumber(mbPerSec, mbPerSec >= 100.0 ? 0 : 1) + L" MB/s";
}

std::wstring formatMBs(double value) {
    if (value < 0.0) {
        return L"—";
    }
    return formatNumber(value, value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2));
}

class BenchWindow {
public:
    BenchWindow(HINSTANCE instance, const LaunchOptions& options) : instance_(instance), options_(options) {}

    int run();
#ifdef SU_UI_SNAPSHOT
    int snapshot(const std::filesystem::path& directory);
#endif

private:
    HINSTANCE instance_;
    LaunchOptions options_;
    HWND hwnd_ = nullptr;

    // rendering
    float scale_ = 1.0f;
    float fitScale_ = 1.0f;
    int clientW_ = kDesignW;
    int clientH_ = kDesignH;
    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0;
    int backH_ = 0;
    HDC bgDc_ = nullptr;
    HBITMAP bgBitmap_ = nullptr;
    HGDIOBJ bgOld_ = nullptr;
    int bgW_ = 0;
    int bgH_ = 0;
    HICON icon_ = nullptr;
    int iconSize_ = 0;
    int frame_ = 0;

    std::vector<Widget> widgets_;
    int hoverId_ = 0;
    int pressedId_ = 0;
    bool tracking_ = false;

    RECT iconRect_{};
    RECT controlRect_{};
    RECT progressRect_{};
    RECT cards_[4]{};
    RECT footerTextRect_{};

    // tests
    RamTest ram_;
    CpuBurner cpu_;
    DiskTest disk_;
    GpuTest gpu_;
    RamStats ramView_;
    CpuStats cpuView_;
    DiskStats diskView_;
    GpuStats gpuView_;
    std::vector<float> ramHist_;
    std::vector<float> cpuHist_;
    std::vector<float> diskHist_;
    std::vector<float> gpuHist_;

    // machine facts shown while idle
    std::wstring cpuName_;
    uint64_t totalRam_ = 0;
    uint64_t freeRam_ = 0;
    std::vector<DriveInfo> drives_;
    size_t driveIndex_ = 0;
    std::wstring gpuName_;
    uint64_t gpuVram_ = 0;
    bool gpuSoftware_ = false;

    // "run everything" session
    int durationIndex_ = 1;
    bool allRunning_ = false;
    double allStart_ = 0.0;
    double allDuration_ = 0.0;  // seconds; 0 = until stopped
    double lastSessionSeconds_ = 0.0;
    int verdictKind_ = 0;       // 0 = none, 1 = pass, 2 = fail
    std::wstring verdict_;
    bool keepAwake_ = false;
    // Full session: the CPU and RAM tests wait until the GPU test is up, so the
    // graphics device (and a first-time shader compile) is not starved of CPU
    // time and memory by the other two.
    bool pendingCpuRam_ = false;

    // window
    bool create();
    void fitToScreen();
    void initWidgets();
    Widget* widget(int id);
    void place(int id, int x, int y, int w, int h);
    void layout();
    void layout(int width, int height);
    void syncWidgets();
    int px(float du) const { return static_cast<int>(std::lround(du * scale_)); }
    void invalidate(const RECT* rect = nullptr) {
        if (hwnd_) {
            InvalidateRect(hwnd_, rect, FALSE);
        }
    }

    // behaviour
    void refreshMachineFacts();
    void poll();
    bool anyActive() const {
        return ram_.active() || cpu_.active() || disk_.active() || gpu_.active();
    }
    void startRam() { ram_.start(0); }
    void startCpu() { cpu_.start(0); }
    void startDisk(bool loopReads);
    void startGpu(GpuTest::Mode mode, bool fullscreen);
    void startAll();
    void stopAll(bool finished);
    void computeVerdict(bool finished);
    void updateKeepAwake();
    void onCommand(int id);
    std::wstring buildReport() const;
    void saveReport();
    void renderHud();
    std::vector<uint8_t> buildHud(int* width, int* height);

    // painting
    void paint();
    void render(HDC dc, int width, int height, const RECT& clip);
    void ensureBackground(HDC reference, int width, int height);
    void releaseSurfaces();
    void drawHeader(Canvas& canvas);
    void drawControl(Canvas& canvas);
    void drawCardFrame(Canvas& canvas, const RECT& rc, const wchar_t* title, ui::Icon icon, Color accent,
                       State state);
    void drawBig(Canvas& canvas, const RECT& rc, const std::wstring& value, const std::wstring& unit,
                 Color color);
    void drawMetrics(Canvas& canvas, const RECT& rc, const std::wstring captions[3],
                     const std::wstring values[3], const Color colors[3]);
    void drawDetail(Canvas& canvas, const RECT& rc, const std::wstring& text);
    void drawSparkline(Canvas& canvas, const RectF& area, const std::vector<float>& values, Color color);
    void drawRamCard(Canvas& canvas);
    void drawCpuCard(Canvas& canvas);
    void drawDiskCard(Canvas& canvas);
    void drawGpuCard(Canvas& canvas);
    void drawFooter(Canvas& canvas);
    void drawWidgets(Canvas& canvas, const RECT& clip);

    // input
    int widgetAt(POINT pt) const;
    void onMouseMove(POINT pt);
    void onLeftDown(POINT pt);
    void onLeftUp(POINT pt);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

void BenchWindow::initWidgets() {
    using ui::ButtonStyle;
    using ui::Icon;
    const auto add = [this](int id, const wchar_t* text, ButtonStyle style, Icon icon) {
        Widget w;
        w.id = id;
        w.text = text;
        w.style = style;
        w.icon = icon;
        widgets_.push_back(std::move(w));
    };
    widgets_.clear();
    add(kStartAll, L"Iniciar todos", ButtonStyle::Primary, Icon::Play);
    for (int i = 0; i < 5; ++i) {
        add(kDuration0 + i, kDurations[i].label, ButtonStyle::Glass, Icon::None);
    }
    add(kRamToggle, L"Iniciar", ButtonStyle::Glass, Icon::Play);
    add(kCpuToggle, L"Iniciar", ButtonStyle::Glass, Icon::Play);
    add(kDiskToggle, L"Iniciar", ButtonStyle::Glass, Icon::Play);
    add(kDiskDrive, L"Unidade C:", ButtonStyle::Ghost, Icon::None);
    add(kGpuBurn, L"Burner (ecrã inteiro)", ButtonStyle::Glass, Icon::Play);
    add(kGpuBench, L"Benchmark 60 s", ButtonStyle::Glass, Icon::Pulse);
    add(kGpuStop, L"Parar", ButtonStyle::Danger, Icon::Cross);
    add(kReport, L"Relatório", ButtonStyle::Ghost, Icon::Info);
    add(kClose, L"Fechar", ButtonStyle::Glass, Icon::None);
}

Widget* BenchWindow::widget(int id) {
    for (Widget& w : widgets_) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

void BenchWindow::place(int id, int x, int y, int w, int h) {
    if (Widget* item = widget(id)) {
        item->rc = RECT{x, y, x + w, y + h};
        item->visible = true;
    }
}

bool BenchWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BenchWindow::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    initWidgets();
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, kDesignW, kDesignH, nullptr, nullptr, instance_,
                            this);
    if (!hwnd_) {
        return false;
    }
    ui::applyDarkFrame(hwnd_);
    fitToScreen();
    layout();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kPollTimer, kPollMs, nullptr);
    return true;
}

void BenchWindow::fitToScreen() {
    if (!hwnd_ || IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        return;
    }
    const UINT dpi = ui::dpiForWindow(hwnd_);
    const RECT work = ui::workAreaFor(hwnd_);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    RECT frame{0, 0, 0, 0};
    ui::adjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)),
                         static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi);
    const int frameW = frame.right - frame.left;
    const int frameH = frame.bottom - frame.top;
    float scale = static_cast<float>(dpi) / 96.0f;
    scale = std::min(scale, static_cast<float>(std::max(200, workW - frameW - 12)) / kDesignW);
    scale = std::min(scale, static_cast<float>(std::max(150, workH - frameH - 12)) / kDesignH);
    scale = std::max(scale, 0.4f);
    fitScale_ = scale;
    const int winW = static_cast<int>(std::lround(kDesignW * scale)) + frameW;
    const int winH = static_cast<int>(std::lround(kDesignH * scale)) + frameH;
    SetWindowPos(hwnd_, nullptr, work.left + std::max(0, (workW - winW) / 2),
                 work.top + std::max(0, (workH - winH) / 2), winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void BenchWindow::layout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        layout(rc.right, rc.bottom);
        invalidate();
    }
}

void BenchWindow::layout(int width, int height) {
    clientW_ = std::max(1, width);
    clientH_ = std::max(1, height);
    scale_ = std::clamp(std::min(static_cast<float>(clientW_) / kDesignW,
                                 static_cast<float>(clientH_) / kDesignH),
                        0.35f, 5.0f);
    for (Widget& w : widgets_) {
        w.visible = false;
    }

    const int margin = px(26.0f);
    const int iconSize = px(38.0f);
    iconRect_ = RECT{margin, px(20.0f), margin + iconSize, px(20.0f) + iconSize};
    if (iconSize != iconSize_) {
        if (icon_) {
            DestroyIcon(icon_);
        }
        icon_ = static_cast<HICON>(
            LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
        iconSize_ = iconSize;
    }

    // Control strip: duration chips · session progress · start/stop everything.
    const int controlTop = px(92.0f);
    const int controlH = px(62.0f);
    controlRect_ = RECT{margin, controlTop, clientW_ - margin, controlTop + controlH};
    const int pad = px(18.0f);
    const int chipH = px(34.0f);
    const int chipW = px(80.0f);
    const int chipGap = px(8.0f);
    const int chipY = controlTop + (controlH - chipH) / 2;
    int x = controlRect_.left + pad + px(84.0f);
    for (int i = 0; i < 5; ++i) {
        place(kDuration0 + i, x, chipY, chipW, chipH);
        x += chipW + chipGap;
    }
    const int startW = px(196.0f);
    const int startH = px(40.0f);
    place(kStartAll, controlRect_.right - pad - startW, controlTop + (controlH - startH) / 2, startW,
          startH);
    progressRect_ = RECT{x + px(14.0f), controlTop, controlRect_.right - pad - startW - px(22.0f),
                         controlTop + controlH};

    // Footer.
    const int footerH = px(40.0f);
    const int footerY = clientH_ - margin - footerH;
    const int closeW = px(124.0f);
    const int reportW = px(140.0f);
    const int gap = px(10.0f);
    place(kClose, clientW_ - margin - closeW, footerY, closeW, footerH);
    place(kReport, clientW_ - margin - closeW - gap - reportW, footerY, reportW, footerH);
    footerTextRect_ = RECT{margin, footerY, clientW_ - margin - closeW - gap - reportW - px(16.0f),
                           footerY + footerH};

    // 2 × 2 cards.
    const int cardsTop = controlRect_.bottom + px(14.0f);
    const int cardsBottom = footerY - px(14.0f);
    const int cardGap = px(14.0f);
    const int cardW = (clientW_ - margin * 2 - cardGap) / 2;
    const int cardH = std::clamp((cardsBottom - cardsTop - cardGap) / 2, px(120.0f), px(300.0f));
    const int gridTop = cardsTop + std::max(0, (cardsBottom - cardsTop - (cardH * 2 + cardGap)) / 2);
    for (int i = 0; i < 4; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const int left = margin + col * (cardW + cardGap);
        const int top = gridTop + row * (cardH + cardGap);
        cards_[i] = RECT{left, top, col == 1 ? clientW_ - margin : left + cardW, top + cardH};
    }
    syncWidgets();
}

// Button texts, styles and placement inside the cards depend on what is running.
void BenchWindow::syncWidgets() {
    const int pad = px(18.0f);
    const int buttonH = px(34.0f);
    const int gap = px(10.0f);
    const auto buttonY = [&](const RECT& card) { return card.bottom - px(14.0f) - buttonH; };
    const auto toggle = [&](int id, bool active) {
        if (Widget* w = widget(id)) {
            w->text = active ? L"Parar" : L"Iniciar";
            w->style = active ? ui::ButtonStyle::Danger : ui::ButtonStyle::Glass;
            w->icon = active ? ui::Icon::Cross : ui::Icon::Play;
        }
    };

    const bool active = anyActive();
    if (Widget* w = widget(kStartAll)) {
        w->text = active ? L"Parar todos" : L"Iniciar todos";
        w->style = active ? ui::ButtonStyle::Danger : ui::ButtonStyle::Primary;
        w->icon = active ? ui::Icon::Cross : ui::Icon::Play;
    }
    for (int i = 0; i < 5; ++i) {
        if (Widget* w = widget(kDuration0 + i)) {
            w->enabled = !allRunning_;
            w->style = i == durationIndex_ ? ui::ButtonStyle::Accent : ui::ButtonStyle::Glass;
        }
    }

    const int toggleW = px(128.0f);
    toggle(kRamToggle, ram_.active());
    place(kRamToggle, cards_[0].left + pad, buttonY(cards_[0]), toggleW, buttonH);
    toggle(kCpuToggle, cpu_.active());
    place(kCpuToggle, cards_[1].left + pad, buttonY(cards_[1]), toggleW, buttonH);

    toggle(kDiskToggle, disk_.active());
    place(kDiskToggle, cards_[2].left + pad, buttonY(cards_[2]), toggleW, buttonH);
    if (Widget* w = widget(kDiskDrive)) {
        const std::wstring letter = driveIndex_ < drives_.size() ? drives_[driveIndex_].letter : L"C:";
        w->text = L"Unidade " + letter;
        w->enabled = !disk_.active() && drives_.size() > 1;
    }
    place(kDiskDrive, cards_[2].left + pad + toggleW + gap, buttonY(cards_[2]), px(132.0f), buttonH);

    if (Widget* w = widget(kGpuBurn)) {
        w->visible = false;
    }
    if (Widget* w = widget(kGpuBench)) {
        w->visible = false;
    }
    if (Widget* w = widget(kGpuStop)) {
        w->visible = false;
    }
    if (gpu_.active()) {
        place(kGpuStop, cards_[3].left + pad, buttonY(cards_[3]), toggleW, buttonH);
    } else {
        const int burnW = px(196.0f);
        place(kGpuBurn, cards_[3].left + pad, buttonY(cards_[3]), burnW, buttonH);
        place(kGpuBench, cards_[3].left + pad + burnW + gap, buttonY(cards_[3]), px(160.0f), buttonH);
    }
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

void BenchWindow::refreshMachineFacts() {
    cpuName_ = CpuBurner::cpuName();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        totalRam_ = memory.ullTotalPhys;
        freeRam_ = memory.ullAvailPhys;
    }
    drives_ = DiskTest::listDrives();
    driveIndex_ = 0;
    GpuTest::queryAdapter(&gpuName_, &gpuVram_, &gpuSoftware_);
}

void BenchWindow::startDisk(bool loopReads) {
    DiskTest::Options options;
    if (driveIndex_ < drives_.size()) {
        options.driveLetter = drives_[driveIndex_].letter;
    }
    options.loopReads = loopReads;
    disk_.start(options);
}

void BenchWindow::startGpu(GpuTest::Mode mode, bool fullscreen) {
    GpuTest::Options options;
    options.mode = mode;
    options.fullscreen = fullscreen;
    options.placeNear = hwnd_;
    gpu_.start(options);
}

void BenchWindow::startAll() {
    if (anyActive()) {
        return;
    }
    allRunning_ = true;
    allStart_ = nowSeconds();
    allDuration_ = static_cast<double>(kDurations[durationIndex_].minutes) * 60.0;
    verdictKind_ = 0;
    verdict_.clear();
    logLine(L"===== Sessão completa: " + std::wstring(kDurations[durationIndex_].label) + L" =====");
    // GPU first (it needs memory for its device); CPU and RAM follow from poll()
    // once it is running, RAM taking whatever memory is left at that point.
    startGpu(GpuTest::Mode::Burn, true);
    startDisk(true);
    pendingCpuRam_ = true;
    syncWidgets();
    invalidate();
}

void BenchWindow::stopAll(bool finished) {
    // Ask everything to wind down first, then wait: the tests stop in parallel.
    ram_.requestStop();
    cpu_.requestStop();
    disk_.requestStop();
    gpu_.requestStop();
    ram_.stop();
    cpu_.stop();
    disk_.stop();
    gpu_.stop();
    if (allRunning_) {
        lastSessionSeconds_ = nowSeconds() - allStart_;
    }
    const bool wasSession = allRunning_;
    allRunning_ = false;
    pendingCpuRam_ = false;
    poll();
    if (wasSession) {
        computeVerdict(finished);
        if (hwnd_) {
            SetForegroundWindow(hwnd_);
        }
    }
    syncWidgets();
    invalidate();
}

void BenchWindow::computeVerdict(bool finished) {
    std::wstring problems;
    const auto add = [&problems](const std::wstring& text) {
        if (!problems.empty()) {
            problems += L" · ";
        }
        problems += text;
    };
    if (ramView_.errors > 0) {
        add(L"RAM: " + formatCount(ramView_.errors) + L" erro(s)");
    }
    if (cpuView_.errors > 0) {
        add(L"CPU: " + formatCount(cpuView_.errors) + L" cálculo(s) errado(s)");
    }
    if (diskView_.verifyErrors > 0 || diskView_.ioErrors > 0) {
        add(L"Disco: " + formatCount(diskView_.verifyErrors) + L" bloco(s) corrompido(s), " +
            formatCount(diskView_.ioErrors) + L" erro(s) de E/S");
    }
    if (gpuView_.deviceResets > 0) {
        add(L"GPU: " + std::to_wstring(gpuView_.deviceResets) + L" reset(s) do controlador");
    }

    const std::wstring time = formatDuration(lastSessionSeconds_);
    if (!problems.empty()) {
        verdictKind_ = 2;
        verdict_ = L"REPROVADO em " + time + L" — " + problems;
    } else {
        std::wstring notes;
        std::wstring open;
        const auto note = [&](const wchar_t* name, State state) {
            if (state == State::Unavailable) {
                notes += std::wstring(notes.empty() ? L" Não correu: " : L", ") + name;
            } else if (state != State::Passed) {
                open += std::wstring(open.empty() ? L" Sem conclusão: " : L", ") + name;
            }
        };
        note(L"RAM", ramView_.state);
        note(L"CPU", cpuView_.state);
        note(L"disco", diskView_.state);
        note(L"GPU", gpuView_.state);
        verdictKind_ = 1;
        verdict_ = std::wstring(finished ? L"APROVADO — sem erros em " : L"Sem erros até à paragem manual (") +
                   time + (finished ? L"." : L").") + (notes.empty() ? L"" : notes + L".") +
                   (open.empty() ? L"" : open + L".");
    }
    logLine(L"Veredicto: " + verdict_);
}

// The PC must not sleep or blank the screen in the middle of a long run.
void BenchWindow::updateKeepAwake() {
    const bool wanted = anyActive();
    if (wanted == keepAwake_) {
        return;
    }
    keepAwake_ = wanted;
    SetThreadExecutionState(wanted ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
                                   : ES_CONTINUOUS);
}

void BenchWindow::poll() {
    ++frame_;
    ramView_ = ram_.stats();
    cpuView_ = cpu_.stats();
    diskView_ = disk_.stats();
    gpuView_ = gpu_.stats();
    ramHist_ = ram_.history();
    cpuHist_ = cpu_.history();
    diskHist_ = disk_.history();
    gpuHist_ = gpu_.history();

    if (!ram_.active() && frame_ % 10 == 0) {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            freeRam_ = memory.ullAvailPhys;
        }
    }

    updateKeepAwake();
    if (gpu_.active()) {
        renderHud();
    }

    if (allRunning_) {
        const double elapsed = nowSeconds() - allStart_;
        if (pendingCpuRam_ && (gpuView_.state != State::Starting || elapsed > 60.0)) {
            pendingCpuRam_ = false;
            startCpu();
            startRam();
        }
        if (allDuration_ > 0.0 && elapsed >= allDuration_) {
            stopAll(true);
            return;
        }
        // Everything ended by itself (e.g. every test unavailable): close the session.
        if (!anyActive() && !pendingCpuRam_ && elapsed > 3.0) {
            stopAll(false);
            return;
        }
    }
    syncWidgets();
    invalidate();
}

void BenchWindow::onCommand(int id) {
    if (id >= kDuration0 && id < kDuration0 + 5) {
        if (!allRunning_) {
            durationIndex_ = id - kDuration0;
        }
        syncWidgets();
        invalidate();
        return;
    }
    switch (id) {
        case kStartAll:
            if (anyActive()) {
                stopAll(false);
            } else {
                startAll();
            }
            break;
        case kRamToggle:
            if (ram_.active()) {
                ram_.stop();
            } else {
                startRam();
            }
            break;
        case kCpuToggle:
            if (cpu_.active()) {
                cpu_.stop();
            } else {
                startCpu();
            }
            break;
        case kDiskToggle:
            if (disk_.active()) {
                disk_.stop();
            } else {
                startDisk(false);
            }
            break;
        case kDiskDrive:
            if (!disk_.active() && !drives_.empty()) {
                driveIndex_ = (driveIndex_ + 1) % drives_.size();
            }
            break;
        case kGpuBurn:
            startGpu(GpuTest::Mode::Burn, true);
            break;
        case kGpuBench:
            startGpu(GpuTest::Mode::Benchmark, false);
            break;
        case kGpuStop:
            gpu_.stop();
            break;
        case kReport:
            saveReport();
            break;
        case kClose:
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        default:
            break;
    }
    poll();
}

std::wstring BenchWindow::buildReport() const {
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
    if (!verdict_.empty()) {
        line(L"VEREDICTO: " + verdict_);
        line(L"");
    }

    line(L"[RAM] " + std::wstring(stateLabel(ramView_.state)) + L" — total " + formatBytes(totalRam_));
    if (ramView_.state != State::Idle) {
        line(L"  Testado: " + formatBytes(ramView_.bytesUnderTest) + L" com " +
             std::to_wstring(ramView_.threads) + L" threads" + (ramView_.locked ? L" (fixada)" : L""));
        line(L"  Cobertura: " + formatNumber(ramView_.coveragePct, 0) + L"%   Passagens: " +
             formatCount(ramView_.passes) + L"   Verificado: " + formatBytes(ramView_.bytesVerified));
        line(L"  Erros: " + formatCount(ramView_.errors) + L"   Tempo: " + formatDuration(ramView_.elapsed));
        for (const RamError& e : ramView_.firstErrors) {
            wchar_t detail[160]{};
            swprintf(detail, 160, L"    offset 0x%llX  esperado %016llX  lido %016llX  (%ls)",
                     static_cast<unsigned long long>(e.offset), static_cast<unsigned long long>(e.expected),
                     static_cast<unsigned long long>(e.actual), RamTest::patternName(e.pattern));
            line(detail);
        }
        if (!ramView_.message.empty()) {
            line(L"  " + ramView_.message);
        }
    }
    line(L"");

    line(L"[CPU] " + std::wstring(stateLabel(cpuView_.state)) + L" — " + cpuName_);
    if (cpuView_.state != State::Idle) {
        line(L"  Threads: " + std::to_wstring(cpuView_.threads) + (cpuView_.avx2 ? L" (AVX2+FMA)" : L" (SSE2)") +
             L"   Iterações: " + formatCount(cpuView_.iterations) + L"   Erros: " +
             formatCount(cpuView_.errors));
        line(L"  Pico: " + formatNumber(cpuView_.peakItersPerSec, 0) + L" iter/s   Tempo: " +
             formatDuration(cpuView_.elapsed));
        if (!cpuView_.message.empty()) {
            line(L"  " + cpuView_.message);
        }
    }
    line(L"");

    const DriveInfo& drive = diskView_.state != State::Idle || drives_.empty()
                                 ? diskView_.drive
                                 : drives_[std::min(driveIndex_, drives_.size() - 1)];
    line(L"[Disco] " + std::wstring(stateLabel(diskView_.state)) + L" — " + drive.letter + L" " + drive.model +
         (drive.kind.empty() ? L"" : L" (" + drive.kind + L")"));
    if (diskView_.state != State::Idle) {
        line(L"  Ficheiro de teste: " + formatBytes(diskView_.fileBytes) + L"                 Leitura MB/s   Escrita MB/s");
        for (const DiskRow& row : diskView_.rows) {
            wchar_t text[160]{};
            swprintf(text, 160, L"  %-14ls %14ls %14ls", row.name, formatMBs(row.readMBs).c_str(),
                     formatMBs(row.writeMBs).c_str());
            line(text);
        }
        line(L"  Verificações completas: " + formatCount(diskView_.verifyCycles) + L"   Blocos corrompidos: " +
             formatCount(diskView_.verifyErrors) + L"   Erros de E/S: " + formatCount(diskView_.ioErrors));
        if (!diskView_.message.empty()) {
            line(L"  " + diskView_.message);
        }
    }
    line(L"");

    line(L"[GPU] " + std::wstring(stateLabel(gpuView_.state)) + L" — " +
         (gpuView_.adapter.empty() ? gpuName_ : gpuView_.adapter));
    if (gpuView_.state != State::Idle) {
        line(L"  Modo: " + std::wstring(gpuView_.benchmark ? L"benchmark 1280×720" : L"burn") + L"   Frames: " +
             formatCount(gpuView_.frames) + L"   Tempo: " + formatDuration(gpuView_.elapsed));
        line(L"  FPS médio: " + formatNumber(gpuView_.avgFps, 1) + L"   mín: " +
             formatNumber(gpuView_.minFps, 1) + L"   máx: " + formatNumber(gpuView_.maxFps, 1));
        if (gpuView_.score > 0) {
            line(L"  Pontuação: " + formatCount(static_cast<uint64_t>(gpuView_.score)));
        }
        line(L"  Resets do controlador: " + std::to_wstring(gpuView_.deviceResets));
        if (!gpuView_.message.empty()) {
            line(L"  " + gpuView_.message);
        }
    }
    return r;
}

void BenchWindow::saveReport() {
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
        MessageBoxW(hwnd_, (L"Não foi possível gravar o relatório em:\r\n" + file.wstring()).c_str(), kTitle,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    out << "\xEF\xBB\xBF" << toUtf8(buildReport());
    out.close();
    ShellExecuteW(hwnd_, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The overlay shown on top of the GPU scene. It is drawn here, on the UI
// thread (GDI+ and the font cache are single-threaded), and handed to the
// renderer as a premultiplied bitmap.
void BenchWindow::renderHud() {
    if (gpuView_.height <= 0) {
        return;
    }
    int w = 0;
    int h = 0;
    std::vector<uint8_t> pixels = buildHud(&w, &h);
    if (!pixels.empty()) {
        gpu_.setHud(w, h, std::move(pixels));
    }
}

// Premultiplied BGRA pixels of the overlay, sized for the GPU window's height.
std::vector<uint8_t> BenchWindow::buildHud(int* width, int* height) {
    const GpuStats& g = gpuView_;
    const float s = std::clamp(static_cast<float>(g.height) / 1080.0f, 0.6f, 2.4f);
    const int w = static_cast<int>(std::lround(700.0f * s));
    const int h = static_cast<int>(std::lround(236.0f * s));
    *width = w;
    *height = h;
    Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppPARGB);
    {
        Canvas canvas(&bitmap, s, true);
        canvas.g().Clear(Color(0, 0, 0, 0));
        const RectF panel(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        canvas.fillRound(panel, 18.0f, Color(168, 9, 6, 22));
        canvas.strokeRound(panel, 18.0f, Color(190, 168, 110, 255), 1.2f);

        const float pad = canvas.s(20.0f);
        const float leftW = canvas.s(290.0f);
        canvas.text(ui::spaced(g.benchmark ? L"Void Burner · benchmark" : L"Void Burner · burn"),
                    RectF(pad, canvas.s(14.0f), leftW, canvas.s(16.0f)), 9.5f, Weight::Semibold,
                    Color(255, 196, 150, 255), ui::kLeft | ui::kVCenter);
        canvas.text(formatNumber(g.fps, g.fps >= 100.0 ? 0 : 1),
                    RectF(pad, canvas.s(32.0f), canvas.s(170.0f), canvas.s(62.0f)), 50.0f, Weight::Semibold,
                    ui::color::text(), ui::kLeft | ui::kVCenter);
        canvas.text(L"FPS", RectF(pad + canvas.s(172.0f), canvas.s(62.0f), canvas.s(60.0f), canvas.s(24.0f)),
                    16.0f, Weight::Semibold, Color(255, 196, 150, 255), ui::kLeft | ui::kVCenter);

        std::wstring line1 = L"média " + formatNumber(g.avgFps, 1) + L" · mín " + formatNumber(g.minFps, 1) +
                             L" · máx " + formatNumber(g.maxFps, 1);
        std::wstring line2 = std::to_wstring(g.width) + L"×" + std::to_wstring(g.height) + L" · interno " +
                             std::to_wstring(g.renderWidth) + L"×" + std::to_wstring(g.renderHeight) + L" · " +
                             formatNumber(g.frameMs, 1) + L" ms";
        std::wstring line3 = g.benchmark ? L"benchmark " + formatDuration(g.elapsed) + L" / " +
                                               formatDuration(g.duration)
                                         : L"tempo " + formatDuration(g.elapsed) + L" · frames " +
                                               formatCount(g.frames);
        const float lineH = canvas.s(19.0f);
        float y = canvas.s(100.0f);
        for (const std::wstring* text : {&line1, &line2, &line3}) {
            canvas.text(*text, RectF(pad, y, leftW + canvas.s(20.0f), lineH), 12.0f, Weight::Regular,
                        Color(255, 208, 214, 235), ui::kLeft | ui::kVCenter | ui::kEllipsis);
            y += lineH;
        }
        canvas.text(g.adapter, RectF(pad, y + canvas.s(2.0f), leftW + canvas.s(20.0f), lineH), 11.0f,
                    Weight::Semibold, Color(255, 150, 163, 196), ui::kLeft | ui::kVCenter | ui::kEllipsis);

        // Right column: the other tests, so the full-screen view shows the whole bench.
        const float rx = pad + leftW + canvas.s(34.0f);
        const float rw = static_cast<float>(w) - rx - pad;
        canvas.text(ui::spaced(L"Sistema"), RectF(rx, canvas.s(14.0f), rw, canvas.s(16.0f)), 9.5f,
                    Weight::Semibold, Color(255, 120, 220, 245), ui::kLeft | ui::kVCenter);
        struct Row {
            const wchar_t* name;
            std::wstring value;
            Color color;
        };
        const auto errorsColor = [](uint64_t errors, State state) {
            if (errors > 0 || state == State::Failed) {
                return ui::color::red();
            }
            return isActive(state) || state == State::Passed ? ui::color::green() : ui::color::muted();
        };
        const Row rows[3] = {
            {L"RAM", isActive(ramView_.state) || ramView_.state == State::Passed || ramView_.state == State::Failed
                         ? formatNumber(ramView_.coveragePct, 0) + L"% · " + formatCount(ramView_.errors) + L" erros"
                         : std::wstring(stateLabel(ramView_.state)),
             errorsColor(ramView_.errors, ramView_.state)},
            {L"CPU", isActive(cpuView_.state) || cpuView_.state == State::Passed || cpuView_.state == State::Failed
                         ? formatNumber(cpuView_.loadPct, 0) + L"% · " + formatCount(cpuView_.errors) + L" erros"
                         : std::wstring(stateLabel(cpuView_.state)),
             errorsColor(cpuView_.errors, cpuView_.state)},
            {L"Disco", isActive(diskView_.state)
                           ? formatRate(diskView_.liveMBs) + L" · " +
                                 formatCount(diskView_.verifyErrors + diskView_.ioErrors) + L" erros"
                           : std::wstring(stateLabel(diskView_.state)),
             errorsColor(diskView_.verifyErrors + diskView_.ioErrors, diskView_.state)},
        };
        float ry = canvas.s(38.0f);
        for (const Row& row : rows) {
            canvas.text(row.name, RectF(rx, ry, canvas.s(54.0f), canvas.s(24.0f)), 12.5f, Weight::Semibold,
                        Color(255, 150, 163, 196), ui::kLeft | ui::kVCenter);
            canvas.text(row.value, RectF(rx + canvas.s(56.0f), ry, rw - canvas.s(56.0f), canvas.s(24.0f)), 13.5f,
                        Weight::Semibold, row.color, ui::kLeft | ui::kVCenter | ui::kEllipsis);
            ry += canvas.s(27.0f);
        }
        if (allRunning_) {
            const double elapsed = nowSeconds() - allStart_;
            const std::wstring session =
                allDuration_ > 0.0 ? L"Sessão " + formatDuration(elapsed) + L" / " + formatDuration(allDuration_)
                                   : L"Sessão " + formatDuration(elapsed) + L" (contínua)";
            canvas.text(session, RectF(rx, ry + canvas.s(4.0f), rw, canvas.s(20.0f)), 12.0f, Weight::Semibold,
                        ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        }
        canvas.text(L"ESC fecha o teste gráfico · F11 janela / ecrã inteiro · H esconde este painel",
                    RectF(pad, static_cast<float>(h) - canvas.s(30.0f), static_cast<float>(w) - pad * 2.0f,
                          canvas.s(18.0f)),
                    10.5f, Weight::Regular, Color(255, 136, 146, 176), ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }

    Gdiplus::BitmapData data{};
    Gdiplus::Rect whole(0, 0, w, h);
    if (bitmap.LockBits(&whole, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
        return {};
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4,
                    static_cast<const uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride,
                    static_cast<size_t>(w) * 4);
    }
    bitmap.UnlockBits(&data);
    return pixels;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void BenchWindow::releaseSurfaces() {
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteDC(backDc_);
        backDc_ = nullptr;
    }
    if (backBitmap_) {
        DeleteObject(backBitmap_);
        backBitmap_ = nullptr;
    }
    if (bgDc_) {
        SelectObject(bgDc_, bgOld_);
        DeleteDC(bgDc_);
        bgDc_ = nullptr;
    }
    if (bgBitmap_) {
        DeleteObject(bgBitmap_);
        bgBitmap_ = nullptr;
    }
    backW_ = backH_ = bgW_ = bgH_ = 0;
}

void BenchWindow::ensureBackground(HDC reference, int width, int height) {
    if (bgBitmap_ != nullptr && bgW_ == width && bgH_ == height) {
        return;
    }
    if (bgDc_) {
        SelectObject(bgDc_, bgOld_);
        DeleteDC(bgDc_);
        bgDc_ = nullptr;
    }
    if (bgBitmap_) {
        DeleteObject(bgBitmap_);
        bgBitmap_ = nullptr;
    }
    bgBitmap_ = ui::renderBackground(reference, width, height);
    bgDc_ = CreateCompatibleDC(reference);
    if (bgDc_ && bgBitmap_) {
        bgOld_ = SelectObject(bgDc_, bgBitmap_);
    }
    bgW_ = width;
    bgH_ = height;
}

void BenchWindow::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd_, &ps);
        return;
    }
    if (backBitmap_ == nullptr || backW_ != w || backH_ != h) {
        if (backDc_) {
            SelectObject(backDc_, backOld_);
            DeleteDC(backDc_);
        }
        if (backBitmap_) {
            DeleteObject(backBitmap_);
        }
        backDc_ = CreateCompatibleDC(hdc);
        backBitmap_ = CreateCompatibleBitmap(hdc, w, h);
        backOld_ = SelectObject(backDc_, backBitmap_);
        backW_ = w;
        backH_ = h;
    }
    RECT clip = ps.rcPaint;
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        clip = rc;
    }
    render(backDc_, w, h, clip);
    BitBlt(hdc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, backDc_, clip.left,
           clip.top, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

void BenchWindow::render(HDC dc, int width, int height, const RECT& clip) {
    ensureBackground(dc, width, height);
    if (bgDc_) {
        BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, bgDc_, clip.left,
               clip.top, SRCCOPY);
    }
    if (icon_ && intersects(clip, iconRect_)) {
        DrawIconEx(dc, iconRect_.left, iconRect_.top, icon_, iconRect_.right - iconRect_.left,
                   iconRect_.bottom - iconRect_.top, 0, nullptr, DI_NORMAL);
    }
    GdiFlush();

    Canvas canvas(dc, scale_);
    canvas.g().SetClip(
        Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
    drawHeader(canvas);
    drawControl(canvas);
    drawRamCard(canvas);
    drawCpuCard(canvas);
    drawDiskCard(canvas);
    drawGpuCard(canvas);
    drawFooter(canvas);
    drawWidgets(canvas, clip);
    canvas.g().Flush(Gdiplus::FlushIntentionSync);
}

void BenchWindow::drawHeader(Canvas& canvas) {
    const float margin = canvas.s(26.0f);
    const float textX = static_cast<float>(iconRect_.right) + canvas.s(14.0f);
    const std::wstring title = L"Testes nativos";
    const float titleW = canvas.measure(title, 21.0f, Weight::Semibold).Width;
    canvas.text(title, RectF(textX, canvas.s(15.0f), titleW + canvas.s(6.0f), canvas.s(30.0f)), 21.0f,
                Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter);
    // "BETA" tag beside the title.
    const RectF tag(textX + titleW + canvas.s(12.0f), canvas.s(21.0f), canvas.s(50.0f), canvas.s(19.0f));
    canvas.fillRound(tag, 9.5f, ui::withAlpha(ui::color::violet(), 60));
    canvas.strokeRound(tag, 9.5f, ui::withAlpha(ui::color::violet(), 190));
    canvas.text(L"BETA", tag, 9.5f, Weight::Bold, Color(255, 214, 200, 255), ui::kCenter | ui::kVCenter);
    canvas.text(L"Bancada própria — RAM, CPU, disco e gráfica, sem ferramentas externas",
                RectF(textX, canvas.s(45.0f), static_cast<float>(clientW_) - textX - canvas.s(250.0f),
                      canvas.s(20.0f)),
                12.5f, Weight::Regular, ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);

    // Status pill.
    std::wstring label = L"Parado";
    Color dot = ui::color::muted();
    bool pulse = false;
    if (allRunning_) {
        label = L"Sessão · " + formatDuration(nowSeconds() - allStart_);
        dot = ui::color::cyan();
        pulse = true;
    } else if (anyActive()) {
        label = L"A correr";
        dot = ui::color::cyan();
        pulse = true;
    } else if (verdictKind_ == 1) {
        label = L"Aprovado";
        dot = ui::color::green();
    } else if (verdictKind_ == 2) {
        label = L"Reprovado";
        dot = ui::color::red();
    }
    const float pillW = canvas.measure(label, 12.0f, Weight::Semibold).Width + canvas.s(44.0f);
    const float phase = static_cast<float>(frame_ % 10) / 10.0f;
    canvas.pill(RectF(static_cast<float>(clientW_) - margin - pillW, canvas.s(24.0f), pillW, canvas.s(30.0f)),
                label, dot, pulse, phase);
    canvas.divider(margin, static_cast<float>(clientW_) - margin, canvas.s(78.0f));
}

void BenchWindow::drawControl(Canvas& canvas) {
    const RectF r = ui::toRectF(controlRect_);
    canvas.glass(r, 16.0f, 0.9f);
    canvas.text(ui::spaced(L"Duração"), RectF(r.X + canvas.s(18.0f), r.Y, canvas.s(84.0f), r.Height), 9.5f,
                Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);

    // Session progress between the chips and the start button.
    const RectF area = ui::toRectF(progressRect_);
    if (area.Width < canvas.s(60.0f)) {
        return;
    }
    std::wstring text = L"Todos os testes em simultâneo";
    float progress = 0.0f;
    if (allRunning_) {
        const double elapsed = nowSeconds() - allStart_;
        if (allDuration_ > 0.0) {
            text = formatDuration(elapsed) + L" / " + formatDuration(allDuration_);
            progress = static_cast<float>(std::min(1.0, elapsed / allDuration_));
        } else {
            text = formatDuration(elapsed) + L" · até parar";
            progress = -1.0f;
        }
    } else if (lastSessionSeconds_ > 0.0 && verdictKind_ != 0) {
        text = L"Última sessão: " + formatDuration(lastSessionSeconds_);
        progress = 1.0f;
    }
    canvas.text(text, RectF(area.X, area.Y + canvas.s(12.0f), area.Width, canvas.s(18.0f)), 12.0f,
                Weight::Semibold, allRunning_ ? ui::color::text() : ui::color::muted(),
                ui::kLeft | ui::kVCenter | ui::kEllipsis);
    canvas.progressBar(RectF(area.X, area.Y + canvas.s(37.0f), area.Width, canvas.s(7.0f)), progress,
                       static_cast<float>(frame_ % 10) / 10.0f);
}

void BenchWindow::drawCardFrame(Canvas& canvas, const RECT& rc, const wchar_t* title, ui::Icon icon,
                                Color accent, State state) {
    const RectF r = ui::toRectF(rc);
    canvas.glass(r, 18.0f);
    const float pad = canvas.s(18.0f);
    const float iconSize = canvas.s(17.0f);
    canvas.icon(icon, RectF(r.X + pad, r.Y + canvas.s(16.0f), iconSize, iconSize), accent);
    canvas.text(title, RectF(r.X + pad + iconSize + canvas.s(10.0f), r.Y + canvas.s(13.0f), r.Width * 0.55f,
                             canvas.s(22.0f)),
                14.5f, Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);

    const std::wstring label = stateLabel(state);
    const float pillW = canvas.measure(label, 12.0f, Weight::Semibold).Width + canvas.s(42.0f);
    canvas.pill(RectF(r.GetRight() - pad - pillW, r.Y + canvas.s(12.0f), pillW, canvas.s(24.0f)), label,
                stateColor(state, accent), isActive(state), static_cast<float>(frame_ % 10) / 10.0f);
}

void BenchWindow::drawBig(Canvas& canvas, const RECT& rc, const std::wstring& value, const std::wstring& unit,
                          Color color) {
    const RectF r = ui::toRectF(rc);
    const float pad = canvas.s(18.0f);
    const float y = r.Y + canvas.s(42.0f);
    const float h = canvas.s(42.0f);
    const float valueW = canvas.measure(value, 31.0f, Weight::Semibold).Width;
    canvas.text(value, RectF(r.X + pad, y, valueW + canvas.s(6.0f), h), 31.0f, Weight::Semibold, color,
                ui::kLeft | ui::kVCenter);
    canvas.text(unit, RectF(r.X + pad + valueW + canvas.s(10.0f), y + canvas.s(13.0f), canvas.s(150.0f),
                            canvas.s(22.0f)),
                12.5f, Weight::Semibold, ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
}

void BenchWindow::drawMetrics(Canvas& canvas, const RECT& rc, const std::wstring captions[3],
                              const std::wstring values[3], const Color colors[3]) {
    const RectF r = ui::toRectF(rc);
    const float pad = canvas.s(18.0f);
    const float y = r.Y + canvas.s(92.0f);
    const float colW = (r.Width - pad * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float x = r.X + pad + colW * static_cast<float>(i);
        canvas.text(captions[i], RectF(x, y, colW - canvas.s(6.0f), canvas.s(14.0f)), 9.0f, Weight::Semibold,
                    ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        canvas.text(values[i], RectF(x, y + canvas.s(15.0f), colW - canvas.s(6.0f), canvas.s(21.0f)),
                    canvas.fitSize(values[i], 14.0f, 10.5f, Weight::Semibold, colW - canvas.s(6.0f)),
                    Weight::Semibold, colors[i], ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }
}

void BenchWindow::drawDetail(Canvas& canvas, const RECT& rc, const std::wstring& text) {
    const RectF r = ui::toRectF(rc);
    const float pad = canvas.s(18.0f);
    canvas.text(text, RectF(r.X + pad, r.Y + canvas.s(134.0f), r.Width - pad * 2.0f, canvas.s(18.0f)), 11.5f,
                Weight::Regular, ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
}

void BenchWindow::drawSparkline(Canvas& canvas, const RectF& area, const std::vector<float>& values,
                                Color color) {
    if (values.size() < 2 || area.Width < 8.0f || area.Height < 8.0f) {
        return;
    }
    const float peak = *std::max_element(values.begin(), values.end());
    if (peak <= 0.0f) {
        return;
    }
    std::vector<Gdiplus::PointF> points;
    points.reserve(values.size());
    const float stepX = area.Width / static_cast<float>(values.size() - 1);
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = std::clamp(values[i] / peak, 0.0f, 1.0f);
        points.emplace_back(area.X + stepX * static_cast<float>(i),
                            area.GetBottom() - v * (area.Height - canvas.s(3.0f)) - canvas.s(1.5f));
    }
    Gdiplus::GraphicsPath fill;
    fill.AddLines(points.data(), static_cast<INT>(points.size()));
    fill.AddLine(points.back(), Gdiplus::PointF(area.GetRight(), area.GetBottom()));
    fill.AddLine(Gdiplus::PointF(area.GetRight(), area.GetBottom()),
                 Gdiplus::PointF(area.X, area.GetBottom()));
    fill.CloseFigure();
    Gdiplus::LinearGradientBrush brush(Gdiplus::PointF(area.X, area.Y),
                                       Gdiplus::PointF(area.X, area.GetBottom() + 1.0f),
                                       ui::withAlpha(color, 110), ui::withAlpha(color, 0));
    brush.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    canvas.g().FillPath(&brush, &fill);
    Gdiplus::Pen pen(color, std::max(1.2f, canvas.s(1.6f)));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    canvas.g().DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
}

void BenchWindow::drawRamCard(Canvas& canvas) {
    const RECT& rc = cards_[0];
    const RamStats& s = ramView_;
    const Color accent = colorRam();
    drawCardFrame(canvas, rc, L"Memória RAM", ui::Icon::Chip, accent, s.state);
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;

    drawBig(canvas, rc, hasData ? formatCount(static_cast<uint64_t>(s.coveragePct)) + L"%" : L"—",
            L"cobertura", hasData ? ui::color::text() : ui::color::dim());

    const std::wstring captions[3] = {L"ERROS", L"MEMÓRIA EM TESTE", L"VELOCIDADE"};
    const std::wstring values[3] = {
        hasData ? formatCount(s.errors) : L"—",
        hasData ? formatBytes(s.bytesUnderTest) : formatBytes(freeRam_) + L" livres",
        isActive(s.state) ? formatRate(s.mbPerSec) : L"—",
    };
    const Color colors[3] = {
        !hasData ? ui::color::dim() : (s.errors > 0 ? ui::color::red() : ui::color::green()),
        ui::color::text(), ui::color::text()};
    drawMetrics(canvas, rc, captions, values, colors);

    std::wstring detail;
    if (isActive(s.state) && s.state == State::Running) {
        detail = L"Padrão: " + s.pattern + L" · " + std::to_wstring(s.threads) + L" threads · " +
                 formatCount(s.passes) + L" passagens · " + formatBytes(s.bytesVerified) + L" verificados" +
                 (s.locked ? L" · fixada" : L"");
    } else if (!s.message.empty()) {
        detail = s.message;
    } else {
        detail = L"Total " + formatBytes(totalRam_) + L" — testa a memória livre com todos os núcleos.";
    }
    drawDetail(canvas, rc, detail);

    const RectF r = ui::toRectF(rc);
    drawSparkline(canvas,
                  RectF(r.GetRight() - canvas.s(18.0f) - canvas.s(170.0f), r.Y + canvas.s(44.0f),
                        canvas.s(170.0f), canvas.s(40.0f)),
                  ramHist_, accent);
}

void BenchWindow::drawCpuCard(Canvas& canvas) {
    const RECT& rc = cards_[1];
    const CpuStats& s = cpuView_;
    const Color accent = colorCpu();
    drawCardFrame(canvas, rc, L"Processador", ui::Icon::Pulse, accent, s.state);
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;
    const bool running = s.state == State::Running;

    drawBig(canvas, rc, running ? formatNumber(s.loadPct, 0) + L"%" : L"—", L"de carga",
            running ? ui::color::text() : ui::color::dim());

    const std::wstring captions[3] = {L"ERROS", L"DESEMPENHO", L"ITERAÇÕES/S"};
    // Throughput against its own peak: falls when the CPU throttles from heat.
    const bool peakKnown = running && s.peakItersPerSec > 0.0;
    const std::wstring values[3] = {
        hasData ? formatCount(s.errors) : L"—",
        peakKnown ? formatNumber(s.pctOfPeak, 0) + L"% do pico" : L"—",
        running ? formatCount(static_cast<uint64_t>(s.itersPerSec)) : L"—",
    };
    const Color colors[3] = {
        !hasData ? ui::color::dim() : (s.errors > 0 ? ui::color::red() : ui::color::green()),
        peakKnown && s.pctOfPeak < 80.0 ? ui::color::amber() : ui::color::text(), ui::color::text()};
    drawMetrics(canvas, rc, captions, values, colors);

    std::wstring detail = cpuName_;
    if (running) {
        detail += L" · " + std::to_wstring(s.threads) + L" threads · " + (s.avx2 ? L"AVX2+FMA" : L"SSE2");
        if (s.maxMhz > 0) {
            detail += L" · " + std::to_wstring(s.currentMhz) + L"/" + std::to_wstring(s.maxMhz) + L" MHz";
        }
    } else if (!s.message.empty()) {
        detail = s.message;
    }
    drawDetail(canvas, rc, detail);

    const RectF r = ui::toRectF(rc);
    drawSparkline(canvas,
                  RectF(r.GetRight() - canvas.s(18.0f) - canvas.s(150.0f), r.Y + canvas.s(44.0f),
                        canvas.s(150.0f), canvas.s(40.0f)),
                  cpuHist_, accent);
}

void BenchWindow::drawDiskCard(Canvas& canvas) {
    const RECT& rc = cards_[2];
    const DiskStats& s = diskView_;
    const Color accent = colorDisk();
    drawCardFrame(canvas, rc, L"Disco", ui::Icon::Grid, accent, s.state);
    const RectF r = ui::toRectF(rc);
    const float pad = canvas.s(18.0f);

    // Results table (the CrystalDiskMark rows).
    const float nameW = canvas.s(112.0f);
    const float colW = canvas.s(86.0f);
    const float tableX = r.X + pad;
    float y = r.Y + canvas.s(42.0f);
    canvas.text(L"LEITURA MB/s", RectF(tableX + nameW, y, colW, canvas.s(14.0f)), 8.5f, Weight::Semibold,
                ui::color::dim(), ui::kRight | ui::kVCenter);
    canvas.text(L"ESCRITA MB/s", RectF(tableX + nameW + colW, y, colW, canvas.s(14.0f)), 8.5f,
                Weight::Semibold, ui::color::dim(), ui::kRight | ui::kVCenter);
    y += canvas.s(16.0f);
    static const wchar_t* kNames[4] = {L"SEQ1M Q8T1", L"SEQ1M Q1T1", L"RND4K Q32T1", L"RND4K Q1T1"};
    const float rowH = canvas.s(18.5f);
    for (int i = 0; i < 4; ++i) {
        const DiskRow& row = s.rows[i];
        canvas.text(kNames[i], RectF(tableX, y, nameW, rowH), 11.5f, Weight::Semibold, ui::color::muted(),
                    ui::kLeft | ui::kVCenter);
        canvas.text(formatMBs(row.readMBs), RectF(tableX + nameW, y, colW, rowH), 13.0f, Weight::Semibold,
                    row.readMBs >= 0.0 ? ui::color::text() : ui::color::dim(), ui::kRight | ui::kVCenter);
        canvas.text(formatMBs(row.writeMBs), RectF(tableX + nameW + colW, y, colW, rowH), 13.0f,
                    Weight::Semibold, row.writeMBs >= 0.0 ? ui::color::text() : ui::color::dim(),
                    ui::kRight | ui::kVCenter);
        y += rowH;
    }

    // Right side: live rate + graph while it runs, errors otherwise.
    const float rightX = tableX + nameW + colW * 2.0f + canvas.s(22.0f);
    const float rightW = r.GetRight() - pad - rightX;
    if (rightW > canvas.s(70.0f)) {
        const bool running = s.state == State::Running;
        const uint64_t errors = s.verifyErrors + s.ioErrors;
        const bool hasData = s.state != State::Idle && s.state != State::Unavailable;
        canvas.text(running ? L"AGORA" : L"ERROS", RectF(rightX, r.Y + canvas.s(42.0f), rightW, canvas.s(14.0f)),
                    8.5f, Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);
        const std::wstring big = running ? formatRate(s.liveMBs) : (hasData ? formatCount(errors) : L"—");
        const Color bigColor = running ? ui::color::text()
                                       : (!hasData ? ui::color::dim()
                                                   : (errors > 0 ? ui::color::red() : ui::color::green()));
        canvas.text(big, RectF(rightX, r.Y + canvas.s(56.0f), rightW, canvas.s(30.0f)),
                    canvas.fitSize(big, 21.0f, 13.0f, Weight::Semibold, rightW), Weight::Semibold, bigColor,
                    ui::kLeft | ui::kVCenter);
        drawSparkline(canvas, RectF(rightX, r.Y + canvas.s(90.0f), rightW, canvas.s(38.0f)), diskHist_, accent);
    }

    std::wstring detail;
    if (s.state == State::Running) {
        detail = s.phase + L" (" + std::to_wstring(s.phaseIndex) + L"/" + std::to_wstring(s.phaseCount) +
                 L") · " + formatNumber(s.phaseProgress * 100.0, 0) + L"%";
        if (s.verifyCycles > 0) {
            detail += L" · " + formatCount(s.verifyCycles) + L" verificação(ões)";
        }
    } else if (!s.message.empty()) {
        detail = s.message;
    } else if (driveIndex_ < drives_.size()) {
        const DriveInfo& d = drives_[driveIndex_];
        detail = d.letter + L" " + (d.model.empty() ? L"" : d.model + L" · ") +
                 (d.kind.empty() ? L"" : d.kind + L" · ") + formatBytes(d.totalBytes) + L" (" +
                 formatBytes(d.freeBytes) + L" livres)";
    }
    drawDetail(canvas, rc, detail);
}

void BenchWindow::drawGpuCard(Canvas& canvas) {
    const RECT& rc = cards_[3];
    const GpuStats& s = gpuView_;
    const Color accent = colorGpu();
    drawCardFrame(canvas, rc, L"Placa gráfica", ui::Icon::Window, accent, s.state);
    const bool running = s.state == State::Running;
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;

    if (!running && s.score > 0) {
        drawBig(canvas, rc, formatCount(static_cast<uint64_t>(s.score)), L"pontos (benchmark)",
                ui::color::text());
    } else {
        drawBig(canvas, rc, running ? formatNumber(s.fps, s.fps >= 100.0 ? 0 : 1) : L"—", L"FPS",
                running ? ui::color::text() : ui::color::dim());
    }

    const std::wstring captions[3] = {L"RESETS DO DRIVER", L"FPS MÉDIO", L"MÍN / MÁX"};
    const std::wstring values[3] = {
        hasData ? std::to_wstring(s.deviceResets) : L"—",
        hasData && s.frames > 0 ? formatNumber(s.avgFps, 1) : L"—",
        hasData && s.maxFps > 0.0 ? formatNumber(s.minFps, 0) + L" / " + formatNumber(s.maxFps, 0) : L"—",
    };
    const Color colors[3] = {
        !hasData ? ui::color::dim() : (s.deviceResets > 0 ? ui::color::red() : ui::color::green()),
        ui::color::text(), ui::color::text()};
    drawMetrics(canvas, rc, captions, values, colors);

    std::wstring detail;
    if (running) {
        detail = s.adapter + L" · " + std::to_wstring(s.renderWidth) + L"×" + std::to_wstring(s.renderHeight) +
                 (s.benchmark ? L" · benchmark " + formatDuration(s.elapsed) + L"/" + formatDuration(s.duration)
                              : L" · burn " + formatDuration(s.elapsed));
    } else if (!s.message.empty()) {
        detail = s.message;
    } else if (gpuSoftware_) {
        detail = L"Sem controlador gráfico instalado — instala o driver antes de testar.";
    } else {
        detail = gpuName_ + (gpuVram_ > 0 ? L" · " + formatBytes(gpuVram_) + L" VRAM" : L"");
    }
    drawDetail(canvas, rc, detail);

    const RectF r = ui::toRectF(rc);
    drawSparkline(canvas,
                  RectF(r.GetRight() - canvas.s(18.0f) - canvas.s(170.0f), r.Y + canvas.s(44.0f),
                        canvas.s(170.0f), canvas.s(40.0f)),
                  gpuHist_, accent);
}

void BenchWindow::drawFooter(Canvas& canvas) {
    const RectF r = ui::toRectF(footerTextRect_);
    if (r.Width < canvas.s(40.0f)) {
        return;
    }
    std::wstring text;
    Color color = ui::color::muted();
    if (!verdict_.empty()) {
        text = verdict_;
        color = verdictKind_ == 2 ? ui::color::red() : ui::color::green();
    } else if (anyActive()) {
        text = L"A testar… os erros aparecem a vermelho em cada cartão. O PC não entra em suspensão durante "
               L"os testes.";
    } else {
        text = L"Escolhe a duração e \"Iniciar todos\" para o teste completo, ou corre cada teste em separado. "
               L"Registo: %TEMP%\\WinAutoUpdater\\native_tests.log";
    }
    canvas.text(text, r, 12.0f, verdict_.empty() ? Weight::Regular : Weight::Semibold, color,
                ui::kLeft | ui::kVCenter | ui::kWrap | ui::kEllipsis);
}

void BenchWindow::drawWidgets(Canvas& canvas, const RECT& clip) {
    const int glow = px(14.0f);
    for (const Widget& w : widgets_) {
        if (!w.visible || !intersects(clip, inflated(w.rc, glow))) {
            continue;
        }
        ui::ButtonState state;
        state.disabled = !w.enabled;
        state.hover = hoverId_ == w.id;
        state.pressed = pressedId_ == w.id && hoverId_ == w.id;
        canvas.button(ui::toRectF(w.rc), w.text, L"", w.style, state, w.icon);
    }
}

// ---------------------------------------------------------------------------
// Input / messages
// ---------------------------------------------------------------------------

int BenchWindow::widgetAt(POINT pt) const {
    for (const Widget& w : widgets_) {
        if (w.visible && w.enabled && PtInRect(&w.rc, pt)) {
            return w.id;
        }
    }
    return 0;
}

void BenchWindow::onMouseMove(POINT pt) {
    if (!tracking_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        tracking_ = true;
    }
    const int id = widgetAt(pt);
    if (id != hoverId_) {
        hoverId_ = id;
        invalidate();
    }
}

void BenchWindow::onLeftDown(POINT pt) {
    const int id = widgetAt(pt);
    if (id == 0) {
        return;
    }
    pressedId_ = id;
    hoverId_ = id;
    SetCapture(hwnd_);
    invalidate();
}

void BenchWindow::onLeftUp(POINT pt) {
    if (pressedId_ == 0) {
        return;
    }
    const int id = pressedId_;
    pressedId_ = 0;
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    invalidate();
    if (widgetAt(pt) == id) {
        onCommand(id);
    }
}

LRESULT CALLBACK BenchWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BenchWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<BenchWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<BenchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT BenchWindow::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                layout();
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = ui::dpiForWindow(hwnd);
            const float minScale = std::min(fitScale_, std::max(0.5f, 0.62f * static_cast<float>(dpi) / 96.0f));
            RECT frame{0, 0, 0, 0};
            ui::adjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                                 static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(kDesignW * minScale)) + (frame.right - frame.left);
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(kDesignH * minScale)) + (frame.bottom - frame.top);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            fitToScreen();
            layout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_TIMER:
            if (wParam == kPollTimer) {
                poll();
            }
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hoverId_ != 0) {
                hoverId_ = 0;
                invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            onLeftDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_LBUTTONUP:
            onLeftUp(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_CAPTURECHANGED:
            if (pressedId_ != 0) {
                pressedId_ = 0;
                invalidate();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hoverId_ != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CLOSE:
            KillTimer(hwnd, kPollTimer);
            stopAll(false);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int BenchWindow::run() {
    ui::startup();
    refreshMachineFacts();
    if (!create()) {
        MessageBoxW(nullptr, L"Não foi possível criar a janela dos testes nativos.", kTitle,
                    MB_OK | MB_ICONERROR);
        ui::shutdown();
        return 1;
    }
    logLine(L"");
    logLine(L"========== Testes nativos abertos ==========");

    if (options_.autostartMinutes > 0) {
        // Pick the closest preset and start the full session right away.
        int best = 0;
        for (int i = 0; i < 5; ++i) {
            if (kDurations[i].minutes > 0 &&
                std::abs(kDurations[i].minutes - options_.autostartMinutes) <
                    std::abs(kDurations[best].minutes - options_.autostartMinutes)) {
                best = i;
            }
        }
        durationIndex_ = best;
        startAll();
        if (options_.autostartSecondsOverride > 0.0) {
            allDuration_ = options_.autostartSecondsOverride;
        }
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetThreadExecutionState(ES_CONTINUOUS);
    releaseSurfaces();
    if (icon_) {
        DestroyIcon(icon_);
    }
    ui::shutdown();
    return static_cast<int>(msg.wParam);
}

#ifdef SU_UI_SNAPSHOT
int BenchWindow::snapshot(const std::filesystem::path& directory) {
    ui::startup();
    initWidgets();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    CLSID png{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);

    const auto shot = [&](const std::wstring& name, int w, int h) {
        layout(w, h);
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        const RECT clip{0, 0, w, h};
        render(mem, w, h, clip);
        GdiFlush();
        {
            Gdiplus::Bitmap image(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
            const auto file =
                directory / (name + L"_" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L".png");
            image.Save(file.c_str(), &png, nullptr);
        }
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    };

    // Idle.
    cpuName_ = L"Intel(R) Core(TM) i5-6500 CPU @ 3.20GHz";
    totalRam_ = 16ULL << 30;
    freeRam_ = 11ULL << 30;
    DriveInfo drive;
    drive.letter = L"C:";
    drive.model = L"Samsung SSD 860 EVO 250GB";
    drive.kind = L"SSD SATA";
    drive.totalBytes = 250ULL * 1000 * 1000 * 1000;
    drive.freeBytes = 131ULL << 30;
    drives_ = {drive, drive};
    gpuName_ = L"NVIDIA GeForce GTX 1050 Ti";
    gpuVram_ = 4ULL << 30;
    shot(L"bench_idle", 1040, 700);

    // Running session with a fault on the disk.
    const auto wave = [](float base, float amp, float freq) {
        std::vector<float> v;
        for (int i = 0; i < 120; ++i) {
            v.push_back(base + amp * std::sin(static_cast<float>(i) * freq) +
                        amp * 0.3f * std::sin(static_cast<float>(i) * freq * 3.7f));
        }
        return v;
    };
    allRunning_ = true;
    allStart_ = nowSeconds() - 754.0;
    allDuration_ = 1800.0;
    durationIndex_ = 2;

    ramView_.state = State::Running;
    ramView_.bytesUnderTest = 9ULL << 30;
    ramView_.coveragePct = 3412.0;
    ramView_.passes = 4;
    ramView_.threads = 4;
    ramView_.locked = true;
    ramView_.mbPerSec = 8650.0;
    ramView_.bytesVerified = 307ULL << 30;
    ramView_.pattern = L"aleatório invertido";
    ramHist_ = wave(8600.0f, 500.0f, 0.21f);

    cpuView_.state = State::Running;
    cpuView_.cpuName = cpuName_;
    cpuView_.threads = 4;
    cpuView_.avx2 = true;
    cpuView_.loadPct = 100.0;
    cpuView_.itersPerSec = 1284.0;
    cpuView_.peakItersPerSec = 1420.0;
    cpuView_.pctOfPeak = 90.0;
    cpuView_.iterations = 968000;
    cpuView_.currentMhz = 3200;
    cpuView_.maxMhz = 3600;
    cpuHist_ = wave(1300.0f, 90.0f, 0.13f);

    diskView_.state = State::Running;
    diskView_.drive = drive;
    diskView_.fileBytes = 1ULL << 30;
    diskView_.rows[0] = {L"SEQ1M Q8T1", 548.2, 512.7, 0, 0};
    diskView_.rows[1] = {L"SEQ1M Q1T1", 531.9, 498.1, 0, 0};
    diskView_.rows[2] = {L"RND4K Q32T1", 352.4, 311.0, 0, 0};
    diskView_.rows[3] = {L"RND4K Q1T1", 41.37, 98.6, 0, 0};
    diskView_.phase = L"A verificar os dados gravados (ciclo)";
    diskView_.phaseIndex = 3;
    diskView_.phaseCount = 3;
    diskView_.phaseProgress = 0.62;
    diskView_.liveMBs = 540.0;
    diskView_.verifyCycles = 37;
    diskHist_ = wave(420.0f, 130.0f, 0.4f);

    gpuView_.state = State::Running;
    gpuView_.adapter = gpuName_;
    gpuView_.fps = 47.3;
    gpuView_.avgFps = 46.1;
    gpuView_.minFps = 39.0;
    gpuView_.maxFps = 58.0;
    gpuView_.frames = 34760;
    gpuView_.elapsed = 754.0;
    gpuView_.width = 1920;
    gpuView_.height = 1080;
    gpuView_.renderWidth = 1440;
    gpuView_.renderHeight = 810;
    gpuView_.frameMs = 21.1;
    gpuHist_ = wave(46.0f, 5.0f, 0.3f);

    hoverId_ = kRamToggle;
    shot(L"bench_running", 1040, 700);
    shot(L"bench_running", 800, 540);
    shot(L"bench_running", 1480, 900);
    hoverId_ = 0;

    // The HUD overlay, composited over a flat dark-violet frame so it can be reviewed.
    {
        int w = 0;
        int h = 0;
        const std::vector<uint8_t> hud = buildHud(&w, &h);
        if (!hud.empty()) {
            std::vector<uint8_t> flat(hud.size());
            const uint8_t back[3] = {70, 22, 52};  // B, G, R
            for (size_t i = 0; i + 3 < hud.size(); i += 4) {
                const unsigned alpha = hud[i + 3];
                for (int c = 0; c < 3; ++c) {
                    flat[i + c] = static_cast<uint8_t>(
                        std::min(255U, hud[i + c] + back[c] * (255U - alpha) / 255U));
                }
                flat[i + 3] = 255;
            }
            Gdiplus::Bitmap image(w, h, w * 4, PixelFormat32bppRGB, flat.data());
            image.Save((directory / L"bench_hud.png").c_str(), &png, nullptr);
        }
        // And the real thing: scene at 60 % internal resolution, upscaled, HUD on top.
        std::wstring error;
        GpuTest::renderSnapshot(directory / L"gpu_composite.png", 1280, 720, 30.0f, 0.75f, &error, &hud, w,
                                h, 0.6f);
    }

    // Finished with a failure.
    allRunning_ = false;
    lastSessionSeconds_ = 1800.0;
    ramView_.state = State::Failed;
    ramView_.errors = 3;
    ramView_.message = L"3 erro(s) de memória detetado(s).";
    cpuView_.state = State::Passed;
    cpuView_.message = L"Sem erros em 2 310 400 iterações.";
    diskView_.state = State::Passed;
    diskView_.message = L"Sem erros — dados verificados 88 vez(es).";
    gpuView_.state = State::Passed;
    gpuView_.message = L"Sem falhas do controlador em 30:00 (46,2 FPS médios).";
    computeVerdict(true);
    shot(L"bench_failed", 1040, 700);

    releaseSurfaces();
    ui::shutdown();
    return 0;
}
#endif

}  // namespace

int runNativeTests(HINSTANCE instance, const LaunchOptions& options) {
    // One bench at a time: a second launch just brings the first to the front.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"WinAutoUpdaterNativeTestsInstance");
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            if (IsIconic(existing)) {
                ShowWindow(existing, SW_RESTORE);
            }
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }
    BenchWindow window(instance, options);
    return window.run();
}

#ifdef SU_UI_SNAPSHOT
int runNativeSnapshot(HINSTANCE instance, const std::filesystem::path& directory) {
    BenchWindow window(instance, LaunchOptions{});
    return window.snapshot(directory);
}
#endif

}  // namespace nt
