#pragma once
#include "common.h"
#include <functional>
#include <unordered_map>

// ------------------------------------------------------------ graphics
struct Graphics {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    ID2D1StrokeStyle* round = nullptr;
    int generation = 0;   // bumps whenever the render target is recreated
    bool Init();
    bool EnsureTarget();
    void DropTarget();
    IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w);
    void ClearFormats();
    // B5: text layouts are cached, so unchanged labels aren't re-shaped on every redraw
    IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h, int align, int valign, bool wrap, bool strike);
    void ClearLayouts();
private:
    struct Fmt { std::wstring fam; float size; int weight; IDWriteTextFormat* f; IDWriteInlineObject* sign; };
    std::vector<Fmt> fmts_;
    std::unordered_map<std::wstring, IDWriteTextLayout*> layouts_;
    IDWriteInlineObject* SignFor(IDWriteTextFormat* f);
};
extern Graphics g_gr;

#define FONT_DISPLAY L"Segoe UI Variable Display"
#define FONT_TEXT    L"Segoe UI Variable Text"
#define FONT_ICONS   L"Segoe Fluent Icons"

struct Ink {
    D2D1_COLOR_F fg, fg2, fg3, accent, onAccent, hover;
    D2D1_COLOR_F accentText;   // accent adjusted for contrast against the glass behind it
    bool shadow = false;   // draw soft text shadow (clear theme)
    float radius = 16;     // panel corner radius (DIP) of the current theme
};

inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline D2D1_RECT_F RectF(float l, float t, float r, float b) { return D2D1_RECT_F{ l, t, r, b }; }

class Gfx {
public:
    ID2D1RenderTarget* rt; ID2D1SolidColorBrush* br; Ink ink;
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
              const wchar_t* family = FONT_TEXT, bool strike = false, bool wrap = false);
    float Measure(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = FONT_TEXT);
    void FillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c);
    void StrokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float width);
    void FillCircle(float x, float y, float r, D2D1_COLOR_F c);
    void StrokeCircle(float x, float y, float r, D2D1_COLOR_F c, float width);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width);
    void Arc(float cx, float cy, float r, float startDeg, float sweepDeg, D2D1_COLOR_F c, float width);
    void Icon(wchar_t glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
};

// ------------------------------------------------------------ widget options (shown in Settings)
struct Opt {
    enum Kind { Toggle, Choice, Action } kind;
    std::wstring label, hint;
    std::vector<std::wstring> choices;     // Choice
    std::function<int()> get;              // Toggle: 0/1, Choice: index
    std::function<void(int)> set;          // Action: set(0) runs it
};

// ------------------------------------------------------------ widget base
class Widget {
public:
    virtual ~Widget();
    std::wstring key;       // "clock", "calendar", "todo", "system", "custom:<file>"
    std::wstring title;
    bool enabled = true;
    bool placed = false;    // has a saved position
    int x = 0, y = 0;       // panel top-left, physical screen px
    // saved placement: corner of a monitor's work area + offset in DIPs (survives resolution/DPI/taskbar changes)
    bool hasAnchor = false;
    int anchor = 0;         // bit0 = right edge, bit1 = bottom edge
    float adx = 0, ady = 0;
    std::wstring mon;       // monitor device name, e.g. \\.\DISPLAY1
    float size = 1.0f;      // user size factor
    std::wstring theme;     // empty = global theme

    // runtime
    HWND hwnd = nullptr;
    HDC memDC = nullptr; HBITMAP dib = nullptr, oldBmp = nullptr; uint32_t* bits = nullptr;
    int bw = 0, bh = 0, margin = 0;
    float scale = 1.0f;     // monitor DPI / 96
    std::vector<uint32_t> bg; bool bgValid = false; float lum = 0.3f;
    std::vector<uint32_t> content; bool contentValid = false; uint32_t contentKey = 0;   // A4: reused in Arrange mode
    ULONGLONG due = 0;
    bool dirty = true;

    // Size model: content size in DIPs (resizable between Min and Max), then uniformly scaled by `size`.
    float cw = 0, ch = 0;                             // user content size (0 = default)
    virtual D2D1_SIZE_F DefaultSize() = 0;
    virtual D2D1_SIZE_F MinSize() { return DefaultSize(); }
    virtual D2D1_SIZE_F MaxSize() { return DefaultSize(); }
    D2D1_SIZE_F BaseSize() {                          // current content size in DIPs
        D2D1_SIZE_F d = DefaultSize(), mn = MinSize(), mx = MaxSize();
        float w = cw > 0 ? cw : d.width, h = ch > 0 ? ch : d.height;
        return D2D1_SIZE_F{ Clamp(w, mn.width, mx.width), Clamp(h, mn.height, mx.height) };
    }
    bool ResizableW() { return MaxSize().width > MinSize().width; }
    bool ResizableH() { return MaxSize().height > MinSize().height; }
    virtual void Draw(Gfx& g, float W, float H) = 0;
    virtual int  Interval() { return 60000; }        // 1000 / 60000 = aligned to second / minute
    virtual void Tick() {}                           // every scheduler pass, even while paused (timers, slideshows); set `dirty` to redraw
    virtual bool Click(float, float) { return false; }
    // B3: instant press feedback. HitId says which pressable part is under a point.
    virtual bool HitId(float, float, int&) { return false; }
    bool isPressed = false; int pressed = 0;
    bool Pressed(int id) const { return isPressed && pressed == id; }
    virtual bool RightClickAt(float, float, HMENU) { return false; } // add item-specific entries
    virtual bool Wheel(float, float, int) { return false; }
    virtual bool Hover(float, float) { return false; }
    virtual void Leave() {}
    virtual void Options(std::vector<Opt>&) {}       // settings-window options
    virtual std::wstring Description() { return L""; }
    virtual void Menu(HMENU) {}                      // widget options (ids 100-199)
    virtual void Command(int) {}
    virtual void Load(const JVal&) {}
    virtual void Save(JVal&) {}
    virtual bool IsCustom() { return false; }
    virtual std::wstring DefaultTheme() { return L""; }
    virtual std::wstring FilePath() { return L""; }
};

std::unique_ptr<Widget> MakeClock();
std::unique_ptr<Widget> MakeCalendar();
std::unique_ptr<Widget> MakeTodo();
std::unique_ptr<Widget> MakeWeather();
std::unique_ptr<Widget> MakeMedia();
std::unique_ptr<Widget> MakeSystem();
std::unique_ptr<Widget> MakeCustom(const std::wstring& file);
std::unique_ptr<Widget> MakeFocus();
std::unique_ptr<Widget> MakeNote();
std::unique_ptr<Widget> MakeWorldClock();
std::unique_ptr<Widget> MakeNetSpeed();
std::unique_ptr<Widget> MakePhoto();
void LoadTodos();
void WriteSampleWidgets();

// host services (app.cpp)
void RequestRedraw(Widget* w);
void RequestSave();
void ShowInput(Widget* w, D2D1_RECT_F localRect, const std::wstring& init, std::function<void(const std::wstring&)> done);
// multi-line editor over a widget: Enter = new line, Ctrl+Enter / Esc / clicking away saves
void ShowNoteInput(Widget* w, D2D1_RECT_F localRect, const std::wstring& init, float fontSize, std::function<void(const std::wstring&)> done);
// small glass card in the bottom-right corner (reminders.cpp)
void ShowToast(const std::wstring& head, const std::wstring& title, wchar_t glyph, bool sound);
