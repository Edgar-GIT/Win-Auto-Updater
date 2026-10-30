#include "updatepicker.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace {

constexpr int kDlgW = 620;
constexpr int kDlgH = 520;
constexpr int kPad = 16;
constexpr int kBtnH = 36;
constexpr int kRowH = 36;
constexpr int kCheckSize = 22;
constexpr int kIdList = 1001;
constexpr int kIdOk = 1002;
constexpr int kIdCancel = 1003;
constexpr int kIdHint = 1004;

struct PickerState {
    std::vector<UpdateChoice>* choices = nullptr;
    HFONT font = nullptr;
    HWND list = nullptr;
    int result = IDCANCEL;
};

PickerState* stateOf(HWND hwnd) {
    return reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void layoutPicker(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int btnY = h - kPad - kBtnH;
    const int btnW = 140;

    HWND hint = GetDlgItem(hwnd, kIdHint);
    HWND list = GetDlgItem(hwnd, kIdList);
    HWND ok = GetDlgItem(hwnd, kIdOk);
    HWND cancel = GetDlgItem(hwnd, kIdCancel);

    if (hint) {
        MoveWindow(hint, kPad, kPad, w - kPad * 2, 44, TRUE);
    }
    if (list) {
        MoveWindow(list, kPad, kPad + 52, w - kPad * 2, btnY - (kPad + 52) - 12, TRUE);
    }
    if (ok) {
        MoveWindow(ok, w - kPad - btnW * 2 - 10, btnY, btnW, kBtnH, TRUE);
    }
    if (cancel) {
        MoveWindow(cancel, w - kPad - btnW, btnY, btnW, kBtnH, TRUE);
    }
}

int hitCheckIndex(HWND list, int x, int y) {
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (count <= 0) {
        return -1;
    }
    RECT item{};
    if (SendMessageW(list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
        return -1;
    }
    RECT client{};
    GetClientRect(list, &client);
    const int topIndex = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    const int index = topIndex + (y - item.top) / kRowH;
    if (index < 0 || index >= count) {
        return -1;
    }
    const int checkLeft = client.right - kPad - kCheckSize;
    if (x >= checkLeft && x <= checkLeft + kCheckSize) {
        return index;
    }
    // Also toggle when clicking the row text.
    if (x >= 0 && x < client.right) {
        return index;
    }
    return -1;
}

void toggleAt(HWND hwnd, int index) {
    auto* state = stateOf(hwnd);
    if (!state || !state->choices || index < 0 ||
        index >= static_cast<int>(state->choices->size())) {
        return;
    }
    (*state->choices)[static_cast<size_t>(index)].enabled =
        !(*state->choices)[static_cast<size_t>(index)].enabled;
    if (state->list) {
        InvalidateRect(state->list, nullptr, TRUE);
    }
}

LRESULT CALLBACK pickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE: {
            auto* state = stateOf(hwnd);
            CreateWindowExW(0, L"STATIC",
                            L"Verde = instalar nesta sessão. Vermelho = saltar.\r\n"
                            L"Esta lista reinicia sempre que abres a aplicação.",
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHint)), nullptr,
                            nullptr);

            state->list = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY |
                    LBS_NOINTEGRALHEIGHT,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), nullptr,
                nullptr);

            CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0,
                            0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOk)), nullptr,
                            nullptr);
            CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), nullptr,
                            nullptr);

            if (state->font) {
                EnumChildWindows(
                    hwnd,
                    [](HWND child, LPARAM lp) -> BOOL {
                        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(state->font));
            }
            if (state->choices) {
                for (const auto& c : *state->choices) {
                    SendMessageW(state->list, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(c.title.c_str()));
                }
            }
            layoutPicker(hwnd);
            return 0;
        }
        case WM_SIZE:
            layoutPicker(hwnd);
            return 0;
        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis && mis->CtlID == kIdList) {
                mis->itemHeight = kRowH;
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || dis->CtlID != kIdList) {
                break;
            }
            auto* state = stateOf(hwnd);
            if (!state || !state->choices) {
                return TRUE;
            }
            const int index = static_cast<int>(dis->itemID);
            if (index < 0 || index >= static_cast<int>(state->choices->size())) {
                return TRUE;
            }
            const auto& choice = (*state->choices)[static_cast<size_t>(index)];
            const bool selected = (dis->itemState & ODS_SELECTED) != 0;

            HBRUSH bg = CreateSolidBrush(selected ? RGB(232, 240, 254) : RGB(255, 255, 255));
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, RGB(30, 30, 30));
            RECT textRc = dis->rcItem;
            textRc.left += 12;
            textRc.right -= (kPad + kCheckSize + 12);
            DrawTextW(dis->hDC, choice.title.c_str(), -1, &textRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            const int cy = (dis->rcItem.top + dis->rcItem.bottom - kCheckSize) / 2;
            const int cx = dis->rcItem.right - kPad - kCheckSize;
            RECT box{cx, cy, cx + kCheckSize, cy + kCheckSize};
            const COLORREF fill = choice.enabled ? RGB(46, 160, 67) : RGB(200, 55, 55);
            HBRUSH brush = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
            HGDIOBJ oldB = SelectObject(dis->hDC, brush);
            HGDIOBJ oldP = SelectObject(dis->hDC, pen);
            RoundRect(dis->hDC, box.left, box.top, box.right, box.bottom, 6, 6);
            SelectObject(dis->hDC, oldB);
            SelectObject(dis->hDC, oldP);
            DeleteObject(brush);
            DeleteObject(pen);

            if (choice.enabled) {
                HPEN white = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                HGDIOBJ old = SelectObject(dis->hDC, white);
                MoveToEx(dis->hDC, box.left + 5, box.top + 11, nullptr);
                LineTo(dis->hDC, box.left + 9, box.bottom - 6);
                LineTo(dis->hDC, box.right - 5, box.top + 6);
                SelectObject(dis->hDC, old);
                DeleteObject(white);
            }
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            auto* state = stateOf(hwnd);
            if (id == kIdList && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
                POINT pt{};
                GetCursorPos(&pt);
                HWND list = GetDlgItem(hwnd, kIdList);
                ScreenToClient(list, &pt);
                const int hit = hitCheckIndex(list, pt.x, pt.y);
                if (hit >= 0) {
                    toggleAt(hwnd, hit);
                }
                return 0;
            }
            if (id == kIdOk) {
                if (state) {
                    state->result = IDOK;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kIdCancel) {
                if (state) {
                    state->result = IDCANCEL;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            auto* state = stateOf(hwnd);
            if (state) {
                state->result = IDCANCEL;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool showUpdatePickerDialog(HWND parent, HINSTANCE instance, HFONT font,
                            std::vector<UpdateChoice>* choices) {
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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"WinAutoUpdaterUpdatePicker";
        RegisterClassExW(&wc);
        registered = true;
    }

    std::vector<UpdateChoice> working = *choices;
    PickerState state;
    state.choices = &working;
    state.font = font;
    state.result = IDCANCEL;

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    HWND dlg =
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"WinAutoUpdaterUpdatePicker",
                        L"Configurar atualizações", WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_VISIBLE,
                        (screenW - kDlgW) / 2, (screenH - kDlgH) / 2, kDlgW, kDlgH, parent, nullptr,
                        instance, &state);
    if (!dlg) {
        return false;
    }

    EnableWindow(parent, FALSE);
    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            (msg.hwnd == dlg || IsChild(dlg, msg.hwnd))) {
            state.result = IDCANCEL;
            DestroyWindow(dlg);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.result == IDOK) {
        *choices = std::move(working);
        return true;
    }
    return false;
}
