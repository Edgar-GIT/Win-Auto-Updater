#pragma once

// Small GDI+ drawing toolkit shared by the main window and the update picker:
// dark "glass" theme, DPI helpers, scalable text and the handful of widgets the
// app needs. Everything is drawn in design units (96-DPI pixels) multiplied by
// a single scale factor, so the whole UI resizes with the window / screen.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <objidl.h>

#include <algorithm>

// GDI+ headers rely on the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <string>
#include <string_view>

namespace ui {

using Gdiplus::Color;
using Gdiplus::RectF;

// ---- lifetime -------------------------------------------------------------
void startup();
void shutdown();

// ---- DPI / window chrome ----------------------------------------------------
// All resolved at runtime so the exe still loads on the oldest Windows 10 builds.
UINT dpiForWindow(HWND hwnd);
UINT dpiForPoint(POINT pt);
// Non-client size for a window of the given style at the given DPI.
void adjustWindowRect(RECT* rect, DWORD style, DWORD exStyle, UINT dpi);
// Work area (screen minus taskbar) of the monitor nearest to the window/point.
RECT workAreaFor(HWND hwnd);
RECT workAreaForPoint(POINT pt);
// Dark title bar + caption colours matching the theme (no-ops where unsupported).
void applyDarkFrame(HWND hwnd);

// ---- theme ------------------------------------------------------------------
namespace color {
inline Color text() { return Color(255, 234, 240, 250); }
inline Color muted() { return Color(255, 150, 163, 186); }
inline Color dim() { return Color(255, 104, 118, 143); }
inline Color cyan() { return Color(255, 61, 214, 245); }
inline Color blue() { return Color(255, 79, 140, 255); }
inline Color violet() { return Color(255, 150, 130, 255); }
inline Color green() { return Color(255, 74, 222, 128); }
inline Color amber() { return Color(255, 251, 191, 36); }
inline Color red() { return Color(255, 248, 113, 113); }
inline Color ink() { return Color(255, 5, 18, 33); }  // text on bright accents
inline COLORREF frame() { return RGB(10, 15, 30); }
}  // namespace color

inline Color withAlpha(Color c, int alpha) {
    return Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), c.GetR(), c.GetG(), c.GetB());
}

enum class Weight { Regular, Semibold, Bold };

enum TextFlags : unsigned {
    kLeft = 0,
    kCenter = 1,
    kRight = 2,
    kTop = 0,
    kVCenter = 4,
    kBottom = 8,
    kWrap = 16,
    kEllipsis = 32,
    kMono = 64,
};

enum class ButtonStyle { Primary, Accent, Glass, Ghost, Danger };

enum class Icon {
    None,
    Download,
    Play,
    Grid,
    Record,
    Pulse,
    Filter,
    Chip,
    Window,
    Retry,
    Check,
    Cross,
    Info,
    Trash,
    Power,
};

struct ButtonState {
    bool hover = false;
    bool pressed = false;
    bool disabled = false;
};

// Pre-renders the window background (gradient + soft colour glows) at the
// given client size. The caller owns the returned bitmap (DeleteObject).
HBITMAP renderBackground(HDC reference, int width, int height);

inline RectF toRectF(const RECT& r) {
    return RectF(static_cast<Gdiplus::REAL>(r.left), static_cast<Gdiplus::REAL>(r.top),
                 static_cast<Gdiplus::REAL>(r.right - r.left),
                 static_cast<Gdiplus::REAL>(r.bottom - r.top));
}

class Canvas {
public:
    Canvas(HDC hdc, float scale);
    // Draws into a GDI+ bitmap. transparentTarget = the bitmap has an alpha
    // channel that must stay meaningful (overlays): text is then anti-aliased in
    // greyscale, because ClearType needs an opaque background.
    Canvas(Gdiplus::Image* image, float scale, bool transparentTarget);

    Gdiplus::Graphics& g() { return g_; }
    float scale() const { return scale_; }
    // Design units → pixels.
    float s(float du) const { return du * scale_; }

    void fillRound(const RectF& r, float radiusDu, Color fill);
    void strokeRound(const RectF& r, float radiusDu, Color stroke, float widthDu = 1.0f);
    void fillRoundGradient(const RectF& r, float radiusDu, Color from, Color to, bool vertical);
    // Translucent "glass" panel: soft fill, light edge, faint top highlight.
    void glass(const RectF& r, float radiusDu, float strength = 1.0f);
    // Soft glow halo around a rounded rect (used behind accent buttons).
    void glow(const RectF& r, float radiusDu, Color color, float spreadDu);

    void text(std::wstring_view value, const RectF& r, float sizeDu, Weight weight, Color color,
              unsigned flags = kLeft | kVCenter);
    // Size of the text; maxWidth > 0 enables wrapping at that width.
    Gdiplus::SizeF measure(std::wstring_view value, float sizeDu, Weight weight,
                           float maxWidth = 0.0f, unsigned flags = 0);

    // Largest font size in [minDu, sizeDu] at which the text fits maxWidth on one line.
    float fitSize(std::wstring_view value, float sizeDu, float minDu, Weight weight,
                  float maxWidth);

    void icon(Icon icon, const RectF& box, Color color);
    void button(const RectF& r, std::wstring_view label, std::wstring_view sub, ButtonStyle style,
                ButtonState state, Icon icon = Icon::None);
    void toggleSwitch(const RectF& track, bool on, bool disabled, Color onFrom, Color onTo);
    void toggleRow(const RectF& r, std::wstring_view label, std::wstring_view sub, bool on,
                   ButtonState state);
    void statChip(const RectF& r, std::wstring_view caption, int value, Color valueColor);
    // progress in [0,1]; negative = indeterminate. phase animates the sweep.
    void ring(float cx, float cy, float radius, float thicknessDu, float progress, float phase);
    void progressBar(const RectF& r, float progress, float phase);
    void pill(const RectF& r, std::wstring_view label, Color dot, bool pulse, float phase);
    void divider(float x1, float x2, float y);

private:
    Gdiplus::Graphics g_;
    float scale_;
};

// "ESTADO" → "E S T A D O"-style tracking for small caps captions.
std::wstring spaced(std::wstring_view value);

}  // namespace ui
