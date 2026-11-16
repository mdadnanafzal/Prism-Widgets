// Prism Widgets - app shell: widget windows, tray menu, config, scheduling.
#include "app.h"
#include "media.h"
#include "weather.h"
#include "events.h"
#include "net.h"
#include "pet/pet.h"
#include <psapi.h>
#include <algorithm>
#include <shellscalingapi.h>
#include <wtsapi32.h>

#define WM_TRAY        (WM_APP + 1)
#define WM_SHOWSETTINGS (WM_APP + 2)
#define WM_STARTARRANGE (WM_APP + 3)
enum { T_TICK = 1, T_RELEASE, T_SAVE, T_WALLCHECK, T_RELOAD_CUSTOM, T_WALLRELOAD, T_GLASS, T_RELAX, T_DEEPIDLE, T_MEDIA, T_FRAME };

HINSTANCE g_inst;
HWND g_main;
static UINT g_msgTaskbarCreated;
std::vector<std::unique_ptr<Widget>> g_widgets;
static JVal g_cfg;
std::wstring g_theme = L"liquid", g_accent = L"system", g_textMode = L"auto", g_uiTheme = L"system";
static std::wstring g_wallSig;
bool g_quickDrag = false, g_snap = true, g_pauseFullscreen = true;
static bool g_firstRun = false, g_sessionLocked = false, g_displayOff = false;
static HANDLE g_dirWatch = INVALID_HANDLE_VALUE;

const AccentPreset g_accents[] = {
    { L"Blue", L"#0A84FF" }, { L"Purple", L"#BF5AF2" }, { L"Pink", L"#FF375F" }, { L"Red", L"#FF453A" },
    { L"Orange", L"#FF9F0A" }, { L"Yellow", L"#FFD60A" }, { L"Green", L"#30D158" }, { L"Teal", L"#40C8E0" },
};
const int g_accentCount = sizeof(g_accents) / sizeof(g_accents[0]);
const float g_sizes[] = { 0.85f, 1.0f, 1.2f, 1.45f };
const wchar_t* g_sizeNames[] = { L"Small", L"Medium", L"Large", L"Extra large" };

static void Render(Widget* w, bool contentSame = false);
static void ScheduleTick();
static void ShowTrayMenu();

// ------------------------------------------------------------------ helpers
static RECT PrimaryWork() { RECT r; SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0); return r; }
static HWND DesktopOwner() { return FindWindowW(L"Progman", nullptr); }
const Theme& ThemeFor(Widget* w) { return FindTheme(w->theme.empty() ? g_theme : w->theme); }
D2D1_COLOR_F Accent() {
    if (g_accent == L"system") return WindowsAccent();
    bool ok; D2D1_COLOR_F c = ParseColor(g_accent, &ok); return ok ? c : WindowsAccent();
}
Ink MakeInk(const Theme& t, float lum) {
    bool dark = g_textMode == L"dark" || (g_textMode == L"auto" && lum > (t.panel ? 0.58f : 0.70f));
    D2D1_COLOR_F b = dark ? D2D1_COLOR_F{ .06f, .06f, .08f, 1 } : D2D1_COLOR_F{ 1, 1, 1, 1 };
    Ink k;
    k.fg = b; k.fg2 = Alpha(b, dark ? .66f : .74f); k.fg3 = Alpha(b, dark ? .40f : .46f); k.hover = Alpha(b, dark ? .07f : .11f);
    k.accent = Accent();
    float al = .2126f * k.accent.r + .7152f * k.accent.g + .0722f * k.accent.b;
    k.onAccent = al > .62f ? D2D1_COLOR_F{ .05f, .05f, .05f, 1 } : D2D1_COLOR_F{ 1, 1, 1, 1 };
    k.shadow = !t.panel && !dark;
    // accent used as text: pull it toward the text colour when it would blend into the glass
    float d = std::fabs(al - lum);
    float mix = d < 0.40f ? (0.40f - d) / 0.40f * 0.85f : 0.f;
    k.accentText = D2D1_COLOR_F{ k.accent.r + (b.r - k.accent.r) * mix, k.accent.g + (b.g - k.accent.g) * mix, k.accent.b + (b.b - k.accent.b) * mix, 1 };
    return k;
}
static bool Paused() {
    if (g_arranging) return false;   // our own full-screen overlay must not count as a "fullscreen app"
    if (g_sessionLocked || g_displayOff) return true;
    QUERY_USER_NOTIFICATION_STATE q;
    if (g_pauseFullscreen && SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE || q == QUNS_BUSY))
        return true;
    return false;
}
static int PanelW(Widget* w) { return w->bw - 2 * w->margin; }
static int PanelH(Widget* w) { return w->bh - 2 * w->margin; }
static float LocalX(Widget* w, int cx) { return (cx - w->margin) / (w->scale * w->size); }
static float LocalY(Widget* w, int cy) { return (cy - w->margin) / (w->scale * w->size); }

static void Layout(Widget* w) {
    POINT c{ w->x + 8, w->y + 8 };
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    w->scale = dx / 96.f;
    D2D1_SIZE_F b = w->BaseSize();
    w->margin = (int)std::ceil(32 * w->scale * std::max(1.f, w->size));
    w->bw = (int)std::ceil(b.width * w->size * w->scale) + 2 * w->margin;
    w->bh = (int)std::ceil(b.height * w->size * w->scale) + 2 * w->margin;
    w->bgValid = false;
}

static void ArmRelease() { SetTimer(g_main, T_RELEASE, 30000, nullptr); }   // keep the wallpaper proxy a while for fast re-renders

// ------------------------------------------------------------------ performance + power mode
PerfStats g_perf;
static LONGLONG Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static double QpcMs(LONGLONG d) { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return d * 1000.0 / f.QuadPart; }
static LONGLONG g_dragT0 = 0; static unsigned g_dragSteps = 0;
void PerfDragBegin() { g_dragT0 = Qpc(); g_dragSteps = 0; }
void PerfDragStep() { g_dragSteps++; }
void PerfDragEnd() {
    double sec = QpcMs(Qpc() - g_dragT0) / 1000.0;
    if (sec > 0.25 && g_dragSteps > 2) g_perf.dragRate = g_dragSteps / sec;
}
void PerfClick(DWORD msgTime, LONGLONG qpcStart) {
    // time spent waiting in the queue (ms ticks) + time we spent handling it (precise)
    // GetTickCount has ~16 ms granularity, so queue wait only counts when it is clearly larger than that
    double queued = std::min(1000.0, (double)(DWORD)(GetTickCount() - msgTime));
    double ms = QpcMs(Qpc() - qpcStart) + (queued > 16 ? queued : 0);
    g_perf.clickLast = ms;
    g_perf.clickAvg = g_perf.clickAvg < 0 ? ms : g_perf.clickAvg * 0.8 + ms * 0.2;
}
typedef struct { ULONG Version, ControlMask, StateMask; } PW_THROTTLE;
static void SetPowerMode(bool responsive) {
    static bool init = false;
    if (init && responsive == g_perf.responsive) return;
    init = true;
    g_perf.responsive = responsive;
    // Interacting: normal priority, no throttling. Idle: below-normal + EcoQoS ("efficiency mode").
    SetPriorityClass(GetCurrentProcess(), responsive ? NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    PW_THROTTLE t{ 1, 1 /*EXECUTION_SPEED*/, responsive ? 0u : 1u };
    SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4 /*ProcessPowerThrottling*/, &t, sizeof(t));
}
void Interactive() {
    SetPowerMode(true);
    SetTimer(g_main, T_RELAX, 3000, nullptr);        // back to efficiency mode 3 s after the last interaction
    SetTimer(g_main, T_DEEPIDLE, 10 * 60 * 1000, nullptr);   // deep memory trim only after 10 idle minutes
}
static bool OurWindowInFront() {
    HWND fg = GetForegroundWindow(); wchar_t cls[32] = {};
    if (fg) GetClassNameW(fg, cls, 32);
    return !wcscmp(cls, L"PrismSettings") || !wcscmp(cls, L"PrismInput") || !wcscmp(cls, L"PrismArrange");
}

