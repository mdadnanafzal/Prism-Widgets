// Prism Widgets - app shell: widget windows, tray menu, config, scheduling.
#include "app.h"
#include <psapi.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>

#define WM_TRAY        (WM_APP + 1)
#define WM_SHOWSETTINGS (WM_APP + 2)
enum { T_TICK = 1, T_RELEASE, T_SAVE, T_WALLCHECK, T_RELOAD_CUSTOM, T_WALLRELOAD };

HINSTANCE g_inst;
HWND g_main;
static UINT g_msgTaskbarCreated;
std::vector<std::unique_ptr<Widget>> g_widgets;
static JVal g_cfg;
std::wstring g_theme = L"liquid", g_accent = L"system", g_textMode = L"auto", g_uiTheme = L"system";
static std::wstring g_wallSig;
bool g_locked = false, g_snap = true, g_pauseFullscreen = true;
static bool g_firstRun = false, g_sessionLocked = false, g_displayOff = false;
static HANDLE g_dirWatch = INVALID_HANDLE_VALUE;

const AccentPreset g_accents[] = {
    { L"Blue", L"#0A84FF" }, { L"Purple", L"#BF5AF2" }, { L"Pink", L"#FF375F" }, { L"Red", L"#FF453A" },
    { L"Orange", L"#FF9F0A" }, { L"Yellow", L"#FFD60A" }, { L"Green", L"#30D158" }, { L"Teal", L"#40C8E0" },
};
const int g_accentCount = sizeof(g_accents) / sizeof(g_accents[0]);
const float g_sizes[] = { 0.85f, 1.0f, 1.2f, 1.45f };
const wchar_t* g_sizeNames[] = { L"Small", L"Medium", L"Large", L"Extra large" };

static void Render(Widget* w);
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

static void ArmRelease() { SetTimer(g_main, T_RELEASE, 8000, nullptr); }

// ------------------------------------------------------------------ rendering
// Draws widget content over its glass layer into the DIB selected into `mem`.
static void DrawWidget(HDC mem, Widget* w, const Theme& t, const uint32_t* bg, int bw, int bh, int margin, float s, float lum) {
    RECT rc{ 0, 0, bw, bh };
    if (FAILED(g_gr.rt->BindDC(mem, &rc))) return;
    float dpi = 96.f * s;
    g_gr.rt->SetDpi(dpi, dpi);
    g_gr.rt->BeginDraw();
    D2D1_MATRIX_3X2_F tr; tr._11 = 1; tr._12 = 0; tr._21 = 0; tr._22 = 1; tr._31 = 0; tr._32 = 0;
    g_gr.rt->SetTransform(&tr);
    D2D1_COLOR_F clear{ 0, 0, 0, 0 };
    g_gr.rt->Clear(&clear);
    // Blit the glass layer through D2D (DC targets don't reliably keep prior DIB contents)
    D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, dpi, dpi };
    ID2D1Bitmap* bgb = nullptr;
    if (SUCCEEDED(g_gr.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)bw, (UINT32)bh }, bg, bw * 4, &bp, &bgb))) {
        g_gr.rt->DrawBitmap(bgb, nullptr, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        bgb->Release();
    }
    tr._31 = tr._32 = margin / s;
    g_gr.rt->SetTransform(&tr);
    Gfx g{ g_gr.rt, g_gr.br, MakeInk(t, lum) };
    D2D1_SIZE_F bs = w->BaseSize();
    w->Draw(g, bs.width, bs.height);
    HRESULT hr = g_gr.rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
    GdiFlush();
}

static HBITMAP MakeDib(HDC mem, int w, int h, void** bits) {
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(mem, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void Render(Widget* w) {
    if (!w->hwnd || !w->enabled) return;
    if (Paused()) { w->dirty = true; return; }
    if (!g_gr.EnsureTarget()) return;
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
        DrawWidget(mem, w, t, w->bg.data(), w->bw, w->bh, w->margin, w->scale * w->size, w->lum);
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
}
static void SaveConfigNow() {
    JVal c = JVal::MakeObj();
    c.set(L"theme") = JVal::MakeStr(g_theme);
    c.set(L"accent") = JVal::MakeStr(g_accent);
    c.set(L"text") = JVal::MakeStr(g_textMode);
    c.set(L"locked") = JVal::MakeBool(g_locked);
    c.set(L"snap") = JVal::MakeBool(g_snap);
    c.set(L"pauseFullscreen") = JVal::MakeBool(g_pauseFullscreen);
    c.set(L"settingsTheme") = JVal::MakeStr(g_uiTheme);
    JVal& arr = c.set(L"widgets"); arr = JVal::MakeArr();
    for (auto& w : g_widgets) {
        JVal e = JVal::MakeObj();
        e.set(L"key") = JVal::MakeStr(w->key);
        e.set(L"enabled") = JVal::MakeBool(w->enabled);
        if (w->placed) { e.set(L"x") = JVal::MakeNum(w->x); e.set(L"y") = JVal::MakeNum(w->y); }
        e.set(L"size") = JVal::MakeNum(w->size);
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
    g_locked = g_cfg.boolean(L"locked", false);
    g_snap = g_cfg.boolean(L"snap", true);
    g_pauseFullscreen = g_cfg.boolean(L"pauseFullscreen", true);
    g_uiTheme = g_cfg.str(L"settingsTheme", L"system");
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
        y += ph + gap; colW = std::max(colW, pw);
    }
}
static void EnsureOnScreen(Widget* w) {
    RECT r{ w->x, w->y, w->x + PanelW(w), w->y + PanelH(w) };
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    RECT x;
    MONITORINFO mi{ sizeof(mi) };
    if (m && GetMonitorInfoW(m, &mi) && IntersectRect(&x, &r, &mi.rcWork) && (x.right - x.left) > 40 && (x.bottom - x.top) > 40) return;
    w->placed = false;
}

// ------------------------------------------------------------------ widget windows
static LRESULT CALLBACK WidgetProc(HWND, UINT, WPARAM, LPARAM);

Widget::~Widget() {
    if (hwnd) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd); hwnd = nullptr; }
}

