#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace nt::cards {

using ui::Canvas;
using ui::Color;
using ui::RectF;
using ui::Weight;

Color accent(TestId id) {
    switch (id) {
        case TestId::Ram:
            return ui::color::cyan();
        case TestId::Cpu:
            return ui::color::violet();
        case TestId::Disk:
            return Color(255, 45, 212, 191);
        case TestId::Gpu:
            return Color(255, 232, 121, 249);
        case TestId::Battery:
            return Color(255, 163, 230, 53);
    }
    return ui::color::cyan();
}

const wchar_t* title(TestId id) {
    switch (id) {
        case TestId::Ram:
            return L"Memória RAM";
        case TestId::Cpu:
            return L"Processador";
        case TestId::Disk:
            return L"Disco";
        case TestId::Gpu:
            return L"Placa gráfica";
        case TestId::Battery:
            return L"Bateria";
    }
    return L"";
}

ui::Icon icon(TestId id) {
    switch (id) {
        case TestId::Ram:
            return ui::Icon::Chip;
        case TestId::Cpu:
            return ui::Icon::Pulse;
        case TestId::Disk:
            return ui::Icon::Grid;
        case TestId::Gpu:
            return ui::Icon::Window;
        case TestId::Battery:
            return ui::Icon::Power;
    }
    return ui::Icon::None;
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

Color stateColor(State state, Color accentColor) {
    switch (state) {
        case State::Starting:
            return ui::color::amber();
        case State::Running:
            return accentColor;
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

std::wstring formatTemp(double celsius) {
    return hasReading(celsius) ? formatNumber(celsius, 0) + L" °C" : std::wstring(L"n/d");
}

Color tempColor(double celsius, double warm, double hot) {
    if (!hasReading(celsius)) {
        return ui::color::dim();
    }
    if (celsius >= hot) {
        return ui::color::red();
    }
    if (celsius >= warm) {
        return ui::color::amber();
    }
    return ui::color::text();
}

void drawSparkline(Canvas& canvas, const RectF& area, const std::vector<float>& values, Color color,
                   float floor, bool fromZero) {
    if (values.size() < 2 || area.Width < 8.0f || area.Height < 8.0f) {
        return;
    }
    float high = *std::max_element(values.begin(), values.end());
    float low = fromZero ? 0.0f : *std::min_element(values.begin(), values.end());
    if (!fromZero) {
        // A little head-room so a steady reading is a line in the middle.
        const float span = std::max(high - low, floor);
        const float mid = (high + low) * 0.5f;
        low = mid - span * 0.6f;
        high = mid + span * 0.6f;
    } else {
        high = std::max(high, floor);
    }
    if (high - low <= 0.0f) {
        return;
    }
    std::vector<Gdiplus::PointF> points;
    points.reserve(values.size());
    const float stepX = area.Width / static_cast<float>(values.size() - 1);
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = std::clamp((values[i] - low) / (high - low), 0.0f, 1.0f);
        points.emplace_back(area.X + stepX * static_cast<float>(i),
                            area.GetBottom() - v * (area.Height - canvas.s(3.0f)) - canvas.s(1.5f));
    }
    Gdiplus::GraphicsPath fill;
    fill.AddLines(points.data(), static_cast<INT>(points.size()));
    fill.AddLine(points.back(), Gdiplus::PointF(area.GetRight(), area.GetBottom()));
    fill.AddLine(Gdiplus::PointF(area.GetRight(), area.GetBottom()), Gdiplus::PointF(area.X, area.GetBottom()));
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

namespace {

constexpr float kPad = 16.0f;

struct Metric {
    std::wstring caption;
    std::wstring value;
    Color color;
};

void drawFrame(Canvas& canvas, const RectF& r, TestId id, const std::wstring& pillText, Color pillColor,
               bool pulse, int frame) {
    canvas.glass(r, 18.0f);
    const float pad = canvas.s(kPad);
    const float iconSize = canvas.s(16.0f);
    const Color color = accent(id);
    canvas.icon(icon(id), RectF(r.X + pad, r.Y + canvas.s(15.0f), iconSize, iconSize), color);
    // The pill draws its label at 12 pt after a dot: 38 units of chrome.
    const float pillW = canvas.measure(pillText, 12.0f, Weight::Semibold).Width + canvas.s(46.0f);
    canvas.text(title(id),
                RectF(r.X + pad + iconSize + canvas.s(9.0f), r.Y + canvas.s(12.0f),
                      r.Width - pad * 2.0f - iconSize - canvas.s(15.0f) - pillW, canvas.s(22.0f)),
                14.0f, Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
    canvas.pill(RectF(r.GetRight() - pad - pillW, r.Y + canvas.s(11.0f), pillW, canvas.s(24.0f)), pillText,
                pillColor, pulse, static_cast<float>(frame % 8) / 8.0f);
}

// Returns the right edge of the value + unit, so a graph can sit beside it.
float drawBig(Canvas& canvas, const RectF& r, const std::wstring& value, const std::wstring& unit,
              Color color) {
    const float pad = canvas.s(kPad);
    const float y = r.Y + canvas.s(40.0f);
    const float h = canvas.s(40.0f);
    const float valueW = canvas.measure(value, 29.0f, Weight::Semibold).Width;
    canvas.text(value, RectF(r.X + pad, y, valueW + canvas.s(6.0f), h), 29.0f, Weight::Semibold, color,
                ui::kLeft | ui::kVCenter);
    const float unitW = canvas.measure(unit, 12.0f, Weight::Semibold).Width;
    canvas.text(unit, RectF(r.X + pad + valueW + canvas.s(8.0f), y + canvas.s(12.0f), unitW + canvas.s(6.0f),
                            canvas.s(22.0f)),
                12.0f, Weight::Semibold, ui::color::muted(), ui::kLeft | ui::kVCenter);
    return r.X + pad + valueW + canvas.s(8.0f) + unitW + canvas.s(10.0f);
}

void drawMetrics(Canvas& canvas, const RectF& r, const Metric metrics[3]) {
    const float pad = canvas.s(kPad);
    const float y = r.Y + canvas.s(88.0f);
    const float colW = (r.Width - pad * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float x = r.X + pad + colW * static_cast<float>(i);
        const float w = colW - canvas.s(6.0f);
        canvas.text(metrics[i].caption, RectF(x, y, w, canvas.s(14.0f)), 8.5f, Weight::Semibold,
                    ui::color::dim(), ui::kLeft | ui::kVCenter | ui::kEllipsis);
        canvas.text(metrics[i].value, RectF(x, y + canvas.s(14.0f), w, canvas.s(21.0f)),
                    canvas.fitSize(metrics[i].value, 13.5f, 9.5f, Weight::Semibold, w), Weight::Semibold,
                    metrics[i].color, ui::kLeft | ui::kVCenter | ui::kEllipsis);
    }
}

// Two lines under the metrics; when the second is empty the first may wrap.
void drawDetails(Canvas& canvas, const RectF& r, const std::wstring& first, const std::wstring& second,
                 Color secondColor) {
    const float pad = canvas.s(kPad);
    const float y = r.Y + canvas.s(128.0f);
    const float w = r.Width - pad * 2.0f;
    if (second.empty()) {
        canvas.text(first, RectF(r.X + pad, y, w, canvas.s(36.0f)), 11.0f, Weight::Regular, ui::color::muted(),
                    ui::kLeft | ui::kTop | ui::kWrap | ui::kEllipsis);
        return;
    }
    canvas.text(first, RectF(r.X + pad, y, w, canvas.s(17.0f)), 11.0f, Weight::Regular, ui::color::muted(),
                ui::kLeft | ui::kVCenter | ui::kEllipsis);
    canvas.text(second, RectF(r.X + pad, y + canvas.s(18.0f), w, canvas.s(17.0f)), 11.0f, Weight::Regular,
                secondColor, ui::kLeft | ui::kVCenter | ui::kEllipsis);
}

// Progress through the five test-minutes (thin bar + "02:13 / 05:00").
void drawRunProgress(Canvas& canvas, const RectF& r, TestId id, const Bench& bench) {
    const float pad = canvas.s(kPad);
    const float y = r.Y + canvas.s(172.0f);
    const float textW = canvas.s(92.0f);
    const double progress = bench.runProgress(id);
    const double elapsed = bench.runElapsed(id);
    std::wstring text;
    float fill = 0.0f;
    if (progress >= 0.0) {
        text = formatDuration(elapsed) + L" / " + formatDuration(testSeconds());
        fill = static_cast<float>(progress);
    } else if (elapsed > 0.0) {
        text = formatDuration(elapsed) + L" de teste";
        fill = static_cast<float>(std::clamp(elapsed / testSeconds(), 0.0, 1.0));
    } else {
        text = formatDuration(testSeconds()) + L" de teste";
    }
    canvas.progressBar(RectF(r.X + pad, y + canvas.s(5.0f), r.Width - pad * 2.0f - textW, canvas.s(5.0f)), fill,
                       static_cast<float>(bench.frame % 8) / 8.0f);
    canvas.text(text, RectF(r.GetRight() - pad - textW, y, textW, canvas.s(15.0f)), 10.5f, Weight::Semibold,
                progress >= 0.0 ? ui::color::text() : ui::color::dim(), ui::kRight | ui::kVCenter);
}

RectF sparkArea(Canvas& canvas, const RectF& r, float leftEdge) {
    const float pad = canvas.s(kPad);
    const float width = std::min(canvas.s(150.0f), r.GetRight() - pad - leftEdge);
    return RectF(r.GetRight() - pad - width, r.Y + canvas.s(42.0f), width, canvas.s(38.0f));
}

Color errorColor(bool hasData, uint64_t errors) {
    if (!hasData) {
        return ui::color::dim();
    }
    return errors > 0 ? ui::color::red() : ui::color::green();
}

// ---------------------------------------------------------------------------

void drawRam(Canvas& canvas, const RectF& r, const Bench& bench) {
    const RamStats& s = bench.ram;
    drawFrame(canvas, r, TestId::Ram, stateLabel(s.state), stateColor(s.state, accent(TestId::Ram)),
              isActive(s.state), bench.frame);
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;

    const float edge = drawBig(canvas, r, hasData ? formatCount(static_cast<uint64_t>(s.coveragePct)) + L"%" : L"—",
                               L"cobertura", hasData ? ui::color::text() : ui::color::dim());
    drawSparkline(canvas, sparkArea(canvas, r, edge), bench.ramHist, accent(TestId::Ram));

    const Metric metrics[3] = {
        {L"ERROS", hasData ? formatCount(s.errors) : L"—", errorColor(hasData, s.errors)},
        {L"EM TESTE", hasData ? formatBytes(s.bytesUnderTest) : formatBytes(bench.freeRam) + L" livres",
         ui::color::text()},
        {L"VELOCIDADE", isActive(s.state) ? formatRate(s.mbPerSec) : L"—", ui::color::text()},
    };
    drawMetrics(canvas, r, metrics);

    if (s.state == State::Running) {
        drawDetails(canvas, r,
                    L"Padrão: " + s.pattern + L" · " + std::to_wstring(s.threads) + L" threads" +
                        (s.locked ? L" · fixada" : L""),
                    formatCount(s.passes) + L" passagens · " + formatBytes(s.bytesVerified) + L" verificados",
                    ui::color::muted());
    } else if (!s.message.empty()) {
        drawDetails(canvas, r, s.message, L"", ui::color::muted());
    } else {
        drawDetails(canvas, r, L"Total " + formatBytes(bench.totalRam) + L" instalados.",
                    L"Testa a memória livre com todos os núcleos.", ui::color::dim());
    }
    drawRunProgress(canvas, r, TestId::Ram, bench);
}

void drawCpu(Canvas& canvas, const RectF& r, const Bench& bench) {
    const CpuStats& s = bench.cpu;
    drawFrame(canvas, r, TestId::Cpu, stateLabel(s.state), stateColor(s.state, accent(TestId::Cpu)),
              isActive(s.state), bench.frame);
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;
    const bool running = s.state == State::Running;

    const float edge = drawBig(canvas, r, formatNumber(bench.sensors.cpuLoadPct, 0) + L"%", L"de carga",
                               running ? ui::color::text() : ui::color::muted());
    drawSparkline(canvas, sparkArea(canvas, r, edge), bench.cpuLoadHist, accent(TestId::Cpu), 100.0f);

    // Throughput against its own peak: falls when the CPU throttles from heat.
    const bool peakKnown = running && s.peakItersPerSec > 0.0;
    const double temp = bench.sensors.cpuTempC;
    const Metric metrics[3] = {
        {L"ERROS", hasData ? formatCount(s.errors) : L"—", errorColor(hasData, s.errors)},
        {L"DESEMPENHO", peakKnown ? formatNumber(s.pctOfPeak, 0) + L"% do pico" : L"—",
         peakKnown && s.pctOfPeak < 80.0 ? ui::color::amber() : ui::color::text()},
        {L"TEMPERATURA", formatTemp(temp), tempColor(temp, 85.0, 95.0)},
    };
    drawMetrics(canvas, r, metrics);

    if (running) {
        std::wstring second = std::to_wstring(s.threads) + L" threads · " + (s.avx2 ? L"AVX2+FMA" : L"SSE2");
        if (bench.sensors.cpuMhz > 0) {
            second += L" · " + formatNumber(bench.sensors.cpuMhz / 1000.0, 2) + L" GHz";
        }
        second += L" · " + formatCount(static_cast<uint64_t>(s.itersPerSec)) + L" iter/s";
        drawDetails(canvas, r, bench.cpuName, second, ui::color::muted());
    } else if (!s.message.empty()) {
        drawDetails(canvas, r, s.message, L"", ui::color::muted());
    } else {
        drawDetails(canvas, r, bench.cpuName, L"Um thread por núcleo, com o resultado verificado.",
                    ui::color::dim());
    }
    drawRunProgress(canvas, r, TestId::Cpu, bench);
}

void drawDisk(Canvas& canvas, const RectF& r, const Bench& bench) {
    const DiskStats& s = bench.disk;
    drawFrame(canvas, r, TestId::Disk, stateLabel(s.state), stateColor(s.state, accent(TestId::Disk)),
              isActive(s.state), bench.frame);
    const float pad = canvas.s(kPad);

    // Results table (the CrystalDiskMark rows).
    const float nameW = canvas.s(98.0f);
    const float colW = canvas.s(78.0f);
    const float tableX = r.X + pad;
    float y = r.Y + canvas.s(40.0f);
    canvas.text(L"LEITURA MB/s", RectF(tableX + nameW, y, colW, canvas.s(14.0f)), 8.0f, Weight::Semibold,
                ui::color::dim(), ui::kRight | ui::kVCenter);
    canvas.text(L"ESCRITA MB/s", RectF(tableX + nameW + colW, y, colW, canvas.s(14.0f)), 8.0f, Weight::Semibold,
                ui::color::dim(), ui::kRight | ui::kVCenter);
    y += canvas.s(15.0f);
    static const wchar_t* kNames[4] = {L"SEQ1M Q8T1", L"SEQ1M Q1T1", L"RND4K Q32T1", L"RND4K Q1T1"};
    const auto cell = [](double value) {
        if (value < 0.0) {
            return std::wstring(L"—");
        }
        return formatNumber(value, value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2));
    };
    const float rowH = canvas.s(17.5f);
    for (int i = 0; i < 4; ++i) {
        const DiskRow& row = s.rows[i];
        canvas.text(kNames[i], RectF(tableX, y, nameW, rowH), 11.0f, Weight::Semibold, ui::color::muted(),
                    ui::kLeft | ui::kVCenter);
        canvas.text(cell(row.readMBs), RectF(tableX + nameW, y, colW, rowH), 12.5f, Weight::Semibold,
                    row.readMBs >= 0.0 ? ui::color::text() : ui::color::dim(), ui::kRight | ui::kVCenter);
        canvas.text(cell(row.writeMBs), RectF(tableX + nameW + colW, y, colW, rowH), 12.5f, Weight::Semibold,
                    row.writeMBs >= 0.0 ? ui::color::text() : ui::color::dim(), ui::kRight | ui::kVCenter);
        y += rowH;
    }

    // Beside the table, when the card is wide enough: the live rate.
    const float rightX = tableX + nameW + colW * 2.0f + canvas.s(18.0f);
    const float rightW = r.GetRight() - pad - rightX;
    if (rightW > canvas.s(64.0f)) {
        const bool running = s.state == State::Running;
        canvas.text(L"AGORA", RectF(rightX, r.Y + canvas.s(40.0f), rightW, canvas.s(14.0f)), 8.0f,
                    Weight::Semibold, ui::color::dim(), ui::kLeft | ui::kVCenter);
        const std::wstring live = running ? formatRate(s.liveMBs) : L"—";
        canvas.text(live, RectF(rightX, r.Y + canvas.s(54.0f), rightW, canvas.s(26.0f)),
                    canvas.fitSize(live, 18.0f, 11.0f, Weight::Semibold, rightW), Weight::Semibold,
                    running ? ui::color::text() : ui::color::dim(), ui::kLeft | ui::kVCenter);
        drawSparkline(canvas, RectF(rightX, r.Y + canvas.s(84.0f), rightW, canvas.s(38.0f)), bench.diskHist,
                      accent(TestId::Disk));
    }

    const uint64_t errors = s.verifyErrors + s.ioErrors;
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;
    const double temp = bench.sensors.diskTempC;
    const std::wstring tempText = hasReading(temp) ? L" · " + formatTemp(temp) : L"";
    if (s.state == State::Running) {
        drawDetails(canvas, r,
                    s.phase + L" · " + formatNumber(s.phaseProgress * 100.0, 0) + L"% · " + formatRate(s.liveMBs),
                    formatCount(errors) + L" erros · " + formatCount(s.verifyCycles) + L" verificação(ões)" +
                        tempText,
                    errorColor(true, errors));
    } else if (hasData && !s.message.empty()) {
        drawDetails(canvas, r, s.message, formatCount(errors) + L" erros" + tempText, errorColor(true, errors));
    } else if (!s.message.empty()) {
        drawDetails(canvas, r, s.message, L"", ui::color::muted());
    } else if (bench.driveIndex < bench.drives.size()) {
        const DriveInfo& d = bench.drives[bench.driveIndex];
        drawDetails(canvas, r,
                    d.letter + (d.model.empty() ? L"" : L" " + d.model) + (d.kind.empty() ? L"" : L" · " + d.kind),
                    formatBytes(d.totalBytes) + L" (" + formatBytes(d.freeBytes) + L" livres)" + tempText,
                    ui::color::dim());
    }
    drawRunProgress(canvas, r, TestId::Disk, bench);
}

void drawGpu(Canvas& canvas, const RectF& r, const Bench& bench) {
    const GpuStats& s = bench.gpu;
    drawFrame(canvas, r, TestId::Gpu, stateLabel(s.state), stateColor(s.state, accent(TestId::Gpu)),
              isActive(s.state), bench.frame);
    const bool running = s.state == State::Running;
    const bool hasData = s.state != State::Idle && s.state != State::Unavailable;

    float edge = 0.0f;
    if (!running && s.score > 0) {
        edge = drawBig(canvas, r, formatCount(static_cast<uint64_t>(s.score)), L"pontos", ui::color::text());
    } else {
        edge = drawBig(canvas, r, running ? formatNumber(s.fps, s.fps >= 100.0 ? 0 : 1) : L"—", L"FPS",
                       running ? ui::color::text() : ui::color::dim());
    }
    drawSparkline(canvas, sparkArea(canvas, r, edge), bench.gpuHist, accent(TestId::Gpu));

    const double temp = bench.sensors.gpuTempC;
    const Metric metrics[3] = {
        {L"RESETS", hasData ? std::to_wstring(s.deviceResets) : L"—",
         errorColor(hasData, static_cast<uint64_t>(std::max(0, s.deviceResets)))},
        {L"FPS MÉDIO", hasData && s.frames > 0 ? formatNumber(s.avgFps, 1) : L"—", ui::color::text()},
        {L"TEMPERATURA", formatTemp(temp), tempColor(temp, 80.0, 92.0)},
    };
    drawMetrics(canvas, r, metrics);

    const std::wstring name = s.adapter.empty() ? bench.adapter.name : s.adapter;
    if (running) {
        drawDetails(canvas, r, name,
                    std::to_wstring(s.width) + L"×" + std::to_wstring(s.height) + L" ecrã inteiro · mín " +
                        formatNumber(s.minFps, 0) + L" · máx " + formatNumber(s.maxFps, 0) +
                        (s.fallbackShader ? L" · cena simplificada" : L""),
                    ui::color::muted());
    } else if (!s.message.empty()) {
        drawDetails(canvas, r, s.message, L"", ui::color::muted());
    } else if (bench.haveAdapter && bench.adapter.software) {
        drawDetails(canvas, r, L"Sem controlador gráfico instalado — instala o driver antes de testar.", L"",
                    ui::color::muted());
    } else {
        drawDetails(canvas, r,
                    name + (bench.adapter.vramBytes > 0 ? L" · " + formatBytes(bench.adapter.vramBytes) + L" VRAM"
                                                        : L""),
                    L"Ecrã inteiro na resolução máxima. ESC fecha.", ui::color::dim());
    }
    drawRunProgress(canvas, r, TestId::Gpu, bench);
}

void drawBattery(Canvas& canvas, const RectF& r, const Bench& bench) {
    const BatteryInfo& b = bench.sensors.battery;
    const BatteryGrade grade = gradeBattery(b);
    Color gradeColor = ui::color::muted();
    if (grade == BatteryGrade::Good) {
        gradeColor = ui::color::green();
    } else if (grade == BatteryGrade::Fair) {
        gradeColor = ui::color::amber();
    } else if (grade == BatteryGrade::Poor) {
        gradeColor = ui::color::red();
    }
    drawFrame(canvas, r, TestId::Battery, batteryGradeText(grade), gradeColor, false, bench.frame);
    const float pad = canvas.s(kPad);

    if (!b.present) {
        drawBig(canvas, r, L"—", L"", ui::color::dim());
        const Metric metrics[3] = {{L"CARGA", L"—", ui::color::dim()},
                                   {L"CAPACIDADE", L"—", ui::color::dim()},
                                   {L"CICLOS", L"—", ui::color::dim()}};
        drawMetrics(canvas, r, metrics);
        drawDetails(canvas, r, L"Este computador não tem bateria (ou o Windows não a deteta).", L"",
                    ui::color::muted());
        return;
    }

    const bool health = b.healthPct >= 0.0;
    drawBig(canvas, r, health ? formatNumber(b.healthPct, 0) + L"%" : std::to_wstring(std::max(0, b.percent)) + L"%",
            health ? L"de saúde" : L"de carga", health ? gradeColor : ui::color::text());

    // Charge gauge on the right.
    {
        const float w = canvas.s(86.0f);
        const float h = canvas.s(30.0f);
        const RectF body(r.GetRight() - pad - w - canvas.s(5.0f), r.Y + canvas.s(46.0f), w, h);
        canvas.strokeRound(body, 7.0f, ui::withAlpha(ui::color::text(), 120), 1.2f);
        canvas.fillRound(RectF(body.GetRight() + canvas.s(1.5f), body.Y + h * 0.3f, canvas.s(3.5f), h * 0.4f),
                         1.5f, ui::withAlpha(ui::color::text(), 120));
        const float level = static_cast<float>(std::clamp(b.percent, 0, 100)) / 100.0f;
        const float inset = canvas.s(3.0f);
        const float fillW = (w - inset * 2.0f) * level;
        if (fillW > canvas.s(4.0f)) {
            const Color fill = b.percent <= 15 ? ui::color::red()
                                               : (b.charging ? ui::color::cyan() : accent(TestId::Battery));
            canvas.fillRound(RectF(body.X + inset, body.Y + inset, fillW, h - inset * 2.0f), 4.5f,
                             ui::withAlpha(fill, 200));
        }
        canvas.text(std::to_wstring(std::max(0, b.percent)) + L"%", body, 11.5f, Weight::Bold, ui::color::text(),
                    ui::kCenter | ui::kVCenter);
    }

    const auto wattHours = [&](uint32_t mwh) {
        return b.relativeUnits ? std::to_wstring(mwh) : formatNumber(mwh / 1000.0, 1);
    };
    const Metric metrics[3] = {
        {L"ESTADO", batteryStatusText(b), b.charging ? ui::color::cyan() : ui::color::text()},
        {L"CAPACIDADE",
         b.detailed ? wattHours(b.fullMwh) + L" / " + wattHours(b.designMwh) + (b.relativeUnits ? L"" : L" Wh")
                    : std::wstring(L"—"),
         b.detailed ? ui::color::text() : ui::color::dim()},
        {L"CICLOS", b.cycles > 0 ? std::to_wstring(b.cycles) : std::wstring(L"—"),
         b.cycles > 0 ? ui::color::text() : ui::color::dim()},
    };
    drawMetrics(canvas, r, metrics);

    std::wstring first;
    if (b.rateMw != 0) {
        first = (b.rateMw > 0 ? L"+" : L"−") + formatNumber(std::abs(b.rateMw) / 1000.0, 1) + L" W";
    }
    if (b.voltageMv > 0) {
        first += (first.empty() ? L"" : L" · ") + formatNumber(b.voltageMv / 1000.0, 2) + L" V";
    }
    if (b.discharging && b.secondsLeft > 0) {
        first += (first.empty() ? L"" : L" · ") + std::wstring(L"restam ") + formatDuration(b.secondsLeft);
    }
    if (health) {
        first += (first.empty() ? L"" : L" · ") + std::wstring(L"desgaste ") +
                 formatNumber(100.0 - b.healthPct, 0) + L"%";
    }
    std::wstring second = b.manufacturer;
    if (!b.name.empty()) {
        second += (second.empty() ? L"" : L" ") + b.name;
    }
    if (!b.chemistry.empty()) {
        second += (second.empty() ? L"" : L" · ") + b.chemistry;
    }
    if (first.empty()) {
        first = health ? L"" : L"A bateria não indica as suas capacidades.";
    }
    drawDetails(canvas, r, first, second, ui::color::dim());

    // Health bar: capacity now against the design capacity.
    if (health) {
        const float y = r.Y + canvas.s(172.0f);
        const float textW = canvas.s(92.0f);
        const RectF track(r.X + pad, y + canvas.s(5.0f), r.Width - pad * 2.0f - textW, canvas.s(5.0f));
        canvas.fillRound(track, 2.5f, ui::withAlpha(ui::color::text(), 26));
        canvas.fillRound(RectF(track.X, track.Y, track.Width * static_cast<float>(b.healthPct / 100.0),
                               track.Height),
                         2.5f, gradeColor);
        canvas.text(L"capacidade", RectF(r.GetRight() - pad - textW, y, textW, canvas.s(15.0f)), 10.5f,
                    Weight::Semibold, ui::color::dim(), ui::kRight | ui::kVCenter);
    }
}

}  // namespace

void drawCard(Canvas& canvas, const RECT& rc, TestId id, const Bench& bench) {
    const RectF r = ui::toRectF(rc);
    switch (id) {
        case TestId::Ram:
            drawRam(canvas, r, bench);
            break;
        case TestId::Cpu:
            drawCpu(canvas, r, bench);
            break;
        case TestId::Disk:
            drawDisk(canvas, r, bench);
            break;
        case TestId::Gpu:
            drawGpu(canvas, r, bench);
            break;
        case TestId::Battery:
            drawBattery(canvas, r, bench);
            break;
    }
}

}  // namespace nt::cards