// ------------------------------------------------------------------ rendering
// Draws widget content over its glass layer into the DIB selected into `mem`.
static void DrawWidget(HDC mem, Widget* w, const Theme& t, const uint32_t* bg, int bw, int bh, int margin, float s, float lum, bool outline = false,
                       const uint32_t* content = nullptr) {
    RECT rc{ 0, 0, bw, bh };
    if (FAILED(g_gr.rt->BindDC(mem, &rc))) return;
    float dpi = 96.f * s;
    g_gr.rt->SetDpi(dpi, dpi);
    g_gr.rt->BeginDraw();
    D2D1_MATRIX_3X2_F tr; tr._11 = 1; tr._12 = 0; tr._21 = 0; tr._22 = 1; tr._31 = 0; tr._32 = 0;
    g_gr.rt->SetTransform(&tr);
    D2D1_COLOR_F clear{ 0, 0, 0, 0 };
    g_gr.rt->Clear(&clear);
    if (outline) {   // make the whole window (incl. shadow margin) clickable so edge handles work
        D2D1_RECT_F all{ 0, 0, bw * 96.f / dpi, bh * 96.f / dpi };
        g_gr.br->SetColor(D2D1_COLOR_F{ 0, 0, 0, 0.012f });
        g_gr.rt->FillRectangle(&all, g_gr.br);
    }
    // Blit the glass layer through D2D (DC targets don't reliably keep prior DIB contents)
    D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, dpi, dpi };
    ID2D1Bitmap* bgb = nullptr;
    if (SUCCEEDED(g_gr.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)bw, (UINT32)bh }, bg, bw * 4, &bp, &bgb))) {
        g_gr.rt->DrawBitmap(bgb, nullptr, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        bgb->Release();
    }
    if (content) {   // A4: pre-rendered content layer
        ID2D1Bitmap* cb = nullptr;
        if (SUCCEEDED(g_gr.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)bw, (UINT32)bh }, content, bw * 4, &bp, &cb))) {
            g_gr.rt->DrawBitmap(cb, nullptr, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            cb->Release();
        }
    }
    tr._31 = tr._32 = margin / s;
    g_gr.rt->SetTransform(&tr);
    Gfx g{ g_gr.rt, g_gr.br, MakeInk(t, lum) };
    D2D1_SIZE_F bs = w->BaseSize();
    if (!content) w->Draw(g, bs.width, bs.height);
    if (outline) {   // Arrange mode frame: accent for the selected widget, soft white for the rest
        float r = std::min(t.radius + 1.5f, std::min(bs.width, bs.height) / 2);
        D2D1_RECT_F fr{ -1.5f, -1.5f, bs.width + 1.5f, bs.height + 1.5f };
        D2D1_COLOR_F acc = Accent(), white{ 1, 1, 1, 1 };
        if (w == g_arrangeSel) {
            g.StrokeRound(fr, r, acc, 3.f / w->size);
            float hs = 1.f / w->size;   // handles keep a constant on-screen size
            float W = bs.width, H = bs.height;
            if (w->ResizableW()) for (float hx : { -1.5f, W + 1.5f }) {           // width handles
                D2D1_RECT_F pr{ hx - 3.5f * hs, H / 2 - 15 * hs, hx + 3.5f * hs, H / 2 + 15 * hs };
                g.FillRound(pr, 3.5f * hs, white); g.StrokeRound(pr, 3.5f * hs, acc, 1.6f * hs);
            }
            if (w->ResizableH()) for (float hy : { -1.5f, H + 1.5f }) {           // height handles
                D2D1_RECT_F pr{ W / 2 - 15 * hs, hy - 3.5f * hs, W / 2 + 15 * hs, hy + 3.5f * hs };
                g.FillRound(pr, 3.5f * hs, white); g.StrokeRound(pr, 3.5f * hs, acc, 1.6f * hs);
            }
            for (float cx : { -1.5f, W + 1.5f }) for (float cy : { -1.5f, H + 1.5f }) {   // scale handles
                g.FillCircle(cx, cy, 6.5f * hs, white); g.StrokeCircle(cx, cy, 6.5f * hs, acc, 2.2f * hs);
            }
            if (!g_arrangeBadge.empty()) {
                float tw = g.Measure(g_arrangeBadge, 12 * hs, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 20 * hs;
                D2D1_RECT_F br{ W / 2 - tw / 2, H + 9 * hs, W / 2 + tw / 2, H + 29 * hs };
                g.FillRound(br, 10 * hs, D2D1_COLOR_F{ .08f, .08f, .10f, .88f });
                bool sh = g.ink.shadow; g.ink.shadow = false;
                g.Text(g_arrangeBadge, br, 12 * hs, DWRITE_FONT_WEIGHT_SEMI_BOLD, white, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g.ink.shadow = sh;
            }
        }
        else g.StrokeRound(fr, r, D2D1_COLOR_F{ 1, 1, 1, .65f }, 1.5f / w->size);
    }
    HRESULT hr = g_gr.rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
    GdiFlush();
}

static HBITMAP MakeDib(HDC mem, int w, int h, void** bits) {
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(mem, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// A4: draw only the widget's content (transparent background) into w->content
static void RenderContent(Widget* w, const Theme& t, HDC mem, void* bits) {
    RECT rc{ 0, 0, w->bw, w->bh };
    if (FAILED(g_gr.rt->BindDC(mem, &rc))) return;
    float s = w->scale * w->size, dpi = 96.f * s;
    g_gr.rt->SetDpi(dpi, dpi);
    g_gr.rt->BeginDraw();
    D2D1_MATRIX_3X2_F tr; tr._11 = 1; tr._12 = 0; tr._21 = 0; tr._22 = 1; tr._31 = w->margin / s; tr._32 = w->margin / s;
    g_gr.rt->SetTransform(&tr);
    D2D1_COLOR_F clear{ 0, 0, 0, 0 }; g_gr.rt->Clear(&clear);
    Gfx g{ g_gr.rt, g_gr.br, MakeInk(t, w->lum) };
    D2D1_SIZE_F bs = w->BaseSize();
    w->Draw(g, bs.width, bs.height);
    if (g_gr.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
    GdiFlush();
    w->content.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w->bw * w->bh);
}
static uint32_t ContentKey(Widget* w, const Theme& t) {
    Ink k = MakeInk(t, w->lum);
    uint32_t h = (uint32_t)(k.fg.r * 255) | ((uint32_t)(k.accent.g * 255) << 8) | ((uint32_t)k.shadow << 16);
    return h ^ ((uint32_t)(w->scale * w->size * 1000) << 17) ^ (uint32_t)(w->bw * 31 + w->bh);
}
static void Render(Widget* w, bool contentSame) {
    if (!w->hwnd || !w->enabled) return;
    if (Paused()) { w->dirty = true; return; }
    if (!g_gr.EnsureTarget()) return;
    LONGLONG t0 = Qpc();
    const Theme& t = ThemeFor(w);
    size_t n = (size_t)w->bw * w->bh;
    if (!w->bgValid || w->bg.size() != n) {
        g_wall.Ensure();
        w->bg.assign(n, 0);
        RenderBackground(t, w->x - w->margin, w->y - w->margin, w->bw, w->bh, w->margin, w->scale * w->size, Accent(), w->bg.data(), &w->lum);
        w->bgValid = true;
        ArmRelease();
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = MakeDib(mem, w->bw, w->bh, &bits);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        const uint32_t* content = nullptr;
        if (g_arranging) {   // A4: keep the content layer while arranging; glass-only refreshes reuse it
            uint32_t key = ContentKey(w, t);
            if (!(contentSame && w->contentValid && w->contentKey == key && w->content.size() == n)) {
                RenderContent(w, t, mem, bits);
                w->contentValid = !w->content.empty(); w->contentKey = key;
            }
            if (w->contentValid) content = w->content.data();
        } else if (w->contentValid) { w->content.clear(); w->content.shrink_to_fit(); w->contentValid = false; }
        DrawWidget(mem, w, t, w->bg.data(), w->bw, w->bh, w->margin, w->scale * w->size, w->lum, g_arranging, content);
        POINT dst{ w->x - w->margin, w->y - w->margin }, src{ 0, 0 };
        SIZE sz{ w->bw, w->bh };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(w->hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);   // DWM keeps its own copy; we don't hold the frame in RAM
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    w->dirty = false;
    {   // C1: redraw timing
        double ms = QpcMs(Qpc() - t0);
        ULONGLONG nowT = GetTickCount64();
        g_perf.renderLast = ms;
        g_perf.renderAvg = g_perf.renders ? g_perf.renderAvg * 0.9 + ms * 0.1 : ms;
        if (ms > g_perf.renderMax || nowT - g_perf.maxAt > 60000) { g_perf.renderMax = ms; g_perf.maxAt = nowT; }
        g_perf.renders++;
    }

    // next due time
    ULONGLONG now = GetTickCount64();
    SYSTEMTIME st; GetLocalTime(&st);
    int iv = w->Interval();
    if (iv == 1000) w->due = now + (1000 - st.wMilliseconds) + 5;
    else if (iv == 60000) w->due = now + (60 - st.wSecond) * 1000 - st.wMilliseconds + 20;
    else w->due = now + iv;
}

bool RenderPreview(Widget* w, const Theme& t, float scale, std::vector<uint32_t>& px, int& W, int& H) {
    if (!g_gr.EnsureTarget()) return false;
    D2D1_SIZE_F b = w->BaseSize();
    int margin = (int)std::ceil(26 * scale);
    W = (int)std::ceil(b.width * scale) + 2 * margin; H = (int)std::ceil(b.height * scale) + 2 * margin;
    // sample the wallpaper where the widget lives (or the middle of the main screen)
    int sx, sy;
    if (w->placed) { sx = w->x - margin; sy = w->y - margin; }
    else { RECT wa = PrimaryWork(); sx = (wa.left + wa.right - W) / 2; sy = (wa.top + wa.bottom - H) / 2; }
    std::vector<uint32_t> bg((size_t)W * H, 0);
    float lum = 0.3f;
    g_wall.Ensure();
    RenderBackground(t, sx, sy, W, H, margin, scale, Accent(), bg.data(), &lum);
    ArmRelease();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = MakeDib(mem, W, H, &bits);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        DrawWidget(mem, w, t, bg.data(), W, H, margin, scale, lum);
        px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)W * H);
        SelectObject(mem, old);
        ok = true;
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

int GlassMargin(float s) { return (int)std::ceil(30 * s); }
void RenderGlassPopup(HWND hwnd, GlassCache& c, const Theme& t, int px, int py, int pw, int ph, float s,
                      const std::function<void(Gfx&, float, float)>& draw) {
    if (!hwnd || !g_gr.EnsureTarget()) return;
    int m = GlassMargin(s), W = pw + 2 * m, H = ph + 2 * m, sx = px - m, sy = py - m;
    if (c.sx != sx || c.sy != sy || c.W != W || c.H != H || c.theme != &t || c.bg.size() != (size_t)W * H) {
        c.bg.assign((size_t)W * H, 0);
        g_wall.Ensure();
        RenderBackground(t, sx, sy, W, H, m, s, Accent(), c.bg.data(), &c.lum);
        ArmRelease();
        c.sx = sx; c.sy = sy; c.W = W; c.H = H; c.theme = &t;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = MakeDib(mem, W, H, &bits);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        RECT rc{ 0, 0, W, H };
        if (SUCCEEDED(g_gr.rt->BindDC(mem, &rc))) {
            float dpi = 96.f * s;
            g_gr.rt->SetDpi(dpi, dpi);
            g_gr.rt->BeginDraw();
            D2D1_MATRIX_3X2_F tr; tr._11 = 1; tr._12 = 0; tr._21 = 0; tr._22 = 1; tr._31 = 0; tr._32 = 0;
            g_gr.rt->SetTransform(&tr);
            D2D1_COLOR_F clear{ 0, 0, 0, 0 }; g_gr.rt->Clear(&clear);
            D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, dpi, dpi };
            ID2D1Bitmap* bgb = nullptr;
            if (SUCCEEDED(g_gr.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)W, (UINT32)H }, c.bg.data(), W * 4, &bp, &bgb))) {
                g_gr.rt->DrawBitmap(bgb, nullptr, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
                bgb->Release();
            }
            tr._31 = tr._32 = m / s;
            g_gr.rt->SetTransform(&tr);
            Gfx g{ g_gr.rt, g_gr.br, MakeInk(t, c.lum) };
            draw(g, pw / s, ph / s);
            if (g_gr.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
            GdiFlush();
        }
        POINT dst{ sx, sy }, src{ 0, 0 }; SIZE sz{ W, H };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void RequestRedraw(Widget* w) { Render(w); }
void Widget_RenderAll(bool invalidateBg) {
    for (auto& w : g_widgets) { if (invalidateBg) w->bgValid = false; Render(w.get()); }
    ScheduleTick();
}

static void ScheduleTick() {
    ULONGLONG now = GetTickCount64(), best = now + 3600000;
    bool any = false;
    for (auto& w : g_widgets) if (w->enabled && w->hwnd) { any = true; if (w->due < best) best = w->due; }
    if (!any) { KillTimer(g_main, T_TICK); return; }
    UINT delay = best > now ? (UINT)(best - now) : 10;
    if (Paused()) delay = std::min<UINT>(delay, 3000);   // poll for fullscreen / lock exit
    SetTimer(g_main, T_TICK, std::max<UINT>(delay, 10), nullptr);
}
static void OnTick() {
    if (!Paused()) {
        ULONGLONG now = GetTickCount64();
        for (auto& w : g_widgets) if (w->enabled && w->hwnd && (w->dirty || w->due <= now + 25)) Render(w.get());
    }
    ScheduleTick();
}

// ------------------------------------------------------------------ config
static std::wstring ConfigPath() { return AppDataDir() + L"\\config.json"; }
bool IsStartup() {
    wchar_t b[1024]; DWORD sz = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", APP_ID, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS;
}
void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --startup";
        RegSetValueExW(k, APP_ID, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_ID);
    RegCloseKey(k);
}

static const JVal* EntryFor(const std::wstring& key) {
    const JVal* arr = g_cfg.get(L"widgets");
    if (!arr || !arr->isArr()) return nullptr;
    for (auto& e : arr->a) if (e.str(L"key", L"") == key) return &e;
    return nullptr;
}
static void ApplyEntry(Widget* w) {
    const JVal* e = EntryFor(w->key);
    if (!e) {
        // defaults: core widgets on, system off; customs off on first run (samples), on when user adds them later
        if (w->key == L"system") w->enabled = false;
        else if (w->IsCustom()) w->enabled = !g_firstRun;
        return;
    }
    w->enabled = e->boolean(L"enabled", true);
    w->placed = e->get(L"x") != nullptr;
    w->x = (int)e->num(L"x", 0); w->y = (int)e->num(L"y", 0);
    w->size = (float)Clamp(e->num(L"size", 1.0), 0.5, 3.0);
    if (e->get(L"theme")) w->theme = e->str(L"theme", L"");
    w->Load(*e);
    w->cw = (float)e->num(L"w", 0); w->ch = (float)e->num(L"h", 0);
    std::wstring a = e->str(L"anchor", L"");
    if (a.size() == 2) {
        w->hasAnchor = true;
        w->anchor = (a[1] == L'r' ? 1 : 0) | (a[0] == L'b' ? 2 : 0);
        w->adx = (float)e->num(L"dx", 0); w->ady = (float)e->num(L"dy", 0);
        w->mon = e->str(L"monitor", L"");
    }
}
static void SaveConfigNow() {
    JVal c = JVal::MakeObj();
    c.set(L"theme") = JVal::MakeStr(g_theme);
    c.set(L"accent") = JVal::MakeStr(g_accent);
    c.set(L"text") = JVal::MakeStr(g_textMode);
    c.set(L"quickDrag") = JVal::MakeBool(g_quickDrag);
    c.set(L"snap") = JVal::MakeBool(g_snap);
    c.set(L"pauseFullscreen") = JVal::MakeBool(g_pauseFullscreen);
    c.set(L"settingsTheme") = JVal::MakeStr(g_uiTheme);
    g_weather.SaveSettings(c.set(L"weather"));
    g_events.SaveSettings(c.set(L"events"));
    PetSaveSettings(c.set(L"pet"));
    JVal& arr = c.set(L"widgets"); arr = JVal::MakeArr();
    for (auto& w : g_widgets) {
        JVal e = JVal::MakeObj();
        e.set(L"key") = JVal::MakeStr(w->key);
        e.set(L"enabled") = JVal::MakeBool(w->enabled);
        if (w->placed) { e.set(L"x") = JVal::MakeNum(w->x); e.set(L"y") = JVal::MakeNum(w->y); }
        if (w->hasAnchor) {
            const wchar_t* names[4] = { L"tl", L"tr", L"bl", L"br" };
            e.set(L"anchor") = JVal::MakeStr(names[w->anchor & 3]);
            e.set(L"dx") = JVal::MakeNum(std::round(w->adx * 10) / 10);
            e.set(L"dy") = JVal::MakeNum(std::round(w->ady * 10) / 10);
            e.set(L"monitor") = JVal::MakeStr(w->mon);
        }
        e.set(L"size") = JVal::MakeNum(std::round(w->size * 100) / 100);
        if (w->cw > 0 || w->ch > 0) { D2D1_SIZE_F bs = w->BaseSize(); e.set(L"w") = JVal::MakeNum(std::round(bs.width)); e.set(L"h") = JVal::MakeNum(std::round(bs.height)); }
        if (w->theme != w->DefaultTheme()) e.set(L"theme") = JVal::MakeStr(w->theme);
        w->Save(e);
        arr.a.push_back(e);
    }
    g_cfg = c;
    WriteTextFile(ConfigPath(), JsonWrite(c));
}
void RequestSave() { SetTimer(g_main, T_SAVE, 800, nullptr); }

static void LoadConfig() {
    bool ok = false;
    std::wstring t = ReadTextFile(ConfigPath(), &ok);
    g_firstRun = !ok;
    if (!ok || !JsonParse(t, g_cfg) || !g_cfg.isObj()) g_cfg = JVal::MakeObj();
    g_theme = g_cfg.str(L"theme", L"liquid");
    g_accent = g_cfg.str(L"accent", L"system");
    g_textMode = g_cfg.str(L"text", L"auto");
    g_quickDrag = g_cfg.boolean(L"quickDrag", false);
    g_snap = g_cfg.boolean(L"snap", true);
    g_pauseFullscreen = g_cfg.boolean(L"pauseFullscreen", true);
    g_uiTheme = g_cfg.str(L"settingsTheme", L"system");
    g_weather.LoadSettings(g_cfg.get(L"weather"));
    g_events.LoadSettings(g_cfg.get(L"events"));
    PetLoadSettings(g_cfg.get(L"pet"));
}

// ------------------------------------------------------------------ placement
static void AutoPlace() {
    RECT wa = PrimaryWork();
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(MonitorFromRect(&wa, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
    float s = dx / 96.f;
    int gap = (int)(18 * s), edge = (int)(36 * s);
    int colRight = wa.right - edge, y = wa.top + edge, colW = 0;
    // placed widgets stay where they are; unplaced ones stack in columns from the right, avoiding overlaps
    for (auto& w : g_widgets) {
        if (!w->enabled || w->placed) continue;
        D2D1_SIZE_F b = w->BaseSize();
        int pw = (int)(b.width * w->size * s), ph = (int)(b.height * w->size * s);
        for (int guard = 0; guard < 200; guard++) {
            if (y + ph > wa.bottom - edge && y > wa.top + edge) { colRight -= std::max(colW, pw) + gap; y = wa.top + edge; colW = 0; }
            if (colRight - pw < wa.left) break;
            RECT c{ colRight - pw - gap, y - gap, colRight + gap, y + ph + gap }, x;
            Widget* hit = nullptr;
            for (auto& o : g_widgets) {
                if (o.get() == w.get() || !o->enabled || !o->placed) continue;
                D2D1_SIZE_F ob = o->BaseSize();
                RECT orc{ o->x, o->y, o->x + (int)(ob.width * o->size * s), o->y + (int)(ob.height * o->size * s) };
                if (IntersectRect(&x, &c, &orc)) { hit = o.get(); break; }
            }
            if (!hit) break;
            y = hit->y + (int)(hit->BaseSize().height * hit->size * s) + gap;
        }
        w->x = colRight - pw; w->y = y; w->placed = true;
        UpdateAnchor(w.get());
        y += ph + gap; colW = std::max(colW, pw);
    }
}
// ------------------------------------------------------------------ placement engine
float ScaleOf(HMONITOR m) { UINT dx = 96, dy = 96; GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy); return dx / 96.f; }
RECT WorkOf(HMONITOR m) { MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(m, &mi); return mi.rcWork; }
static HMONITOR MonitorAt(int x, int y) { POINT p{ x, y }; return MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST); }
static SIZE PanelSizeAt(Widget* w, float s) {
    D2D1_SIZE_F b = w->BaseSize();
    return SIZE{ (LONG)std::ceil(b.width * w->size * s), (LONG)std::ceil(b.height * w->size * s) };
}
RECT PanelRect(Widget* w) {
    SIZE z = PanelSizeAt(w, ScaleOf(MonitorAt(w->x + 8, w->y + 8)));
    return RECT{ w->x, w->y, w->x + z.cx, w->y + z.cy };
}
void ClampWidget(Widget* w, int& x, int& y, const POINT* pref) {
    HMONITOR m;
    if (pref) m = MonitorFromPoint(*pref, MONITOR_DEFAULTTONEAREST);
    else {
        SIZE z0 = PanelSizeAt(w, ScaleOf(MonitorAt(x + 8, y + 8)));
        RECT r{ x, y, x + z0.cx, y + z0.cy };
        m = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    }
    float s = ScaleOf(m);
    SIZE z = PanelSizeAt(w, s);
    RECT wa = WorkOf(m);
    int mg = (int)(8 * s);
    x = std::max((int)wa.left + mg, std::min(x, (int)wa.right - mg - (int)z.cx));
    y = std::max((int)wa.top + mg, std::min(y, (int)wa.bottom - mg - (int)z.cy));
}
void UpdateAnchor(Widget* w) {
    RECT pr = PanelRect(w);
    HMONITOR hm = MonitorFromRect(&pr, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return;
    RECT wa = mi.rcWork;
    float s = ScaleOf(hm);
    bool right = (pr.left + pr.right) / 2 > (wa.left + wa.right) / 2, bottom = (pr.top + pr.bottom) / 2 > (wa.top + wa.bottom) / 2;
    w->anchor = (right ? 1 : 0) | (bottom ? 2 : 0);
    w->adx = (right ? wa.right - pr.right : pr.left - wa.left) / s;
    w->ady = (bottom ? wa.bottom - pr.bottom : pr.top - wa.top) / s;
    w->mon = mi.szDevice;
    w->hasAnchor = true;
}
namespace { struct MonFind { std::wstring dev; HMONITOR found = nullptr; }; }
static BOOL CALLBACK FindMonProc(HMONITOR m, HDC, LPRECT, LPARAM p) {
    auto* f = (MonFind*)p;
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi) && f->dev == mi.szDevice) { f->found = m; return FALSE; }
    return TRUE;
}
static bool PositionFromAnchor(Widget* w) {
    if (!w->hasAnchor) return false;
    MonFind f; f.dev = w->mon;
    if (!w->mon.empty()) EnumDisplayMonitors(nullptr, nullptr, FindMonProc, (LPARAM)&f);
    HMONITOR hm = f.found;
    if (!hm) { POINT o{ 0, 0 }; hm = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY); }   // monitor gone: use the main one
    float s = ScaleOf(hm);
    RECT wa = WorkOf(hm);
    SIZE z = PanelSizeAt(w, s);
    w->x = (w->anchor & 1) ? wa.right - (int)std::lround(w->adx * s) - z.cx : wa.left + (int)std::lround(w->adx * s);
    w->y = (w->anchor & 2) ? wa.bottom - (int)std::lround(w->ady * s) - z.cy : wa.top + (int)std::lround(w->ady * s);
    w->placed = true;
    return true;
}
static void ClampInPlace(Widget* w) { int x = w->x, y = w->y; ClampWidget(w, x, y, nullptr); w->x = x; w->y = y; }

