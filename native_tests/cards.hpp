#pragma once

#include "bench.hpp"

#include "ui.hpp"

// The card of each test (RAM, CPU, disk, graphics, battery): the same drawing
// is used by the dashboard of the automatic session and by the separate test
// windows of the manual mode.

namespace nt::cards {

// Height (design units) the content of a card needs.
constexpr float kCardHeight = 200.0f;

ui::Color accent(TestId id);
const wchar_t* title(TestId id);
ui::Icon icon(TestId id);

const wchar_t* stateLabel(State state);
ui::Color stateColor(State state, ui::Color accent);

std::wstring formatRate(double mbPerSec);
// "67 °C" or "n/d".
std::wstring formatTemp(double celsius);
ui::Color tempColor(double celsius, double warm, double hot);

// floor > 0 keeps the vertical range at least that tall (flat lines stay flat).
void drawSparkline(ui::Canvas& canvas, const ui::RectF& area, const std::vector<float>& values,
                   ui::Color color, float floor = 0.0f, bool fromZero = true);

void drawCard(ui::Canvas& canvas, const RECT& rc, TestId id, const Bench& bench);

}  // namespace nt::cards
