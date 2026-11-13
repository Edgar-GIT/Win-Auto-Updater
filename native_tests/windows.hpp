#pragma once

#include "bench.hpp"
#include "panel.hpp"

#include <functional>

// The three kinds of window of the native tests:
//   MonitorBar  — always-on-top strip docked on the left edge of the screen:
//                 temperatures, loads, FPS, progress and errors of everything.
//   DashWindow  — automatic mode: every test at once, verdict at the end.
//   ToolWindow  — manual mode: one small "app" per test, started by hand.

namespace nt {

constexpr const wchar_t* kBenchTitle = L"Win Auto Updater — Testes nativos (beta)";

class MonitorBar : public Panel {
public:
    explicit MonitorBar(Bench& bench) : bench_(bench) {}

    void addWidgets();
    bool open(HINSTANCE instance);
    // Docks the bar on the left edge of the primary monitor's work area.
    void dock();
    // Width in pixels of the docked bar (other windows are placed beside it).
    int width() const { return barWidth_; }
    RECT workArea() const { return work_; }

    // Close everything (the "×" on the bar).
    std::function<void()> onCloseAll;

protected:
    SIZE designSize() const override;
    float scaleFor(int clientWidth, int clientHeight) const override;
    void onLayout() override;
    void onDraw(ui::Canvas& canvas) override;
    void onCommand(int id) override;
    bool onCloseRequest() override;
    void onDisplayChange() override;

private:
    Bench& bench_;
    int barWidth_ = 250;
    RECT work_{};
};

class DashWindow : public Panel {
public:
    explicit DashWindow(Bench& bench) : bench_(bench) {}

    void addWidgets();
    // area = the part of the screen left free by the monitor bar.
    bool open(HINSTANCE instance, const RECT& area);
    std::function<void()> onClosed;

protected:
    SIZE designSize() const override;
    void onLayout() override;
    void onDraw(ui::Canvas& canvas) override;
    void onCommand(int id) override;
    bool onCloseRequest() override;
    void onDestroyed() override;

private:
    void drawHeader(ui::Canvas& canvas);
    void drawSession(ui::Canvas& canvas);
    void drawFooter(ui::Canvas& canvas);

    Bench& bench_;
    RECT cards_[kTestCount]{};
    RECT sessionRect_{};
    RECT footerTextRect_{};
};

class ToolWindow : public Panel {
public:
    ToolWindow(Bench& bench, TestId id) : bench_(bench), id_(id) {}

    void addWidgets();
    bool open(HINSTANCE instance);
    TestId id() const { return id_; }
    std::function<void()> onClosed;

    static SIZE design();

protected:
    SIZE designSize() const override { return design(); }
    void onLayout() override;
    void onDraw(ui::Canvas& canvas) override;
    void onCommand(int id) override;
    bool onCloseRequest() override;
    void onDestroyed() override;

private:
    Bench& bench_;
    TestId id_;
    RECT card_{};
    RECT noteRect_{};
};

}  // namespace nt
