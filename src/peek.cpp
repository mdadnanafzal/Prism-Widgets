// Peek: a global hotkey lifts the widgets above every window over a blurred, dimmed backdrop,
// like glancing at the desktop without minimising anything. The same key, Esc or a click puts them back.
#include "app.h"
#include "pet/pet.h"

std::wstring g_peekKey = L"win+alt+w";   // "win+alt+w", "ctrl+alt+w", "ctrl+shift+space" or "off"
bool g_peekKeyOk = true;                 // false: another app owns the shortcut
bool g_peeking = false;

namespace {
const int HK_PEEK = 0x5057;   // 'PW'
HWND g_over = nullptr;
bool g_reg = false;
const UINT WM_RAISE = WM_APP + 1;

bool g_raising = false;

// Widgets above the backdrop: lift them into the topmost band, then tuck the backdrop in just under the lowest one.
// Re-run whenever the backdrop moves (activation can lift it back over the widgets after StartPeek returns).
void RaiseWidgets() {
    if (!g_over) return;
    g_raising = true;
    HWND lowest = nullptr;
    if (HWND pet = PetHwnd()) {   // the pet comes up too, under the widgets
        SetWindowPos(pet, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        lowest = pet;
    }
    for (auto& w : g_widgets) if (w->enabled && w->hwnd) {
        SetWindowPos(w->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!lowest) lowest = w->hwnd;   // each one lands on top, so the first is the lowest
    }
    if (lowest) SetWindowPos(g_over, lowest, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_raising = false;
}

struct AccentPolicy { int state, flags; DWORD color; int anim; };
struct CompAttr { int attr; void* data; SIZE_T size; };
typedef BOOL(WINAPI* SetCompAttrFn)(HWND, CompAttr*);

bool Ours(HWND h) {
    wchar_t cls[40] = {};
    if (h) GetClassNameW(h, cls, 40);
    return h == g_over || !wcscmp(cls, L"PrismWidget") || !wcscmp(cls, L"PrismDayPopup") || !wcscmp(cls, L"PrismInput") ||
           !wcscmp(cls, L"PrismSettings") || !wcscmp(cls, L"PrismReminder");
}

bool WidgetBelow() {   // is any of our widgets under the backdrop in the z-order?
    for (HWND h = GetWindow(g_over, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
        wchar_t cls[16] = {};
        GetClassNameW(h, cls, 16);
        if ((!wcscmp(cls, L"PrismWidget") || !wcscmp(cls, L"PrismPet")) && IsWindowVisible(h)) return true;
    }
    return false;
}

LRESULT CALLBACK OverProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {   // black paints as fully transparent over the blur
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps); return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: EndPeek(); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { EndPeek(); return 0; } break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !Ours((HWND)lp)) PostMessageW(h, WM_CLOSE, 0, 0);   // clicked into another app
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (g_peeking && !g_raising && !(((WINDOWPOS*)lp)->flags & SWP_NOZORDER)) PostMessageW(h, WM_RAISE, 0, 0);
        break;
    case WM_RAISE: if (g_peeking) RaiseWidgets(); return 0;
    case WM_TIMER:   // guard: if anything lifted the backdrop over a widget, put the widgets back on top
        if (g_peeking && WidgetBelow()) RaiseWidgets();
        return 0;
    case WM_CLOSE: EndPeek(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
} // namespace

void StartPeek() {
    if (g_peeking || g_arranging) return;
    static bool cls = false;
    if (!cls) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = OverProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismPeek"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc); cls = true;
    }
    Interactive();
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetCompAttrFn setAttr = (SetCompAttrFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    g_over = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (setAttr ? 0 : WS_EX_LAYERED), L"PrismPeek", L"Prism Widgets peek", WS_POPUP,
                             vx, vy, vw, vh, nullptr, nullptr, g_inst, nullptr);
    if (!g_over) return;
    bool blurred = false;
    if (setAttr) {   // acrylic: the windows behind are blurred, with a dark tint (AABBGGRR)
        AccentPolicy ap{ 4 /*ACCENT_ENABLE_ACRYLICBLURBEHIND*/, 2, 0x40100A0A, 0 };   // light tint: the blur does the work
        CompAttr d{ 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
        blurred = setAttr(g_over, &d) != FALSE;
    }
    if (!blurred) {   // no blur available: a plain dim
        SetWindowLongW(g_over, GWL_EXSTYLE, GetWindowLongW(g_over, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g_over, 0, 100, LWA_ALPHA);
    }
    g_peeking = true;
    PetPeek(true);
    ShowWindow(g_over, SW_SHOW);
    SetForegroundWindow(g_over);
    SetFocus(g_over);
    RaiseWidgets();
    PostMessageW(g_over, WM_RAISE, 0, 0);
    SetTimer(g_over, 1, 250, nullptr);
}

void EndPeek() {
    if (!g_peeking) return;
    g_peeking = false;
    PetPeek(false);
    for (auto& w : g_widgets) if (w->hwnd) {
        SetWindowPos(w->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(w->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_over) { HWND h = g_over; g_over = nullptr; DestroyWindow(h); }
}

void TogglePeek() { if (g_peeking) EndPeek(); else StartPeek(); }

bool RegisterPeekKey() {
    if (g_reg) { UnregisterHotKey(g_main, HK_PEEK); g_reg = false; }
    g_peekKeyOk = true;
    if (g_peekKey == L"off") return true;
    UINT mods = MOD_NOREPEAT, vk = 'W';
    if (g_peekKey == L"ctrl+alt+w") mods |= MOD_CONTROL | MOD_ALT;
    else if (g_peekKey == L"ctrl+shift+space") { mods |= MOD_CONTROL | MOD_SHIFT; vk = VK_SPACE; }
    else mods |= MOD_WIN | MOD_ALT;
    g_reg = RegisterHotKey(g_main, HK_PEEK, mods, vk) != FALSE;
    g_peekKeyOk = g_reg;
    return g_reg;
}
bool IsPeekHotkey(WPARAM id) { return id == (WPARAM)HK_PEEK; }

std::wstring PeekKeyLabel() {
    if (g_peekKey == L"ctrl+alt+w") return L"Ctrl + Alt + W";
    if (g_peekKey == L"ctrl+shift+space") return L"Ctrl + Shift + Space";
    if (g_peekKey == L"off") return L"";
    return L"Win + Alt + W";
}
