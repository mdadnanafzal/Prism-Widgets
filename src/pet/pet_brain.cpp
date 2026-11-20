// Prism Pet - behaviour engine: settings, the pet window, frame loop, triggers and the state machine.
#include "pet_internal.h"
#include <cmath>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace pet {

enum class Mode : uint8_t { None, Coding, Browser, Both };   // what the pet is doing with its props
enum class Kind : uint8_t { Idle, Loop, Trans, Pacing, Tickle, Drag, Phys, Mischief, Grab, Sleep, Wake };

static PetSettings g_set;
static HWND g_wnd = nullptr;

// running scene
static Scene g_scene;
static size_t g_step = 0;
static std::function<void()> g_onDone;
static Kind g_kind = Kind::Idle;
static Mode g_mode = Mode::None;
static bool g_dragging = false, g_released = false, g_asleep = false;
static int g_grabHold = 0; static bool g_grabActive = false; static float g_grabStart = 0;
static Pose g_blendFrom, g_shown; static float g_blend = 1;   // short blend when a scene interrupts another

// frame loop
static LONGLONG g_lastQpc = 0; static POINT g_lastCur{};
static HANDLE g_timer = nullptr; static int g_timerFps = 0;
static bool g_arrange = false, g_paused = false;
static double g_renderMs = 0;
static RECT g_hit{};
static V2 g_lastDrawn{};   // pelvis in the last frame, to tell whether the pet is moving
static Surface g_surf;

// home spot: a corner distance on a named monitor, in DIP (unset = bottom-right of the primary screen)
// Home is always the bottom-right of a screen, on the taskbar edge: the main screen unless set to another.
static bool g_homeSet = false; static std::wstring g_homeMon;

// app modes
static bool g_codingOn = false, g_browserOn = false, g_leaveCode = false, g_leaveWeb = false;
static int g_lastFg = 0;   // 0 other, 1 coding tool, 2 browser
static HANDLE g_codingProc = nullptr, g_browserProc = nullptr;
static HWINEVENTHOOK g_fgHook = nullptr;

// timers
static double g_activeSec = 0;   // active (not idle or away) time since the last click on the pet
static unsigned g_round = 0;
static float g_idleSince = 0;
static bool g_mischiefDue = false;

static void Next();
static void UpdateVisibility();

// ---------------------------------------------------------------- helpers
static LONGLONG Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static double QpcFreq() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (double)f.QuadPart; }
static bool ButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}
static bool Visible() { return g_wnd && g_set.enabled && !g_arrange && !g_paused; }
static D2D1_COLOR_F GlowColour() { return g_set.glow == L"accent" ? Accent() : Hex(PaletteOf(g_set.theme).glow); }
static bool ReducedMotion() { BOOL on = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0); return !on; }