// Overlap resolution. Widgets are settled one by one (the `fixed` one first, it never moves);
// a widget that collides with an already-settled one moves to the nearest spot that is completely free.
static RECT RectAt(Widget* w, int x, int y) { SIZE z = PanelSizeAt(w, ScaleOf(MonitorAt(x + 8, y + 8))); return RECT{ x, y, x + z.cx, y + z.cy }; }
static bool Collides(const RECT& r, const std::vector<RECT>& settled, int g) {
    RECT x;
    for (auto& o : settled) { RECT ig{ o.left - g, o.top - g, o.right + g, o.bottom + g }; if (IntersectRect(&x, &r, &ig)) return true; }
    return false;
}
static bool FindFreeSpot(Widget* w, const std::vector<RECT>& settled, int& outX, int& outY) {
    RECT cur = RectAt(w, w->x, w->y);
    HMONITOR hm = MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST);
    float s = ScaleOf(hm);
    RECT wa = WorkOf(hm);
    int g = (int)(12 * s), pw = cur.right - cur.left, ph = cur.bottom - cur.top;
    std::vector<POINT> cand;
    for (auto& o : settled) {   // snug positions next to every settled widget
        int xs[4] = { (int)o.right + g, (int)o.left - g - pw, (int)o.left, (int)o.right - pw };
        int ys[4] = { (int)o.bottom + g, (int)o.top - g - ph, (int)o.top, (int)o.bottom - ph };
        for (int xi : xs) cand.push_back(POINT{ xi, w->y });
        for (int yi : ys) cand.push_back(POINT{ w->x, yi });
        for (int xi : xs) for (int yi : ys) cand.push_back(POINT{ xi, yi });
    }
    int step = std::max(4, (int)(16 * s));   // plus a coarse scan of the whole screen
    for (int yy = wa.top; yy < wa.bottom; yy += step) for (int xx = wa.left; xx < wa.right; xx += step) cand.push_back(POINT{ xx, yy });
    // clamp + collide with plain arithmetic (no per-candidate API calls)
    int mg = (int)(8 * s);
    int minX = wa.left + mg, maxX = std::max(minX, (int)wa.right - mg - pw), minY = wa.top + mg, maxY = std::max(minY, (int)wa.bottom - mg - ph);
    long long best = -1;
    for (auto c : cand) {
        int x = Clamp((int)c.x, minX, maxX), y = Clamp((int)c.y, minY, maxY);
        if (Collides(RECT{ x, y, x + pw, y + ph }, settled, g)) continue;
        long long dx = x - w->x, dy = y - w->y, d = dx * dx + dy * dy;
        if (best < 0 || d < best) { best = d; outX = x; outY = y; }
    }
    return best >= 0;
}
void ResolveOverlaps(Widget* fixed, bool updateAnchors) {
    std::vector<Widget*> order;
    if (fixed && fixed->enabled && fixed->placed) order.push_back(fixed);
    for (auto& w : g_widgets) if (w.get() != fixed && w->enabled && w->placed) order.push_back(w.get());
    std::vector<RECT> settled;
    for (Widget* w : order) {
        ClampInPlace(w);
        RECT r = RectAt(w, w->x, w->y);
        int g = (int)(12 * ScaleOf(MonitorAt(w->x + 8, w->y + 8)));
        int ox = w->x, oy = w->y;
        if (w != fixed && Collides(r, settled, g)) {
            int nx, ny;
            if (FindFreeSpot(w, settled, nx, ny)) { w->x = nx; w->y = ny; }
        }
        if (w->x != ox || w->y != oy) {
            if (w->hwnd) { Layout(w); Render(w); }
            if (updateAnchors) UpdateAnchor(w);
        }
        settled.push_back(RectAt(w, w->x, w->y));
    }
}
void LayoutWidget(Widget* w) { Layout(w); }
void PreviewWidget(Widget* w) {
    if (!w->enabled || !w->hwnd) return;
    PositionFromAnchor(w);
    ClampInPlace(w);
    Layout(w); Render(w);
}
void RenderWidget(Widget* w, bool contentSame) { Render(w, contentSame); }

