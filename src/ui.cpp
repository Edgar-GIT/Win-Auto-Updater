#include "ui.hpp"

#include <dwmapi.h>

#include <cmath>
#include <cwctype>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace ui {

namespace {

using Gdiplus::GraphicsPath;
using Gdiplus::LinearGradientBrush;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::REAL;
using Gdiplus::SolidBrush;

constexpr float kPi = 3.14159265358979323846f;

ULONG_PTR g_token = 0;

// ---- dynamic API lookup -------------------------------------------------------

template <typename T>
T lookup(const wchar_t* dll, const char* name) {
    HMODULE module = GetModuleHandleW(dll);
    if (module == nullptr) {
        module = LoadLibraryW(dll);
    }
    if (module == nullptr) {
        return nullptr;
    }
    // Double cast: FARPROC → generic function pointer → target signature.
    return reinterpret_cast<T>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

UINT systemDpi() {
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) {
        ReleaseDC(nullptr, screen);
    }
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

UINT dpiForMonitor(HMONITOR monitor) {
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const Fn fn = lookup<Fn>(L"shcore.dll", "GetDpiForMonitor");
    if (fn != nullptr && monitor != nullptr) {
        UINT x = 0;
        UINT y = 0;
        if (SUCCEEDED(fn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x > 0) {
            return x;
        }
    }
    return systemDpi();
}

RECT workAreaOf(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    RECT fallback{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

// ---- fonts ----------------------------------------------------------------

bool familyAvailable(const wchar_t* name) {
    Gdiplus::FontFamily family(name);
    return family.IsAvailable() != FALSE;
}

struct FontChoice {
    std::wstring regular;
    std::wstring semibold;  // empty → use the regular family in bold
    std::wstring mono;
};

const FontChoice& fontChoice() {
    static const FontChoice choice = [] {
        FontChoice c;
        if (familyAvailable(L"Segoe UI")) {
            c.regular = L"Segoe UI";
            if (familyAvailable(L"Segoe UI Semibold")) {
                c.semibold = L"Segoe UI Semibold";
            }
        } else if (familyAvailable(L"Tahoma")) {
            c.regular = L"Tahoma";
        } else {
            c.regular = L"Arial";
        }
        if (familyAvailable(L"Cascadia Mono")) {
            c.mono = L"Cascadia Mono";
        } else if (familyAvailable(L"Consolas")) {
            c.mono = L"Consolas";
        } else {
            c.mono = L"Courier New";
        }
        return c;
    }();
    return choice;
}

std::map<std::tuple<int, int, bool>, std::unique_ptr<Gdiplus::Font>>& fontCache() {
    static std::map<std::tuple<int, int, bool>, std::unique_ptr<Gdiplus::Font>> cache;
    return cache;
}

Gdiplus::Font* fontFor(float pixels, Weight weight, bool mono) {
    const int key = std::max(4, static_cast<int>(std::lround(pixels * 4.0f)));
    const auto id = std::make_tuple(key, static_cast<int>(weight), mono);
    auto& cache = fontCache();
    const auto found = cache.find(id);
    if (found != cache.end()) {
        return found->second.get();
    }

    const FontChoice& choice = fontChoice();
    std::wstring family = mono ? choice.mono : choice.regular;
    int style = Gdiplus::FontStyleRegular;
    if (weight == Weight::Bold) {
        style = Gdiplus::FontStyleBold;
    } else if (weight == Weight::Semibold) {
        if (!mono && !choice.semibold.empty()) {
            family = choice.semibold;
        } else {
            style = Gdiplus::FontStyleBold;
        }
    }

    auto font = std::make_unique<Gdiplus::Font>(family.c_str(), key / 4.0f, style,
                                                Gdiplus::UnitPixel);
    if (font->GetLastStatus() != Gdiplus::Ok) {
        font = std::make_unique<Gdiplus::Font>(Gdiplus::FontFamily::GenericSansSerif(), key / 4.0f,
                                               style, Gdiplus::UnitPixel);
    }
    Gdiplus::Font* raw = font.get();
    cache.emplace(id, std::move(font));
    return raw;
}

// ---- geometry -------------------------------------------------------------

void addRoundRect(GraphicsPath& path, const RectF& r, float radius) {
    float d = radius * 2.0f;
    d = std::min(d, std::min(r.Width, r.Height));
    if (d < 1.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void softBlob(Gdiplus::Graphics& g, float cx, float cy, float rx, float ry, Color center) {
    GraphicsPath path;
    path.AddEllipse(cx - rx, cy - ry, rx * 2.0f, ry * 2.0f);
    Gdiplus::PathGradientBrush brush(&path);
    brush.SetCenterPoint(PointF(cx, cy));
    brush.SetCenterColor(center);
    Color edge(0, center.GetR(), center.GetG(), center.GetB());
    INT count = 1;
    brush.SetSurroundColors(&edge, &count);
    brush.SetBlendBellShape(1.0f, 1.0f);
    g.FillPath(&brush, &path);
}

// Drops '\r' and puts a space on empty lines so blank lines keep their height.
std::wstring withoutCarriageReturns(std::wstring_view value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t c : value) {
        if (c == L'\r') {
            continue;
        }
        if (c == L'\n' && (out.empty() || out.back() == L'\n')) {
            out.push_back(L' ');
        }
        out.push_back(c);
    }
    return out;
}

void configureFormat(Gdiplus::StringFormat& format, unsigned flags) {
    INT formatFlags = format.GetFormatFlags();
    if ((flags & kWrap) == 0) {
        formatFlags |= Gdiplus::StringFormatFlagsNoWrap;
    }
    format.SetFormatFlags(formatFlags);
    format.SetAlignment((flags & kCenter) != 0
                            ? Gdiplus::StringAlignmentCenter
                            : ((flags & kRight) != 0 ? Gdiplus::StringAlignmentFar
                                                     : Gdiplus::StringAlignmentNear));
    format.SetLineAlignment((flags & kVCenter) != 0
                                ? Gdiplus::StringAlignmentCenter
                                : ((flags & kBottom) != 0 ? Gdiplus::StringAlignmentFar
                                                          : Gdiplus::StringAlignmentNear));
    format.SetTrimming((flags & kEllipsis) != 0 ? Gdiplus::StringTrimmingEllipsisCharacter
                                                : Gdiplus::StringTrimmingNone);
}

}  // namespace

// ---- lifetime ---------------------------------------------------------------

void startup() {
    if (g_token != 0) {
        return;
    }
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_token, &input, nullptr);
}

void shutdown() {
    fontCache().clear();
    if (g_token != 0) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
    }
}

// ---- DPI / chrome -------------------------------------------------------------

UINT dpiForWindow(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static const Fn fn = lookup<Fn>(L"user32.dll", "GetDpiForWindow");
    if (fn != nullptr && hwnd != nullptr) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) {
            return dpi;
        }
    }
    if (hwnd != nullptr) {
        return dpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    }
    return systemDpi();
}

UINT dpiForPoint(POINT pt) {
    return dpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

void adjustWindowRect(RECT* rect, DWORD style, DWORD exStyle, UINT dpi) {
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const Fn fn = lookup<Fn>(L"user32.dll", "AdjustWindowRectExForDpi");
    if (fn != nullptr && fn(rect, style, FALSE, exStyle, dpi)) {
        return;
    }
    AdjustWindowRectEx(rect, style, FALSE, exStyle);
}

RECT workAreaFor(HWND hwnd) {
    return workAreaOf(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

RECT workAreaForPoint(POINT pt) {
    return workAreaOf(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

void applyDarkFrame(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    // Dark title bar: attribute 20 on Windows 10 2004+/11, 19 on 1809–1909.
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    // Windows 11: caption / border colours matching the app background.
    COLORREF caption = color::frame();
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    COLORREF border = RGB(38, 52, 82);
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    COLORREF captionText = RGB(234, 240, 250);
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));
}

// ---- background -------------------------------------------------------------

HBITMAP renderBackground(HDC reference, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);

    // The scene is all smooth gradients: paint it at quarter resolution and
    // stretch it, which is visually identical and ~16x cheaper on old CPUs.
    const int lw = std::max(48, width / 4);
    const int lh = std::max(48, height / 4);
    Gdiplus::Bitmap low(lw, lh, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&low);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        LinearGradientBrush base(PointF(0.0f, 0.0f), PointF(0.0f, static_cast<REAL>(lh)),
                                 Color(255, 8, 12, 24), Color(255, 13, 20, 38));
        base.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.FillRectangle(&base, 0, 0, lw, lh);

        const float w = static_cast<float>(lw);
        const float h = static_cast<float>(lh);
        softBlob(g, w * 0.10f, h * 0.02f, w * 0.62f, h * 0.80f, Color(120, 37, 99, 235));
        softBlob(g, w * 0.96f, h * 0.94f, w * 0.58f, h * 0.78f, Color(105, 124, 58, 237));
        softBlob(g, w * 0.88f, h * 0.04f, w * 0.36f, h * 0.48f, Color(70, 34, 211, 238));
        softBlob(g, w * 0.30f, h * 1.04f, w * 0.40f, h * 0.44f, Color(46, 14, 165, 233));
    }

    HDC mem = CreateCompatibleDC(reference);
    HBITMAP bitmap = CreateCompatibleBitmap(reference, width, height);
    if (mem == nullptr || bitmap == nullptr) {
        if (mem) {
            DeleteDC(mem);
        }
        return bitmap;
    }
    HGDIOBJ old = SelectObject(mem, bitmap);
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(&low, Gdiplus::Rect(0, 0, width, height), 0, 0, lw, lh, Gdiplus::UnitPixel,
                    &attributes);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    return bitmap;
}

// ---- canvas -----------------------------------------------------------------

Canvas::Canvas(HDC hdc, float scale) : g_(hdc), scale_(scale) {
    g_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g_.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g_.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
}

Canvas::Canvas(Gdiplus::Image* image, float scale, bool transparentTarget)
    : g_(image), scale_(scale) {
    g_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g_.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g_.SetTextRenderingHint(transparentTarget ? Gdiplus::TextRenderingHintAntiAliasGridFit
                                              : Gdiplus::TextRenderingHintClearTypeGridFit);
}

void Canvas::fillRound(const RectF& r, float radiusDu, Color fill) {
    if (r.Width <= 0.0f || r.Height <= 0.0f) {
        return;
    }
    GraphicsPath path;
    addRoundRect(path, r, s(radiusDu));
    SolidBrush brush(fill);
    g_.FillPath(&brush, &path);
}

void Canvas::strokeRound(const RectF& r, float radiusDu, Color stroke, float widthDu) {
    if (r.Width <= 1.0f || r.Height <= 1.0f) {
        return;
    }
    const float width = std::max(1.0f, s(widthDu));
    RectF inset = r;
    inset.Inflate(-width / 2.0f, -width / 2.0f);
    GraphicsPath path;
    addRoundRect(path, inset, std::max(0.0f, s(radiusDu) - width / 2.0f));
    Pen pen(stroke, width);
    g_.DrawPath(&pen, &path);
}

void Canvas::fillRoundGradient(const RectF& r, float radiusDu, Color from, Color to,
                               bool vertical) {
    if (r.Width <= 0.0f || r.Height <= 0.0f) {
        return;
    }
    GraphicsPath path;
    addRoundRect(path, r, s(radiusDu));
    const PointF a(r.X, r.Y);
    const PointF b = vertical ? PointF(r.X, r.GetBottom()) : PointF(r.GetRight(), r.Y);
    LinearGradientBrush brush(a, b, from, to);
    brush.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g_.FillPath(&brush, &path);
}

void Canvas::glass(const RectF& r, float radiusDu, float strength) {
    if (r.Width <= 2.0f || r.Height <= 2.0f) {
        return;
    }
    const auto alpha = [strength](float base) {
        return static_cast<BYTE>(std::clamp(base * strength, 0.0f, 255.0f));
    };
    fillRoundGradient(r, radiusDu, Color(alpha(30), 255, 255, 255), Color(alpha(13), 255, 255, 255),
                      true);

    RectF inset = r;
    inset.Inflate(-0.5f, -0.5f);
    GraphicsPath edge;
    addRoundRect(edge, inset, std::max(0.0f, s(radiusDu) - 0.5f));
    LinearGradientBrush edgeBrush(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                                  Color(alpha(84), 255, 255, 255),
                                  Color(alpha(22), 255, 255, 255));
    edgeBrush.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    Pen pen(&edgeBrush, 1.0f);
    g_.DrawPath(&pen, &edge);
}

void Canvas::glow(const RectF& r, float radiusDu, Color color, float spreadDu) {
    const int layers = 5;
    const float spread = s(spreadDu);
    for (int i = layers; i >= 1; --i) {
        const float grow = spread * static_cast<float>(i) / static_cast<float>(layers);
        RectF halo = r;
        halo.Inflate(grow, grow);
        const int alpha = static_cast<int>(color.GetA() * 0.34f / static_cast<float>(i));
        GraphicsPath path;
        addRoundRect(path, halo, s(radiusDu) + grow);
        SolidBrush brush(withAlpha(color, alpha));
        g_.FillPath(&brush, &path);
    }
}

void Canvas::text(std::wstring_view value, const RectF& r, float sizeDu, Weight weight, Color color,
                  unsigned flags) {
    if (value.empty() || r.Width <= 0.0f || r.Height <= 0.0f) {
        return;
    }
    const std::wstring clean = withoutCarriageReturns(value);
    Gdiplus::Font* font = fontFor(s(sizeDu), weight, (flags & kMono) != 0);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    configureFormat(format, flags);
    SolidBrush brush(color);
    g_.DrawString(clean.c_str(), static_cast<INT>(clean.size()), font, r, &format, &brush);
}

Gdiplus::SizeF Canvas::measure(std::wstring_view value, float sizeDu, Weight weight, float maxWidth,
                               unsigned flags) {
    if (value.empty()) {
        return Gdiplus::SizeF(0.0f, 0.0f);
    }
    const std::wstring clean = withoutCarriageReturns(value);
    Gdiplus::Font* font = fontFor(s(sizeDu), weight, (flags & kMono) != 0);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    unsigned measureFlags = flags & ~static_cast<unsigned>(kEllipsis);
    if (maxWidth > 0.0f) {
        measureFlags |= kWrap;
    }
    configureFormat(format, measureFlags);
    format.SetFormatFlags(format.GetFormatFlags() |
                          Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    const RectF layout(0.0f, 0.0f, maxWidth > 0.0f ? maxWidth : 100000.0f, 100000.0f);
    RectF bounds;
    g_.MeasureString(clean.c_str(), static_cast<INT>(clean.size()), font, layout, &format, &bounds);
    return Gdiplus::SizeF(bounds.Width, bounds.Height);
}

float Canvas::fitSize(std::wstring_view value, float sizeDu, float minDu, Weight weight,
                      float maxWidth) {
    float size = sizeDu;
    while (size > minDu && measure(value, size, weight).Width > maxWidth) {
        size -= 0.5f;
    }
    return std::max(size, minDu);
}

void Canvas::icon(Icon icon, const RectF& box, Color color) {
    if (icon == Icon::None || box.Width <= 0.0f) {
        return;
    }
    const auto X = [&box](float u) { return box.X + box.Width * u; };
    const auto Y = [&box](float v) { return box.Y + box.Height * v; };
    const float stroke = std::max(1.4f, box.Width * 0.105f);
    Pen pen(color, stroke);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    SolidBrush brush(color);

    const auto line = [&](float x1, float y1, float x2, float y2) {
        g_.DrawLine(&pen, X(x1), Y(y1), X(x2), Y(y2));
    };
    const auto poly = [&](std::initializer_list<PointF> points) {
        std::vector<PointF> mapped;
        mapped.reserve(points.size());
        for (const PointF& p : points) {
            mapped.emplace_back(X(p.X), Y(p.Y));
        }
        g_.DrawLines(&pen, mapped.data(), static_cast<INT>(mapped.size()));
    };

    switch (icon) {
        case Icon::Download:
            line(0.5f, 0.10f, 0.5f, 0.62f);
            poly({PointF(0.27f, 0.40f), PointF(0.5f, 0.64f), PointF(0.73f, 0.40f)});
            line(0.16f, 0.88f, 0.84f, 0.88f);
            break;
        case Icon::Play: {
            const PointF triangle[3] = {PointF(X(0.28f), Y(0.14f)), PointF(X(0.86f), Y(0.5f)),
                                        PointF(X(0.28f), Y(0.86f))};
            g_.FillPolygon(&brush, triangle, 3);
            break;
        }
        case Icon::Grid: {
            const float cell = 0.30f;
            const float origins[2] = {0.14f, 0.56f};
            for (float oy : origins) {
                for (float ox : origins) {
                    GraphicsPath path;
                    addRoundRect(path,
                                 RectF(X(ox), Y(oy), box.Width * cell, box.Height * cell),
                                 box.Width * 0.07f);
                    g_.FillPath(&brush, &path);
                }
            }
            break;
        }
        case Icon::Record:
            g_.FillEllipse(&brush, X(0.30f), Y(0.30f), box.Width * 0.40f, box.Height * 0.40f);
            g_.DrawEllipse(&pen, X(0.10f), Y(0.10f), box.Width * 0.80f, box.Height * 0.80f);
            break;
        case Icon::Pulse:
            poly({PointF(0.04f, 0.55f), PointF(0.28f, 0.55f), PointF(0.42f, 0.18f),
                  PointF(0.58f, 0.86f), PointF(0.72f, 0.55f), PointF(0.96f, 0.55f)});
            break;
        case Icon::Filter:
            line(0.10f, 0.24f, 0.90f, 0.24f);
            line(0.25f, 0.50f, 0.75f, 0.50f);
            line(0.40f, 0.76f, 0.60f, 0.76f);
            break;
        case Icon::Chip:
            g_.DrawRectangle(&pen, X(0.26f), Y(0.26f), box.Width * 0.48f, box.Height * 0.48f);
            for (float t : {0.38f, 0.62f}) {
                line(t, 0.08f, t, 0.24f);
                line(t, 0.76f, t, 0.92f);
                line(0.08f, t, 0.24f, t);
                line(0.76f, t, 0.92f, t);
            }
            break;
        case Icon::Window:
            g_.DrawRectangle(&pen, X(0.10f), Y(0.16f), box.Width * 0.80f, box.Height * 0.68f);
            line(0.10f, 0.38f, 0.90f, 0.38f);
            break;
        case Icon::Retry: {
            const float cx = X(0.5f);
            const float cy = Y(0.5f);
            const float radius = box.Width * 0.36f;
            const float start = 40.0f;
            const float sweep = 280.0f;
            g_.DrawArc(&pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, start, sweep);
            const float end = (start + sweep) * kPi / 180.0f;
            const PointF tip(cx + radius * std::cos(end), cy + radius * std::sin(end));
            // Tangent of the arc at its end (clockwise sweep).
            const float tx = -std::sin(end);
            const float ty = std::cos(end);
            const float length = box.Width * 0.26f;
            for (float turn : {2.45f, -2.45f}) {
                const float c = std::cos(turn);
                const float sn = std::sin(turn);
                const PointF back(tip.X + (tx * c - ty * sn) * length,
                                  tip.Y + (tx * sn + ty * c) * length);
                g_.DrawLine(&pen, tip, back);
            }
            break;
        }
        case Icon::Check:
            poly({PointF(0.18f, 0.53f), PointF(0.41f, 0.76f), PointF(0.83f, 0.26f)});
            break;
        case Icon::Cross:
            line(0.22f, 0.22f, 0.78f, 0.78f);
            line(0.78f, 0.22f, 0.22f, 0.78f);
            break;
        case Icon::Info:
            g_.DrawEllipse(&pen, X(0.08f), Y(0.08f), box.Width * 0.84f, box.Height * 0.84f);
            line(0.5f, 0.46f, 0.5f, 0.72f);
            g_.FillEllipse(&brush, X(0.5f) - stroke * 0.7f, Y(0.29f) - stroke * 0.7f, stroke * 1.4f,
                           stroke * 1.4f);
            break;
        case Icon::Trash:
            line(0.14f, 0.26f, 0.86f, 0.26f);
            line(0.38f, 0.12f, 0.62f, 0.12f);
            poly({PointF(0.24f, 0.26f), PointF(0.30f, 0.88f), PointF(0.70f, 0.88f),
                  PointF(0.76f, 0.26f)});
            break;
        case Icon::Power:
            g_.DrawArc(&pen, X(0.14f), Y(0.18f), box.Width * 0.72f, box.Height * 0.72f, -55.0f,
                       290.0f);
            line(0.5f, 0.08f, 0.5f, 0.46f);
            break;
        case Icon::None:
            break;
    }
}

void Canvas::button(const RectF& r, std::wstring_view label, std::wstring_view sub,
                    ButtonStyle style, ButtonState state, Icon iconKind) {
    if (r.Width <= 0.0f || r.Height <= 0.0f) {
        return;
    }
    const float heightDu = r.Height / std::max(0.01f, scale_);
    const float radius = heightDu >= 50.0f ? 14.0f : 11.0f;
    const bool hover = state.hover && !state.disabled;
    const bool pressed = state.pressed && !state.disabled;

    Color textColor = color::text();
    Color subColor = color::muted();

    switch (style) {
        case ButtonStyle::Primary:
        case ButtonStyle::Accent: {
            const Color from = style == ButtonStyle::Primary ? color::cyan() : Color(255, 178, 148, 255);
            const Color to = style == ButtonStyle::Primary ? color::blue() : Color(255, 112, 134, 255);
            if (!state.disabled) {
                glow(r, radius, withAlpha(to, hover ? 150 : 96), 9.0f);
            }
            fillRoundGradient(r, radius, from, to, false);
            // Glassy top sheen.
            RectF sheen = r;
            sheen.Height = r.Height * 0.52f;
            fillRoundGradient(sheen, radius, Color(hover ? 86 : 58, 255, 255, 255),
                              Color(0, 255, 255, 255), true);
            if (pressed) {
                fillRound(r, radius, Color(52, 0, 0, 0));
            }
            strokeRound(r, radius, Color(70, 255, 255, 255));
            textColor = color::ink();
            subColor = Color(205, 5, 18, 33);
            if (state.disabled) {
                fillRound(r, radius, Color(168, 12, 18, 34));
                textColor = Color(150, 5, 18, 33);
                subColor = Color(110, 5, 18, 33);
            }
            break;
        }
        case ButtonStyle::Glass: {
            const int fill = state.disabled ? 10 : (pressed ? 16 : (hover ? 44 : 24));
            fillRoundGradient(r, radius, Color(static_cast<BYTE>(fill + 8), 255, 255, 255),
                              Color(static_cast<BYTE>(fill), 255, 255, 255), true);
            strokeRound(r, radius,
                        Color(static_cast<BYTE>(state.disabled ? 22 : (hover ? 120 : 58)), 255, 255,
                              255));
            if (state.disabled) {
                textColor = withAlpha(color::text(), 96);
                subColor = withAlpha(color::muted(), 96);
            }
            break;
        }
        case ButtonStyle::Ghost: {
            if (hover || pressed) {
                fillRound(r, radius, Color(static_cast<BYTE>(pressed ? 14 : 26), 255, 255, 255));
            }
            strokeRound(r, radius,
                        Color(static_cast<BYTE>(state.disabled ? 16 : (hover ? 84 : 36)), 255, 255,
                              255));
            textColor = state.disabled ? withAlpha(color::muted(), 96)
                                       : (hover ? color::text() : color::muted());
            break;
        }
        case ButtonStyle::Danger: {
            const Color red = color::red();
            fillRound(r, radius,
                      withAlpha(red, state.disabled ? 8 : (pressed ? 22 : (hover ? 58 : 26))));
            strokeRound(r, radius, withAlpha(red, state.disabled ? 34 : (hover ? 200 : 110)));
            textColor = state.disabled ? withAlpha(red, 100) : Color(255, 255, 170, 170);
            break;
        }
    }

    const bool twoLine = !sub.empty() && heightDu >= 46.0f;
    if (twoLine) {
        // Large action: icon on the left, label + caption stacked beside it.
        const float pad = s(18.0f);
        float textX = r.X + pad;
        if (iconKind != Icon::None) {
            const float size = s(22.0f);
            icon(iconKind, RectF(r.X + pad, r.Y + (r.Height - size) / 2.0f, size, size), textColor);
            textX += size + s(14.0f);
        }
        const float labelH = s(21.0f);
        const float subH = s(16.0f);
        const float blockY = r.Y + (r.Height - labelH - subH) / 2.0f;
        const float textW = r.GetRight() - s(12.0f) - textX;
        text(label, RectF(textX, blockY, textW, labelH),
             fitSize(label, 15.0f, 11.5f, Weight::Semibold, textW), Weight::Semibold, textColor,
             kLeft | kVCenter | kEllipsis);
        text(sub, RectF(textX, blockY + labelH, textW, subH),
             fitSize(sub, 11.5f, 9.5f, Weight::Regular, textW), Weight::Regular, subColor,
             kLeft | kVCenter | kEllipsis);
        return;
    }

    // Compact action: icon + label centred as one group.
    const float iconSize = iconKind != Icon::None ? s(16.0f) : 0.0f;
    const float iconGap = iconKind != Icon::None ? s(9.0f) : 0.0f;
    const float available = r.Width - s(20.0f) - iconSize - iconGap;
    const float fontDu =
        fitSize(label, heightDu >= 44.0f ? 14.5f : 13.5f, 11.0f, Weight::Semibold, available);
    const float labelW = std::min(measure(label, fontDu, Weight::Semibold).Width + 2.0f,
                                  std::max(0.0f, available));
    float x = r.X + (r.Width - (iconSize + iconGap + labelW)) / 2.0f;
    if (iconKind != Icon::None) {
        icon(iconKind, RectF(x, r.Y + (r.Height - iconSize) / 2.0f, iconSize, iconSize), textColor);
        x += iconSize + iconGap;
    }
    text(label, RectF(x, r.Y, labelW + 2.0f, r.Height), fontDu, Weight::Semibold, textColor,
         kLeft | kVCenter | kEllipsis);
}

void Canvas::toggleSwitch(const RectF& track, bool on, bool disabled, Color onFrom, Color onTo) {
    const float radiusDu = track.Height / std::max(0.01f, scale_) / 2.0f;
    if (on) {
        if (!disabled) {
            glow(track, radiusDu, withAlpha(onTo, 90), 5.0f);
        }
        fillRoundGradient(track, radiusDu, onFrom, onTo, false);
    } else {
        fillRound(track, radiusDu, Color(40, 255, 255, 255));
        strokeRound(track, radiusDu, Color(70, 255, 255, 255));
    }
    const float knob = track.Height - s(6.0f);
    const float knobX = on ? track.GetRight() - knob - s(3.0f) : track.X + s(3.0f);
    SolidBrush knobBrush(on ? Color(255, 255, 255, 255) : Color(255, 176, 188, 208));
    g_.FillEllipse(&knobBrush, knobX, track.Y + s(3.0f), knob, knob);
    if (disabled) {
        fillRound(track, radiusDu, Color(120, 12, 18, 34));
    }
}

void Canvas::toggleRow(const RectF& r, std::wstring_view label, std::wstring_view sub, bool on,
                       ButtonState state) {
    const bool hover = state.hover && !state.disabled;
    const int fill = state.disabled ? 8 : (hover ? 34 : 18);
    fillRoundGradient(r, 12.0f, Color(static_cast<BYTE>(fill + 6), 255, 255, 255),
                      Color(static_cast<BYTE>(fill), 255, 255, 255), true);
    strokeRound(r, 12.0f, Color(static_cast<BYTE>(hover ? 96 : 44), 255, 255, 255));

    const float trackW = s(42.0f);
    const float trackH = s(24.0f);
    const RectF track(r.GetRight() - s(14.0f) - trackW, r.Y + (r.Height - trackH) / 2.0f, trackW,
                      trackH);
    toggleSwitch(track, on, state.disabled, color::green(), Color(255, 45, 212, 191));

    const float textX = r.X + s(16.0f);
    const float textW = track.X - s(12.0f) - textX;
    const Color labelColor = state.disabled ? withAlpha(color::text(), 110) : color::text();
    const float heightDu = r.Height / std::max(0.01f, scale_);
    if (!sub.empty() && heightDu >= 44.0f) {
        const float labelH = s(19.0f);
        const float subH = s(15.0f);
        const float blockY = r.Y + (r.Height - labelH - subH) / 2.0f;
        text(label, RectF(textX, blockY, textW, labelH), 13.5f, Weight::Semibold, labelColor,
             kLeft | kVCenter | kEllipsis);
        text(sub, RectF(textX, blockY + labelH, textW, subH), 11.0f, Weight::Regular,
             on ? withAlpha(color::green(), 230) : color::muted(), kLeft | kVCenter | kEllipsis);
    } else {
        text(label, RectF(textX, r.Y, textW, r.Height), 13.5f, Weight::Semibold, labelColor,
             kLeft | kVCenter | kEllipsis);
    }
}

void Canvas::statChip(const RectF& r, std::wstring_view caption, int value, Color valueColor) {
    glass(r, 13.0f, 0.82f);
    const float pad = s(14.0f);
    const float heightDu = r.Height / std::max(0.01f, scale_);
    const float captionH = s(15.0f);
    const float top = r.Y + s(heightDu >= 68.0f ? 12.0f : 8.0f);
    const float captionW = r.Width - pad * 2.0f;
    text(caption, RectF(r.X + pad, top, captionW, captionH),
         fitSize(caption, 10.0f, 7.5f, Weight::Semibold, captionW), Weight::Semibold, color::dim(),
         kLeft | kVCenter | kEllipsis);
    const float valueTop = top + captionH;
    const float valueDu = std::clamp(heightDu * 0.36f, 17.0f, 28.0f);
    text(std::to_wstring(value),
         RectF(r.X + pad, valueTop, r.Width - pad * 2.0f, r.GetBottom() - s(6.0f) - valueTop),
         valueDu, Weight::Semibold, valueColor, kLeft | kVCenter);
}

void Canvas::ring(float cx, float cy, float radius, float thicknessDu, float progress,
                  float phase) {
    const float t = std::max(2.0f, s(thicknessDu));
    const RectF box(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    Pen track(Color(30, 255, 255, 255), t);
    g_.DrawEllipse(&track, box);

    LinearGradientBrush gradient(PointF(box.X, box.Y), PointF(box.GetRight(), box.GetBottom()),
                                 color::cyan(), color::violet());
    gradient.SetWrapMode(Gdiplus::WrapModeTileFlipXY);

    float start = -90.0f;
    float sweep = 0.0f;
    if (progress < 0.0f) {
        // Indeterminate: a comet that travels around the ring and breathes.
        start = phase * 360.0f - 90.0f;
        sweep = 70.0f + 40.0f * std::sin(phase * 2.0f * kPi);
    } else {
        sweep = 360.0f * std::clamp(progress, 0.0f, 1.0f);
    }
    if (sweep > 0.5f) {
        Pen halo(Color(46, 79, 140, 255), t * 2.3f);
        halo.SetStartCap(Gdiplus::LineCapRound);
        halo.SetEndCap(Gdiplus::LineCapRound);
        g_.DrawArc(&halo, box, start, sweep);

        Pen arc(&gradient, t);
        arc.SetStartCap(Gdiplus::LineCapRound);
        arc.SetEndCap(Gdiplus::LineCapRound);
        g_.DrawArc(&arc, box, start, sweep);
    }
    if (progress >= 0.0f && progress < 1.0f) {
        // Orbiting spark: shows the app is alive even when the percentage stalls.
        const float angle = (phase * 360.0f - 90.0f) * kPi / 180.0f;
        const float dot = t * 0.34f;
        SolidBrush spark(Color(200, 255, 255, 255));
        g_.FillEllipse(&spark, cx + radius * std::cos(angle) - dot,
                       cy + radius * std::sin(angle) - dot, dot * 2.0f, dot * 2.0f);
    }
}

void Canvas::progressBar(const RectF& r, float progress, float phase) {
    const float radiusDu = r.Height / std::max(0.01f, scale_) / 2.0f;
    fillRound(r, radiusDu, Color(30, 255, 255, 255));
    RectF fill = r;
    if (progress < 0.0f) {
        fill.Width = r.Width * 0.28f;
        fill.X = r.X + (r.Width - fill.Width) * (0.5f - 0.5f * std::cos(phase * 2.0f * kPi));
    } else {
        fill.Width = std::max(r.Height, r.Width * std::clamp(progress, 0.0f, 1.0f));
        if (progress <= 0.0f) {
            return;
        }
    }
    fillRoundGradient(fill, radiusDu, color::cyan(), color::violet(), false);
}

void Canvas::pill(const RectF& r, std::wstring_view label, Color dot, bool pulse, float phase) {
    const float radiusDu = r.Height / std::max(0.01f, scale_) / 2.0f;
    fillRound(r, radiusDu, Color(22, 255, 255, 255));
    strokeRound(r, radiusDu, Color(50, 255, 255, 255));
    const float d = s(8.0f);
    const float dx = r.X + s(12.0f);
    const float dy = r.Y + (r.Height - d) / 2.0f;
    if (pulse) {
        const float wave = 0.5f + 0.5f * std::sin(phase * 2.0f * kPi);
        const float grow = s(4.0f) * wave;
        SolidBrush haloBrush(withAlpha(dot, static_cast<int>(90.0f * (1.0f - wave))));
        g_.FillEllipse(&haloBrush, dx - grow, dy - grow, d + grow * 2.0f, d + grow * 2.0f);
    }
    SolidBrush dotBrush(dot);
    g_.FillEllipse(&dotBrush, dx, dy, d, d);
    const float textX = dx + d + s(8.0f);
    text(label, RectF(textX, r.Y, r.GetRight() - s(10.0f) - textX, r.Height), 12.0f,
         Weight::Semibold, color::text(), kLeft | kVCenter | kEllipsis);
}

void Canvas::divider(float x1, float x2, float y) {
    if (x2 <= x1) {
        return;
    }
    LinearGradientBrush brush(PointF(x1, y), PointF(x2, y), Color(0, 255, 255, 255),
                              Color(0, 255, 255, 255));
    const Color colors[3] = {Color(0, 61, 214, 245), Color(110, 120, 170, 255),
                             Color(0, 150, 130, 255)};
    const REAL positions[3] = {0.0f, 0.5f, 1.0f};
    brush.SetInterpolationColors(colors, positions, 3);
    g_.FillRectangle(&brush, x1, y, x2 - x1, std::max(1.0f, s(1.0f)));
}

std::wstring spaced(std::wstring_view value) {
    std::wstring out;
    out.reserve(value.size() * 2);
    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t c = value[i];
        if (c == L' ') {
            out += L"  ";
            continue;
        }
        out.push_back(c);
        if (i + 1 < value.size()) {
            out.push_back(L' ');
        }
    }
    // CharUpperBuffW handles accented letters (ç, õ, í...) regardless of the C locale.
    CharUpperBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

}  // namespace ui