static std::wstring MonName(HMONITOR m) { MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(m, &mi); return mi.szDevice; }
static HMONITOR MonByName(const std::wstring& name) {
    struct Find { const std::wstring* name; HMONITOR found; } f{ &name, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        Find* f = (Find*)lp;
        if (MonName(m) == *f->name) { f->found = m; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.found;
}
static void UseMonitor(HMONITOR m) {
    W.work = WorkOf(m);
    W.st.scale = ScaleOf(m) * g_set.size;
}

static void DefaultLists(PetSettings& s) {
    s.codingApps = { L"Code.exe", L"WindowsTerminal.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"devenv.exe",
                     L"idea64.exe", L"pycharm64.exe", L"Postman.exe", L"pgAdmin4.exe", L"dbeaver.exe" };
    s.browserApps = { L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe" };
}

// ---------------------------------------------------------------- home and placement
static HMONITOR HomeMonitor() {
    HMONITOR m = g_homeSet ? MonByName(g_homeMon) : nullptr;   // an unplugged screen falls back to the main one
    return m ? m : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
}
static void HomePlace(HMONITOR& mon, float& x, float& floorY) {
    mon = HomeMonitor();
    RECT wa = WorkOf(mon);
    x = wa.right - 45 * ScaleOf(mon) * g_set.size;   // bottom-right, standing on the taskbar's top edge
    floorY = (float)wa.bottom;
}

static void PlaceAtHome() {
    HMONITOR mon; float x, floorY;
    HomePlace(mon, x, floorY);
    UseMonitor(mon);
    W.st.floor = floorY;
    C.homeX = x;
    C.faceIn = x > (W.work.left + W.work.right) / 2.f ? -1.f : 1.f;
    PutAwayProps();
    W.parts.clear();
    W.hanging = false;
    g_mode = Mode::None; g_dragging = false; g_asleep = false;
    Pose p = Stand(W.st);
    p.x = x; p.face = C.faceIn;
    W.p = p; g_shown = p; g_blend = 1;
}

static HMONITOR PetMonitor() { return MonitorFromPoint(POINT{ (LONG)W.p.x, (LONG)(W.p.y - U(20)) }, MONITOR_DEFAULTTONEAREST); }
static void UpdateHomeCtx() {
    HMONITOR mon; float x, floorY;
    HomePlace(mon, x, floorY);
    RECT wa = WorkOf(mon);
    C.homeX = x;
    C.faceIn = x > (wa.left + wa.right) / 2.f ? -1.f : 1.f;
}
bool OnHomeScreen() { return PetMonitor() == HomeMonitor(); }
void AdoptThisScreen() {
    g_homeSet = true; g_homeMon = MonName(PetMonitor());
    UpdateHomeCtx();
    RequestSave();
}
void HomeAfterPortal(float& x, float& floorY) {
    HMONITOR mon;
    HomePlace(mon, x, floorY);
    UpdateHomeCtx();
    UseMonitor(mon);
}

// ---------------------------------------------------------------- rendering
void Surface::Free() {
    if (rt) ReleaseDrawCache();   // cached head bitmaps belong to this target
    SafeRelease(br); SafeRelease(rt);
    if (dib) { SelectObject(mem, old); DeleteObject(dib); dib = nullptr; }
    if (mem) { DeleteDC(mem); mem = nullptr; }
    w = h = 0; placed = false;
}
void Present(Surface& s, HWND hwnd, RECT box, const std::function<void(ID2D1RenderTarget*)>& draw, const POINT* anchor) {
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (!hwnd || bw <= 0 || bh <= 0) return;
    if (anchor) {   // centred on the anchor: size for the farther side of the box
        bw = 2 * std::max(anchor->x - box.left, box.right - anchor->x);
        bh = 2 * std::max(anchor->y - box.top, box.bottom - anchor->y);
    }
    s.oversized = s.w > bw + 160 || s.h > bh + 160 ? s.oversized + 1 : 0;
    if (s.oversized > 30) { s.Free(); s.oversized = 0; }   // shrink back after a while (e.g. once the bed is gone)
    if (bw > s.w || bh > s.h || !s.rt) {   // grow in 64 px steps with some slack; frames reuse it
        int w = std::max(s.w, (bw + 32 + 63) / 64 * 64), h = std::max(s.h, (bh + 32 + 63) / 64 * 64);
        s.Free();
        HDC screen = GetDC(nullptr); s.mem = CreateCompatibleDC(screen); ReleaseDC(nullptr, screen);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        s.dib = CreateDIBSection(s.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!s.dib) { s.Free(); return; }
        s.old = SelectObject(s.mem, s.dib); s.w = w; s.h = h;
        D2D1_RENDER_TARGET_PROPERTIES p{};
        p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;   // CPU raster, like the widgets
        p.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
        RECT all{ 0, 0, w, h };
        D2D1_COLOR_F white{ 1, 1, 1, 1 };
        if (FAILED(g_gr.d2d->CreateDCRenderTarget(&p, &s.rt)) || FAILED(s.rt->BindDC(s.mem, &all)) ||
            FAILED(s.rt->CreateSolidColorBrush(&white, nullptr, &s.br))) { s.Free(); return; }
        s.rt->SetDpi(96, 96);   // physical px; the pet carries the DPI x size scale itself
        s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    // Moving a layered window costs more than drawing the pet, so the window stays put while the
    // content fits inside it and only moves (re-centred) when the content reaches an edge.
    if (anchor) { s.org = { anchor->x - s.w / 2, anchor->y - s.h / 2 }; s.placed = true; }
    else if (!s.placed || box.left < s.org.x || box.top < s.org.y || box.right > s.org.x + s.w || box.bottom > s.org.y + s.h) {
        s.org = { box.left - (s.w - bw) / 2, box.top - (s.h - bh) / 2 }; s.placed = true;
    }
    {
        ID2D1SolidColorBrush* shared = g_gr.br;
        g_gr.br = s.br;   // the drawing code paints with g_gr.br
        s.rt->BeginDraw();
        D2D1_MATRIX_3X2_F id{ 1, 0, 0, 1, (float)-s.org.x, (float)-s.org.y };
        s.rt->SetTransform(&id);
        D2D1_COLOR_F clear{ 0, 0, 0, 0 };
        s.rt->Clear(&clear);
        draw(s.rt);
        HRESULT hr = s.rt->EndDraw();
        g_gr.br = shared;
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { s.Free(); return; }
        GdiFlush();
        POINT src{ 0, 0 }; SIZE full{ s.w, s.h };   // the window keeps the surface's size too
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, nullptr, &s.org, &full, s.mem, &src, 0, &bf, ULW_ALPHA);
    }
}

static void Render() {
    if (!g_wnd) return;
    LONGLONG t0 = Qpc();
    Pose p = g_blend < 1 ? LerpPose(g_blendFrom, W.p, Eased(Ease::InOut, g_blend)) : W.p;
    if (W.idle && !C.reduced) {   // breathing, small head movements
        float t = W.time;
        p.t += .018f * std::sin(t * 2.1f) * (1 - p.v); p.y += .7f * K() * std::sin(t * 2.1f + 1);
        p.h += .07f * std::sin(t * .55f) + .03f * std::sin(t * 1.7f);
        p.aF1 += .03f * std::sin(t * 2.1f); p.aB1 -= .03f * std::sin(t * 2.1f);
    }
    g_shown = p;
    bool blink = p.eyes == Eyes::Open && std::fmod(W.time + W.seed, 3.6f) < .13f;
    Expr e = VisorExpr(p.eyes, p.mouth, blink);

    // Window box: the body's joints plus room for the helmet, glow and props; the bed; floating text and dust.
    Fk f = Kinematics(p);
    V2 pts[] = { f.head, f.sh, f.hF, f.hB, f.eF, f.eB, f.fF, f.fB, f.kF, f.kB, { 0, 0 } };
    float l = 1e9f, r = -1e9f, t = 1e9f, b = -1e9f, k = K(), M = U(32);
    for (V2 v : pts) { V2 w = ToWorld(p, v, W.st.scale); l = std::min(l, w.x); r = std::max(r, w.x); t = std::min(t, w.y); b = std::max(b, w.y); }
    float x0 = l, y0 = t, x1 = r, y1 = b;   // body box, for the cursor hook
    l -= M; r += M; t -= M; b += M;
    if (W.laptop) { l = std::min(l, p.x - U(45)); r = std::max(r, p.x + U(45)); }
    for (const Particle& q : W.parts) {
        if (std::fabs(q.x - p.x) > U(160) || std::fabs(q.y - p.y) > U(200)) continue;   // stray particles never widen the window
        l = std::min(l, q.x - 30 * k); r = std::max(r, q.x + 90 * k); t = std::min(t, q.y - 40 * k); b = std::max(b, q.y + 20 * k);
    }
    if (W.portal) { l = std::min(l, W.portalX - U(36)); r = std::max(r, W.portalX + U(36)); t = std::min(t, W.portalFloor - U(90)); b = std::max(b, W.portalFloor + U(4)); }
    if (W.bed) { l = std::min(l, W.bd.x - 4); r = std::max(r, W.bd.x + U(127)); t = std::min(t, W.bd.floor - U(30)); b = std::max(b, W.bd.floor + U(4)); }
    RECT box{ (LONG)std::floor(l), (LONG)std::floor(t), (LONG)std::ceil(r), (LONG)std::ceil(b) };
    // Moving (walking, climbing, falling, dragged, a portal): the window follows the pet every frame.
    // Jumping it in steps showed as a trail. Sitting still, the window stays put, which is cheaper.
    bool moving = std::fabs(p.x - g_lastDrawn.x) + std::fabs(p.y - g_lastDrawn.y) > .5f || W.portal;
    g_lastDrawn = { p.x, p.y };
    bool fast = moving || g_dragging || W.hanging || g_kind == Kind::Phys;
    POINT body{ (LONG)std::lround(p.x), (LONG)std::lround(p.y - U(22)) };   // middle of the body: moves smoothly
    Present(g_surf, g_wnd, box, [&](ID2D1RenderTarget* rt) { DrawPet(rt, W, p, e); }, fast ? &body : nullptr);

    // Body box for the cursor hook: joints plus the limbs' thickness, the helmet a bit more.
    float pad = U(10), head = U(28);
    g_hit = { (LONG)(x0 - pad), (LONG)(std::min(y0, ToWorld(p, f.head, W.st.scale).y - head)), (LONG)(x1 + pad), (LONG)(y1 + pad) };
    HookSetHitBox(g_hit);

    double ms = (Qpc() - t0) * 1000.0 / QpcFreq();
    g_renderMs = g_renderMs ? g_renderMs * .9 + ms * .1 : ms;
}

// ---------------------------------------------------------------- frame rate
static int TargetFps() {
    if (!Visible()) return 0;
    int f = W.fps;
    if (g_dragging || W.hanging || FxActive()) f = std::max(f, 30);
    if (C.reduced && !g_dragging && !W.hanging) f = std::min(f, 2);   // still poses
    return std::clamp(f, 0, 30);   // budget rule: movement at 30 fps, never 60
}
void SetFrameRate(int fps) {
    if (fps == g_timerFps) return;
    g_timerFps = fps;
    if (g_timer) {
        if (!fps) { CancelWaitableTimer(g_timer); return; }
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(10000000LL / fps);
        SetWaitableTimer(g_timer, &due, 1000 / fps, nullptr, nullptr, FALSE);
    } else if (g_wnd) {   // older Windows: no high-resolution waitable timer
        if (!fps) KillTimer(g_wnd, T_PET_FRAME); else SetTimer(g_wnd, T_PET_FRAME, 1000 / fps, nullptr);
    }
}

// ---------------------------------------------------------------- scenes and the state machine
static void Start(Scene s, Kind k, std::function<void()> onDone = nullptr, bool blend = false) {
    if (blend) { g_blendFrom = g_shown; g_blend = 0; }
    g_scene = std::move(s); g_step = 0; g_kind = k; g_onDone = std::move(onDone);
    W.idle = false;
}

static bool PacingDue() { return g_set.pacing && g_kind == Kind::Idle && W.time - g_idleSince >= g_set.pacingMinutes * 60.f; }
static Mode Desired() {
    if (g_mischiefDue) return Mode::None;   // mischief outranks the app modes: props go away first
    if (g_codingOn && g_browserOn) {
        if (g_lastFg == 2) return Mode::Both;
        if (g_lastFg == 1) return Mode::Coding;
        return g_mode == Mode::Both || g_mode == Mode::Coding ? g_mode : Mode::Coding;
    }
    if (g_codingOn) return Mode::Coding;
    if (g_browserOn) return Mode::Browser;
    return Mode::None;
}
static bool WantsChange() {
    if (g_asleep || Desired() != g_mode) return true;
    return g_mode == Mode::None && (g_mischiefDue || PacingDue());
}

static void HookRefresh();
static int GrabHold() {
    if (g_grabHold) return g_grabHold;
    if (!g_grabActive) {   // the moment it reaches the cursor
        g_grabActive = true; g_grabStart = W.time;
        if (ButtonDown() || g_paused || SettingsOpen()) g_grabHold = 1;   // safety: never grab then; it just drops
        HookRefresh();
        return g_grabHold;
    }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || W.time - g_grabStart > 3.5f || g_paused) { g_grabHold = 1; HookRefresh(); }
    return g_grabHold;
}
static bool GrabPossible() {
    POINT c; GetCursorPos(&c);
    HMONITOR pm = MonitorFromPoint(POINT{ (LONG)W.p.x, (LONG)W.p.y }, MONITOR_DEFAULTTONEAREST);
    if (MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST) != pm || ButtonDown()) return false;
    return c.y > W.st.floor - U(230) && c.y < W.st.floor - U(60) && c.x > W.work.left + U(40) && c.x < W.work.right - U(40);
}
static void StartMischief() {
    g_activeSec = 0; g_mischiefDue = false;
    bool grab = (g_round++ % 2 == 1) && GrabPossible();   // every 2nd round: the cursor grab
    if (!grab) { Start(SceneFlowerPot(), Kind::Mischief); return; }
    g_grabHold = 0; g_grabActive = false;
    Start(SceneGrab([] { return GrabHold(); }), Kind::Grab, [] { g_grabActive = false; HookRefresh(); Next(); });
}

static void Transition(Mode d) {
    switch (g_mode) {
    case Mode::None:
        if (d == Mode::Browser) Start(SceneBrowserEnter(), Kind::Trans, [] { g_mode = Mode::Browser; Next(); });
        else Start(SceneCodingEnter(), Kind::Trans, [] { g_mode = Mode::Coding; Next(); });
        break;
    case Mode::Coding:
        if (d == Mode::Both) Start(SceneCodingToBoth(), Kind::Trans, [] { g_mode = Mode::Both; Next(); });
        else Start(SceneCodingExit(), Kind::Trans, [] { g_mode = Mode::None; Next(); });
        break;
    case Mode::Browser: Start(SceneBrowserExit(), Kind::Trans, [] { g_mode = Mode::None; Next(); }); break;
    case Mode::Both: Start(SceneBothToCoding(), Kind::Trans, [] { g_mode = Mode::Coding; Next(); }); break;
    }
}

static void Next() {
    if (g_asleep) { Start(SceneSleep([] { return !g_asleep; }), Kind::Sleep); return; }
    Mode d = Desired();
    if (d != g_mode) { Transition(d); return; }
    if (g_mode == Mode::Coding) { Start(SceneTyping(WantsChange), Kind::Loop); return; }
    if (g_mode != Mode::None) { Start(SceneScrolling(WantsChange), Kind::Loop); return; }
    if (g_mischiefDue) { StartMischief(); return; }
    if (PacingDue()) { Start(ScenePacing(g_set.pacingRange), Kind::Pacing, [] { g_idleSince = W.time; Next(); }); return; }
    if (g_kind != Kind::Idle) g_idleSince = W.time;
    Start(SceneIdle(WantsChange), Kind::Idle);
}

static void Tick(float h) {
    if (!RunStep(g_scene, g_step, h)) {
        auto done = std::move(g_onDone); g_onDone = nullptr;
        if (done) done(); else Next();
    }
    TickWorld(h);
    FxTick(h);
}

static void Frame() {
    LONGLONG now = Qpc();
    float dt = std::clamp((float)((now - g_lastQpc) / QpcFreq()), 0.f, .2f);
    g_lastQpc = now;
    if (dt <= 0) dt = 1 / 120.f;
    POINT cp; GetCursorPos(&cp);
    POINT at = g_dragging || W.hanging ? cp : POINT{ (LONG)W.p.x, (LONG)(W.p.y - U(20)) };
    UseMonitor(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST));
    W.pal = &PaletteOf(g_set.theme); W.glow = GlowColour();
    C.walkSpeed = g_set.walkSpeed; C.wayDown = g_set.wayDown; C.portalHome = g_set.portalHome;
    int n = std::max(1, (int)std::ceil(dt * 120));   // fixed 1/120 s simulation steps
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        float u = (i + 1) / (float)n;
        W.cur.x = Lerp((float)g_lastCur.x, (float)cp.x, u); W.cur.y = Lerp((float)g_lastCur.y, (float)cp.y, u);
        Tick(h);
    }
    g_lastCur = cp;
    g_blend = std::min(1.f, g_blend + dt / .15f);
    Render();
    if (FxActive()) FxPresent();
    SetFrameRate(TargetFps());
}