// A2: while resizing, mouse moves only update the geometry; the widget is redrawn at most once per display frame.
static Widget* g_framePending = nullptr;
static LONGLONG g_lastFrame = 0;
static double FrameMs() {
    static double ms = 0;
    if (!ms) { DEVMODEW dm{}; dm.dmSize = sizeof(dm); ms = (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) ? 1000.0 / dm.dmDisplayFrequency : 16.7; }
    return ms;
}
void FlushFrame() {
    KillTimer(g_main, T_FRAME);
    Widget* w = g_framePending; g_framePending = nullptr;
    for (auto& x : g_widgets) if (x.get() == w && w->enabled && w->hwnd) { Layout(w); Render(w); PerfDragStep(); g_lastFrame = Qpc(); break; }
}
void RenderSoon(Widget* w) {
    g_framePending = w;
    double since = QpcMs(Qpc() - g_lastFrame);
    if (since >= FrameMs()) FlushFrame();
    else SetTimer(g_main, T_FRAME, (UINT)std::ceil(FrameMs() - since), nullptr);   // catches the final size when the mouse stops
}
void ScheduleRelease() { ArmRelease(); }

// Re-derive every position after resolution / DPI / taskbar / monitor changes.
static void RelayoutAll() {
    for (auto& w : g_widgets) {
        if (!w->enabled) continue;
        PositionFromAnchor(w.get());
        ClampInPlace(w.get());
        if (w->hwnd) Layout(w.get());
    }
    ResolveOverlaps(nullptr, false);
    for (auto& w : g_widgets) if (w->hwnd) Render(w.get());
}

