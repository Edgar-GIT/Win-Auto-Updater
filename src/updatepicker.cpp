#include "updatepicker.hpp"

#include "ui.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// "Filtro de updates": modal list of pending updates, each with an
// install/block switch. Drawn with the same glass toolkit as the main window
// and scaled to the monitor, so it fits small screens too.

namespace {

using ui::Canvas;
using ui::Color;
using ui::RectF;
using ui::Weight;

constexpr int kDesignW = 680;
constexpr int kDesignH = 520;
constexpr float kRowDu = 42.0f;

enum Button { kNone = 0, kSave, kCancel, kAllOn, kAllOff };

struct PickerState {
    std::vector<UpdateChoice>* choices = nullptr;
    int result = IDCANCEL;
    float scale = 1.0f;
    int width = kDesignW;
    int height = kDesignH;
    int scroll = 0;        // pixels
    int selected = 0;      // keyboard row
    int hoverRow = -1;
    int hoverButton = kNone;
    int pressedButton = kNone;
    bool draggingThumb = false;
    int dragOffset = 0;
    bool tracking = false;
    HBITMAP background = nullptr;
    int bgW = 0;
    int bgH = 0;
    RECT listRect{};
    RECT buttons[5]{};
};

PickerState* stateOf(HWND hwnd) {
    return reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int px(const PickerState& s, float du) {
    return static_cast<int>(std::lround(du * s.scale));
}

int rowHeight(const PickerState& s) {
    return std::max(1, px(s, kRowDu));
}

int contentHeight(const PickerState& s) {
    return s.choices ? static_cast<int>(s.choices->size()) * rowHeight(s) : 0;
}

int maxScroll(const PickerState& s) {
    return std::max<int>(0, contentHeight(s) - (s.listRect.bottom - s.listRect.top));
}

void clampScroll(PickerState& s) {
    s.scroll = std::clamp(s.scroll, 0, maxScroll(s));
}

void layoutPicker(PickerState& s, int width, int height) {
    s.width = std::max(1, width);
    s.height = std::max(1, height);
    s.scale = std::clamp(std::min(static_cast<float>(s.width) / kDesignW,
                                  static_cast<float>(s.height) / kDesignH),
                         0.4f, 5.0f);
    const int margin = px(s, 22.0f);
    const int buttonH = px(s, 40.0f);
    const int buttonY = s.height - margin - buttonH;
    const int gap = px(s, 10.0f);

    s.listRect = RECT{margin, px(s, 92.0f), s.width - margin, buttonY - px(s, 16.0f)};

    const int saveW = px(s, 132.0f);
    const int cancelW = px(s, 118.0f);
    s.buttons[kSave] = RECT{s.width - margin - saveW, buttonY, s.width - margin, buttonY + buttonH};
    s.buttons[kCancel] = RECT{s.buttons[kSave].left - gap - cancelW, buttonY,
                              s.buttons[kSave].left - gap, buttonY + buttonH};
    const int quickW = px(s, 136.0f);
    s.buttons[kAllOn] = RECT{margin, buttonY, margin + quickW, buttonY + buttonH};
    s.buttons[kAllOff] = RECT{margin + quickW + gap, buttonY, margin + quickW * 2 + gap,
                              buttonY + buttonH};
    clampScroll(s);
}

// Scroll thumb rectangle (empty when everything fits).
RECT thumbRect(const PickerState& s) {
    const int limit = maxScroll(s);
    if (limit <= 0) {
        return RECT{};
    }
    const int trackH = s.listRect.bottom - s.listRect.top - px(s, 12.0f);
    const int thumbH = std::max(px(s, 28.0f), trackH * trackH / std::max(1, contentHeight(s)));
    const int top = s.listRect.top + px(s, 6.0f) + (trackH - thumbH) * s.scroll / limit;
    const int right = s.listRect.right - px(s, 5.0f);
    return RECT{right - px(s, 6.0f), top, right, top + thumbH};
}

void ensureVisible(PickerState& s, int index) {
    const int rowH = rowHeight(s);
    const int viewH = s.listRect.bottom - s.listRect.top;
    const int top = index * rowH;
    if (top < s.scroll) {
        s.scroll = top;
    } else if (top + rowH > s.scroll + viewH) {
        s.scroll = top + rowH - viewH;
    }
    clampScroll(s);
}

int rowAt(const PickerState& s, POINT pt) {
    if (!s.choices || !PtInRect(&s.listRect, pt)) {
        return -1;
    }
    const int index = (pt.y - s.listRect.top + s.scroll) / rowHeight(s);
    return index >= 0 && index < static_cast<int>(s.choices->size()) ? index : -1;
}

int buttonAt(const PickerState& s, POINT pt) {
    for (int id = kSave; id <= kAllOff; ++id) {
        if (PtInRect(&s.buttons[id], pt)) {
            return id;
        }
    }
    return kNone;
}

void toggle(PickerState& s, int index) {
    if (s.choices && index >= 0 && index < static_cast<int>(s.choices->size())) {
        auto& choice = (*s.choices)[static_cast<size_t>(index)];
        choice.enabled = !choice.enabled;
    }
}

void paintPicker(HWND hwnd, PickerState& s) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    const int w = s.width;
    const int h = s.height;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bitmap);