// ---------------------------------------------------------------- triggers
static void CloseProc(HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } }
static bool InList(const std::vector<std::wstring>& list, const wchar_t* name) {
    for (auto& s : list) if (!_wcsicmp(s.c_str(), name)) return true;
    return false;
}
// 1 = coding tool, 2 = browser, 0 = anything else (including processes we may not open, e.g. elevated ones)
static int Classify(HWND hwnd, HANDLE* proc) {
    DWORD pid = 0;
    if (!hwnd || !GetWindowThreadProcessId(hwnd, &pid) || !pid) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h) return 0;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH; int kind = 0;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* f = wcsrchr(path, L'\\'); f = f ? f + 1 : path;
        kind = InList(g_set.codingApps, f) ? 1 : InList(g_set.browserApps, f) ? 2 : 0;
    }
    if (kind) *proc = h; else CloseHandle(h);
    return kind;
}
static void OnForeground(HWND hwnd) {
    HANDLE proc = nullptr;
    int kind = Classify(hwnd, &proc);
    if (kind == 1) { g_codingOn = true; KillTimer(g_wnd, T_PET_LEAVE_CODE); g_leaveCode = false; CloseProc(g_codingProc); g_codingProc = proc; }
    else if (g_codingOn && !g_leaveCode) { SetTimer(g_wnd, T_PET_LEAVE_CODE, 60000, nullptr); g_leaveCode = true; }   // left the tool: 1 min
    if (kind == 2) { g_browserOn = true; KillTimer(g_wnd, T_PET_LEAVE_WEB); g_leaveWeb = false; CloseProc(g_browserProc); g_browserProc = proc; }
    else if (g_browserOn && !g_leaveWeb) { SetTimer(g_wnd, T_PET_LEAVE_WEB, 60000, nullptr); g_leaveWeb = true; }
    g_lastFg = kind;
}
static void CALLBACK FgProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject == OBJID_WINDOW) OnForeground(hwnd);
}
static void EndCoding() { g_codingOn = false; g_leaveCode = false; if (g_wnd) KillTimer(g_wnd, T_PET_LEAVE_CODE); CloseProc(g_codingProc); }
static void EndBrowser() { g_browserOn = false; g_leaveWeb = false; if (g_wnd) KillTimer(g_wnd, T_PET_LEAVE_WEB); CloseProc(g_browserProc); }