static void ShowWidget(Widget* w) {
    if (!w->enabled) { if (w->hwnd) { DestroyWindow(w->hwnd); w->hwnd = nullptr; } w->bg.clear(); w->bg.shrink_to_fit(); return; }
    if (!w->placed) AutoPlace();
    Layout(w);
    EnsureOnScreen(w);
    if (!w->placed) { AutoPlace(); Layout(w); }
    if (!w->hwnd) {
        w->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PrismWidget", w->title.c_str(), WS_POPUP,
                                  w->x - w->margin, w->y - w->margin, w->bw, w->bh, DesktopOwner(), nullptr, g_inst, nullptr);
        if (!w->hwnd) return;
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
        Render(w);
        ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else Render(w);
}

static void BuildWidgets() {
    g_widgets.clear();
    g_widgets.push_back(MakeClock());
    g_widgets.push_back(MakeCalendar());
    g_widgets.push_back(MakeTodo());
    g_widgets.push_back(MakeSystem());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((CustomWidgetDir() + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) g_widgets.push_back(MakeCustom(fd.cFileName)); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& w : g_widgets) ApplyEntry(w.get());
    for (auto& w : g_widgets) ShowWidget(w.get());
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
    for (auto& w : g_widgets) if (w->IsCustom()) ShowWidget(w.get());
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
void ApplyWidget(Widget* w) { ShowWidget(w); ScheduleTick(); RequestSave(); }
void ApplyGlobal(bool rebuildGlass) { Widget_RenderAll(rebuildGlass); RequestSave(); }
void ResetPositions() {
    for (auto& w : g_widgets) w->placed = false;
    for (auto& w : g_widgets) ShowWidget(w.get());
    ScheduleTick(); RequestSave();
}
void FreeMemoryNow() { g_wall.Release(); g_gr.ClearFormats(); TrimMemory(); }
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
    CloseInput(false);
    float s = w->scale * w->size;
    int x = w->x + (int)(lr.left * s), y = w->y + (int)(lr.top * s), wd = (int)((lr.right - lr.left) * s), ht = (int)((lr.bottom - lr.top) * s);
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
    SendMessageW(g_input.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type and press Enter");
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
    AppendMenuW(m, MF_STRING | (g_locked ? MF_CHECKED : 0), 230, L"Lock positions");
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
    else if (id == 230) g_locked = !g_locked;
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
static struct { Widget* w = nullptr; POINT start{}; int ox = 0, oy = 0; bool down = false, dragging = false; } g_drag;

static LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Widget* w = (Widget*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!w) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: {
        auto* p = (WINDOWPOS*)lp;
        if (!(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM;   // always stay on the desktop layer
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g_drag.w = w; g_drag.down = true; g_drag.dragging = false;
        GetCursorPos(&g_drag.start); g_drag.ox = w->x; g_drag.oy = w->y;
        return 0;
    case WM_MOUSEMOVE: {
        if (g_drag.down && g_drag.w == w) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - g_drag.start.x, dy = p.y - g_drag.start.y;
            if (!g_drag.dragging && !g_locked && (std::abs(dx) > 4 || std::abs(dy) > 4)) g_drag.dragging = true;
            if (g_drag.dragging) {
                int nx = g_drag.ox + dx, ny = g_drag.oy + dy;
                if (g_snap && !(GetKeyState(VK_MENU) & 0x8000)) Snap(w, nx, ny);   // hold Alt to disable snapping
                if (nx != w->x || ny != w->y) { w->x = nx; w->y = ny; Layout(w); Render(w); }
            }
            return 0;
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        if (w->Hover(LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)))) Render(w);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag.down && g_drag.w == w) {
            bool dragged = g_drag.dragging;
            g_drag.down = g_drag.dragging = false; g_drag.w = nullptr;
            ReleaseCapture();
            if (dragged) { w->placed = true; RequestSave(); ArmRelease(); }
            else if (w->Click(LocalX(w, GET_X_LPARAM(lp)), LocalY(w, GET_Y_LPARAM(lp)))) { Render(w); RequestSave(); }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_drag.w == w && (HWND)lp != h) { if (g_drag.dragging) { w->placed = true; RequestSave(); } g_drag.down = g_drag.dragging = false; g_drag.w = nullptr; }
        return 0;
    case WM_MOUSELEAVE: w->Leave(); Render(w); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (w->Wheel(LocalX(w, p.x), LocalY(w, p.y), GET_WHEEL_DELTA_WPARAM(wp))) Render(w);
        return 0;
    }
    case WM_RBUTTONUP: WidgetMenu(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
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
        L"• Drag a widget to move it (hold Alt to turn off snapping).\n"
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
    AppendMenuW(m, MF_STRING | (g_locked ? MF_CHECKED : 0), 1180, L"Lock widget positions");
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
    else if (id == 1180) { g_locked = !g_locked; RequestSave(); }
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
    for (auto& w : g_widgets) if (w->hwnd) { Layout(w.get()); }
    Widget_RenderAll(true);
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
            if (!g_drag.dragging) { g_wall.Release(); g_gr.ClearFormats(); TrimMemory(); }
            break;
        case T_SAVE: KillTimer(h, T_SAVE); SaveConfigNow(); break;
        case T_WALLCHECK: { std::wstring s = g_wall.Signature(); if (s != g_wallSig) ReloadWallpaper(); break; }
        case T_RELOAD_CUSTOM: KillTimer(h, T_RELOAD_CUSTOM); ReloadCustom(); break;
        case T_WALLRELOAD: KillTimer(h, T_WALLRELOAD); ReloadWallpaper(); break;
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU: ShowTrayMenu(); break;
        case NIN_SELECT: case NIN_KEYSELECT: OpenSettings(); break;
        }
        return 0;
    case WM_SHOWSETTINGS: OpenSettings(); return 0;
    case WM_APP + 9: if (g_input.wnd && GetForegroundWindow() != g_input.wnd) CloseInput(true); return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER) SetTimer(h, T_WALLRELOAD, 700, nullptr);
        else if (wp == SPI_SETWORKAREA) { for (auto& w : g_widgets) if (w->hwnd) { EnsureOnScreen(w.get()); if (!w->placed) ShowWidget(w.get()); } }
        else if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") && g_accent == L"system") Widget_RenderAll(true);
        return 0;
    case WM_DISPLAYCHANGE: SetTimer(h, T_WALLRELOAD, 1000, nullptr); return 0;
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
        } else if (wp == PBT_APMRESUMEAUTOMATIC) Widget_RenderAll(false);
        return TRUE;
    case WM_DESTROY:
        CloseSettings();
        SaveConfigNow();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ entry