    if (s.background == nullptr || s.bgW != w || s.bgH != h) {
        if (s.background) {
            DeleteObject(s.background);
        }
        s.background = ui::renderBackground(hdc, w, h);
        s.bgW = w;
        s.bgH = h;
    }
    if (s.background) {
        HDC bg = CreateCompatibleDC(hdc);
        HGDIOBJ oldBg = SelectObject(bg, s.background);
        BitBlt(mem, 0, 0, w, h, bg, 0, 0, SRCCOPY);
        SelectObject(bg, oldBg);
        DeleteDC(bg);
    }
    GdiFlush();

    {
        Canvas canvas(mem, s.scale);
        const float margin = canvas.s(22.0f);
        canvas.text(L"Filtro de updates", RectF(margin, canvas.s(18.0f), w - margin * 2, canvas.s(28.0f)),
                    19.0f, Weight::Semibold, ui::color::text(), ui::kLeft | ui::kVCenter);
        canvas.text(L"Verde = instalar · vermelho = bloquear. O bloqueio vale para a família da "
                    L"atualização (ignora KB e data) e fica em helpers\\update_filter.txt.",
                    RectF(margin, canvas.s(48.0f), w - margin * 2, canvas.s(36.0f)), 12.0f,
                    Weight::Regular, ui::color::muted(), ui::kLeft | ui::kTop | ui::kWrap | ui::kEllipsis);

        const RectF list = ui::toRectF(s.listRect);
        canvas.glass(list, 16.0f, 0.85f);

        if (s.choices) {
            const Gdiplus::GraphicsState saved = canvas.g().Save();
            RectF clip = list;
            clip.Inflate(-1.0f, -canvas.s(4.0f));
            canvas.g().SetClip(clip, Gdiplus::CombineModeIntersect);

            const int rowH = rowHeight(s);
            const float padX = canvas.s(16.0f);
            const float trackW = canvas.s(42.0f);
            const float trackH = canvas.s(24.0f);
            const int count = static_cast<int>(s.choices->size());
            const int first = std::max(0, s.scroll / rowH);
            for (int i = first; i < count; ++i) {
                const float y = list.Y + static_cast<float>(i * rowH - s.scroll);
                if (y > list.GetBottom()) {
                    break;
                }
                const auto& choice = (*s.choices)[static_cast<size_t>(i)];
                const RectF row(list.X + canvas.s(6.0f), y + canvas.s(3.0f),
                                list.Width - canvas.s(22.0f), static_cast<float>(rowH) - canvas.s(6.0f));
                if (i == s.selected) {
                    canvas.fillRound(row, 10.0f, Color(30, 255, 255, 255));
                    canvas.strokeRound(row, 10.0f, ui::withAlpha(ui::color::cyan(), 120));
                } else if (i == s.hoverRow) {
                    canvas.fillRound(row, 10.0f, Color(18, 255, 255, 255));
                }
                const RectF track(row.GetRight() - canvas.s(10.0f) - trackW,
                                  row.Y + (row.Height - trackH) / 2.0f, trackW, trackH);
                if (choice.enabled) {
                    canvas.toggleSwitch(track, true, false, ui::color::green(),
                                        Color(255, 45, 212, 191));
                } else {
                    // Blocked: red track with the knob on the left.
                    canvas.fillRoundGradient(track, 12.0f, Color(255, 248, 113, 113),
                                             Color(255, 214, 72, 92), false);
                    const float knob = track.Height - canvas.s(6.0f);
                    Gdiplus::SolidBrush knobBrush(Color(255, 255, 255, 255));
                    canvas.g().FillEllipse(&knobBrush, track.X + canvas.s(3.0f),
                                           track.Y + canvas.s(3.0f), knob, knob);
                }
                const wchar_t* tag = choice.enabled ? L"Instalar" : L"Bloquear";
                const float tagW = canvas.s(62.0f);
                canvas.text(tag, RectF(track.X - canvas.s(10.0f) - tagW, row.Y, tagW, row.Height),
                            11.0f, Weight::Semibold,
                            choice.enabled ? ui::color::green() : ui::color::red(),
                            ui::kRight | ui::kVCenter);
                canvas.text(choice.title,
                            RectF(row.X + padX, row.Y,
                                  track.X - canvas.s(22.0f) - tagW - (row.X + padX), row.Height),
                            13.0f, Weight::Regular,
                            choice.enabled ? ui::color::text() : ui::color::muted(),
                            ui::kLeft | ui::kVCenter | ui::kEllipsis);
            }
            canvas.g().Restore(saved);
        }

        const RECT thumb = thumbRect(s);
        if (thumb.bottom > thumb.top) {
            canvas.fillRound(ui::toRectF(thumb), 3.0f,
                             Color(s.draggingThumb ? 150 : 90, 255, 255, 255));
        }

        const auto button = [&](int id, const wchar_t* label, ui::ButtonStyle style, ui::Icon icon) {
            ui::ButtonState state;
            state.hover = s.hoverButton == id;
            state.pressed = s.pressedButton == id && s.hoverButton == id;
            canvas.button(ui::toRectF(s.buttons[id]), label, L"", style, state, icon);
        };
        button(kAllOn, L"Instalar todas", ui::ButtonStyle::Ghost, ui::Icon::None);
        button(kAllOff, L"Bloquear todas", ui::ButtonStyle::Ghost, ui::Icon::None);
        button(kCancel, L"Cancelar", ui::ButtonStyle::Glass, ui::Icon::None);
        button(kSave, L"Guardar", ui::ButtonStyle::Primary, ui::Icon::Check);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void finish(HWND hwnd, PickerState& s, int result) {
    s.result = result;
    // Re-enable the owner first so activation returns to it, not to another app.
    if (HWND owner = GetWindow(hwnd, GW_OWNER)) {
        EnableWindow(owner, TRUE);
    }
    DestroyWindow(hwnd);
}

void runButton(HWND hwnd, PickerState& s, int id) {
    switch (id) {
        case kSave:
            finish(hwnd, s, IDOK);
            return;
        case kCancel:
            finish(hwnd, s, IDCANCEL);
            return;
        case kAllOn:
        case kAllOff:
            if (s.choices) {
                for (auto& choice : *s.choices) {
                    choice.enabled = id == kAllOn;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        default:
            return;
    }
}

LRESULT CALLBACK pickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    PickerState* s = stateOf(hwnd);
    if (s == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_SIZE:
            layoutPicker(*s, LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 420;
            info->ptMinTrackSize.y = 320;
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paintPicker(hwnd, *s);
            return 0;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (!s->tracking) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                s->tracking = true;
            }
            if (s->draggingThumb) {
                const RECT thumb = thumbRect(*s);
                const int trackH = s->listRect.bottom - s->listRect.top - px(*s, 12.0f);
                const int thumbH = thumb.bottom - thumb.top;
                const int range = std::max(1, trackH - thumbH);
                const int top = pt.y - s->dragOffset - (s->listRect.top + px(*s, 6.0f));
                s->scroll = maxScroll(*s) * std::clamp(top, 0, range) / range;
                clampScroll(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int row = rowAt(*s, pt);
            const int button = buttonAt(*s, pt);
            if (row != s->hoverRow || button != s->hoverButton) {
                s->hoverRow = row;
                s->hoverButton = button;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            s->tracking = false;
            s->hoverRow = -1;
            s->hoverButton = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetFocus(hwnd);
            RECT thumb = thumbRect(*s);
            if (thumb.bottom > thumb.top) {
                InflateRect(&thumb, px(*s, 8.0f), 0);
                if (PtInRect(&thumb, pt)) {
                    s->draggingThumb = true;
                    s->dragOffset = pt.y - thumb.top;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            const int button = buttonAt(*s, pt);
            if (button != kNone) {
                s->pressedButton = button;
                s->hoverButton = button;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int row = rowAt(*s, pt);
            if (row >= 0) {
                s->selected = row;
                toggle(*s, row);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int pressed = s->pressedButton;
            const bool wasDragging = s->draggingThumb;
            s->pressedButton = kNone;
            s->draggingThumb = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!wasDragging && pressed != kNone && buttonAt(*s, pt) == pressed) {
                runButton(hwnd, *s, pressed);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            s->scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / 120 * rowHeight(*s) * 2;
            clampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && (s->hoverButton != kNone || s->hoverRow >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_KEYDOWN: {
            const int count = s->choices ? static_cast<int>(s->choices->size()) : 0;
            const int page = std::max<int>(1, (s->listRect.bottom - s->listRect.top) / rowHeight(*s) - 1);
            int selected = s->selected;
            switch (wParam) {
                case VK_ESCAPE:
                    finish(hwnd, *s, IDCANCEL);
                    return 0;
                case VK_SPACE:
                    toggle(*s, s->selected);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case VK_RETURN:
                    // Ctrl+Enter saves; Enter alone toggles (as before).
                    if (GetKeyState(VK_CONTROL) < 0) {
                        finish(hwnd, *s, IDOK);
                    } else {
                        toggle(*s, s->selected);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case VK_UP:
                    --selected;
                    break;
                case VK_DOWN:
                    ++selected;
                    break;
                case VK_PRIOR:
                    selected -= page;
                    break;
                case VK_NEXT:
                    selected += page;
                    break;
                case VK_HOME:
                    selected = 0;
                    break;
                case VK_END:
                    selected = count - 1;
                    break;
                default:
                    return 0;
            }
            if (count > 0) {
                s->selected = std::clamp(selected, 0, count - 1);
                ensureVisible(*s, s->selected);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CLOSE:
            finish(hwnd, *s, IDCANCEL);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool showUpdatePickerDialog(HWND parent, HINSTANCE instance, std::vector<UpdateChoice>* choices) {
    if (!choices) {
        return false;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = pickerWndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"WinAutoUpdaterUpdatePicker";
        RegisterClassExW(&wc);
        registered = true;
    }

    std::vector<UpdateChoice> working = *choices;
    PickerState state;
    state.choices = &working;

    // Size for the parent's monitor: design size at its DPI, shrunk to fit.
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    const UINT dpi = ui::dpiForWindow(parent);
    const RECT work = ui::workAreaFor(parent);
    RECT frame{0, 0, 0, 0};
    ui::adjustWindowRect(&frame, style, exStyle, dpi);
    const int frameW = frame.right - frame.left;
    const int frameH = frame.bottom - frame.top;
    float scale = static_cast<float>(dpi) / 96.0f;
    scale = std::min(scale, static_cast<float>(work.right - work.left - frameW - 16) / kDesignW);
    scale = std::min(scale, static_cast<float>(work.bottom - work.top - frameH - 16) / kDesignH);
    scale = std::max(scale, 0.5f);
    const int winW = static_cast<int>(std::lround(kDesignW * scale)) + frameW;
    const int winH = static_cast<int>(std::lround(kDesignH * scale)) + frameH;
    const int x = work.left + std::max<int>(0, (work.right - work.left - winW) / 2);
    const int y = work.top + std::max<int>(0, (work.bottom - work.top - winH) / 2);

    HWND dlg = CreateWindowExW(exStyle, L"WinAutoUpdaterUpdatePicker", L"Filtro de updates", style,
                               x, y, winW, winH, parent, nullptr, instance, &state);
    if (!dlg) {
        return false;
    }
    ui::applyDarkFrame(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(dlg);

    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.background) {
        DeleteObject(state.background);
    }

    if (state.result == IDOK) {
        *choices = std::move(working);
        return true;
    }
    return false;
}