// Collision hook: only while the cursor is within about 150 px of the pet (budget rule 2).
static void Proximity() {
    if (!g_set.collision || !Visible() || g_dragging || W.hanging) return;
    POINT c; GetCursorPos(&c);
    float dx = std::max({ (float)g_hit.left - c.x, 0.f, (float)c.x - g_hit.right }), dy = std::max({ (float)g_hit.top - c.y, 0.f, (float)c.y - g_hit.bottom });
    float d = std::hypot(dx, dy);
    if (d < 150) HookSet(HookMode::Collide);
    else if (d > 200) HookSet(HookMode::Off);
}
static void HookRefresh() {
    bool grabbing = g_kind == Kind::Grab && g_grabActive && !g_grabHold && W.hanging;
    if (grabbing) { HookSet(HookMode::Grab); return; }
    HookSet(HookMode::Off);
    Proximity();
}

// Once a second: pause/fullscreen check and the mischief clock (active time only).
static void Tick1s() {
    bool p = Paused();
    if (p != g_paused) { g_paused = p; UpdateVisibility(); }
    LASTINPUTINFO li{ sizeof(li), 0 }; GetLastInputInfo(&li);
    bool active = GetTickCount() - li.dwTime < 60000;
    if (!p && Visible() && active && g_set.mischiefMinutes > 0) g_activeSec += 1;
    g_mischiefDue = g_set.mischiefMinutes > 0 && g_activeSec >= g_set.mischiefMinutes * 60.0 && !p && !SettingsOpen() && !ButtonDown() && !g_dragging;
}

