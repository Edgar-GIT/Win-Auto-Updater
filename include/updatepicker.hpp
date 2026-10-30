#pragma once

#include <string>
#include <vector>

#include <windows.h>

struct UpdateChoice {
    std::wstring title;
    bool enabled = true;  // true = green (install), false = red (skip)
};

// Modal dialog: scrollable list of pending updates with green/red toggle squares.
// Returns true if the user pressed OK (choices updated in place).
bool showUpdatePickerDialog(HWND parent, HINSTANCE instance, HFONT font,
                            std::vector<UpdateChoice>* choices);
