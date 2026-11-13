// Automatic mode: one dashboard with a card per test and the session panel.
// Everything starts by itself, runs for five minutes on the graphics test's
// clock and ends with a verdict.

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

constexpr int kDesignW = 1040;
constexpr int kDesignH = 584;

enum Id : int { kStartStop = 1, kReport, kClose };

constexpr TestId kOrder[kTestCount] = {TestId::Ram, TestId::Cpu, TestId::Disk, TestId::Gpu, TestId::Battery};

}  // namespace

SIZE DashWindow::designSize() const {
    return SIZE{kDesignW, kDesignH};
}

void DashWindow::addWidgets() {
    addWidget(kStartStop, L"Parar", ui::ButtonStyle::Danger, ui::Icon::Cross);
    addWidget(kReport, L"Relatório", ui::ButtonStyle::Ghost, ui::Icon::Info);
    addWidget(kClose, L"Fechar", ui::ButtonStyle::Glass, ui::Icon::None);
}

bool DashWindow::open(HINSTANCE instance, const RECT& area) {
    addWidgets();
    if (!create(instance, L"Win Auto Updater — Testes automáticos (beta)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                WS_EX_APPWINDOW)) {
        return false;
    }
    // As large as the DPI asks for, shrunk to fit beside the monitor bar.
    const SIZE frame = outerSize(0, 0);
    const int availW = std::max<int>(320, (area.right - area.left) - frame.cx - 16);
    const int availH = std::max<int>(200, (area.bottom - area.top) - frame.cy - 16);
    float scale = static_cast<float>(ui::dpiForWindow(hwnd_)) / 96.0f;
    scale = std::min(scale, static_cast<float>(availW) / kDesignW);
    scale = std::min(scale, static_cast<float>(availH) / kDesignH);
    scale = std::max(scale, 0.45f);
    const int clientW = static_cast<int>(std::lround(kDesignW * scale));
    const int clientH = static_cast<int>(std::lround(kDesignH * scale));
    const SIZE outer = outerSize(clientW, clientH);
    const int x = area.left + std::max<int>(0, ((area.right - area.left) - outer.cx) / 2);
    const int y = area.top + std::max<int>(0, ((area.bottom - area.top) - outer.cy) / 2);
    placeClient(x, y, clientW, clientH, true);
    bench_.attach(hwnd_);
    return true;
}

void DashWindow::onLayout() {
    const int margin = px(26.0f);
    const int gap = px(14.0f);

    const int footerH = px(40.0f);
    const int footerY = clientH_ - px(24.0f) - footerH;
    const int closeW = px(124.0f);
    const int reportW = px(140.0f);
    place(kClose, clientW_ - margin - closeW, footerY, closeW, footerH);
    place(kReport, clientW_ - margin - closeW - px(10.0f) - reportW, footerY, reportW, footerH);
    footerTextRect_ = RECT{margin, footerY, clientW_ - margin - closeW - reportW - px(26.0f), footerY + footerH};

    // 3 × 2 grid: the five tests and the session panel.
    const int top = px(92.0f);
    const int bottom = footerY - gap;
    const int cardW = (clientW_ - margin * 2 - gap * 2) / 3;
    const int cardH = std::clamp((bottom - top - gap) / 2, px(120.0f), px(cards::kCardHeight + 60.0f));
    const int gridTop = top + std::max(0, (bottom - top - (cardH * 2 + gap)) / 2);
    for (int i = 0; i < 6; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        const int left = margin + col * (cardW + gap);
        const int y = gridTop + row * (cardH + gap);
        const RECT rc{left, y, col == 2 ? clientW_ - margin : left + cardW, y + cardH};
        if (i < kTestCount) {
            cards_[i] = rc;
        } else {
            sessionRect_ = rc;
        }
    }

    const int buttonW = px(128.0f);
    const int buttonH = px(34.0f);
    place(kStartStop, sessionRect_.right - px(16.0f) - buttonW, sessionRect_.bottom - px(14.0f) - buttonH, buttonW,
          buttonH);
    if (Widget* w = widget(kStartStop)) {
        const bool running = bench_.session.running || bench_.anyActive();
        w->text = running ? L"Parar" : L"Repetir";
        w->style = running ? ui::ButtonStyle::Danger : ui::ButtonStyle::Primary;
        w->icon = running ? ui::Icon::Cross : ui::Icon::Retry;
    }
}

void DashWindow::onCommand(int id) {
    switch (id) {
        case kStartStop:
            if (bench_.session.running || bench_.anyActive()) {
                bench_.stopAll();
            } else {
                bench_.startSession(hwnd_);
            }
            relayout();
            break;
        case kReport:
            bench_.saveReport(hwnd_);
            break;
        case kClose:
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

bool DashWindow::onCloseRequest() {
    bench_.detach(hwnd_);
    bench_.stopAll();
    return true;
}

void DashWindow::onDestroyed() {
    if (onClosed) {
        onClosed();
    }
}

void DashWindow::onDraw(Canvas& canvas) {
    // The start / stop button follows the session state.
    if (Widget* w = widget(kStartStop)) {
        const bool running = bench_.session.running || bench_.anyActive();
        w->text = running ? L"Parar" : L"Repetir";
        w->style = running ? ui::ButtonStyle::Danger : ui::ButtonStyle::Primary;
        w->icon = running ? ui::Icon::Cross : ui::Icon::Retry;
    }
    drawHeader(canvas);
    for (int i = 0; i < kTestCount; ++i) {
        cards::drawCard(canvas, cards_[i], kOrder[i], bench_);
    }
    drawSession(canvas);
    drawFooter(canvas);
}

void DashWindow::drawHeader(Canvas& canvas) {
    const float margin = canvas.s(26.0f);
    const RectF badge(margin, canvas.s(20.0f), canvas.s(38.0f), canvas.s(38.0f));
    canvas.fillRoundGradient(badge, 11.0f, ui::withAlpha(ui::color::violet(), 230), ui::withAlpha(ui::color::cyan(), 230),
                             false);
    canvas.icon(ui::Icon::Pulse, RectF(badge.X + canvas.s(10.0f), badge.Y + canvas.s(10.0f), canvas.s(18.0f), canvas.s(18.0f)),
                ui::color::ink());

    const float textX = badge.GetRight() + canvas.s(14.0f);
    const std::wstring title = L"Testes automáticos";
    const float titleW = canvas.measure(title, 21.0f, Weight::Semibold).Width;
    canvas.text(title, RectF(textX, canvas.s(15.0f), titleW + canvas.s(6.0f), canvas.s(30.0f)), 21.0f,
                Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter);
    const RectF tag(textX + titleW + canvas.s(12.0f), canvas.s(21.0f), canvas.s(50.0f), canvas.s(19.0f));
    canvas.fillRound(tag, 9.5f, ui::withAlpha(ui::color::violet(), 60));
    canvas.strokeRound(tag, 9.5f, ui::withAlpha(ui::color::violet(), 190));
    canvas.text(L"BETA", tag, 9.5f, Weight::Bold, Color(255, 214, 200, 255), ui::kCenter | ui::kVCenter);
    canvas.text(L"RAM, CPU, disco, gráfica e bateria em simultâneo — " + formatDuration(testSeconds()) +
                    L" no relógio do teste gráfico",
                RectF(textX, canvas.s(45.0f), static_cast<float>(clientW_) - textX - canvas.s(250.0f), canvas.s(20.0f)),
                12.5f, Weight::Regular, ui::color::muted(), ui::kLeft | ui::kVCenter | ui::kEllipsis);

    const SessionView& session = bench_.session;
    std::wstring label = L"Parado";
    Color dot = ui::color::muted();
    bool pulse = false;
    if (session.running) {
        label = session.preparing ? L"A preparar" : L"A testar · " + formatDuration(session.testElapsed);
        dot = ui::color::cyan();
        pulse = true;
    } else if (session.verdictKind == 1) {
        label = L"Aprovado";
        dot = ui::color::green();
    } else if (session.verdictKind == 3) {
        label = L"Aprovado com avisos";
        dot = ui::color::amber();
    } else if (session.verdictKind == 2) {
        label = L"Reprovado";
        dot = ui::color::red();
    }
    const float pillW = canvas.measure(label, 12.0f, Weight::Semibold).Width + canvas.s(44.0f);
    canvas.pill(RectF(static_cast<float>(clientW_) - margin - pillW, canvas.s(24.0f), pillW, canvas.s(30.0f)), label,
                dot, pulse, static_cast<float>(bench_.frame % 8) / 8.0f);
    canvas.divider(margin, static_cast<float>(clientW_) - margin, canvas.s(78.0f));
}

// The sixth cell: the test clock as a ring, and what the session found.
void DashWindow::drawSession(Canvas& canvas) {
    const RectF r = ui::toRectF(sessionRect_);
    canvas.glass(r, 18.0f, 1.15f);
    const float pad = canvas.s(16.0f);
    const SessionView& session = bench_.session;
    const float phase = static_cast<float>(bench_.frame % 8) / 8.0f;

    canvas.text(ui::spaced(L"Sessão"), RectF(r.X + pad, r.Y + canvas.s(12.0f), r.Width - pad * 2.0f, canvas.s(18.0f)),
                9.5f, Weight::Semibold, ui::color::cyan(), ui::kLeft | ui::kVCenter);

    // Ring.
    const float radius = canvas.s(44.0f);
    const float cx = r.X + pad + radius + canvas.s(4.0f);
    const float cy = r.Y + canvas.s(40.0f) + radius + canvas.s(4.0f);
    float progress = 0.0f;
    if (session.running) {
        progress = session.preparing ? -1.0f
                                     : static_cast<float>(std::clamp(session.testElapsed / testSeconds(), 0.0, 1.0));
    } else if (session.verdictKind != 0) {
        progress = static_cast<float>(std::clamp(session.testElapsed / testSeconds(), 0.0, 1.0));
    }
    canvas.ring(cx, cy, radius, 7.0f, progress, phase);
    const std::wstring clock = formatDuration(session.testElapsed);
    canvas.text(clock, RectF(cx - radius, cy - canvas.s(17.0f), radius * 2.0f, canvas.s(24.0f)), 17.0f,
                Weight::Semibold, ui::color::text(), ui::kCenter | ui::kVCenter);
    canvas.text(L"de " + formatDuration(testSeconds()),
                RectF(cx - radius, cy + canvas.s(5.0f), radius * 2.0f, canvas.s(15.0f)), 9.5f, Weight::Regular,
                ui::color::muted(), ui::kCenter | ui::kVCenter);

    // Right column.
    const float x = cx + radius + canvas.s(18.0f);
    const float w = r.GetRight() - pad - x;
    std::wstring state = L"PRONTO";
    Color stateColor = ui::color::muted();
    if (session.running) {
        state = session.preparing ? L"A PREPARAR" : L"A TESTAR";
        stateColor = ui::color::cyan();
    } else if (session.verdictKind == 2) {
        state = L"REPROVADO";
        stateColor = ui::color::red();
    } else if (session.verdictKind == 3) {
        state = L"APROVADO*";
        stateColor = ui::color::amber();
    } else if (session.verdictKind == 1) {
        state = L"APROVADO";
        stateColor = ui::color::green();
    }
    canvas.text(state, RectF(x, r.Y + canvas.s(40.0f), w, canvas.s(28.0f)),
                canvas.fitSize(state, 19.0f, 12.0f, Weight::Bold, w), Weight::Bold, stateColor,
                ui::kLeft | ui::kVCenter);

    const uint64_t errors = bench_.errorCount();
    std::wstring pace;
    if (session.running && !session.preparing) {
        pace = bench_.clockRate < 0.97 ? L"relógio a " + formatNumber(bench_.clockRate * 100.0, 0) + L"% (PC lento)"
                                       : std::wstring(L"relógio em tempo real");
    } else if (session.running) {
        pace = L"à espera da cena gráfica";
    } else {
        pace = L"tempo real " + formatDuration(session.realElapsed);
    }
    const std::wstring lines[3] = {
        formatCount(errors) + (errors == 1 ? L" erro" : L" erros"),
        pace,
        L"máx. CPU " + cards::formatTemp(bench_.sensors.cpuTempMaxC) + L" · GPU " +
            cards::formatTemp(bench_.sensors.gpuTempMaxC),
    };
    const Color colors[3] = {errors > 0 ? ui::color::red() : ui::color::green(), ui::color::muted(),
                             ui::color::muted()};
    float y = r.Y + canvas.s(70.0f);
    for (int i = 0; i < 3; ++i) {
        canvas.text(lines[i], RectF(x, y, w, canvas.s(17.0f)), i == 0 ? 12.5f : 10.5f,
                    i == 0 ? Weight::Semibold : Weight::Regular, colors[i], ui::kLeft | ui::kVCenter | ui::kEllipsis);
        y += canvas.s(i == 0 ? 19.0f : 16.0f);
    }
}

void DashWindow::drawFooter(Canvas& canvas) {
    const RectF r = ui::toRectF(footerTextRect_);
    if (r.Width < canvas.s(40.0f)) {
        return;
    }
    const SessionView& session = bench_.session;
    std::wstring text;
    Color color = ui::color::muted();
    if (!session.verdict.empty() && !session.running) {
        text = session.verdict;
        color = session.verdictKind == 2 ? ui::color::red()
                                         : (session.verdictKind == 3 ? ui::color::amber() : ui::color::green());
    } else if (session.running) {
        text = L"A testar… a barra à esquerda mostra temperaturas e FPS por cima do teste gráfico. ESC no ecrã "
               L"do teste gráfico interrompe a sessão.";
    } else {
        text = L"\"Repetir\" corre outra vez todos os testes. Registo: %TEMP%\\WinAutoUpdater\\native_tests.log";
    }
    canvas.text(text, r, 12.0f, session.verdict.empty() || session.running ? Weight::Regular : Weight::Semibold,
                color, ui::kLeft | ui::kVCenter | ui::kWrap | ui::kEllipsis);
}

}  // namespace nt
