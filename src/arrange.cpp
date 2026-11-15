// Arrange mode: a full-screen editing layer (dimmed wallpaper, dot grid, safe area, alignment guides)
// with the widgets lifted above it and a floating toolbar. Everything here exists only while arranging.
#include "app.h"

bool g_arranging = false;
Widget* g_arrangeSel = nullptr;
std::wstring g_arrangeBadge;

static bool operator==(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

namespace {

struct Overlay {
    HWND wnd = nullptr, bar = nullptr;
    int vx = 0, vy = 0, vw = 0, vh = 0;                  // virtual screen
    struct Mon { RECT rc; RECT work; float s; int pw, ph; std::vector<uint32_t> px; };
    std::vector<Mon> mons;                               // dimmed wallpaper per monitor
    std::vector<RECT> guides;                            // screen coords
    std::vector<HWND> guideWnds;                         // thin accent windows above the widgets
    // toolbar
    float bs = 1; int bw = 0, bh = 0; int hover = -1;   // 0 auto-arrange, 1 done
    D2D1_RECT_F btn[2]{};
} O;
bool g_registered = false;

COLORREF AccentRef() { D2D1_COLOR_F a = Accent(); return RGB((int)(a.r * 255), (int)(a.g * 255), (int)(a.b * 255)); }
RECT ToClient(RECT r) { OffsetRect(&r, -O.vx, -O.vy); return r; }

void BuildDimmedWallpaper() {
    O.mons.clear();
    g_wall.Ensure();
    for (auto& m : g_wall.Mons()) {
        Overlay::Mon d;
        d.rc = m.rc; d.pw = m.pw; d.ph = m.ph;
        HMONITOR hm = MonitorFromRect(&m.rc, MONITOR_DEFAULTTONEAREST);
        d.work = WorkOf(hm); d.s = ScaleOf(hm);
        d.px = m.px;
        float sx = (float)m.pw / (m.rc.right - m.rc.left), sy = (float)m.ph / (m.rc.bottom - m.rc.top);
        int wx0 = (int)((d.work.left - m.rc.left) * sx), wx1 = (int)((d.work.right - m.rc.left) * sx);
        int wy0 = (int)((d.work.top - m.rc.top) * sy), wy1 = (int)((d.work.bottom - m.rc.top) * sy);
        for (int y = 0; y < d.ph; y++) for (int x = 0; x < d.pw; x++) {
            uint32_t& p = d.px[(size_t)y * d.pw + x];
            bool in = x >= wx0 && x < wx1 && y >= wy0 && y < wy1;
            int k = in ? 150 : 70;   // dim (x/256); taskbar area darker
            uint32_t r = ((p >> 16) & 255) * k >> 8, g = ((p >> 8) & 255) * k >> 8, b = (p & 255) * k >> 8;
            p = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        O.mons.push_back(std::move(d));
    }
}

void PaintOverlay(HDC dc, const RECT& paint) {
    HBRUSH dark = CreateSolidBrush(RGB(18, 18, 22));
    FillRect(dc, &paint, dark);
    DeleteObject(dark);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    HBRUSH dot = CreateSolidBrush(RGB(150, 152, 168));
    HPEN dash = CreatePen(PS_DOT, 1, RGB(165, 168, 185));
    for (auto& m : O.mons) {
        RECT d = ToClient(m.rc), x;
        if (!IntersectRect(&x, &d, &paint)) continue;
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = m.pw; bi.bmiHeader.biHeight = -m.ph;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, m.pw, m.ph, m.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        // dot grid (every 32 DIP) inside the safe area
        int mg = (int)(8 * m.s), sp = (int)(32 * m.s), dsz = std::max(2, (int)(2 * m.s));
        RECT sa = ToClient(m.work); InflateRect(&sa, -mg, -mg);
        for (int yy = sa.top; yy <= sa.bottom; yy += sp) {
            if (yy + dsz < paint.top || yy > paint.bottom) continue;
            for (int xx = sa.left; xx <= sa.right; xx += sp) {
                if (xx + dsz < paint.left || xx > paint.right) continue;
                RECT r{ xx, yy, xx + dsz, yy + dsz }; FillRect(dc, &r, dot);
            }
        }
        HGDIOBJ op = SelectObject(dc, dash), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        Rectangle(dc, sa.left, sa.top, sa.right, sa.bottom);
        SelectObject(dc, op); SelectObject(dc, ob);
    }
    DeleteObject(dot); DeleteObject(dash);
}

HBRUSH g_guideBrush = nullptr;
LRESULT CALLBACK GuideProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, g_guideBrush); return 1; }
    return DefWindowProcW(h, m, wp, lp);
}
// Guides live in tiny click-through topmost windows so they are drawn on top of the widgets.
void SetGuides(std::vector<RECT> gs) {
    if (!O.wnd) return;
    if (gs == O.guides) return;
    O.guides.swap(gs);
    while (O.guideWnds.size() < O.guides.size()) {
        HWND g = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                 L"PrismArrangeGuide", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
        if (!g) break;
        SetLayeredWindowAttributes(g, 0, 255, LWA_ALPHA);
        O.guideWnds.push_back(g);
    }
    for (size_t i = 0; i < O.guideWnds.size(); i++) {
        if (i < O.guides.size()) {
            const RECT& r = O.guides[i];
            SetWindowPos(O.guideWnds[i], HWND_TOPMOST, r.left, r.top, std::max(1L, r.right - r.left), std::max(1L, r.bottom - r.top),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else ShowWindow(O.guideWnds[i], SW_HIDE);
    }
}

// ---------------------------------------------------------------- toolbar
void RenderBar() {
    if (!O.bar || !g_gr.EnsureTarget()) return;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = O.bw; bi.bmiHeader.biHeight = -O.bh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        RECT rc{ 0, 0, O.bw, O.bh };
        if (SUCCEEDED(g_gr.rt->BindDC(mem, &rc))) {
            g_gr.rt->SetDpi(96 * O.bs, 96 * O.bs);
            g_gr.rt->BeginDraw();
            D2D1_MATRIX_3X2_F id; id._11 = 1; id._12 = 0; id._21 = 0; id._22 = 1; id._31 = 0; id._32 = 0;
            g_gr.rt->SetTransform(&id);
            D2D1_COLOR_F clr{ 0, 0, 0, 0 }; g_gr.rt->Clear(&clr);
            Ink ink{}; ink.shadow = false;
            Gfx g{ g_gr.rt, g_gr.br, ink };
            float W = O.bw / O.bs, H = O.bh / O.bs;
            D2D1_COLOR_F acc = Accent();
            g.FillRound(D2D1_RECT_F{ 1, 1, W - 1, H - 1 }, (H - 2) / 2, D2D1_COLOR_F{ .10f, .10f, .12f, .94f });
            g.StrokeRound(D2D1_RECT_F{ 1.5f, 1.5f, W - 1.5f, H - 1.5f }, (H - 3) / 2, D2D1_COLOR_F{ 1, 1, 1, .14f }, 1);
            g.FillCircle(34, H / 2, 17, Alpha(acc, .22f));
            g.Icon(0xE7C2, D2D1_RECT_F{ 17, H / 2 - 17, 51, H / 2 + 17 }, 15, acc);
            D2D1_COLOR_F white{ 1, 1, 1, 1 }, dim{ 1, 1, 1, .62f };
            g.Text(L"Arrange widgets", D2D1_RECT_F{ 62, 10, W - 300, 32 }, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, white);
            g.Text(L"Drag to move · arrow keys nudge (Shift × 10) · Tab selects · hold Alt to place freely",
                   D2D1_RECT_F{ 62, 31, W - 300, 52 }, 12, DWRITE_FONT_WEIGHT_NORMAL, dim);
            // buttons
            float bh = 36, by = (H - bh) / 2;
            float wDone = g.Measure(L"Done", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 40;
            float wAuto = g.Measure(L"Auto-arrange", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 36;
            O.btn[1] = D2D1_RECT_F{ W - 14 - wDone, by, W - 14, by + bh };
            O.btn[0] = D2D1_RECT_F{ O.btn[1].left - 10 - wAuto, by, O.btn[1].left - 10, by + bh };
            g.FillRound(O.btn[0], bh / 2, O.hover == 0 ? D2D1_COLOR_F{ 1, 1, 1, .18f } : D2D1_COLOR_F{ 1, 1, 1, .10f });
            g.Text(L"Auto-arrange", O.btn[0], 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, white, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.FillRound(O.btn[1], bh / 2, O.hover == 1 ? Alpha(acc, .85f) : acc);
            float al = .2126f * acc.r + .7152f * acc.g + .0722f * acc.b;
            g.Text(L"Done", O.btn[1], 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, al > .62f ? D2D1_COLOR_F{ 0, 0, 0, 1 } : white,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (g_gr.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
        }
        GdiFlush();
        RECT wr; GetWindowRect(O.bar, &wr);
        POINT dst{ wr.left, wr.top }, src{ 0, 0 }; SIZE sz{ O.bw, O.bh };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(O.bar, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem); ReleaseDC(nullptr, screen);
}
int BarHit(LPARAM lp) {
    float x = GET_X_LPARAM(lp) / O.bs, y = GET_Y_LPARAM(lp) / O.bs;
    for (int i = 0; i < 2; i++) if (x >= O.btn[i].left && x < O.btn[i].right && y >= O.btn[i].top && y < O.btn[i].bottom) return i;
    return -1;
}
LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        int hv = BarHit(lp); if (hv != O.hover) { O.hover = hv; RenderBar(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (O.hover != -1) { O.hover = -1; RenderBar(); } return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, O.hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_LBUTTONUP: {
        int hit = BarHit(lp);
        if (hit == 0) { ResetPositions(); if (O.wnd) SetForegroundWindow(O.wnd); }
        else if (hit == 1) EndArrange();
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------- keyboard
std::vector<Widget*> Active() { std::vector<Widget*> v; for (auto& w : g_widgets) if (w->enabled && w->hwnd) v.push_back(w.get()); return v; }
void Nudge(int dx, int dy) {
    auto v = Active(); if (v.empty()) return;
    Widget* w = g_arrangeSel ? g_arrangeSel : v[0];
    if (!g_arrangeSel) ArrangeSelect(w);
    int x = w->x + dx, y = w->y + dy;
    ClampWidget(w, x, y, nullptr);
    if (x == w->x && y == w->y) return;
    w->x = x; w->y = y;
    LayoutWidget(w); RenderWidget(w);
    UpdateAnchor(w);
    RequestSave();
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintOverlay(dc, ps.rcPaint); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: ArrangeSelect(nullptr); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_DPICHANGED: return 0;   // we span monitors and work in physical pixels
    case WM_KEYDOWN: {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int step = shift ? 10 : 1;
        if (g_arrangeSel) { float s = ScaleOf(MonitorFromWindow(g_arrangeSel->hwnd, MONITOR_DEFAULTTONEAREST)); step = std::max(1, (int)std::lround(step * s)); }
        switch (wp) {
        case VK_ESCAPE: case VK_RETURN: EndArrange(); return 0;
        case VK_LEFT: Nudge(-step, 0); return 0;
        case VK_RIGHT: Nudge(step, 0); return 0;
        case VK_UP: Nudge(0, -step); return 0;
        case VK_DOWN: Nudge(0, step); return 0;
        case VK_TAB: {
            auto v = Active(); if (v.empty()) return 0;
            size_t i = 0; for (; i < v.size(); i++) if (v[i] == g_arrangeSel) break;
            size_t n = g_arrangeSel ? (shift ? (i + v.size() - 1) % v.size() : (i + 1) % v.size()) : 0;
            ArrangeSelect(v[n]);
            return 0;
        }
        }
        break;
    }
    case WM_CLOSE: EndArrange(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void Register() {
    if (g_registered) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = OverlayProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismArrange"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = BarProc; wc.lpszClassName = L"PrismArrangeBar";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = GuideProc; wc.lpszClassName = L"PrismArrangeGuide";
    RegisterClassExW(&wc);
    g_registered = true;
}

} // namespace

void StartArrange() {
    if (g_arranging) { if (O.wnd) SetForegroundWindow(O.wnd); return; }
    Register();
    g_arranging = true;
    g_arrangeSel = nullptr;
    BuildDimmedWallpaper();
    if (g_guideBrush) DeleteObject(g_guideBrush);
    g_guideBrush = CreateSolidBrush(AccentRef());
    O.vx = GetSystemMetrics(SM_XVIRTUALSCREEN); O.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    O.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN); O.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    O.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"PrismArrange", L"Arrange widgets", WS_POPUP,
                            O.vx, O.vy, O.vw, O.vh, nullptr, nullptr, g_inst, nullptr);
    if (!O.wnd) { g_arranging = false; O = Overlay(); return; }
    ShowWindow(O.wnd, SW_SHOW);
    SetForegroundWindow(O.wnd);
    SetFocus(O.wnd);
    // lift widgets above the overlay, with their arrange frames
    for (auto& w : g_widgets) if (w->enabled && w->hwnd) {
        SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RenderWidget(w.get());
    }
    // toolbar centred at the top of the main screen
    POINT o{ 0, 0 };
    HMONITOR pm = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    RECT wa = WorkOf(pm);
    O.bs = ScaleOf(pm);
    O.bw = (int)(800 * O.bs); O.bh = (int)(62 * O.bs);
    O.bw = std::min(O.bw, (int)(wa.right - wa.left) - (int)(24 * O.bs));
    O.bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"PrismArrangeBar", L"", WS_POPUP,
                            (wa.left + wa.right - O.bw) / 2, wa.top + (int)(18 * O.bs), O.bw, O.bh, nullptr, nullptr, g_inst, nullptr);
    if (O.bar) { RenderBar(); ShowWindow(O.bar, SW_SHOWNOACTIVATE); }
    SettingsRefresh(false);
}

void EndArrange() {
    if (!g_arranging) return;
    g_arranging = false;
    g_arrangeSel = nullptr;
    for (HWND g : O.guideWnds) DestroyWindow(g);
    if (O.bar) DestroyWindow(O.bar);
    if (O.wnd) DestroyWindow(O.wnd);
    O = Overlay();
    ResolveOverlaps(nullptr, true);
    for (auto& w : g_widgets) if (w->enabled && w->hwnd) {
        SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // back onto the desktop
        RenderWidget(w.get());
    }
    RequestSave();
    ScheduleRelease();
    SettingsRefresh(false);
}

void ArrangeSelect(Widget* w) {
    if (w == g_arrangeSel) return;
    Widget* old = g_arrangeSel;
    g_arrangeSel = w;
    if (old) RenderWidget(old);
    if (w) {
        RenderWidget(w);
        SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // its handles above neighbours
        if (O.bar) SetWindowPos(O.bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ArrangeClearGuides() { if (!O.guides.empty()) SetGuides({}); }

// Grid snapping + alignment guides + equal spacing. Positions are physical px.
void ArrangeSnap(Widget* w, int& x, int& y, POINT cursor) {
    HMONITOR hm = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    float s = ScaleOf(hm);
    RECT wa = WorkOf(hm);
    D2D1_SIZE_F b = w->BaseSize();
    int pw = (int)std::ceil(b.width * w->size * s), ph = (int)std::ceil(b.height * w->size * s);
    int th = (int)(10 * s), grid = std::max(1, (int)(8 * s)), G = (int)(16 * s), E = (int)(24 * s), t = std::max(2, (int)std::lround(2 * s));

    enum { K_EDGE, K_CENTER, K_ALIGN, K_GAP_AFTER, K_GAP_BEFORE };
    struct Cand { long v; int kind; long line; RECT o; };   // line = guide coordinate for align/edge/center
    std::vector<Cand> cx, cy;
    cx.push_back({ wa.left + E, K_EDGE, wa.left + E, {} });
    cx.push_back({ wa.right - E - pw, K_EDGE, wa.right - E, {} });
    cx.push_back({ (wa.left + wa.right) / 2 - pw / 2, K_CENTER, (wa.left + wa.right) / 2, {} });
    cy.push_back({ wa.top + E, K_EDGE, wa.top + E, {} });
    cy.push_back({ wa.bottom - E - ph, K_EDGE, wa.bottom - E, {} });
    cy.push_back({ (wa.top + wa.bottom) / 2 - ph / 2, K_CENTER, (wa.top + wa.bottom) / 2, {} });

    std::vector<RECT> others;
    for (auto& o : g_widgets) if (o.get() != w && o->enabled && o->hwnd) others.push_back(PanelRect(o.get()));
    for (auto& r : others) {
        // edge / centre alignment
        cx.push_back({ r.left, K_ALIGN, r.left, r });
        cx.push_back({ r.right - pw, K_ALIGN, r.right, r });
        cx.push_back({ (r.left + r.right) / 2 - pw / 2, K_ALIGN, (r.left + r.right) / 2, r });
        cy.push_back({ r.top, K_ALIGN, r.top, r });
        cy.push_back({ r.bottom - ph, K_ALIGN, r.bottom, r });
        cy.push_back({ (r.top + r.bottom) / 2 - ph / 2, K_ALIGN, (r.top + r.bottom) / 2, r });
        // spacing: standard gap, or the same gap this neighbour already has to its own neighbour
        bool rowOverlap = std::min(r.bottom, (LONG)(y + ph)) > std::max(r.top, (LONG)y);
        bool colOverlap = std::min(r.right, (LONG)(x + pw)) > std::max(r.left, (LONG)x);
        if (rowOverlap) {
            std::vector<int> gaps = { G };
            for (auto& n : others) {
                if (std::min(n.bottom, r.bottom) <= std::max(n.top, r.top)) continue;
                if (n.right <= r.left) gaps.push_back(r.left - n.right);
                if (n.left >= r.right) gaps.push_back(n.left - r.right);
            }
            for (int gp : gaps) if (gp > 0 && gp < 200 * s) {
                cx.push_back({ r.right + gp, K_GAP_AFTER, 0, r });
                cx.push_back({ r.left - gp - pw, K_GAP_BEFORE, 0, r });
            }
        }
        if (colOverlap) {
            std::vector<int> gaps = { G };
            for (auto& n : others) {
                if (std::min(n.right, r.right) <= std::max(n.left, r.left)) continue;
                if (n.bottom <= r.top) gaps.push_back(r.top - n.bottom);
                if (n.top >= r.bottom) gaps.push_back(n.top - r.bottom);
            }
            for (int gp : gaps) if (gp > 0 && gp < 200 * s) {
                cy.push_back({ r.bottom + gp, K_GAP_AFTER, 0, r });
                cy.push_back({ r.top - gp - ph, K_GAP_BEFORE, 0, r });
            }
        }
    }
    // only targets that keep the widget fully on screen (otherwise the guide would lie)
    int mg = (int)(8 * s);
    long minX = wa.left + mg, maxX = wa.right - mg - pw, minY = wa.top + mg, maxY = wa.bottom - mg - ph;
    auto pick = [&](std::vector<Cand>& cs, int v, long lo, long hi, const Cand** best) {
        int bd = th + 1;
        for (auto& c : cs) { if (c.v < lo || c.v > hi) continue; int d = std::abs((int)c.v - v); if (d < bd) { bd = d; *best = &c; } }
    };
    const Cand *bx = nullptr, *by = nullptr;
    pick(cx, x, minX, maxX, &bx); pick(cy, y, minY, maxY, &by);
    // nothing to align with: fall back to the 8-DIP grid
    x = bx ? bx->v : wa.left + E + (int)std::lround((x - wa.left - E) / (double)grid) * grid;
    y = by ? by->v : wa.top + E + (int)std::lround((y - wa.top - E) / (double)grid) * grid;

    std::vector<RECT> gs;
    auto vline = [&](int lx, int y0, int y1) { gs.push_back(RECT{ lx - t / 2, y0, lx - t / 2 + t, y1 }); };
    auto hline = [&](int ly, int x0, int x1) { gs.push_back(RECT{ x0, ly - t / 2, x1, ly - t / 2 + t }); };
    int tick = (int)(10 * s);
    if (bx) {
        if (bx->kind == K_EDGE || bx->kind == K_CENTER) vline(bx->line, wa.top, wa.bottom);
        else if (bx->kind == K_ALIGN) vline(bx->line, std::min((int)bx->o.top, y) - tick, std::max((int)bx->o.bottom, y + ph) + tick);
        else {   // spacing marker between the two facing edges
            int a = bx->kind == K_GAP_AFTER ? bx->o.right : x + pw, c = bx->kind == K_GAP_AFTER ? x : bx->o.left;
            int my = (std::max((int)bx->o.top, y) + std::min((int)bx->o.bottom, y + ph)) / 2;
            hline(my, a, c); vline(a + t / 2, my - tick / 2, my + tick / 2); vline(c - t / 2, my - tick / 2, my + tick / 2);
        }
    }
    if (by) {
        if (by->kind == K_EDGE || by->kind == K_CENTER) hline(by->line, wa.left, wa.right);
        else if (by->kind == K_ALIGN) hline(by->line, std::min((int)by->o.left, x) - tick, std::max((int)by->o.right, x + pw) + tick);
        else {
            int a = by->kind == K_GAP_AFTER ? by->o.bottom : y + ph, c = by->kind == K_GAP_AFTER ? y : by->o.top;
            int mx = (std::max((int)by->o.left, x) + std::min((int)by->o.right, x + pw)) / 2;
            vline(mx, a, c); hline(a + t / 2, mx - tick / 2, mx + tick / 2); hline(c - t / 2, mx - tick / 2, mx + tick / 2);
        }
    }
    SetGuides(std::move(gs));
}

// ======================================================================= resize & scale
// zone = (hx+1)*3 + (hy+1); hx/hy in {-1,0,1}. Corners scale, edges resize the content.
namespace {
struct ResizeState {
    Widget* w = nullptr; int hx = 0, hy = 0; POINT start{};
    int x0 = 0, y0 = 0, pw0 = 0, ph0 = 0; float cw0 = 0, ch0 = 0, size0 = 1, s = 1; RECT wa{};
} R;
int ZoneHx(int z) { return z / 3 - 1; }
int ZoneHy(int z) { return z % 3 - 1; }
}

int ArrangeZone(Widget* w, float lx, float ly) {
    if (!w) return -1;
    D2D1_SIZE_F b = w->BaseSize();
    float band = 8.f / w->size, corner = 15.f / w->size;
    float W = b.width, H = b.height;
    if (lx < -band - 4 || ly < -band - 4 || lx > W + band + 4 || ly > H + band + 4) return -1;
    float dxc = std::min(std::fabs(lx), std::fabs(lx - W)), dyc = std::min(std::fabs(ly), std::fabs(ly - H));
    if (dxc <= corner && dyc <= corner) return ((lx < W / 2 ? -1 : 1) + 1) * 3 + ((ly < H / 2 ? -1 : 1) + 1);
    if (w->ResizableW() && dxc <= band && ly >= 0 && ly <= H) return ((lx < W / 2 ? -1 : 1) + 1) * 3 + 1;
    if (w->ResizableH() && dyc <= band && lx >= 0 && lx <= W) return 1 * 3 + ((ly < H / 2 ? -1 : 1) + 1);
    return -1;
}

LPCWSTR ArrangeZoneCursor(int z) {
    int hx = ZoneHx(z), hy = ZoneHy(z);
    if (hx && hy) return hx == hy ? IDC_SIZENWSE : IDC_SIZENESW;
    return hx ? IDC_SIZEWE : IDC_SIZENS;
}

void ArrangeResizeBegin(Widget* w, int zone, POINT p) {
    R = ResizeState();
    R.w = w; R.hx = ZoneHx(zone); R.hy = ZoneHy(zone); R.start = p;
    RECT pr = PanelRect(w);
    R.x0 = pr.left; R.y0 = pr.top; R.pw0 = pr.right - pr.left; R.ph0 = pr.bottom - pr.top;
    D2D1_SIZE_F b = w->BaseSize(); R.cw0 = b.width; R.ch0 = b.height; R.size0 = w->size;
    HMONITOR hm = MonitorFromRect(&pr, MONITOR_DEFAULTTONEAREST);
    R.s = ScaleOf(hm); R.wa = WorkOf(hm);
}

void ArrangeResizeMove(Widget* w, POINT p, bool alt) {
    if (R.w != w) return;
    const float s = R.s, k = s * R.size0;              // px per content DIP
    const int dx = p.x - R.start.x, dy = p.y - R.start.y;
    const int mg = (int)(8 * s), th = (int)(10 * s), E = (int)(24 * s), t = std::max(2, (int)std::lround(2 * s)), tick = (int)(10 * s);
    const RECT& wa = R.wa;
    // edges we can snap a moving edge to: other widgets and the screen insets
    struct Edge { long v; RECT o; bool screen; };
    std::vector<Edge> ex, ey;
    ex.push_back({ wa.left + E, {}, true }); ex.push_back({ wa.right - E, {}, true });
    ey.push_back({ wa.top + E, {}, true }); ey.push_back({ wa.bottom - E, {}, true });
    for (auto& o : g_widgets) if (o.get() != w && o->enabled && o->hwnd) {
        RECT r = PanelRect(o.get());
        ex.push_back({ r.left, r, false }); ex.push_back({ r.right, r, false });
        ey.push_back({ r.top, r, false }); ey.push_back({ r.bottom, r, false });
    }
    auto nearest = [&](const std::vector<Edge>& es, long v) -> const Edge* {
        const Edge* best = nullptr; long bd = th + 1;
        for (auto& e : es) { long d = std::labs(e.v - v); if (d < bd) { bd = d; best = &e; } }
        return best;
    };
    std::vector<RECT> gs;
    auto vguide = [&](const Edge& e, int y0, int y1) {
        int a = e.screen ? wa.top : std::min((int)e.o.top, y0) - tick, b = e.screen ? wa.bottom : std::max((int)e.o.bottom, y1) + tick;
        gs.push_back(RECT{ (LONG)e.v - t / 2, a, (LONG)e.v - t / 2 + t, b });
    };
    auto hguide = [&](const Edge& e, int x0, int x1) {
        int a = e.screen ? wa.left : std::min((int)e.o.left, x0) - tick, b = e.screen ? wa.right : std::max((int)e.o.right, x1) + tick;
        gs.push_back(RECT{ a, (LONG)e.v - t / 2, b, (LONG)e.v - t / 2 + t });
    };
    // room available from the fixed side to the screen edge
    int availW = R.hx > 0 ? wa.right - mg - R.x0 : (R.x0 + R.pw0) - (wa.left + mg);
    int availH = R.hy > 0 ? wa.bottom - mg - R.y0 : (R.y0 + R.ph0) - (wa.top + mg);
    D2D1_SIZE_F mn = w->MinSize(), mx = w->MaxSize();
    wchar_t badge[64];

    if (R.hx && R.hy) {                                  // ---- corner: proportional scale
        double rx = (R.pw0 + R.hx * dx) / (double)R.pw0, ry = (R.ph0 + R.hy * dy) / (double)R.ph0;
        double ratio = std::abs(dx) * (double)R.ph0 >= std::abs(dy) * (double)R.pw0 ? rx : ry;
        double maxS = std::min(2.0, R.size0 * std::min(availW / (double)R.pw0, availH / (double)R.ph0));
        double minS = std::min(0.6, maxS);
        double ns = Clamp(R.size0 * ratio, minS, maxS);
        if (!alt) {
            bool snapped = false;
            int pw = (int)std::lround(R.pw0 * ns / R.size0), ph = (int)std::lround(R.ph0 * ns / R.size0);
            long exv = R.hx > 0 ? R.x0 + pw : R.x0 + R.pw0 - pw, eyv = R.hy > 0 ? R.y0 + ph : R.y0 + R.ph0 - ph;
            if (const Edge* e = nearest(ex, exv)) {
                double c = R.size0 * (R.hx > 0 ? e->v - R.x0 : R.x0 + R.pw0 - e->v) / (double)R.pw0;
                if (c >= minS && c <= maxS) { ns = c; snapped = true; vguide(*e, R.y0, R.y0 + ph); }
            }
            if (!snapped) if (const Edge* e = nearest(ey, eyv)) {
                double c = R.size0 * (R.hy > 0 ? e->v - R.y0 : R.y0 + R.ph0 - e->v) / (double)R.ph0;
                if (c >= minS && c <= maxS) { ns = c; snapped = true; hguide(*e, R.x0, R.x0 + pw); }
            }
            if (!snapped) ns = Clamp(std::round(ns * 20) / 20, minS, maxS);   // 5 % steps
        }
        w->size = (float)ns;
        int pw = (int)std::ceil(R.cw0 * ns * s), ph = (int)std::ceil(R.ch0 * ns * s);
        w->x = R.hx > 0 ? R.x0 : R.x0 + R.pw0 - pw;
        w->y = R.hy > 0 ? R.y0 : R.y0 + R.ph0 - ph;
        swprintf(badge, 64, L"%d%%", (int)std::lround(ns * 100));
    } else if (R.hx) {                                   // ---- left/right edge: width
        float lo = mn.width, hi = std::min(mx.width, availW / k);
        float ncw = Clamp(R.cw0 + R.hx * dx / k, std::min(lo, hi), hi);
        if (!alt) {
            long e0 = R.hx > 0 ? R.x0 + (long)std::lround(ncw * k) : R.x0 + R.pw0 - (long)std::lround(ncw * k);
            const Edge* e = nearest(ex, e0);
            float c = e ? (R.hx > 0 ? e->v - R.x0 : R.x0 + R.pw0 - e->v) / k : -1;
            if (e && c >= lo && c <= hi) { ncw = c; vguide(*e, R.y0, R.y0 + R.ph0); }
            else ncw = Clamp(std::round(ncw / 8) * 8, std::min(lo, hi), hi);   // 8-DIP grid
        }
        w->cw = ncw; w->ch = R.ch0;
        int pw = (int)std::ceil(ncw * k);
        w->x = R.hx > 0 ? R.x0 : R.x0 + R.pw0 - pw; w->y = R.y0;
        swprintf(badge, 64, L"%d × %d", (int)std::lround(ncw), (int)std::lround(R.ch0));
    } else {                                             // ---- top/bottom edge: height
        float lo = mn.height, hi = std::min(mx.height, availH / k);
        float nch = Clamp(R.ch0 + R.hy * dy / k, std::min(lo, hi), hi);
        if (!alt) {
            long e0 = R.hy > 0 ? R.y0 + (long)std::lround(nch * k) : R.y0 + R.ph0 - (long)std::lround(nch * k);
            const Edge* e = nearest(ey, e0);
            float c = e ? (R.hy > 0 ? e->v - R.y0 : R.y0 + R.ph0 - e->v) / k : -1;
            if (e && c >= lo && c <= hi) { nch = c; hguide(*e, R.x0, R.x0 + R.pw0); }
            else nch = Clamp(std::round(nch / 8) * 8, std::min(lo, hi), hi);
        }
        w->ch = nch; w->cw = R.cw0;
        int ph = (int)std::ceil(nch * k);
        w->y = R.hy > 0 ? R.y0 : R.y0 + R.ph0 - ph; w->x = R.x0;
        swprintf(badge, 64, L"%d × %d", (int)std::lround(R.cw0), (int)std::lround(nch));
    }
    g_arrangeBadge = badge;
    SetGuides(std::move(gs));
    LayoutWidget(w);
    RenderWidget(w);
}

void ArrangeResizeEnd(Widget* w) {
    if (R.w != w) return;
    R = ResizeState();
    g_arrangeBadge.clear();
    ArrangeClearGuides();
    D2D1_SIZE_F d = w->DefaultSize(), b = w->BaseSize();   // store "default" as 0 so future default changes apply
    if (std::fabs(b.width - d.width) < 0.5f) w->cw = 0;
    if (std::fabs(b.height - d.height) < 0.5f) w->ch = 0;
    UpdateAnchor(w);
    ResolveOverlaps(w, true);
    LayoutWidget(w); RenderWidget(w);
    RequestSave();
    SettingsRefresh(true);
}

void ArrangeResetZone(Widget* w, int zone) {
    int hx = ZoneHx(zone), hy = ZoneHy(zone);
    if (hx && hy) w->size = 1.0f;          // corner: back to 100 %
    else if (hx) w->cw = 0;                // edge: back to the default width/height
    else w->ch = 0;
    ApplyWidget(w);                        // keeps the anchored corner, moves neighbours if needed
    RenderWidget(w);
    SettingsRefresh(true);
}
