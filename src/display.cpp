#include "display.hpp"

#include <windows.h>

#include <string>

namespace {

std::wstring modeToString(const DEVMODEW& mode) {
    return std::to_wstring(mode.dmPelsWidth) + L" x " + std::to_wstring(mode.dmPelsHeight) +
           L" @ " + std::to_wstring(mode.dmBitsPerPel) + L"-bit";
}

}  // namespace

namespace Display {

bool setMaxResolution(std::wstring* detail) {
    const auto fail = [detail](const std::wstring& message) {
        if (detail) {
            *detail = message;
        }
        return false;
    };

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) {
        return fail(L"Unable to read the current display settings.");
    }

    DEVMODEW best = current;
    DWORD bestPixels = current.dmPelsWidth * current.dmPelsHeight;
    DWORD bestBpp = current.dmBitsPerPel;

    for (DWORD index = 0;; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(nullptr, index, &mode)) {
            break;
        }
        const DWORD pixels = mode.dmPelsWidth * mode.dmPelsHeight;
        if (pixels > bestPixels || (pixels == bestPixels && mode.dmBitsPerPel > bestBpp)) {
            bestPixels = pixels;
            bestBpp = mode.dmBitsPerPel;
            best = mode;
        }
    }

    if (best.dmPelsWidth == current.dmPelsWidth && best.dmPelsHeight == current.dmPelsHeight &&
        best.dmBitsPerPel == current.dmBitsPerPel) {
        if (detail) {
            *detail = L"Already at the highest available resolution (" + modeToString(current) + L").";
        }
        return true;
    }

    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    LONG result = ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        best.dmFields |= DM_DISPLAYFREQUENCY;
        result = ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_UPDATEREGISTRY, nullptr);
    }

    switch (result) {
        case DISP_CHANGE_SUCCESSFUL:
            if (detail) {
                *detail = L"Resolution set to " + modeToString(best) + L".";
            }
            return true;
        case DISP_CHANGE_RESTART:
            if (detail) {
                *detail =
                    L"Resolution change to " + modeToString(best) + L" will apply after a restart.";
            }
            return false;
        case DISP_CHANGE_BADMODE:
            return fail(L"The display does not support the requested resolution.");
        case DISP_CHANGE_BADFLAGS:
            return fail(L"The resolution change was rejected (invalid flags).");
        case DISP_CHANGE_BADPARAM:
            return fail(L"The resolution change was rejected (invalid parameters).");
        case DISP_CHANGE_FAILED:
            return fail(L"The display driver refused the resolution change.");
        default:
            return fail(L"Unable to change the display resolution.");
    }
}

}  // namespace Display