// ------------------------------------------------------------------ widget windows
static LRESULT CALLBACK WidgetProc(HWND, UINT, WPARAM, LPARAM);

Widget::~Widget() {
    if (hwnd) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd); hwnd = nullptr; }
}

static void ShowWidget(Widget* w) {
    if (!w->enabled) { if (w->hwnd) { DestroyWindow(w->hwnd); w->hwnd = nullptr; } w->bg.clear(); w->bg.shrink_to_fit(); return; }
    if (!PositionFromAnchor(w) && !w->placed) AutoPlace();
    ClampInPlace(w);                     // never partly off-screen
    Layout(w);
    if (!w->hwnd) {
        w->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PrismWidget", w->title.c_str(), WS_POPUP,
                                  w->x - w->margin, w->y - w->margin, w->bw, w->bh, DesktopOwner(), nullptr, g_inst, nullptr);
        if (!w->hwnd) return;
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
        Render(w);
        ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(w->hwnd, g_arranging ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowPos(w->hwnd, nullptr, w->x - w->margin, w->y - w->margin, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Render(w);
    }
}

static void BuildWidgets() {
    g_widgets.clear();
    g_widgets.push_back(MakeClock());
    g_widgets.push_back(MakeWeather());
    g_widgets.push_back(MakeCalendar());
    g_widgets.push_back(MakeTodo());
    g_widgets.push_back(MakeMedia());
    g_widgets.push_back(MakeSystem());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((CustomWidgetDir() + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) g_widgets.push_back(MakeCustom(fd.cFileName)); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& w : g_widgets) ApplyEntry(w.get());
    for (auto& w : g_widgets) ShowWidget(w.get());
    ResolveOverlaps(nullptr, false);
    for (auto& w : g_widgets) if (w->enabled && !w->hasAnchor) UpdateAnchor(w.get());   // migrate old x/y configs
}

void ReloadCustom() {
    SaveConfigNow();   // persist current custom positions before rebuilding
    std::vector<std::unique_ptr<Widget>> keep;
    for (auto& w : g_widgets) if (!w->IsCustom()) keep.push_back(std::move(w));
    g_widgets.swap(keep);
    keep.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((CustomWidgetDir() + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            auto w = MakeCustom(fd.cFileName);
            ApplyEntry(w.get());
            g_widgets.push_back(std::move(w));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& w : g_widgets) if (w->IsCustom()) { ShowWidget(w.get()); if (w->enabled && !w->hasAnchor) UpdateAnchor(w.get()); }
    ResolveOverlaps(nullptr, true);
    ScheduleTick();
    RequestSave();
    SettingsRefresh(true);
}

// snapping to screen edges and other widgets
static void Snap(Widget* w, int& x, int& y) {
    int pw = PanelW(w), ph = PanelH(w), th = (int)(10 * w->scale), gap = (int)(18 * w->scale), edge = (int)(24 * w->scale);
    RECT r{ x, y, x + pw, y + ph };
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    std::vector<int> cx = { mi.rcWork.left + edge, mi.rcWork.right - edge - pw }, cy = { mi.rcWork.top + edge, mi.rcWork.bottom - edge - ph };
    for (auto& o : g_widgets) {
        if (o.get() == w || !o->enabled || !o->hwnd) continue;
        int ox = o->x, oy = o->y, ow = PanelW(o.get()), oh = PanelH(o.get());
        cx.insert(cx.end(), { ox, ox + ow - pw, ox + ow + gap, ox - gap - pw });
        cy.insert(cy.end(), { oy, oy + oh - ph, oy + oh + gap, oy - gap - ph });
    }
    int bx = th + 1, by = th + 1, sx = x, sy = y;
    for (int c : cx) if (std::abs(c - x) < bx) { bx = std::abs(c - x); sx = c; }
    for (int c : cy) if (std::abs(c - y) < by) { by = std::abs(c - y); sy = c; }
    x = sx; y = sy;
}

// ------------------------------------------------------------------ services used by Settings
void ApplyWidget(Widget* w) {
    if (w->key == L"media" && w->enabled) MediaKick(100);
    ShowWidget(w);                              // re-derives from its anchor, so growing keeps the anchored edge
    if (w->enabled) { if (!w->hasAnchor) UpdateAnchor(w); ResolveOverlaps(w, true); }
    ScheduleTick(); RequestSave();
}
// Posted (never synchronous): weather state can change while a widget is being drawn.
void WeatherChanged() { PostMessageW(g_main, WM_APP + 7, 0, 0); }
void EventsChanged() { PostMessageW(g_main, WM_APP + 10, 0, 0); }
static void OnWeatherChanged() {
    for (auto& w : g_widgets) if (w->key == L"weather" && w->enabled && w->hwnd) Render(w.get());
    SettingsRefresh(true);
}
void ApplyGlobal(bool rebuildGlass) { Widget_RenderAll(rebuildGlass); RequestSave(); }
void ResetPositions() {
    for (auto& w : g_widgets) { w->placed = false; w->hasAnchor = false; }
    for (auto& w : g_widgets) ShowWidget(w.get());
    ResolveOverlaps(nullptr, true);
    ScheduleTick(); RequestSave();
}
// B4: the media session is polled here, outside of drawing; the widget redraws only on changes (or each second while playing)
static Widget* MediaWidgetOn() { for (auto& w : g_widgets) if (w->key == L"media" && w->enabled && w->hwnd) return w.get(); return nullptr; }
void MediaKick(UINT ms) { SetTimer(g_main, T_MEDIA, ms, nullptr); }
static void MediaTick() {
    KillTimer(g_main, T_MEDIA);
    Widget* mw = MediaWidgetOn();
    if (!mw) return;                                  // stops until the widget is shown again
    if (Paused()) { MediaKick(3000); return; }
    bool changed = g_media.Pump();
    if (changed || g_media.st.playing) Render(mw);   // progress bar moves while playing
    MediaKick(g_media.Busy() ? 200 : (g_media.st.playing ? 1000 : 2000));
}
void ReleaseCaches() { g_wall.Release(); g_gr.ClearFormats(); HeapCompact(GetProcessHeap(), 0); }
void FreeMemoryNow() { ReleaseCaches(); TrimMemory(); }

// A1: during a drag the finished picture just slides; the glass is resampled when the mouse rests or on drop.
static Widget* g_glassPending = nullptr;
void MoveWidgetFast(Widget* w) {
    if (!w->hwnd) return;
    float s = ScaleOf(MonitorAt(w->x + 8, w->y + 8));
    if (std::fabs(s - w->scale) > 0.01f) { Layout(w); Render(w); return; }   // moved onto a monitor with other scaling
    w->bgValid = false;
    SetWindowPos(w->hwnd, nullptr, w->x - w->margin, w->y - w->margin, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_glassPending = w;
    SetTimer(g_main, T_GLASS, 140, nullptr);
}
static void FlushGlass() {
    KillTimer(g_main, T_GLASS);
    Widget* w = g_glassPending; g_glassPending = nullptr;
    for (auto& x : g_widgets) if (x.get() == w && w->enabled && w->hwnd && !w->bgValid) { Render(w, true); break; }
}
void QuitApp() { DestroyWindow(g_main); }
void OpenDataFolder() { ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ------------------------------------------------------------------ input box (todo entry)
struct InputState { HWND wnd = nullptr, edit = nullptr; WNDPROC oldProc = nullptr; HFONT font = nullptr; HBRUSH bg = nullptr;
                    std::function<void(const std::wstring&)> done; bool closing = false; } g_input;

static void CloseInput(bool commit) {
    if (!g_input.wnd || g_input.closing) return;
    g_input.closing = true;
    std::wstring text;
    if (commit) {
        int n = GetWindowTextLengthW(g_input.edit);
        text.resize(n + 1); GetWindowTextW(g_input.edit, &text[0], n + 1); text.resize(n);
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        while (!text.empty() && iswspace(text[0])) text.erase(0, 1);
    }
    auto cb = std::move(g_input.done);
    DestroyWindow(g_input.wnd);
    if (g_input.font) DeleteObject(g_input.font);
    if (g_input.bg) DeleteObject(g_input.bg);
    g_input = InputState();
    if (commit && cb) cb(text);
}
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    WNDPROC old = g_input.oldProc;
    if (m == WM_KEYDOWN && wp == VK_RETURN) { CloseInput(true); return 0; }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { CloseInput(false); return 0; }
    if (m == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;
    if (m == WM_KILLFOCUS) { LRESULT r = CallWindowProcW(old, h, m, wp, lp); PostMessageW(g_main, WM_APP + 9, 0, 0); return r; }
    return CallWindowProcW(old, h, m, wp, lp);
}
static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CTLCOLOREDIT) {
        SetTextColor((HDC)wp, RGB(245, 245, 245)); SetBkColor((HDC)wp, RGB(32, 32, 36));
        return (LRESULT)g_input.bg;
    }
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, g_input.bg); return 1; }
    if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) { PostMessageW(g_main, WM_APP + 9, 0, 0); }
    return DefWindowProcW(h, m, wp, lp);
}
void ShowInput(Widget* w, D2D1_RECT_F lr, const std::wstring& init, std::function<void(const std::wstring&)> done) {
    float sc = w->scale * w->size;
    int x = w->x + (int)(lr.left * sc), y = w->y + (int)(lr.top * sc);
    ShowInputAt(RECT{ x, y, x + (int)((lr.right - lr.left) * sc), y + (int)((lr.bottom - lr.top) * sc) }, sc, init, L"Type and press Enter", std::move(done));
}
void ShowInputAt(RECT rc, float s, const std::wstring& init, const wchar_t* cue, std::function<void(const std::wstring&)> done) {
    CloseInput(false);
    int x = rc.left, y = rc.top, wd = rc.right - rc.left, ht = rc.bottom - rc.top;
    g_input.done = std::move(done);
    g_input.bg = CreateSolidBrush(RGB(32, 32, 36));
    g_input.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"PrismInput", L"", WS_POPUP, x, y, wd, ht, nullptr, nullptr, g_inst, nullptr);
    if (!g_input.wnd) return;
    int corner = 2 /*DWMWCP_ROUND*/; BOOL dark = TRUE;
    DwmSetWindowAttribute(g_input.wnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    DwmSetWindowAttribute(g_input.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    D2D1_COLOR_F a = Accent(); COLORREF bc = RGB((int)(a.r * 255), (int)(a.g * 255), (int)(a.b * 255));
    DwmSetWindowAttribute(g_input.wnd, 34 /*DWMWA_BORDER_COLOR*/, &bc, sizeof(bc));
    int fh = (int)(14 * s * 1.33f), pad = (int)(12 * s);
    g_input.font = CreateFontW(-fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    g_input.edit = CreateWindowExW(0, L"EDIT", init.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, pad, (ht - fh - 4) / 2, wd - 2 * pad, fh + 4, g_input.wnd, nullptr, g_inst, nullptr);
    SendMessageW(g_input.edit, WM_SETFONT, (WPARAM)g_input.font, TRUE);
    SendMessageW(g_input.edit, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    SendMessageW(g_input.edit, EM_SETSEL, 0, -1);
    g_input.oldProc = (WNDPROC)SetWindowLongPtrW(g_input.edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    ShowWindow(g_input.wnd, SW_SHOW);
    SetForegroundWindow(g_input.wnd);
    SetFocus(g_input.edit);
}

// ------------------------------------------------------------------ widget context menu
static void WidgetMenu(Widget* w, int cx, int cy) {
    HMENU m = CreatePopupMenu();
    w->RightClickAt(LocalX(w, cx), LocalY(w, cy), m);
    int before = GetMenuItemCount(m);
    w->Menu(m);
    if (GetMenuItemCount(m) > before) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU sz = CreatePopupMenu();
    for (int i = 0; i < 4; i++) AppendMenuW(sz, MF_STRING | (std::fabs(w->size - g_sizes[i]) < .01f ? MF_CHECKED : 0), 201 + i, g_sizeNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, L"Size");
    HMENU th = CreatePopupMenu();
    AppendMenuW(th, MF_STRING | (w->theme.empty() ? MF_CHECKED : 0), 210, L"Use global theme");
    AppendMenuW(th, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < g_themeCount; i++) AppendMenuW(th, MF_STRING | (w->theme == g_themes[i].id ? MF_CHECKED : 0), 211 + i, g_themes[i].name);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"Theme");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 230, g_arranging ? L"Finish arranging" : L"Arrange widgets\u2026");
    AppendMenuW(m, MF_STRING, 231, L"Hide this widget");
    AppendMenuW(m, MF_STRING, 232, L"Widget settings\u2026");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    PostMessageW(g_main, WM_NULL, 0, 0);
    if (!id) return;
    if (id >= 201 && id <= 204) w->size = g_sizes[id - 201];
    else if (id == 210) w->theme.clear();
    else if (id >= 211 && id < 211 + g_themeCount) w->theme = g_themes[id - 211].id;
    else if (id == 230) { if (g_arranging) EndArrange(); else StartArrange(); return; }
    else if (id == 231) { w->enabled = false; ShowWidget(w); RequestSave(); SettingsRefresh(true); return; }
    else if (id == 232) { OpenSettings(w); return; }
    else w->Command(id);
    Layout(w);
    Render(w);
    ScheduleTick();
    RequestSave();
    SettingsRefresh(true);
}

// ------------------------------------------------------------------ widget window proc
static struct { Widget* w = nullptr; POINT start{}; int ox = 0, oy = 0; bool down = false, dragging = false, resizing = false; } g_drag;
static bool InPanel(Widget* w, float lx, float ly) { D2D1_SIZE_F b = w->BaseSize(); return lx >= 0 && ly >= 0 && lx < b.width && ly < b.height; }
static Widget* WidgetPanelAt(POINT p, Widget* except) {
    for (auto it = g_widgets.rbegin(); it != g_widgets.rend(); ++it) {
        Widget* o = it->get();
        if (o == except || !o->enabled || !o->hwnd) continue;
        RECT r = PanelRect(o);
        if (PtInRect(&r, p)) return o;
    }
    return nullptr;
}

static LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Widget* w = (Widget*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!w) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: {
        auto* p = (WINDOWPOS*)lp;
        if (!g_arranging && !(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM;   // stay on the desktop layer
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        Interactive();
        if (g_arranging && w == g_arrangeSel) {
            int zone = ArrangeZone(w, LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)));
            if (zone >= 0) { ArrangeResetZone(w, zone); return 0; }
        }
        [[fallthrough]];
    case WM_LBUTTONDOWN:
        Interactive();
        if (g_arranging) {
            float lx = LocalX(w, GET_X_LPARAM(lp)), ly = LocalY(w, GET_Y_LPARAM(lp));
            POINT cp; GetCursorPos(&cp);
            int zone = w == g_arrangeSel ? ArrangeZone(w, lx, ly) : -1;
            if (zone >= 0) {   // resize / scale handle
                SetCapture(h);
                g_drag.w = w; g_drag.down = true; g_drag.dragging = false; g_drag.resizing = true;
                ArrangeResizeBegin(w, zone, cp);
                return 0;
            }
            if (!InPanel(w, lx, ly)) {   // click landed in this window's shadow margin: hand it to the widget really under the cursor
                Widget* o = WidgetPanelAt(cp, w);
                if (!o) { ArrangeSelect(nullptr); return 0; }
                w = o; h = o->hwnd;
            }
            ArrangeSelect(w);
        } else {
            int id;   // B3: show the press immediately
            if (w->HitId(LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)), id)) {
                LONGLONG q0 = Qpc();
                w->isPressed = true; w->pressed = id;
                Render(w);
                PerfClick(GetMessageTime(), q0);
            }
        }
        SetCapture(h);
        g_drag.w = w; g_drag.down = true; g_drag.dragging = false;
        GetCursorPos(&g_drag.start); g_drag.ox = w->x; g_drag.oy = w->y;
        return 0;
    case WM_MOUSEMOVE: {
        Interactive();
        if (g_drag.down && g_drag.w == w && g_drag.resizing) {
            POINT p; GetCursorPos(&p);
            ArrangeResizeMove(w, p, (GetKeyState(VK_MENU) & 0x8000) != 0);
            return 0;
        }
        if (g_drag.down && g_drag.w == w) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - g_drag.start.x, dy = p.y - g_drag.start.y;
            if (!g_drag.dragging && (g_arranging || g_quickDrag) && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
                g_drag.dragging = true; PerfDragBegin();
                if (w->isPressed) { w->isPressed = false; Render(w); }
            }
            if (g_drag.dragging) {
                int nx = g_drag.ox + dx, ny = g_drag.oy + dy;
                bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;   // hold Alt for free placement
                if (g_arranging) { if (alt) ArrangeClearGuides(); else ArrangeSnap(w, nx, ny, p); }
                else if (g_snap && !alt) Snap(w, nx, ny);
                ClampWidget(w, nx, ny, &p);                        // never leaves the screen
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; MoveWidgetFast(w); PerfDragStep(); }
            }
            return 0;
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        if (!g_arranging && w->Hover(LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)))) Render(w);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag.down && g_drag.w == w && g_drag.resizing) {
            g_drag.down = g_drag.resizing = false; g_drag.w = nullptr;
            ReleaseCapture();
            ArrangeResizeEnd(w);
            return 0;
        }
        if (g_drag.down && g_drag.w == w) {
            bool dragged = g_drag.dragging;
            g_drag.down = g_drag.dragging = false; g_drag.w = nullptr;
            ReleaseCapture();
            if (dragged) {
                if (g_arranging) ArrangeClearGuides();
                PerfDragEnd();
                g_glassPending = w; FlushGlass();   // final glass at the drop position
                w->placed = true;
                UpdateAnchor(w);
                ResolveOverlaps(w, true);   // neighbours make room
                RequestSave(); ArmRelease();
            }
            else if (!g_arranging) {
                bool wasPressed = w->isPressed; w->isPressed = false;
                if (w->Click(LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)))) { Render(w); RequestSave(); }
                else if (wasPressed) Render(w);   // release the pressed look
            }
            if (w->isPressed) { w->isPressed = false; Render(w); }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (w->isPressed && (HWND)lp != h) { w->isPressed = false; Render(w); }
        if (g_drag.w == w && (HWND)lp != h && g_drag.resizing) {
            g_drag.down = g_drag.resizing = false; g_drag.w = nullptr;
            ArrangeResizeEnd(w);
            return 0;
        }
        if (g_drag.w == w && (HWND)lp != h) {
            if (g_drag.dragging) { if (g_arranging) ArrangeClearGuides(); w->placed = true; UpdateAnchor(w); ResolveOverlaps(w, true); RequestSave(); }
            g_drag.down = g_drag.dragging = false; g_drag.w = nullptr;
        }
        return 0;
    case WM_MOUSELEAVE: if (!g_arranging) { w->Leave(); Render(w); } return 0;
    case WM_MOUSEWHEEL: {
        Interactive();
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (!g_arranging && w->Wheel(LocalX(w, p.x), LocalY(w, p.y), GET_WHEEL_DELTA_WPARAM(wp))) Render(w);
        return 0;
    }
    case WM_RBUTTONUP: Interactive(); WidgetMenu(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_SETCURSOR: {
        LPCWSTR cur = g_quickDrag ? IDC_SIZEALL : IDC_ARROW;
        if (g_arranging) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            float lx = LocalX(w, p.x), ly = LocalY(w, p.y);
            int zone = (w == g_arrangeSel || (g_drag.resizing && g_drag.w == w)) ? ArrangeZone(w, lx, ly) : -1;
            cur = zone >= 0 ? ArrangeZoneCursor(zone) : (InPanel(w, lx, ly) ? IDC_SIZEALL : IDC_ARROW);
        }
        SetCursor(LoadCursorW(nullptr, cur));
        return TRUE;
    }
    case WM_DPICHANGED: Layout(w); Render(w); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ tray
