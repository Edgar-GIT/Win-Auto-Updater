// The monitor bar: a narrow always-on-top strip on the left edge of the screen
// with everything worth watching during a test — temperatures, loads, FPS, the
// test clock and the error count. It stays visible over the full-screen
// graphics test (which is a borderless window, not exclusive full screen).

#include "windows.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace nt {

namespace {

using ui::Canvas;
using ui::Color;
using ui::RectF;
using ui::Weight;

constexpr int kDesignW = 250;
constexpr int kDesignH = 760;

enum Id : int { kStop = 1, kReport, kCloseAll };

// Section heights in design units.
constexpr float kHeaderH = 40.0f;
constexpr float kTimeH = 92.0f;
constexpr float kGpuH = 176.0f;
constexpr float kCpuH = 150.0f;
constexpr float kSmallH = 62.0f;
constexpr float kBatteryH = 46.0f;
constexpr float kFooterH = 76.0f;
constexpr float kGap = 6.0f;

struct Cell {
    const wchar_t* caption;
    std::wstring value;
    Color color;
};

void drawCells(Canvas& canvas, float x, float y, float width, const Cell* cells, int count) {
    const float colW = width / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const float cx = x + colW * static_cast<float>(i);
        const float w = colW - canvas.s(4.0f);
        canvas.text(cells[i].caption, RectF(cx, y - canvas.s(2.0f), w, canvas.s(15.0f)), 7.5f, Weight::Semibold,
                    ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        canvas.text(cells[i].value, RectF(cx, y + canvas.s(12.0f), w, canvas.s(19.0f)),
                    canvas.fitSize(cells[i].value, 13.0f, 9.0f, Weight::Semibold, w), Weight::Semibold,
                    cells[i].color, ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }
}

void drawCaption(Canvas& canvas, const RectF& r, const wchar_t* caption, const std::wstring& detail,
                 Color accent) {
    const float pad = canvas.s(11.0f);
    // Same weight and size as the updater's captions (letter-spaced text is
    // sensitive to the font's metrics).
    const float captionW = canvas.measure(ui::spaced(caption), 9.0f, Weight::Semibold).Width + canvas.s(6.0f);
    canvas.text(ui::spaced(caption), RectF(r.X + pad, r.Y + canvas.s(7.0f), captionW, canvas.s(16.0f)), 9.0f,
                Weight::Semibold, accent, ui::kLeft | ui::kVCenter);
    canvas.text(detail,
                RectF(r.X + pad + captionW + canvas.s(4.0f), r.Y + canvas.s(8.0f),
                      r.Width - pad * 2.0f - captionW - canvas.s(4.0f), canvas.s(14.0f)),
                9.0f, Weight::Regular, ui::color::dim(), ui::kRight | ui::kVCenter | ui::kEllipsis);
}

Color errorColor(uint64_t errors, bool tested) {
    if (errors > 0) {
        return ui::color::red();
    }
    return tested ? ui::color::green() : ui::color::muted();
}

bool tested(State state) {
    return state == State::Running || state == State::Passed || state == State::Failed;
}

}  // namespace

SIZE MonitorBar::designSize() const {
    return SIZE{kDesignW, kDesignH};
}

// The bar is as wide as its scale says and as tall as the screen: the scale
// comes from the width alone.
float MonitorBar::scaleFor(int clientWidth, int) const {
    return std::clamp(static_cast<float>(clientWidth) / static_cast<float>(kDesignW), 0.35f, 5.0f);
}

void MonitorBar::addWidgets() {
    addWidget(kStop, L"Parar", ui::ButtonStyle::Danger, ui::Icon::Cross);
    addWidget(kReport, L"Relatório", ui::ButtonStyle::Glass, ui::Icon::Info);
    addWidget(kCloseAll, L"", ui::ButtonStyle::Ghost, ui::Icon::Cross);
}

bool MonitorBar::open(HINSTANCE instance) {
    addWidgets();
    // Tool window: no taskbar button; never takes the focus from the tests.
    if (!create(instance, L"Monitor — Win Auto Updater", WS_POPUP,
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED)) {
        return false;
    }
    // Slightly see-through, so the scene behind it is still perceived.
    SetLayeredWindowAttributes(hwnd_, 0, 238, LWA_ALPHA);
    dock();
    bench_.attach(hwnd_);
    return true;
}

void MonitorBar::dock() {
    if (hwnd_ == nullptr) {
        return;
    }
    const POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        info.rcWork = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    work_ = info.rcWork;
    const int height = std::max<int>(200, work_.bottom - work_.top);
    const float dpiScale = static_cast<float>(ui::dpiForPoint(POINT{work_.left + 8, work_.top + 8})) / 96.0f;
    // Whole design visible on a short screen; never wider than a fifth of it.
    float scale = std::min(dpiScale, static_cast<float>(height) / static_cast<float>(kDesignH));
    scale = std::min(scale, static_cast<float>(work_.right - work_.left) * 0.22f / static_cast<float>(kDesignW));
    scale = std::clamp(scale, 0.5f, 3.0f);
    barWidth_ = static_cast<int>(std::lround(kDesignW * scale));
    SetWindowPos(hwnd_, HWND_TOPMOST, work_.left, work_.top, barWidth_, height, SWP_NOACTIVATE);
    relayout();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void MonitorBar::onDisplayChange() {
    dock();
}

bool MonitorBar::onCloseRequest() {
    // Alt+F4 on the bar closes the whole bench, like its "×".
    if (onCloseAll) {
        onCloseAll();
    }
    return false;
}

void MonitorBar::onLayout() {
    const int pad = px(8.0f);
    const int close = px(24.0f);
    place(kCloseAll, clientW_ - pad - close, px(8.0f), close, close);

    const int buttonH = px(32.0f);
    const int y = clientH_ - pad - buttonH;
    const int gap = px(6.0f);
    const int half = (clientW_ - pad * 2 - gap) / 2;
    place(kStop, pad, y, half, buttonH);
    place(kReport, pad + half + gap, y, clientW_ - pad * 2 - gap - half, buttonH);
    if (Widget* stop = widget(kStop)) {
        stop->enabled = bench_.anyActive() || bench_.session.running;
    }
}

void MonitorBar::onCommand(int id) {
    switch (id) {
        case kStop:
            bench_.stopAll();
            break;
        case kReport:
            bench_.saveReport(nullptr);
            break;
        case kCloseAll:
            if (onCloseAll) {
                onCloseAll();
            }
            break;
        default:
            break;
    }
}

void MonitorBar::onDraw(Canvas& canvas) {
    if (Widget* stop = widget(kStop)) {
        stop->enabled = bench_.anyActive() || bench_.session.running;
    }
    const SensorSnapshot& s = bench_.sensors;
    const float W = static_cast<float>(clientW_);
    const float H = static_cast<float>(clientH_);
    const float margin = canvas.s(8.0f);
    const float pad = canvas.s(11.0f);
    const float width = W - margin * 2.0f;
    const float inner = width - pad * 2.0f;
    const float phase = static_cast<float>(bench_.frame % 8) / 8.0f;

    // Right edge: a faint line separating the bar from the desktop.
    canvas.fillRound(RectF(W - 1.0f, 0.0f, 1.0f, H), 0.0f, ui::withAlpha(ui::color::violet(), 90));

    // The two graphs absorb whatever height is left on a tall screen.
    const float fixed = kHeaderH + kTimeH + kGpuH + kCpuH + kSmallH * 2.0f + kBatteryH + kFooterH + kGap * 7.0f + 8.0f;
    const float extra = std::clamp((H / canvas.scale() - fixed) / 2.0f, 0.0f, 130.0f);

    // ---- header ----
    canvas.icon(ui::Icon::Pulse, RectF(margin + canvas.s(4.0f), canvas.s(13.0f), canvas.s(15.0f), canvas.s(15.0f)),
                ui::color::violet());
    canvas.text(ui::spaced(L"Monitor"), RectF(margin + canvas.s(26.0f), canvas.s(9.0f), width, canvas.s(14.0f)),
                9.5f, Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter);
    canvas.text(bench_.config().automatic ? L"sessão automática" : L"testes manuais",
                RectF(margin + canvas.s(26.0f), canvas.s(22.0f), width - canvas.s(60.0f), canvas.s(13.0f)), 8.5f,
                Weight::Regular, ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
    float y = canvas.s(kHeaderH);

    // ---- test clock ----
    {
        const RectF r(margin, y, width, canvas.s(kTimeH));
        canvas.glass(r, 14.0f);
        const SessionView& session = bench_.session;

        // What is being timed: the session, or the manual test closest to its end.
        double elapsed = -1.0;
        bool running = false;
        if (session.running) {
            running = true;
            elapsed = session.preparing ? 0.0 : session.testElapsed;
        } else {
            static const TestId kTimed[4] = {TestId::Ram, TestId::Cpu, TestId::Disk, TestId::Gpu};
            for (const TestId id : kTimed) {
                if (bench_.active(id)) {
                    running = true;
                    elapsed = std::max(elapsed, bench_.runElapsed(id));
                }
            }
        }

        std::wstring caption = L"Tempo de teste";
        std::wstring big;
        std::wstring small;
        Color bigColor = ui::color::text();
        float progress = 0.0f;
        if (running) {
            big = formatDuration(std::max(0.0, elapsed));
            progress = static_cast<float>(std::clamp(elapsed / testSeconds(), 0.0, 1.0));
            if (session.running && session.preparing) {
                small = L"a preparar a gráfica…";
                progress = -1.0f;
            } else if (bench_.clockRate < 0.97) {
                small = L"relógio a " + formatNumber(bench_.clockRate * 100.0, 0) + L"% · PC lento";
            } else {
                small = L"em tempo real";
            }
            if (session.running && !session.preparing) {
                small += L" · real " + formatDuration(session.realElapsed);
            }
        } else if (session.verdictKind != 0) {
            caption = L"Resultado";
            big = session.verdictKind == 2 ? L"REPROVADO" : L"APROVADO";
            bigColor = session.verdictKind == 2 ? ui::color::red()
                                                : (session.verdictKind == 3 ? ui::color::amber() : ui::color::green());
            small = formatDuration(session.testElapsed) + L" de teste · " + formatDuration(session.realElapsed) +
                    L" reais";
            progress = 1.0f;
        } else {
            big = L"Pronto";
            bigColor = ui::color::muted();
            small = formatDuration(testSeconds()) + L" por teste";
        }
        drawCaption(canvas, r, caption.c_str(), running ? L"de " + formatDuration(testSeconds()) : L"",
                    ui::color::cyan());
        canvas.text(big, RectF(r.X + pad, r.Y + canvas.s(24.0f), inner, canvas.s(34.0f)),
                    canvas.fitSize(big, 26.0f, 15.0f, Weight::Semibold, inner), Weight::Semibold, bigColor,
                    ui::kLeft | ui::kVCenter);
        canvas.progressBar(RectF(r.X + pad, r.Y + canvas.s(62.0f), inner, canvas.s(5.0f)), progress, phase);
        canvas.text(small, RectF(r.X + pad, r.Y + canvas.s(70.0f), inner, canvas.s(15.0f)), 9.5f, Weight::Regular,
                    ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        y += canvas.s(kTimeH + kGap);
    }

    // ---- graphics card ----
    {
        const GpuStats& g = bench_.gpu;
        const Color accent = cards::accent(TestId::Gpu);
        const RectF r(margin, y, width, canvas.s(kGpuH + extra));
        canvas.glass(r, 14.0f);
        drawCaption(canvas, r, L"Gráfica", g.adapter.empty() ? bench_.adapter.name : g.adapter, accent);

        const bool running = g.state == State::Running;
        const std::wstring fps = running ? formatNumber(g.fps, g.fps >= 100.0 ? 0 : 1) : L"—";
        const float fpsW = canvas.measure(fps, 27.0f, Weight::Semibold).Width;
        canvas.text(fps, RectF(r.X + pad, r.Y + canvas.s(24.0f), fpsW + canvas.s(6.0f), canvas.s(36.0f)), 27.0f,
                    Weight::Semibold, running ? ui::color::text() : ui::color::dim(), ui::kLeft | ui::kVCenter);
        canvas.text(L"FPS", RectF(r.X + pad + fpsW + canvas.s(7.0f), r.Y + canvas.s(36.0f), canvas.s(34.0f),
                                  canvas.s(18.0f)),
                    10.5f, Weight::Bold, accent, ui::kLeft | ui::kVCenter);
        if (g.frames > 0) {
            const float statsX = r.X + pad + fpsW + canvas.s(44.0f);
            const float statsW = r.GetRight() - pad - statsX;
            if (statsW > canvas.s(50.0f)) {
                canvas.text(L"méd " + formatNumber(g.avgFps, 1), RectF(statsX, r.Y + canvas.s(27.0f), statsW, canvas.s(14.0f)),
                            9.5f, Weight::Semibold, ui::color::muted(), ui::kRight | ui::kVCenter);
                canvas.text(L"mín " + formatNumber(g.minFps, 0) + L" · máx " + formatNumber(g.maxFps, 0),
                            RectF(statsX, r.Y + canvas.s(42.0f), statsW, canvas.s(14.0f)), 9.5f, Weight::Regular,
                            ui::color::dim(), ui::kRight | ui::kVCenter);
            }
        }

        const Cell row1[3] = {
            {L"TEMP.", cards::formatTemp(s.gpuTempC), cards::tempColor(s.gpuTempC, 80.0, 92.0)},
            {L"CARGA", hasReading(s.gpuLoadPct) ? formatNumber(s.gpuLoadPct, 0) + L"%" : std::wstring(L"n/d"),
             hasReading(s.gpuLoadPct) ? ui::color::text() : ui::color::dim()},
            {L"VENT.", s.gpuFanRpm >= 0 ? formatCount(static_cast<uint64_t>(s.gpuFanRpm)) + L" rpm" : std::wstring(L"n/d"),
             s.gpuFanRpm >= 0 ? ui::color::text() : ui::color::dim()},
        };
        drawCells(canvas, r.X + pad, r.Y + canvas.s(66.0f), inner, row1, 3);
        const Cell row2[2] = {
            {L"MEMÓRIA DE VÍDEO",
             s.vramTotal > 0 ? formatBytes(s.vramUsed) + L" / " + formatBytes(s.vramTotal) : std::wstring(L"n/d"),
             s.vramTotal > 0 ? ui::color::text() : ui::color::dim()},
            {L"RESETS DO DRIVER", std::to_wstring(g.deviceResets),
             errorColor(static_cast<uint64_t>(std::max(0, g.deviceResets)), tested(g.state))},
        };
        drawCells(canvas, r.X + pad, r.Y + canvas.s(100.0f), inner, row2, 2);

        // Temperature over time (the FurMark-style graph); FPS when the card
        // does not report a temperature.
        const bool haveTemp = bench_.gpuTempHist.size() >= 2;
        const RectF graph(r.X + pad, r.Y + canvas.s(138.0f), inner, r.GetBottom() - canvas.s(8.0f) - (r.Y + canvas.s(138.0f)));
        canvas.fillRound(graph, 6.0f, ui::withAlpha(Color(255, 0, 0, 0), 60));
        if (haveTemp) {
            // The line turns amber / red with the temperature.
            const Color line = s.gpuTempC >= 80.0 ? cards::tempColor(s.gpuTempC, 80.0, 92.0) : accent;
            cards::drawSparkline(canvas, graph, bench_.gpuTempHist, line, 12.0f, false);
        } else {
            cards::drawSparkline(canvas, graph, bench_.gpuHist, accent);
        }
        canvas.text(haveTemp ? L"temperatura" : L"fps",
                    RectF(graph.X + canvas.s(5.0f), graph.Y + canvas.s(1.0f), graph.Width, canvas.s(12.0f)), 7.5f,
                    Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);
        y += canvas.s(kGpuH + extra + kGap);
    }

    // ---- processor ----
    {
        const CpuStats& c = bench_.cpu;
        const Color accent = cards::accent(TestId::Cpu);
        const RectF r(margin, y, width, canvas.s(kCpuH + extra));
        canvas.glass(r, 14.0f);
        drawCaption(canvas, r, L"Processador", bench_.cpuName, accent);

        const std::wstring load = formatNumber(s.cpuLoadPct, 0) + L"%";
        const float loadW = canvas.measure(load, 22.0f, Weight::Semibold).Width;
        canvas.text(load, RectF(r.X + pad, r.Y + canvas.s(24.0f), loadW + canvas.s(6.0f), canvas.s(30.0f)), 22.0f,
                    Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter);
        const float barX = r.X + pad + loadW + canvas.s(10.0f);
        const RectF track(barX, r.Y + canvas.s(36.0f), r.GetRight() - pad - barX, canvas.s(6.0f));
        canvas.fillRound(track, 3.0f, ui::withAlpha(ui::color::text(), 26));
        canvas.fillRound(RectF(track.X, track.Y, track.Width * static_cast<float>(s.cpuLoadPct / 100.0), track.Height),
                         3.0f, accent);

        const bool burning = c.state == State::Running && c.peakItersPerSec > 0.0;
        const Cell row[3] = {
            {L"TEMP.", cards::formatTemp(s.cpuTempC), cards::tempColor(s.cpuTempC, 85.0, 95.0)},
            {L"FREQ.", s.cpuMhz > 0 ? formatNumber(s.cpuMhz / 1000.0, 2) + L" GHz" : std::wstring(L"n/d"),
             s.cpuMhz > 0 ? ui::color::text() : ui::color::dim()},
            {L"DO PICO", burning ? formatNumber(c.pctOfPeak, 0) + L"%" : std::wstring(L"—"),
             burning && c.pctOfPeak < 80.0 ? ui::color::amber() : (burning ? ui::color::text() : ui::color::dim())},
        };
        drawCells(canvas, r.X + pad, r.Y + canvas.s(58.0f), inner, row, 3);
        canvas.text(tested(c.state) ? formatCount(c.errors) + L" erros de cálculo" : std::wstring(L"teste parado"),
                    RectF(r.X + pad, r.Y + canvas.s(92.0f), inner, canvas.s(14.0f)), 9.5f, Weight::Semibold,
                    errorColor(c.errors, tested(c.state)), ui::kLeft | ui::kVCenter | ui::kEllipsis);

        const bool haveTemp = bench_.cpuTempHist.size() >= 2;
        const RectF graph(r.X + pad, r.Y + canvas.s(112.0f), inner, r.GetBottom() - canvas.s(8.0f) - (r.Y + canvas.s(112.0f)));
        canvas.fillRound(graph, 6.0f, ui::withAlpha(Color(255, 0, 0, 0), 60));
        if (haveTemp) {
            const Color line = s.cpuTempC >= 85.0 ? cards::tempColor(s.cpuTempC, 85.0, 95.0) : accent;
            cards::drawSparkline(canvas, graph, bench_.cpuTempHist, line, 12.0f, false);
        } else {
            cards::drawSparkline(canvas, graph, bench_.cpuLoadHist, accent, 100.0f);
        }
        canvas.text(haveTemp ? L"temperatura" : L"carga",
                    RectF(graph.X + canvas.s(5.0f), graph.Y + canvas.s(1.0f), graph.Width, canvas.s(12.0f)), 7.5f,
                    Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);
        y += canvas.s(kCpuH + extra + kGap);
    }

    const auto smallSection = [&](const wchar_t* caption, const std::wstring& detail, Color accent,
                                  const std::wstring& first, const std::wstring& second, Color secondColor,
                                  float height) {
        const RectF r(margin, y, width, canvas.s(height));
        canvas.glass(r, 14.0f);
        drawCaption(canvas, r, caption, detail, accent);
        canvas.text(first, RectF(r.X + pad, r.Y + canvas.s(23.0f), inner, canvas.s(18.0f)),
                    canvas.fitSize(first, 12.5f, 9.0f, Weight::Semibold, inner), Weight::Semibold, ui::color::text(),
                    ui::kLeft | ui::kVCenter | ui::kEllipsis);
        if (!second.empty()) {
            canvas.text(second, RectF(r.X + pad, r.Y + canvas.s(41.0f), inner, canvas.s(15.0f)), 9.5f,
                        Weight::Semibold, secondColor, ui::kLeft | ui::kVCenter | ui::kEllipsis);
        }
        y += canvas.s(height + kGap);
    };

    // ---- memory ----
    {
        const RamStats& m = bench_.ram;
        const double usedPct = s.ramTotal > 0 ? 100.0 * static_cast<double>(s.ramUsed) / static_cast<double>(s.ramTotal) : 0.0;
        smallSection(L"Memória", formatNumber(usedPct, 0) + L"% em uso", cards::accent(TestId::Ram),
                     formatBytes(s.ramUsed) + L" / " + formatBytes(s.ramTotal),
                     tested(m.state) ? L"cobertura " + formatCount(static_cast<uint64_t>(m.coveragePct)) + L"% · " +
                                           formatCount(m.errors) + L" erros"
                                     : std::wstring(L"teste parado"),
                     errorColor(m.errors, tested(m.state)), kSmallH);
    }

    // ---- disk ----
    {
        const DiskStats& d = bench_.disk;
        const uint64_t errors = d.verifyErrors + d.ioErrors;
        std::wstring drive = d.drive.letter;
        if (drive.empty() && bench_.driveIndex < bench_.drives.size()) {
            drive = bench_.drives[bench_.driveIndex].letter;
        }
        std::wstring first = d.state == State::Running ? cards::formatRate(d.liveMBs) : std::wstring(L"—");
        if (hasReading(s.diskTempC)) {
            first += L" · " + cards::formatTemp(s.diskTempC);
        }
        smallSection(L"Disco", drive, cards::accent(TestId::Disk), first,
                     tested(d.state) ? formatCount(errors) + L" erros · " + formatCount(d.verifyCycles) + L" verificações"
                                     : std::wstring(L"teste parado"),
                     errorColor(errors, tested(d.state)), kSmallH);
    }

    // ---- battery ----
    {
        const BatteryInfo& b = s.battery;
        std::wstring first = L"sem bateria";
        if (b.present) {
            first = (b.percent >= 0 ? std::to_wstring(b.percent) + L"%" : std::wstring(L"n/d")) + L" · " +
                    batteryStatusText(b);
            if (b.rateMw != 0) {
                first += L" · " + formatNumber(std::abs(b.rateMw) / 1000.0, 0) + L" W";
            }
        }
        smallSection(L"Bateria", b.present && b.healthPct >= 0.0 ? L"saúde " + formatNumber(b.healthPct, 0) + L"%" : L"",
                     cards::accent(TestId::Battery), first, L"", ui::color::muted(), kBatteryH);
    }

    // ---- errors (always in the same place, just above the buttons) ----
    {
        const uint64_t errors = bench_.errorCount();
        const bool any = tested(bench_.ram.state) || tested(bench_.cpu.state) || tested(bench_.disk.state) ||
                         tested(bench_.gpu.state);
        const float top = H - canvas.s(kFooterH);
        const RectF r(margin, top, width, canvas.s(30.0f));
        canvas.fillRound(r, 10.0f, ui::withAlpha(errorColor(errors, any), errors > 0 ? 60 : 26));
        canvas.strokeRound(r, 10.0f, ui::withAlpha(errorColor(errors, any), 150));
        canvas.text(ui::spaced(L"Erros"), RectF(r.X + pad, r.Y, inner * 0.6f, r.Height), 9.0f, Weight::Semibold,
                    ui::color::muted(), ui::kLeft | ui::kVCenter);
        canvas.text(formatCount(errors), RectF(r.X + pad, r.Y, inner, r.Height), 16.0f, Weight::Bold,
                    errorColor(errors, any), ui::kRight | ui::kVCenter);
    }
}

}  // namespace nt
