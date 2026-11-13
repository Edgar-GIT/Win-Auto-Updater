#include "panel.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace nt {

namespace {
constexpr const wchar_t* kPanelClass = L"WinAutoUpdaterNativePanel";
}  // namespace

Panel::~Panel() {
    destroy();
    releaseSurfaces();
}

bool Panel::create(HINSTANCE instance, const wchar_t* title, DWORD style, DWORD exStyle) {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Panel::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPanelClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    const SIZE design = designSize();
    hwnd_ = CreateWindowExW(exStyle, kPanelClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT, design.cx,
                            design.cy, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    if ((style & WS_CAPTION) == WS_CAPTION) {
        ui::applyDarkFrame(hwnd_);
    }
    return true;
}

void Panel::destroy() {
    if (hwnd_ != nullptr) {
        HWND window = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
}

void Panel::invalidate() {
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

SIZE Panel::outerSize(int clientWidth, int clientHeight) const {
    RECT frame{0, 0, clientWidth, clientHeight};
    if (hwnd_ != nullptr) {
        ui::adjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)),
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), ui::dpiForWindow(hwnd_));
    }
    return SIZE{frame.right - frame.left, frame.bottom - frame.top};
}

void Panel::placeClient(int x, int y, int clientWidth, int clientHeight, bool show) {
    if (hwnd_ == nullptr) {
        return;
    }
    const SIZE outer = outerSize(clientWidth, clientHeight);
    SetWindowPos(hwnd_, nullptr, x, y, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    relayout();
    if (show) {
        const bool noActivate = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0;
        ShowWindow(hwnd_, noActivate ? SW_SHOWNOACTIVATE : SW_SHOW);
        UpdateWindow(hwnd_);
    }
}

float Panel::scaleFor(int clientWidth, int clientHeight) const {
    const SIZE design = designSize();
    return std::clamp(std::min(static_cast<float>(clientWidth) / static_cast<float>(design.cx),
                               static_cast<float>(clientHeight) / static_cast<float>(design.cy)),
                      0.35f, 5.0f);
}

int Panel::px(float du) const {
    return static_cast<int>(std::lround(du * scale_));
}

void Panel::addWidget(int id, const wchar_t* text, ui::ButtonStyle style, ui::Icon icon) {
    Widget w;
    w.id = id;
    w.text = text;
    w.style = style;
    w.icon = icon;
    widgets_.push_back(std::move(w));
}

Panel::Widget* Panel::widget(int id) {
    for (Widget& w : widgets_) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

void Panel::place(int id, int x, int y, int w, int h) {
    if (Widget* item = widget(id)) {
        item->rc = RECT{x, y, x + w, y + h};
        item->visible = true;
    }
}

void Panel::hideWidgets() {
    for (Widget& w : widgets_) {
        w.visible = false;
    }
}

void Panel::drawWidgets(ui::Canvas& canvas) {
    for (const Widget& w : widgets_) {
        if (!w.visible) {
            continue;
        }
        ui::ButtonState state;
        state.disabled = !w.enabled;
        state.hover = hoverId_ == w.id;
        state.pressed = pressedId_ == w.id && hoverId_ == w.id;
        canvas.button(ui::toRectF(w.rc), w.text, L"", w.style, state, w.icon);
    }
}

void Panel::relayout() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        layout(rc.right, rc.bottom);
        invalidate();
    }
}

