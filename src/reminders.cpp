// Event reminders: a small glass card in the corner a few minutes before a timed event
// (your own or the synced calendar), and in the morning for birthdays and all-day events of yours.
#include "events.h"
#include "app.h"
#include "pet/pet.h"
#include <deque>
#include <mmsystem.h>

namespace {
const UINT T_HIDE = 1;
const float CARD_W = 340, CARD_H = 92;
struct Note { int day; std::wstring head, title; wchar_t glyph = 0xE7ED; bool sound = true; bool accentBar = false; };
struct Toast {
    HWND hwnd = nullptr; int x = 0, y = 0, W = 0, H = 0; float s = 1;
    Note cur; int hover = -1; GlassCache glass;
} T;
std::deque<Note> g_queue;
bool g_cls = false;

Widget* Calendar() { for (auto& w : g_widgets) if (w->key == L"calendar") return w.get(); return nullptr; }
const Theme& CardTheme() {
    Widget* c = Calendar();
    const Theme& t = c ? ThemeFor(c) : FindTheme(g_theme);
    return t.panel ? t : FindTheme(L"frosted");
}
D2D1_RECT_F Rc(float l, float t, float r, float b) { return D2D1_RECT_F{ l, t, r, b }; }
D2D1_RECT_F CloseRect() { return Rc(CARD_W - 38, 10, CARD_W - 12, 36); }

void Draw(Gfx& g, float W, float H) {
    const Ink& k = g.ink;
    g.FillRound(Rc(16, 18, 20, H - 18), 2, T.cur.accentBar ? k.accentText : EventColor(EV_MINE, k.accentText));
    g.Icon(T.cur.glyph, Rc(30, 12, 52, 34), 12, k.accentText);   // bell by default
    g.Text(T.cur.head, Rc(54, 12, W - 44, 34), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, k.fg2, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.Text(T.cur.title, Rc(30, 38, W - 18, H - 14), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, k.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    D2D1_RECT_F x = CloseRect();
    if (T.hover == 1) g.FillRound(x, 7, k.hover);
    g.Icon(0xE711, x, 10, k.fg2);
}
void Redraw() { if (T.hwnd) RenderGlassPopup(T.hwnd, T.glass, CardTheme(), T.x, T.y, T.W, T.H, T.s, Draw); }

void ShowNext();
void Dismiss() {
    if (!T.hwnd) return;
    KillTimer(T.hwnd, T_HIDE);
    HWND h = T.hwnd; T.hwnd = nullptr; T.glass = GlassCache();
    DestroyWindow(h);
    ShowNext();
}
int HitAt(LPARAM lp) {
    float m = (float)GlassMargin(T.s);
    float x = (GET_X_LPARAM(lp) - m) / T.s, y = (GET_Y_LPARAM(lp) - m) / T.s;
    D2D1_RECT_F c = CloseRect();
    if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) return 1;
    if (x >= 0 && x < CARD_W && y >= 0 && y < CARD_H) return 0;
    return -1;
}
LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        SetTimer(h, T_HIDE, 15000, nullptr);   // reading it: keep it up
        int id = HitAt(lp);
        if (id != T.hover) { T.hover = id; Redraw(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (T.hover != -1) { T.hover = -1; Redraw(); } return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONUP: {
        int id = HitAt(lp);
        if (id == 0) {   // open the day
            int day = T.cur.day;
            RECT a{ T.x, T.y, T.x + T.W, T.y + T.H };
            Dismiss();
            if (day) ShowDayPopup(day, a);
        } else if (id == 1) Dismiss();
        return 0;
    }
    case WM_TIMER: if (wp == T_HIDE) Dismiss(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void ShowNext() {
    if (T.hwnd || g_queue.empty()) return;
    if (!g_cls) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ToastProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismReminder"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc); g_cls = true;
    }
    T.cur = g_queue.front(); g_queue.pop_front();
    POINT o{ 0, 0 };
    HMONITOR hm = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
    T.s = ScaleOf(hm);
    RECT wa = WorkOf(hm);
    T.W = (int)(CARD_W * T.s); T.H = (int)(CARD_H * T.s);
    T.x = wa.right - T.W - (int)(18 * T.s); T.y = wa.bottom - T.H - (int)(18 * T.s);
    RECT pet, card{ T.x, T.y, T.x + T.W, T.y + T.H }, both;
    if (PetScreenRect(pet) && IntersectRect(&both, &card, &pet))   // the desktop pet stands there: sit above its head
        T.y = std::max((int)wa.top + (int)(8 * T.s), (int)pet.top - T.H - (int)(10 * T.s));
    T.hover = -1;
    T.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"PrismReminder", L"Prism Widgets reminder", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    if (!T.hwnd) return;
    Redraw();
    ShowWindow(T.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(T.hwnd, T_HIDE, 15000, nullptr);
    if (T.cur.sound) PlaySoundW(L"Notification.Reminder", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}
std::wstring Clock(int h, int m) { wchar_t b[8]; swprintf(b, 8, L"%02d:%02d", h, m); return b; }
} // namespace

// Called every 30 s by the main window.
void RemindersTick() {
    if (!g_events.enabled || (!g_events.remindMins && !g_events.remindAllDay)) return;
    Widget* cal = Calendar();
    if (!cal || !cal->enabled) return;
    g_events.Ensure();
    SYSTEMTIME st; GetLocalTime(&st);
    int today = DayKey(st.wYear, st.wMonth, st.wDay);
    double now = st.wHour * 60 + st.wMinute + st.wSecond / 60.0;
    if (g_events.remindedDay != today) { g_events.remindedDay = today; g_events.reminded.clear(); }
    std::vector<const Occ*> ev; g_events.ForDay(today, ev);
    bool paused = Paused(), changed = false;
    for (const Occ* o : ev) {
        if (o->src == EV_HOLIDAY) continue;
        std::wstring key = std::to_wstring(o->sh) + L":" + std::to_wstring(o->sm) + L":" + o->title;
        if (std::find(g_events.reminded.begin(), g_events.reminded.end(), key) != g_events.reminded.end()) continue;
        Note n{ today, L"", o->title };
        n.sound = g_events.remindSound;
        if (o->sh >= 0) {
            if (!g_events.remindMins) continue;
            double left = o->sh * 60 + o->sm - now;
            if (left > g_events.remindMins) continue;                 // not yet
            if (left < -1) { g_events.reminded.push_back(key); changed = true; continue; }   // missed while away: don't nag
            int mins = (int)std::ceil(left);
            n.head = (mins > 0 ? L"In " + std::to_wstring(mins) + L" min" : std::wstring(L"Starting now")) + L" · " + Clock(o->sh, o->sm);
        } else {
            if (!g_events.remindAllDay || o->src != EV_MINE || now < 9 * 60) continue;
            n.head = L"Today";
        }
        if (paused) continue;   // after the game or the lock screen
        g_events.reminded.push_back(key); changed = true;
        g_queue.push_back(n);
    }
    if (changed) RequestSave();
    ShowNext();
}

void ShowTestReminder() {
    SYSTEMTIME st; GetLocalTime(&st);
    int lead = g_events.remindMins ? g_events.remindMins : 10, m = st.wHour * 60 + st.wMinute + lead;
    Note n{ 0, L"In " + std::to_wstring(lead) + L" min \u00B7 " + Clock((m / 60) % 24, m % 60), L"This is how a reminder looks" };
    n.sound = g_events.remindSound;
    g_queue.push_back(n);
    ShowNext();
}
void ShowToast(const std::wstring& head, const std::wstring& title, wchar_t glyph, bool sound) {
    Note n{ 0, head, title };
    n.glyph = glyph; n.sound = sound; n.accentBar = true;
    g_queue.push_back(n);
    ShowNext();
}
void CloseReminders() { g_queue.clear(); Dismiss(); }
