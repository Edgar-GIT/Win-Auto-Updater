// Manual mode: each test is its own small window (its own "app"), started and
// stopped by hand — the native counterpart of opening MemTest, powerMAX,
// CrystalDiskMark, FurMark and BatteryCare side by side.

#include "windows.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace nt {

namespace {

using ui::Canvas;
using ui::RectF;
using ui::Weight;

constexpr int kDesignW = 344;
constexpr int kDesignH = 272;

enum Id : int { kToggle = 1, kDrive };

const wchar_t* windowTitle(TestId id) {
    switch (id) {
        case TestId::Ram:
            return L"Teste de memória — Win Auto Updater";
        case TestId::Cpu:
            return L"Teste de processador — Win Auto Updater";
        case TestId::Disk:
            return L"Teste de disco — Win Auto Updater";
        case TestId::Gpu:
            return L"Teste gráfico — Win Auto Updater";
        case TestId::Battery:
            return L"Bateria — Win Auto Updater";
    }
    return L"Win Auto Updater";
}

}  // namespace

SIZE ToolWindow::design() {
    return SIZE{kDesignW, kDesignH};
}

void ToolWindow::addWidgets() {
    if (id_ != TestId::Battery) {
        addWidget(kToggle, L"Iniciar", ui::ButtonStyle::Primary, ui::Icon::Play);
    }
    if (id_ == TestId::Disk) {
        addWidget(kDrive, L"Unidade C:", ui::ButtonStyle::Ghost, ui::Icon::None);
    }
}

bool ToolWindow::open(HINSTANCE instance) {
    addWidgets();
    if (!create(instance, windowTitle(id_), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, WS_EX_APPWINDOW)) {
        return false;
    }
    bench_.attach(hwnd_);
    return true;
}

void ToolWindow::onLayout() {
    const int margin = px(12.0f);
    const int buttonH = px(36.0f);
    const int buttonY = clientH_ - margin - buttonH;
    card_ = RECT{margin, margin, clientW_ - margin, buttonY - px(10.0f)};
    noteRect_ = RECT{margin + px(4.0f), buttonY, clientW_ - margin, buttonY + buttonH};

    const bool active = bench_.active(id_);
    if (Widget* w = widget(kToggle)) {
        w->text = active ? L"Parar" : (id_ == TestId::Gpu ? L"Iniciar (ecrã inteiro)" : L"Iniciar");
        w->style = active ? ui::ButtonStyle::Danger : ui::ButtonStyle::Primary;
        w->icon = active ? ui::Icon::Cross : ui::Icon::Play;
        const int toggleW = id_ == TestId::Gpu ? px(204.0f) : px(132.0f);
        place(kToggle, margin, buttonY, toggleW, buttonH);
        noteRect_.left = margin + toggleW + px(12.0f);
    }
    if (Widget* w = widget(kDrive)) {
        const std::wstring letter =
            bench_.driveIndex < bench_.drives.size() ? bench_.drives[bench_.driveIndex].letter : L"C:";
        w->text = L"Unidade " + letter;
        w->enabled = !active && bench_.drives.size() > 1;
        const int driveW = px(126.0f);
        place(kDrive, margin + px(132.0f) + px(8.0f), buttonY, driveW, buttonH);
        noteRect_.left = margin + px(132.0f) + px(8.0f) + driveW + px(12.0f);
    }
}

void ToolWindow::onCommand(int id) {
    switch (id) {
        case kToggle:
            if (bench_.active(id_)) {
                bench_.stop(id_);
            } else {
                bench_.start(id_, hwnd_);
            }
            break;
        case kDrive:
            bench_.cycleDrive();
            break;
        default:
            break;
    }
    relayout();
}

bool ToolWindow::onCloseRequest() {
    bench_.detach(hwnd_);
    if (bench_.active(id_)) {
        bench_.stop(id_);
    }
    return true;
}

void ToolWindow::onDestroyed() {
    if (onClosed) {
        onClosed();
    }
}

void ToolWindow::onDraw(Canvas& canvas) {
    // Keep the button in step with the test (it may end by itself).
    const bool active = bench_.active(id_);
    if (Widget* w = widget(kToggle)) {
        w->text = active ? L"Parar" : (id_ == TestId::Gpu ? L"Iniciar (ecrã inteiro)" : L"Iniciar");
        w->style = active ? ui::ButtonStyle::Danger : ui::ButtonStyle::Primary;
        w->icon = active ? ui::Icon::Cross : ui::Icon::Play;
    }
    if (Widget* w = widget(kDrive)) {
        w->enabled = !active && bench_.drives.size() > 1;
    }
    cards::drawCard(canvas, card_, id_, bench_);

    const RectF note = ui::toRectF(noteRect_);
    if (note.Width > canvas.s(60.0f)) {
        std::wstring text;
        switch (id_) {
            case TestId::Battery:
                text = L"Leitura da própria bateria, atualizada a cada 2 segundos.";
                break;
            case TestId::Gpu:
                text = L"ESC fecha";
                break;
            default:
                text = formatDuration(testSeconds()) + L" no relógio do teste gráfico";
                break;
        }
        canvas.text(text, note, 10.0f, Weight::Regular, ui::color::dim(),
                    ui::kLeft | ui::kVCenter | ui::kWrap | ui::kEllipsis);
    }
}

}  // namespace nt
