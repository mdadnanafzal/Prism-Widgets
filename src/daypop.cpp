// Day popover: lists a day's events (holidays, own, synced) with add / edit / delete / repeat-yearly.
// Drawn as a small glass panel with the calendar widget's theme, so it matches the widgets.
#include "events.h"
#include "app.h"

namespace {
struct PopHot { D2D1_RECT_F r; int id; std::function<void()> act; };
struct Pop {
    HWND hwnd = nullptr; int day = 0; RECT anchor{}; float s = 1; int x = 0, y = 0, W = 0, H = 0;
    std::vector<PopHot> hots; int hover = -1, nextId = 0;
    GlassCache glass;
} P;
bool g_reg = false;
const float POP_W = 256, HEAD_H = 50, ROW_H = 40, FOOT_H = 40;
const int MAX_ROWS = 6;

D2D1_RECT_F Rc(float l, float t, float r, float b) { return D2D1_RECT_F{ l, t, r, b }; }

Widget* Calendar() { for (auto& w : g_widgets) if (w->key == L"calendar") return w.get(); return nullptr; }
const Theme& PopTheme() {
    Widget* c = Calendar();
    const Theme& t = c ? ThemeFor(c) : FindTheme(g_theme);
    return t.panel ? t : FindTheme(L"frosted");   // "Clear (no panel)" would be unreadable floating over things
}

std::wstring DayTitle(int day) {
    int y, m, d; SplitDay(day, y, m, d);
    SYSTEMTIME st{}; st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    int today = TodayKey();
    std::wstring t = FormatDateTime(st, L"ddd, d MMM");
    if (day == today) t = L"Today · " + t;
    else if (day == AddDays(today, 1)) t = L"Tomorrow · " + t;
    if (y != today / 10000) t += L" " + std::to_wstring(y);
    return t;
}
int RowsFor(int day) { std::vector<const Occ*> v; g_events.ForDay(day, v); return (int)v.size(); }
float HeightFor(int n) { return HEAD_H + (n ? std::min(n, MAX_ROWS) * ROW_H + (n > MAX_ROWS ? 20 : 0) : 34) + FOOT_H + 4; }

void Place() {
    HMONITOR hm = MonitorFromRect(&P.anchor, MONITOR_DEFAULTTONEAREST);
    Widget* c = Calendar();
    P.s = ScaleOf(hm) * (c ? std::max(.85f, std::min(c->size, 1.3f)) : 1.f);
    RECT wa = WorkOf(hm);
    P.W = (int)(POP_W * P.s); P.H = (int)(HeightFor(RowsFor(P.day)) * P.s);
    int gap = (int)(12 * P.s), m = GlassMargin(P.s);
    int x = P.anchor.right + gap;                                    // to the right of the day…
    if (x + P.W > wa.right - 8) x = P.anchor.left - gap - P.W;        // …or to the left
    P.x = std::max((int)wa.left + 8, std::min(x, (int)wa.right - P.W - 8));
    P.y = std::max((int)wa.top + 8, std::min((int)P.anchor.top - (int)(14 * P.s), (int)wa.bottom - P.H - 8));
    SetWindowPos(P.hwnd, HWND_TOPMOST, P.x - m, P.y - m, P.W + 2 * m, P.H + 2 * m, SWP_NOACTIVATE);
}

void InputAt(D2D1_RECT_F r, const std::wstring& init, const wchar_t* cue, std::function<void(const std::wstring&)> done) {
    RECT sr{ P.x + (LONG)(r.left * P.s), P.y + (LONG)(r.top * P.s), P.x + (LONG)(r.right * P.s), P.y + (LONG)(r.bottom * P.s) };
    float s = P.s;
    CloseDayPopup();                       // the input box takes focus; the popover reopens after saving
    ShowInputAt(sr, s, init, cue, std::move(done));
}

void DrawPop(Gfx& g, float W, float H) {
    P.hots.clear(); P.nextId = 0;
    const Ink& k = g.ink;
    D2D1_COLOR_F sep = Alpha(k.fg, .14f), press = Alpha(k.fg, .2f);
    auto hot = [&](D2D1_RECT_F r, std::function<void()> a) { int i = P.nextId++; P.hots.push_back({ r, i, std::move(a) }); return P.hover == i; };

    // header
    std::vector<const Occ*> ev; g_events.ForDay(P.day, ev);
    int n = (int)ev.size();
    g.Text(DayTitle(P.day), Rc(16, 10, W - 40, 30), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, k.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.Text(n == 0 ? L"No events" : n == 1 ? L"1 event" : std::to_wstring(n) + L" events", Rc(16, 29, W - 40, 44), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, k.fg2);
    D2D1_RECT_F xr = Rc(W - 36, 10, W - 10, 36);
    if (hot(xr, [] { CloseDayPopup(); })) g.FillRound(xr, 8, k.hover);
    g.Icon(0xE711, xr, 10, k.fg2);
    g.Line(14, HEAD_H - 1, W - 14, HEAD_H - 1, sep, 1);

    // rows
    float y = HEAD_H;
    if (n == 0) { g.Text(g_events.mine ? L"Nothing planned" : L"Nothing on this day", Rc(16, y + 6, W - 16, y + 30), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, k.fg3, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER); y += 34; }
    for (int i = 0; i < std::min(n, MAX_ROWS); i++) {
        const Occ& o = *ev[i];
        D2D1_RECT_F row = Rc(6, y + 2, W - 6, y + ROW_H - 2);
        int mineIdx = o.mine;
        bool own = o.src == EV_MINE && mineIdx >= 0;
        if (own) {   // clicking the row edits it
            int dayKey = P.day; float ry = y;
            if (hot(row, [mineIdx, dayKey, ry] {
                    RECT a = P.anchor;
                    InputAt(Rc(10, ry + 4, POP_W - 10, ry + ROW_H - 4), g_events.TextOf(mineIdx), L"e.g. 18:30 Dinner with Sam",
                            [mineIdx, dayKey, a](const std::wstring& t) { g_events.EditFromText(mineIdx, t); ShowDayPopup(dayKey, a); });
                })) g.FillRound(row, 8, k.hover);
        }
        g.FillRound(Rc(14, y + 8, 17, y + ROW_H - 8), 1.5f, EventColor(o.src, k.accentText));
        float tx1 = own ? W - 62 : W - 14;
        g.Text(o.title, Rc(25, y + 4, tx1, y + 22), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, k.fg);
        wchar_t tm[16]; if (o.sh >= 0) swprintf(tm, 16, L"%02d:%02d", o.sh, o.sm); else wcscpy(tm, L"All day");
        std::wstring sub = tm;
        if (o.src == EV_HOLIDAY) sub += L" · Holiday";
        else if (o.src == EV_SYNC) sub += L" · Synced";
        else if (own && g_events.my[mineIdx].yearly) sub += L" · Every year";
        g.Text(sub, Rc(25, y + 21, tx1, y + 36), 11, DWRITE_FONT_WEIGHT_NORMAL, k.fg2);
        if (own) {   // repeat yearly / delete (edit = click the row)
            struct B { wchar_t glyph; int kind; } btns[2] = { { 0xE8EE, 0 }, { 0xE74D, 2 } };
            for (int b = 0; b < 2; b++) {
                D2D1_RECT_F br = Rc(W - 60 + b * 26, y + 8, W - 36 + b * 26, y + 32);
                int kind = btns[b].kind;
                bool hv = hot(br, [kind, mineIdx] { if (kind == 0) g_events.ToggleYearly(mineIdx); else g_events.Delete(mineIdx); });
                bool on = kind == 0 && g_events.my[mineIdx].yearly;
                if (on) g.FillRound(br, 7, Alpha(k.accentText, .25f)); else if (hv) g.FillRound(br, 7, press);
                g.Icon(btns[b].glyph, br, 11, kind == 2 && hv ? D2D1_COLOR_F{ 1.f, .45f, .42f, 1 } : on ? k.accentText : k.fg2);
            }
        }
        y += ROW_H;
    }
    if (n > MAX_ROWS) { g.Text(L"+" + std::to_wstring(n - MAX_ROWS) + L" more", Rc(25, y, W - 14, y + 18), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, k.fg3); }

    // footer: same style as the To-Do "Add a task" row
    D2D1_RECT_F ab = Rc(6, H - FOOT_H, W - 6, H - 6);
    if (g_events.mine) {
        int dayKey = P.day;
        bool hv = hot(ab, [ab, dayKey] {
            RECT a = P.anchor;
            InputAt(Rc(10, ab.top, POP_W - 10, ab.bottom), L"", L"e.g. 18:30 Team meeting",
                    [dayKey, a](const std::wstring& t) { g_events.AddFromText(dayKey, t); ShowDayPopup(dayKey, a); });
        });
        if (hv) g.FillRound(ab, 8, k.hover);
        g.Icon(0xE710, Rc(ab.left + 6, ab.top, ab.left + 26, ab.bottom), 11, k.fg2);
        g.Text(L"Add event", Rc(ab.left + 32, ab.top, ab.right - 8, ab.bottom), 13, DWRITE_FONT_WEIGHT_NORMAL, k.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        g.Text(L"Turn on “My events” to add your own", Rc(16, ab.top, W - 14, ab.bottom), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, k.fg3, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void Redraw() { if (P.hwnd) RenderGlassPopup(P.hwnd, P.glass, PopTheme(), P.x, P.y, P.W, P.H, P.s, DrawPop); }

int HitAt(LPARAM lp) {   // client coords include the shadow margin
    float m = (float)GlassMargin(P.s);
    float x = (GET_X_LPARAM(lp) - m) / P.s, y = (GET_Y_LPARAM(lp) - m) / P.s;
    for (int i = (int)P.hots.size() - 1; i >= 0; i--) {
        auto& h = P.hots[i];
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.id;
    }
    return -1;
}

LRESULT CALLBACK PopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_MOUSEMOVE: {
        Interactive();
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        int id = HitAt(lp);
        if (id != P.hover) { P.hover = id; Redraw(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (P.hover != -1) { P.hover = -1; Redraw(); } return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, P.hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_LBUTTONUP: {
        int id = HitAt(lp);
        std::function<void()> act;
        for (auto& x : P.hots) if (x.id == id) act = x.act;
        if (act) act();
        if (P.hwnd) { P.hover = -1; Place(); Redraw(); }
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { CloseDayPopup(); return 0; } break;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) PostMessageW(h, WM_CLOSE, 0, 0); return 0;   // click elsewhere closes it
    case WM_CLOSE: CloseDayPopup(); return 0;
    case WM_DESTROY: if (P.hwnd == h) P = Pop(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
} // namespace

void ShowDayPopup(int day, RECT anchor) {
    if (!g_reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = PopProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismDayPopup"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc); g_reg = true;
    }
    if (!P.hwnd) {
        P.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PrismDayPopup", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
        if (!P.hwnd) return;
    }
    P.day = day; P.anchor = anchor; P.hover = -1;
    Place();
    Redraw();
    ShowWindow(P.hwnd, SW_SHOW);
    SetForegroundWindow(P.hwnd);
}
void RefreshDayPopup() { if (P.hwnd) { P.glass.sx = INT_MIN; Place(); Redraw(); } }   // theme / wallpaper may have changed
void CloseDayPopup() { if (P.hwnd) { HWND h = P.hwnd; P = Pop(); DestroyWindow(h); } }
