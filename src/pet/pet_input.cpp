// Prism Pet - the pet window's input (click, drag, menu) and the mouse-hook thread (cursor grab and collision).
#include "pet_internal.h"
#include <atomic>
#include <cmath>

namespace pet {

// ---------------------------------------------------------------- pet window
static bool g_pressed = false;
static POINT g_downAt{};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never takes focus
    case WM_WINDOWPOSCHANGING: {   // desktop only: stay under every window, like the widgets
        auto* p = (WINDOWPOS*)lp;
        if (KeepAtBottom() && !(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM;
        break;
    }
    case WM_LBUTTONDOWN:
        g_pressed = true; GetCursorPos(&g_downAt); SetCapture(h); Interactive();
        return 0;
    case WM_MOUSEMOVE:
        if (g_pressed && !Dragging()) {
            POINT c; GetCursorPos(&c);
            if (std::abs(c.x - g_downAt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(c.y - g_downAt.y) > GetSystemMetrics(SM_CYDRAG)) BeginDrag();
        }
        return 0;
    case WM_LBUTTONUP: {
        bool was = g_pressed; g_pressed = false;
        if (GetCapture() == h) ReleaseCapture();
        if (Dragging()) EndDrag(); else if (was) OnClick();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h) { g_pressed = false; if (Dragging()) EndDrag(); }
        return 0;
    case WM_RBUTTONUP: {
        HMENU mn = CreatePopupMenu();
        AppendMenuW(mn, MF_STRING, 1, L"Pet settings…");
        AppendMenuW(mn, MF_STRING, 2, L"Hide pet");
        POINT c; GetCursorPos(&c);
        SetForegroundWindow(g_main);
        int id = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, c.x, c.y, 0, g_main, nullptr);
        DestroyMenu(mn);
        if (id == 1) OpenPetSettings();
        else if (id == 2) { PetConfig().enabled = false; PetApplySettings(); SettingsRefresh(false); }
        return 0;
    }
    case WM_TIMER: OnTimer((UINT)wp); return 0;
    case WM_PET_YANK: OnYank((float)(int)wp, (float)(int)lp); return 0;
    case WM_PET_CONTACT: OnContact((float)(int)wp, (float)(int)lp); return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: Relayout(); return 0;
    case WM_SETTINGCHANGE:   // taskbar moved, resized or set to auto-hide; animation effects switched
        if (wp == SPI_SETWORKAREA) Relayout();
        else if (wp == SPI_SETCLIENTAREAANIMATION) OnSettingChange();
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------- mouse-hook thread
// WH_MOUSE_LL runs on its own thread, so a slow redraw on the UI thread never stalls the system's mouse.
// It exists only while needed: during a grab, or while the cursor is near the pet with collision on.
static std::atomic<int> s_mode{ 0 };
static std::atomic<long> s_box[4];
static std::atomic<bool> s_installed{ false };
static HANDLE s_thread = nullptr, s_ready = nullptr;
static DWORD s_tid = 0;

struct Move { DWORD t; int dx, dy; };
static Move s_moves[32]; static int s_head = 0;   // ring of intended (pre-slowdown) moves
static float s_fx = 0, s_fy = 0;                   // fractional remainder of slowed moves
static DWORD s_slowUntil = 0; static bool s_inside = false;

static void Sum(DWORD now, DWORD span, int& dx, int& dy) {
    dx = dy = 0;
    for (const Move& m : s_moves) if (m.t && now - m.t <= span) { dx += m.dx; dy += m.dy; }
}
static bool InBox(POINT p) { return p.x >= s_box[0] && p.x < s_box[2] && p.y >= s_box[1] && p.y < s_box[3]; }
// Swallow the move and apply only `factor` of it ourselves.
static LRESULT Slow(POINT cur, int dx, int dy, float factor) {
    s_fx += dx * factor; s_fy += dy * factor;
    int ix = (int)s_fx, iy = (int)s_fy;
    s_fx -= ix; s_fy -= iy;
    SetCursorPos(cur.x + ix, cur.y + iy);
    return 1;
}

static LRESULT CALLBACK LowLevelMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        int mode = s_mode;
        if (!(ms->flags & LLMHF_INJECTED) && mode) {
            POINT cur; GetCursorPos(&cur);
            int dx = ms->pt.x - cur.x, dy = ms->pt.y - cur.y;
            s_moves[s_head] = Move{ ms->time ? ms->time : 1, dx, dy }; s_head = (s_head + 1) % 32;
            int vx, vy; Sum(ms->time, 100, vx, vy);   // px per 100 ms, so x10 = px/s
            HWND pet = PetWindow();
            if (mode == (int)HookMode::Grab) {
                int tx, ty; Sum(ms->time, 150, tx, ty);
                if (std::hypot((float)tx, (float)ty) > 260) {   // yanked hard: let go and fling
                    s_mode = (int)HookMode::Off;
                    PostMessageW(pet, WM_PET_YANK, (WPARAM)(vx * 10), (LPARAM)(vy * 10));
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                return Slow(cur, dx, dy, .25f);   // heavy: it hangs on the cursor
            }
            if (mode == (int)HookMode::Collide) {
                bool in = InBox(ms->pt);
                if (in && !s_inside) {   // entering the pet's body
                    PostMessageW(pet, WM_PET_CONTACT, (WPARAM)(vx * 10), (LPARAM)(vy * 10));
                    float sp = std::hypot((float)vx, (float)vy) * 10;
                    if (sp >= 150 && sp < 800) s_slowUntil = ms->time + 550;
                }
                s_inside = in;
                if ((int)(ms->time - s_slowUntil) < 0) return Slow(cur, dx, dy, .4f);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the message queue
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, GetModuleHandleW(nullptr), 0);
    s_installed = hook != nullptr;
    SetEvent(s_ready);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    if (hook) UnhookWindowsHookEx(hook);
    s_installed = false;
    return 0;
}

void HookSetHitBox(RECT r) { s_box[0] = r.left; s_box[1] = r.top; s_box[2] = r.right; s_box[3] = r.bottom; }
bool HookInstalled() { return s_installed; }

void HookSet(HookMode m) {
    if ((int)m != s_mode) { s_fx = s_fy = 0; s_slowUntil = 0; s_inside = false; for (Move& mv : s_moves) mv = Move{}; }
    s_mode = (int)m;
    if (m != HookMode::Off && !s_thread) {
        if (!s_ready) s_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_thread = CreateThread(nullptr, 64 * 1024, HookThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &s_tid);
        if (s_thread) WaitForSingleObject(s_ready, 500);
    } else if (m == HookMode::Off && s_thread) {
        PostThreadMessageW(s_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(s_thread, 500);
        CloseHandle(s_thread);
        s_thread = nullptr; s_tid = 0;
    }
}

}  // namespace pet