static void TrayAdd(bool balloon) {
    NOTIFYICONDATAW n{ sizeof(n) };
    n.hWnd = g_main; n.uID = 1; n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(n.szTip, APP_NAME);
    if (balloon) {
        n.uFlags |= NIF_INFO;
        wcscpy_s(n.szInfoTitle, L"Prism Widgets is running");
        wcscpy_s(n.szInfo, L"Drag widgets to move them. Right-click a widget or this icon for themes, widgets and Start with Windows.");
        n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    }
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}
static void TrayRemove() { NOTIFYICONDATAW n{ sizeof(n) }; n.hWnd = g_main; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }

void CreateNewWidget() {
    std::wstring dir = CustomWidgetDir(), p;
    for (int i = 1; i < 100; i++) { p = dir + L"\\My Widget " + std::to_wstring(i) + L".json"; if (!FileExists(p)) break; }
    WriteTextFile(p, LR"JSON({
  "name": "My Widget",
  "width": 260,
  "height": 110,
  "refresh": 1000,
  "elements": [
    { "type": "text", "x": 20, "y": 14, "text": "{time:HH:mm:ss}", "size": 34, "weight": "light" },
    { "type": "text", "x": 22, "y": 66, "text": "CPU {cpu}%   RAM {ram}%", "size": 13, "color": "fg2" }
  ]
}
)JSON");
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

