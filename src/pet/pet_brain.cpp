// Prism Pet - settings, the pet window, placement and rendering.
#include "pet.h"
#include "pet_internal.h"
#include <cmath>

namespace pet {

// Window box around the pelvis, in local units: room for the helmet, raised arms and the feet.
constexpr float BOX_SIDE = 60, BOX_UP = 100, BOX_DOWN = STANDH + 12;
constexpr float HOME_RIGHT = 45;   // pelvis distance from the right edge of the work area

static bool g_petEnabled = true;
static std::wstring g_petTheme = L"pearl", g_petGlow = L"theme";
static float g_petSize = .85f;

static HWND g_wnd = nullptr;
static POINT g_org{};           // window top-left, screen px
static SIZE g_box{};            // window size, px
static Stage g_stage;
static Pose g_pose;

static D2D1_COLOR_F GlowColour() {
    if (g_petGlow == L"accent") return Accent();
    uint32_t c = PaletteOf(g_petTheme).glow;
    return { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1 };
}

static void Render() {
    if (!g_wnd || !g_gr.EnsureTarget()) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = g_box.cx; bi.bmiHeader.biHeight = -g_box.cy;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    RECT rc{ 0, 0, g_box.cx, g_box.cy };
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        if (SUCCEEDED(g_gr.rt->BindDC(mem, &rc))) {
            g_gr.rt->SetDpi(96, 96);   // draw in physical px; the pose carries the DPI x size scale
            g_gr.rt->BeginDraw();
            D2D1_MATRIX_3X2_F id{ 1, 0, 0, 1, 0, 0 };
            g_gr.rt->SetTransform(&id);
            D2D1_COLOR_F clear{ 0, 0, 0, 0 };
            g_gr.rt->Clear(&clear);
            DrawRobot(g_gr.rt, g_pose, g_stage.scale, PaletteOf(g_petTheme), GlowColour(), VisorExpr(g_pose.eyes, g_pose.mouth, false));
            if (g_gr.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) g_gr.DropTarget();
            GdiFlush();
        }
        SIZE sz = g_box; POINT src{ 0, 0 };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_wnd, screen, &g_org, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);   // DWM keeps its own copy of the frame
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// Home spot: bottom-right of the primary screen, standing on the taskbar's top edge
// (the bottom of the work area; with an auto-hidden taskbar, the bottom of the screen).
void Relayout() {
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    RECT wa = WorkOf(mon);
    float s = ScaleOf(mon) * g_petSize;
    int side = (int)std::ceil(BOX_SIDE * s), up = (int)std::ceil(BOX_UP * s), floorY = up + (int)std::round(STANDH * s);
    g_box = { 2 * side, floorY + (int)std::ceil((BOX_DOWN - STANDH) * s) };
    g_stage = { s, (float)floorY };
    Pose p = Stand(g_stage);
    p.x = (float)side; p.face = 1;
    g_pose = p;
    g_org = { wa.right - (int)std::round(HOME_RIGHT * s) - side, wa.bottom - floorY };
    Render();
}

static void Show(bool on) {
    if (on && !g_wnd) {
        g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"PrismPet", L"Prism Pet",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
        Relayout();
    }
    if (g_wnd) ShowWindow(g_wnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

}  // namespace pet

using namespace pet;

void PetLoadSettings(const JVal* j) {
    JVal none;
    if (!j || !j->isObj()) j = &none;
    g_petEnabled = j->boolean(L"enabled", true);
    g_petTheme = PaletteOf(j->str(L"theme", L"pearl")).id;   // unknown names fall back to Pearl
    g_petGlow = j->str(L"glow", L"theme") == L"accent" ? L"accent" : L"theme";
    g_petSize = Clamp((float)j->num(L"size", .85), .6f, 1.5f);
}

void PetSaveSettings(JVal& j) {
    j = JVal::MakeObj();
    j.set(L"enabled") = JVal::MakeBool(g_petEnabled);
    j.set(L"theme") = JVal::MakeStr(g_petTheme);
    j.set(L"glow") = JVal::MakeStr(g_petGlow);
    j.set(L"size") = JVal::MakeNum(std::round(g_petSize * 100) / 100);
}

void PetInit() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismPet";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    if (g_petEnabled) Show(true);
}

void PetShutdown() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
    ReleaseDrawCache();
}

void PetArrange(bool on) {
    if (g_petEnabled) Show(!on);
}