typedef struct { ULONG Version, ControlMask, StateMask; } PW_THROTTLE;
static void BeLightweight() {
    // Below-normal priority + EcoQoS ("efficiency mode") + low memory priority:
    // the OS treats the widgets as the least important thing running.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    PW_THROTTLE t{ 1, 1, 1 };                     // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4 /*ProcessPowerThrottling*/, &t, sizeof(t));
    ULONG memPrio = 2;                            // MEMORY_PRIORITY_LOW
    SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)0 /*ProcessMemoryPriority*/, &memPrio, sizeof(memPrio));
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
            if (!(cmd && wcsstr(cmd, L"--startup"))) PostMessageW(other, WM_SHOWSETTINGS, 0, 0);
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BeLightweight();

    bool startup = cmd && wcsstr(cmd, L"--startup");
    bool hidden = startup || (cmd && wcsstr(cmd, L"--hidden"));
    if (startup) for (int i = 0; i < 60 && !FindWindowW(L"Progman", nullptr); i++) Sleep(500);   // wait for Explorer

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = L"PrismWidgetsMain";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    wc.lpfnWndProc = WidgetProc; wc.lpszClassName = L"PrismWidget"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
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
    if (g_firstRun) SaveConfigNow();
    bool showWelcome = g_firstRun;
    g_firstRun = false;   // custom widgets added from now on start enabled
    TrayAdd(showWelcome && hidden);
    if (!hidden) OpenSettings();
    ScheduleTick();
    SetTimer(g_main, T_WALLCHECK, 60000, nullptr);   // catches slideshow / Spotlight wallpaper changes
    ArmRelease();

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
    if (pn) UnregisterPowerSettingNotification(pn);
    WTSUnRegisterSessionNotification(g_main);
    g_gr.DropTarget(); g_gr.ClearFormats();
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