static void ShowHelp() {
    std::wstring msg =
        L"Prism Widgets " APP_VERSION L"\n\n"
        L"\u2022 To move widgets, choose Arrange widgets (tray, right-click or Settings), then drag. Esc or Done to finish.\n"
        L"• Right-click a widget for its options, size and theme.\n"
        L"• Right-click the tray icon to show/hide widgets, change the theme, accent and Start with Windows.\n"
        L"• Custom widgets are JSON files in:\n   " + CustomWidgetDir() + L"\n"
        L"   Edit or add files there - changes appear instantly.\n\n"
        L"Widgets pause automatically while a fullscreen app or game is running, when the screen is off, and when the PC is locked.";
    MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_OK | MB_ICONINFORMATION);
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1189, L"Open Settings\u2026");
    SetMenuDefaultItem(m, 1189, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU wm = CreatePopupMenu();
    bool sep = false;
    for (size_t i = 0; i < g_widgets.size(); i++) {
        if (g_widgets[i]->IsCustom() && !sep) { AppendMenuW(wm, MF_SEPARATOR, 0, nullptr); sep = true; }
        AppendMenuW(wm, MF_STRING | (g_widgets[i]->enabled ? MF_CHECKED : 0), 1000 + i, g_widgets[i]->title.c_str());
    }
    AppendMenuW(wm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wm, MF_STRING, 1300, L"Create new custom widget…");
    AppendMenuW(wm, MF_STRING, 1301, L"Open custom widgets folder");
    AppendMenuW(wm, MF_STRING, 1302, L"Reload custom widgets");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)wm, L"Widgets");
    HMENU tm = CreatePopupMenu();
    for (int i = 0; i < g_themeCount; i++) AppendMenuW(tm, MF_STRING | (g_theme == g_themes[i].id ? MF_CHECKED : 0), 1100 + i, g_themes[i].name);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Theme");
    HMENU am = CreatePopupMenu();
    AppendMenuW(am, MF_STRING | (g_accent == L"system" ? MF_CHECKED : 0), 1150, L"Windows accent colour");
    AppendMenuW(am, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < (int)(sizeof(g_accents) / sizeof(g_accents[0])); i++)
        AppendMenuW(am, MF_STRING | (g_accent == g_accents[i].hex ? MF_CHECKED : 0), 1151 + i, g_accents[i].name);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)am, L"Accent colour");
    HMENU xm = CreatePopupMenu();
    AppendMenuW(xm, MF_STRING | (g_textMode == L"auto" ? MF_CHECKED : 0), 1170, L"Automatic (match wallpaper)");
    AppendMenuW(xm, MF_STRING | (g_textMode == L"light" ? MF_CHECKED : 0), 1171, L"Light text");
    AppendMenuW(xm, MF_STRING | (g_textMode == L"dark" ? MF_CHECKED : 0), 1172, L"Dark text");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)xm, L"Text colour");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1180, g_arranging ? L"Finish arranging" : L"Arrange widgets\u2026");
    AppendMenuW(m, MF_STRING, 1181, L"Reset widget positions");
    AppendMenuW(m, MF_STRING | (IsStartup() ? MF_CHECKED : 0), 1182, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1190, L"Help");
    AppendMenuW(m, MF_STRING, 1199, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    PostMessageW(g_main, WM_NULL, 0, 0);
    if (!id) return;
    if (id == 1189) { OpenSettings(); return; }
    if (id >= 1000 && id < 1000 + (int)g_widgets.size()) {
        Widget* w = g_widgets[id - 1000].get(); w->enabled = !w->enabled; ShowWidget(w); ScheduleTick(); RequestSave();
    } else if (id >= 1100 && id < 1100 + g_themeCount) { g_theme = g_themes[id - 1100].id; Widget_RenderAll(true); RequestSave(); }
    else if (id == 1150) { g_accent = L"system"; Widget_RenderAll(true); RequestSave(); }
    else if (id > 1150 && id < 1170) { g_accent = g_accents[id - 1151].hex; Widget_RenderAll(true); RequestSave(); }
    else if (id >= 1170 && id <= 1172) { g_textMode = id == 1170 ? L"auto" : id == 1171 ? L"light" : L"dark"; Widget_RenderAll(false); RequestSave(); }
    else if (id == 1180) { if (g_arranging) EndArrange(); else StartArrange(); return; }
    else if (id == 1181) ResetPositions();
    else if (id == 1182) SetStartup(!IsStartup());
    else if (id == 1190) ShowHelp();
    else if (id == 1199) DestroyWindow(g_main);
    else if (id == 1300) CreateNewWidget();
    else if (id == 1301) ShellExecuteW(nullptr, L"open", CustomWidgetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (id == 1302) ReloadCustom();
    SettingsRefresh(true);
}

// ------------------------------------------------------------------ main window
static void ReloadWallpaper() {
    g_wall.Release();
    g_wallSig = g_wall.Signature();
    for (auto& w : g_widgets) w->bgValid = false;
    RelayoutAll();   // resolution / DPI / monitor changes keep widgets anchored and on-screen
    ScheduleTick();
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == g_msgTaskbarCreated && m) {
        TrayAdd(false);
        HWND owner = DesktopOwner();
        for (auto& w : g_widgets) if (w->hwnd) SetWindowLongPtrW(w->hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner);
        SetTimer(h, T_WALLRELOAD, 1500, nullptr);
        return 0;
    }
    switch (m) {
    case WM_TIMER:
        switch (wp) {
        case T_TICK: KillTimer(h, T_TICK); OnTick(); break;
        case T_RELEASE:   // drop wallpaper proxies + trim working set once idle
            KillTimer(h, T_RELEASE);
            if (!g_drag.dragging && !g_arranging) g_wall.Release();   // proxy only; no working-set trim here (B2)
            break;
        case T_SAVE: KillTimer(h, T_SAVE); SaveConfigNow(); break;
        case T_WALLCHECK: { std::wstring s = g_wall.Signature(); if (s != g_wallSig) ReloadWallpaper(); break; }
        case T_RELOAD_CUSTOM: KillTimer(h, T_RELOAD_CUSTOM); ReloadCustom(); break;
        case T_WALLRELOAD: KillTimer(h, T_WALLRELOAD); ReloadWallpaper(); break;
        case T_GLASS: FlushGlass(); break;
        case T_MEDIA: MediaTick(); break;
        case T_FRAME: FlushFrame(); break;
        case T_RELAX:
            if (g_arranging || g_drag.down || OurWindowInFront()) break;   // still being used: stay responsive
            KillTimer(h, T_RELAX); SetPowerMode(false);
            break;
        case T_DEEPIDLE:   // 10 idle minutes: now it's worth giving memory back to Windows
            KillTimer(h, T_DEEPIDLE);
            if (!g_arranging && !g_drag.down && !OurWindowInFront()) { ReleaseCaches(); TrimMemory(); }
            else SetTimer(h, T_DEEPIDLE, 10 * 60 * 1000, nullptr);
            break;
        case EVENTS_TIMER_ID: {
            bool on = false; for (auto& w : g_widgets) if (w->key == L"calendar" && w->enabled) on = true;
            if (on && !Paused()) g_events.Refresh(false);
            break;
        }
        case WEATHER_TIMER_ID: {
            KillTimer(h, WEATHER_TIMER_ID);
            bool on = false; for (auto& w : g_widgets) if (w->key == L"weather" && w->enabled) on = true;
            if (on && !Paused()) g_weather.Refresh(false);
            else if (on) SetTimer(h, WEATHER_TIMER_ID, 5 * 60 * 1000, nullptr);   // try again after the game / lock
            break;
        }
        }
        return 0;
    case WM_TRAY:
        Interactive();
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU: ShowTrayMenu(); break;
        case NIN_SELECT: case NIN_KEYSELECT: OpenSettings(); break;
        }
        return 0;
    case WM_SHOWSETTINGS: OpenSettings(); return 0;
    case WM_STARTARRANGE: StartArrange(); return 0;
    case WM_WEATHER: g_weather.OnResult(wp, lp); return 0;
    case WM_NET: NetDispatch(lp); return 0;
    case WM_APP + 10: {   // events changed (posted; may originate inside Draw)
        MSG dup; while (PeekMessageW(&dup, h, WM_APP + 10, WM_APP + 10, PM_REMOVE)) {}
        for (auto& w : g_widgets) if (w->key == L"calendar" && w->enabled && w->hwnd) Render(w.get());
        SettingsRefresh(true);
        RefreshDayPopup();
        return 0;
    }
    case WM_APP + 7: { MSG dup; while (PeekMessageW(&dup, h, WM_APP + 7, WM_APP + 7, PM_REMOVE)) {} OnWeatherChanged(); return 0; }
    case WM_APP + 9: if (g_input.wnd && GetForegroundWindow() != g_input.wnd) CloseInput(true); return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER) SetTimer(h, T_WALLRELOAD, 700, nullptr);
        else if (wp == SPI_SETWORKAREA) { EndArrange(); RelayoutAll(); }
        else if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") && g_accent == L"system") Widget_RenderAll(true);
        return 0;
    case WM_DISPLAYCHANGE: EndArrange(); SetTimer(h, T_WALLRELOAD, 1000, nullptr); return 0;
    case WM_TIMECHANGE: Widget_RenderAll(false); return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_sessionLocked = true;
        else if (wp == WTS_SESSION_UNLOCK) { g_sessionLocked = false; SetTimer(h, T_TICK, 200, nullptr); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* ps = (POWERBROADCAST_SETTING*)lp;
            if (ps->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) {
                g_displayOff = *(DWORD*)ps->Data == 0;
                if (!g_displayOff) SetTimer(h, T_TICK, 200, nullptr);
            }
        } else if (wp == PBT_APMRESUMEAUTOMATIC) { Widget_RenderAll(false); SetTimer(h, WEATHER_TIMER_ID, 15000, nullptr); }   // fresh weather after sleep
        return TRUE;
    case WM_DESTROY:
        CloseDayPopup();
        EndArrange();
        CloseSettings();
        SaveConfigNow();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ entry
