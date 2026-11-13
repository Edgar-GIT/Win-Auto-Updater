#pragma once

#include "nt_core.hpp"

#include "ui.hpp"

#include <filesystem>

// Base of every native-test window (dashboard, the separate test windows, the
// monitor bar): one custom-drawn GDI+ surface with the updater's glass theme,
// laid out in design units × scale, plus the few clickable widgets it needs.

namespace nt {

class Panel {
public:
    struct Widget {
        int id = 0;
        RECT rc{};
        std::wstring text;
        ui::ButtonStyle style = ui::ButtonStyle::Glass;
        ui::Icon icon = ui::Icon::None;
        bool visible = false;
        bool enabled = true;
    };

    Panel() = default;
    virtual ~Panel();
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    bool create(HINSTANCE instance, const wchar_t* title, DWORD style, DWORD exStyle);
    void destroy();
    HWND hwnd() const { return hwnd_; }
    void invalidate();
    // Moves / sizes the window so that its client area has this size.
    void placeClient(int x, int y, int clientWidth, int clientHeight, bool show);
    // Outer window size for a client area of the given size.
    SIZE outerSize(int clientWidth, int clientHeight) const;

#ifdef SU_UI_SNAPSHOT
    // Developer aid: renders the window at the given client size into a PNG.
    bool snapshot(const std::filesystem::path& file, int width, int height);
#endif

protected:
    // Client size (96-DPI pixels) the layout is authored for.
    virtual SIZE designSize() const = 0;
    // Scale for a client area; the default fits the whole design inside it.
    virtual float scaleFor(int clientWidth, int clientHeight) const;
    virtual void onLayout() {}
    virtual void onDraw(ui::Canvas& canvas) = 0;
    virtual void onCommand(int /*id*/) {}
    // Return false to keep the window open.
    virtual bool onCloseRequest() { return true; }
    virtual void onDestroyed() {}
    virtual void onDisplayChange() {}

    void addWidget(int id, const wchar_t* text, ui::ButtonStyle style, ui::Icon icon);
    Widget* widget(int id);
    void place(int id, int x, int y, int w, int h);
    void hideWidgets();
    void drawWidgets(ui::Canvas& canvas);
    int px(float du) const;
    void relayout();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    int clientW_ = 1;
    int clientH_ = 1;
    int hoverId_ = 0;
    int pressedId_ = 0;

private:
    void layout(int width, int height);
    void paint();
    void render(HDC dc, int width, int height);
    void releaseSurfaces();
    int widgetAt(POINT pt) const;
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::vector<Widget> widgets_;
    bool tracking_ = false;
    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0;
    int backH_ = 0;
    HDC bgDc_ = nullptr;
    HBITMAP bgBitmap_ = nullptr;
    HGDIOBJ bgOld_ = nullptr;
    int bgW_ = 0;
    int bgH_ = 0;
};

}  // namespace nt