static void UpdateVisibility() {
    if (!g_wnd) return;
    bool v = Visible();
    ShowWindow(g_wnd, v ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (v) { g_lastQpc = Qpc(); GetCursorPos(&g_lastCur); }
    if (!v) { g_surf.Free(); if (FxActive()) FxShutdown(); }
    if (g_set.collision && v) SetTimer(g_wnd, T_PET_PROXIMITY, 33, nullptr); else KillTimer(g_wnd, T_PET_PROXIMITY);
    if (!v) HookSet(HookMode::Off); else HookRefresh();
    SetFrameRate(TargetFps());
}

static void Enable(bool atLogin) {
    if (g_wnd) return;
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"PrismPet", L"Prism Pet",
                            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return;
    PlaceAtHome();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FgProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    OnForeground(GetForegroundWindow());
    SetTimer(g_wnd, T_PET_TICK, 1000, nullptr);
    g_kind = Kind::Trans;
    if (atLogin) { SnapAsleep(); Start(SceneWakeUp(), Kind::Wake); }
    else Next();
    g_paused = Paused();
    g_lastQpc = Qpc(); GetCursorPos(&g_lastCur); W.cur.x = W.cur.px = (float)g_lastCur.x; W.cur.y = W.cur.py = (float)g_lastCur.y;
    W.pal = &PaletteOf(g_set.theme); W.glow = GlowColour();
    Render();
    UpdateVisibility();
}

static void Disable() {
    if (!g_wnd) return;
    HookSet(HookMode::Off);
    if (g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
    SetFrameRate(0);
    EndCoding(); EndBrowser();
    FxShutdown();
    DestroyWindow(g_wnd);
    g_surf.Free();
    g_wnd = nullptr; g_timerFps = 0;
    g_scene.clear(); g_onDone = nullptr;
    W.parts.clear();
}

// ---------------------------------------------------------------- called by the window and the hook
void Relayout() {
    if (!g_wnd) return;
    bool asleep = g_asleep;
    PlaceAtHome();
    if (asleep) { SnapAsleep(); g_asleep = true; }
    g_kind = Kind::Trans;
    Next();
    if (Visible()) Render();
}
void OnTimer(UINT id) {
    switch (id) {
    case T_PET_FRAME: Frame(); break;
    case T_PET_TICK: Tick1s(); break;
    case T_PET_PROXIMITY: Proximity(); break;
    case T_PET_LEAVE_CODE: EndCoding(); break;
    case T_PET_LEAVE_WEB: EndBrowser(); break;
    }
}
void OnSettingChange() { C.reduced = ReducedMotion(); }

void BeginDrag() {
    Interactive();
    g_dragging = true; g_released = false; g_asleep = false;
    PutAwayProps(); g_mode = Mode::None;
    g_grabActive = false; g_grabHold = 0;
    POINT c; GetCursorPos(&c);
    W.cur = Cursor{}; W.cur.x = W.cur.px = (float)c.x; W.cur.y = W.cur.py = (float)c.y; g_lastCur = c;
    Start(SceneDragHang([] { return g_released; }), Kind::Drag, [] { g_dragging = false; Next(); }, true);
    HookRefresh();
    SetFrameRate(TargetFps());
}
void EndDrag() { g_released = true; Interactive(); }
bool Dragging() { return g_dragging; }

void OnClick() {
    Interactive();
    g_activeSec = 0; g_mischiefDue = false;   // the no-click clock restarts
    if (g_kind == Kind::Drag || g_kind == Kind::Phys || g_kind == Kind::Grab || g_kind == Kind::Wake || W.hanging) return;
    if (g_asleep) { g_asleep = false; Start(SceneWakeUp(), Kind::Wake); return; }
    PutAwayProps(); g_mode = Mode::None;
    Start(SceneTickle(), Kind::Tickle, nullptr, true);
}

void OnYank(float vx, float vy) {
    if (g_kind != Kind::Grab || !W.hanging || g_grabHold) return;
    float sp = std::hypot(vx, vy), m = std::min(1.f, 1150 * K() / std::max(1.f, sp));
    W.phys = Phys{ vx * m, vy * m - 150 * K(), vx >= 0 ? 13.f : -13.f };
    g_grabHold = 2;
    HookRefresh();
}

void OnContact(float vx, float vy) {
    if (!g_set.collision || g_dragging || W.hanging || g_asleep) return;
    if (g_kind == Kind::Drag || g_kind == Kind::Phys || g_kind == Kind::Grab || g_kind == Kind::Wake || g_kind == Kind::Sleep) return;
    float sp = std::hypot(vx, vy), dir = vx >= 0 ? 1.f : -1.f;
    if (sp < 150) return;   // a gentle approach (to click it) is not a hit
    PutAwayProps(); g_mode = Mode::None;
    if (sp < 800) { Start(SceneKnockedOver(dir), Kind::Phys, nullptr, true); return; }
    W.phys = Phys{ dir * std::min(std::fabs(vx) * .5f, 1150 * K()), -560 * K(), 12 * dir };
    Start(SceneThrown(), Kind::Phys, nullptr, true);
}

RECT PetHitBox() { return g_hit; }
HWND PetWindow() { return g_wnd; }

}  // namespace pet

// ---------------------------------------------------------------- public API
using namespace pet;

PetSettings& PetConfig() { return g_set; }

void PetLoadSettings(const JVal* j) {
    JVal none;
    if (!j || !j->isObj()) j = &none;
    PetSettings s;
    DefaultLists(s);
    s.enabled = j->boolean(L"enabled", true);
    s.theme = PaletteOf(j->str(L"theme", L"pearl")).id;   // unknown names fall back to Pearl
    s.glow = j->str(L"glow", L"theme") == L"accent" ? L"accent" : L"theme";
    s.size = Clamp((float)j->num(L"size", .85), .6f, 1.5f);
    s.mischiefMinutes = Clamp((float)j->num(L"mischiefMinutes", 30), .5f, 24 * 60.f);   // at least 30 s
    if (const JVal* pc = j->get(L"pacing"); pc && pc->isObj()) {
        s.pacing = pc->boolean(L"enabled", true);
        s.pacingMinutes = Clamp((float)pc->num(L"idleMinutes", 5), .25f, 120.f);   // at least 15 s
        s.pacingRange = Clamp((float)pc->num(L"range", 1 / 3.0), .1f, 1.f);
    }
    s.collision = j->boolean(L"collision", false);
    s.walkSpeed = Clamp((float)j->num(L"walkSpeed", 1), .5f, 2.f);
    std::wstring wd = j->str(L"wayDown", L"climb");
    s.wayDown = wd == L"jump" ? 1 : wd == L"screen" ? 2 : wd == L"ledges" ? 3 : wd == L"portal" ? 4 : 0;
    s.portalHome = j->str(L"otherScreen", L"portal") != L"adopt";
    auto list = [&](const wchar_t* key, std::vector<std::wstring>& out) {
        const JVal* a = j->get(key);
        if (!a || !a->isArr()) return;
        out.clear();
        for (auto& v : a->a) if (v.type == JVal::Str && !v.s.empty()) out.push_back(v.s);
    };
    list(L"codingApps", s.codingApps);
    list(L"browserApps", s.browserApps);
    g_set = s;
    g_homeSet = false;
    if (const JVal* h = j->get(L"home"); h && h->isObj() && !h->str(L"monitor", L"").empty()) { g_homeSet = true; g_homeMon = h->str(L"monitor", L""); }
}

void PetSaveSettings(JVal& j) {
    j = JVal::MakeObj();
    j.set(L"enabled") = JVal::MakeBool(g_set.enabled);
    j.set(L"theme") = JVal::MakeStr(g_set.theme);
    j.set(L"glow") = JVal::MakeStr(g_set.glow);
    j.set(L"size") = JVal::MakeNum(std::round(g_set.size * 100) / 100);
    j.set(L"mischiefMinutes") = JVal::MakeNum(std::round(g_set.mischiefMinutes * 1000) / 1000);
    JVal& pc = j.set(L"pacing"); pc = JVal::MakeObj();
    pc.set(L"enabled") = JVal::MakeBool(g_set.pacing);
    pc.set(L"idleMinutes") = JVal::MakeNum(std::round(g_set.pacingMinutes * 1000) / 1000);
    pc.set(L"range") = JVal::MakeNum(std::round(g_set.pacingRange * 1000) / 1000);
    j.set(L"collision") = JVal::MakeBool(g_set.collision);
    auto list = [&](const wchar_t* key, const std::vector<std::wstring>& in) {
        JVal& a = j.set(key); a = JVal::MakeArr();
        for (auto& s : in) a.a.push_back(JVal::MakeStr(s));
    };
    list(L"codingApps", g_set.codingApps);
    list(L"browserApps", g_set.browserApps);
    j.set(L"walkSpeed") = JVal::MakeNum(std::round(g_set.walkSpeed * 100) / 100);
    const wchar_t* ways[] = { L"climb", L"jump", L"screen", L"ledges", L"portal" };
    j.set(L"wayDown") = JVal::MakeStr(ways[Clamp(g_set.wayDown, 0, 4)]);
    j.set(L"otherScreen") = JVal::MakeStr(g_set.portalHome ? L"portal" : L"adopt");
    if (g_homeSet) { JVal& h = j.set(L"home"); h = JVal::MakeObj(); h.set(L"monitor") = JVal::MakeStr(g_homeMon); }
}

void PetInit(bool atLogin) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismPet";
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"PrismPetFx"; wc.hCursor = nullptr;
    RegisterClassExW(&wc);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    C.reduced = ReducedMotion();
    W.seed = (GetTickCount() % 1000) / 100.f;
    if (g_set.enabled) Enable(atLogin);
}