static void BeLightweight() {
    // Start in efficiency mode (below-normal + EcoQoS). Memory priority stays normal so Windows doesn't
    // page us out first; that caused slow first clicks after idle (B2).
    SetPowerMode(false);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    g_inst = inst;
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\PrismWidgets.Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: bring up its Settings window instead of starting twice
        HWND other = FindWindowW(L"PrismWidgetsMain", nullptr);
        if (other) {
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (cmd && wcsstr(cmd, L"--arrange")) PostMessageW(other, WM_STARTARRANGE, 0, 0);
            else if (!(cmd && wcsstr(cmd, L"--startup"))) PostMessageW(other, WM_SHOWSETTINGS, 0, 0);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BeLightweight();

    bool startup = cmd && wcsstr(cmd, L"--startup");
    bool arrange = cmd && wcsstr(cmd, L"--arrange");
    bool hidden = startup || arrange || (cmd && wcsstr(cmd, L"--hidden"));
    if (startup) for (int i = 0; i < 60 && !FindWindowW(L"Progman", nullptr); i++) Sleep(500);   // wait for Explorer

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = L"PrismWidgetsMain";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    wc.lpfnWndProc = WidgetProc; wc.lpszClassName = L"PrismWidget"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = InputProc; wc.lpszClassName = L"PrismInput";
    RegisterClassExW(&wc);
    RegisterSettingsClass();

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"PrismWidgetsMain", APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_main, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    WTSRegisterSessionNotification(g_main, NOTIFY_FOR_THIS_SESSION);
    HPOWERNOTIFY pn = RegisterPowerSettingNotification(g_main, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!g_gr.Init()) { MessageBoxW(nullptr, L"Direct2D is not available.", APP_NAME, MB_ICONERROR); return 1; }
    LoadConfig();
    if (IsStartup()) SetStartup(true);   // keep the Run entry pointing at this exe if it was moved
    if (g_firstRun) WriteSampleWidgets();
    LoadTodos();
    g_wallSig = g_wall.Signature();
    BuildWidgets();
    PetInit();
    if (g_firstRun) SaveConfigNow(); else RequestSave();   // persist any clamping / migrated anchors
    bool showWelcome = g_firstRun;
    g_firstRun = false;   // custom widgets added from now on start enabled
    TrayAdd(showWelcome && hidden);
    if (!hidden) OpenSettings();
    if (arrange) PostMessageW(g_main, WM_STARTARRANGE, 0, 0);
    ScheduleTick();
    SetTimer(g_main, T_WALLCHECK, 60000, nullptr);   // catches slideshow / Spotlight wallpaper changes
    ArmRelease();
    SetTimer(g_main, T_DEEPIDLE, 10 * 60 * 1000, nullptr);
    MediaKick(400);

    g_dirWatch = FindFirstChangeNotificationW(CustomWidgetDir().c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);

    MSG msg;
    for (;;) {
        DWORD n = g_dirWatch != INVALID_HANDLE_VALUE ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjectsEx(n, &g_dirWatch, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (n && r == WAIT_OBJECT_0) {
            SetTimer(g_main, T_RELOAD_CUSTOM, 400, nullptr);   // debounce editor saves
            FindNextChangeNotification(g_dirWatch);
            continue;
        }
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }
    if (g_dirWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_dirWatch);
    g_widgets.clear();
    PetShutdown();
    g_media.Shutdown();
    g_weather.Shutdown();
    if (pn) UnregisterPowerSettingNotification(pn);
    WTSUnRegisterSessionNotification(g_main);
    g_gr.DropTarget(); g_gr.ClearFormats();
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
