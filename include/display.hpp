#pragma once

#include <string>

namespace Display {

// Switches the primary display to the highest resolution it supports and
// persists the change in the registry. The change is applied directly (no
// "keep these settings?" prompt) which is the desired auto-confirm behaviour.
bool setMaxResolution(std::wstring* detail);

}  // namespace Display
