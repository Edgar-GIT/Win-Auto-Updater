#pragma once

#include <string>
#include <vector>

#include <windows.h>

struct UpdateChoice {
    std::wstring title;
    bool enabled = true;  // true = green (install), false = red (skip)
};

// Modal dialog: scrollable list of pending updates with green/red switches.
// Returns true if the user pressed "Guardar" (choices updated in place).
bool showUpdatePickerDialog(HWND parent, HINSTANCE instance, std::vector<UpdateChoice>* choices);