void PetShutdown() {
    Disable();
    if (g_timer) { CloseHandle(g_timer); g_timer = nullptr; }
    ReleaseDrawCache();
}

void PetApplySettings() {
    static float lastSize = g_set.size;
    if (g_set.enabled && !g_wnd) Enable(false);
    else if (!g_set.enabled && g_wnd) Disable();
    else if (g_wnd) {
        if (lastSize != g_set.size) Relayout();
        UpdateVisibility();
        if (Visible()) { W.pal = &PaletteOf(g_set.theme); W.glow = GlowColour(); Render(); }
    }
    lastSize = g_set.size;
    RequestSave();
}

void PetResetPosition() {
    g_homeSet = false;
    Relayout();   // back home on the main screen
    RequestSave();
}

void PetSetHomeHere() {
    if (!g_wnd) return;
    AdoptThisScreen();
}

std::wstring PetHomeName() {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(HomeMonitor(), &mi);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) return L"Main screen";
    std::wstring n = mi.szDevice; size_t p = n.rfind(L'\\');
    return L"Screen " + (p == std::wstring::npos ? n : n.substr(p + 1));
}

void PetArrange(bool on) {
    g_arrange = on;
    UpdateVisibility();
}

void PetOnSession(PetSession e) {
    if (!g_wnd) return;
    switch (e) {
    case PetSession::Lock: case PetSession::Suspend: case PetSession::EndSession:
        if (g_asleep || g_dragging) return;   // snap to bed silently: the screen is already hidden
        HookSet(HookMode::Off);
        FxShutdown();
        PlaceAtHome();   // the bed is at home
        SnapAsleep();
        g_asleep = true; g_mode = Mode::None;
        Start(SceneSleep([] { return !g_asleep; }), Kind::Sleep);
        break;
    case PetSession::Unlock: case PetSession::Resume:
        if (!g_asleep) return;
        g_asleep = false;
        Start(SceneWakeUp(), Kind::Wake);
        break;
    }
}

int PetWaitHandles(HANDLE* out, int max) {
    int n = 0;
    if (g_timer && g_timerFps && n < max) out[n++] = g_timer;
    if (g_codingProc && n < max) out[n++] = g_codingProc;
    if (g_browserProc && n < max) out[n++] = g_browserProc;
    return n;
}

void PetOnHandle(HANDLE h) {
    if (h == g_timer) Frame();
    else if (h == g_codingProc) EndCoding();     // the tool was closed: the mode ends now
    else if (h == g_browserProc) EndBrowser();
}

void PetDrawPreview(ID2D1RenderTarget* rt, float cx, float floorY, float scale) {
    Stage st{ scale, floorY };
    Pose p = Stand(st);
    p.x = cx;
    DrawRobot(rt, p, scale, PaletteOf(g_set.theme), GlowColour(), Expr::Open);
}

PetStats PetGetStats() { return PetStats{ g_renderMs, g_timerFps, Visible(), HookInstalled() }; }