void Panel::layout(int width, int height) {
    clientW_ = std::max(1, width);
    clientH_ = std::max(1, height);
    scale_ = scaleFor(clientW_, clientH_);
    hideWidgets();
    onLayout();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void Panel::releaseSurfaces() {
    if (backDc_) {
        SelectObject(backDc_, backOld_);
        DeleteDC(backDc_);
        backDc_ = nullptr;
    }
    if (backBitmap_) {
        DeleteObject(backBitmap_);
        backBitmap_ = nullptr;
    }
    if (bgDc_) {
        SelectObject(bgDc_, bgOld_);
        DeleteDC(bgDc_);
        bgDc_ = nullptr;
    }
    if (bgBitmap_) {
        DeleteObject(bgBitmap_);
        bgBitmap_ = nullptr;
    }
    backW_ = backH_ = bgW_ = bgH_ = 0;
}

void Panel::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w > 0 && h > 0) {
        if (backBitmap_ == nullptr || backW_ != w || backH_ != h) {
            if (backDc_) {
                SelectObject(backDc_, backOld_);
                DeleteDC(backDc_);
            }
            if (backBitmap_) {
                DeleteObject(backBitmap_);
            }
            backDc_ = CreateCompatibleDC(hdc);
            backBitmap_ = CreateCompatibleBitmap(hdc, w, h);
            backOld_ = SelectObject(backDc_, backBitmap_);
            backW_ = w;
            backH_ = h;
        }
        render(backDc_, w, h);
        BitBlt(hdc, 0, 0, w, h, backDc_, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

void Panel::render(HDC dc, int width, int height) {
    if (bgBitmap_ == nullptr || bgW_ != width || bgH_ != height) {
        if (bgDc_) {
            SelectObject(bgDc_, bgOld_);
            DeleteDC(bgDc_);
            bgDc_ = nullptr;
        }
        if (bgBitmap_) {
            DeleteObject(bgBitmap_);
        }
        bgBitmap_ = ui::renderBackground(dc, width, height);
        bgDc_ = CreateCompatibleDC(dc);
        if (bgDc_ && bgBitmap_) {
            bgOld_ = SelectObject(bgDc_, bgBitmap_);
        }
        bgW_ = width;
        bgH_ = height;
    }
    if (bgDc_) {
        BitBlt(dc, 0, 0, width, height, bgDc_, 0, 0, SRCCOPY);
    }
    GdiFlush();

    ui::Canvas canvas(dc, scale_);
    onDraw(canvas);
    drawWidgets(canvas);
    canvas.g().Flush(Gdiplus::FlushIntentionSync);
}

#ifdef SU_UI_SNAPSHOT
bool Panel::snapshot(const std::filesystem::path& file, int width, int height) {
    layout(width, height);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    render(mem, width, height);
    GdiFlush();
    bool saved = false;
    {
        CLSID png{};
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
        Gdiplus::Bitmap image(width, height, width * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
        saved = image.Save(file.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved;
}
#endif

// ---------------------------------------------------------------------------
// Input / messages
// ---------------------------------------------------------------------------

int Panel::widgetAt(POINT pt) const {
    for (const Widget& w : widgets_) {
        if (w.visible && w.enabled && PtInRect(&w.rc, pt)) {
            return w.id;
        }
    }
    return 0;
}

LRESULT CALLBACK Panel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Panel* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Panel*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Panel::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                relayout();
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // Never smaller than 45 % of the design: below that nothing is readable.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const SIZE design = designSize();
            const SIZE outer = outerSize(static_cast<int>(design.cx * 0.45f), static_cast<int>(design.cy * 0.45f));
            info->ptMinTrackSize.x = outer.cx;
            info->ptMinTrackSize.y = outer.cy;
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            onDisplayChange();
            relayout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                tracking_ = true;
            }
            const int id = widgetAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (id != hoverId_) {
                hoverId_ = id;
                invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hoverId_ != 0) {
                hoverId_ = 0;
                invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const int id = widgetAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (id != 0) {
                pressedId_ = id;
                hoverId_ = id;
                SetCapture(hwnd);
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (pressedId_ == 0) {
                return 0;
            }
            const int id = pressedId_;
            pressedId_ = 0;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            invalidate();
            if (widgetAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) == id) {
                onCommand(id);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressedId_ != 0) {
                pressedId_ = 0;
                invalidate();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hoverId_ != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CLOSE:
            if (onCloseRequest()) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            onDestroyed();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace nt
