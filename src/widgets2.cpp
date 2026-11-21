// v1.9 widgets: Focus timer, Sticky note, World clocks, Network speed and Photo frame.
// All are off by default; turn them on in Settings -> Widgets.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00   // same as common.h
#define WINVER 0x0A00
#define NOMINMAX
#include <winsock2.h>   // before windows.h, so iphlpapi.h brings in GetIfTable2
#include <ws2ipdef.h>
#undef NOMINMAX          // common.h defines it again
#include "app.h"
#include <iphlpapi.h>
#include <random>

namespace {
void AddCheck(HMENU m, int id, const wchar_t* text, bool on) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, text); }
long long NowMs() {   // wall clock, so timers survive sleep and restarts
    FILETIME f; GetSystemTimeAsFileTime(&f);
    return (long long)((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000ULL);
}
int TodayKey() { SYSTEMTIME st; GetLocalTime(&st); return st.wYear * 10000 + st.wMonth * 100 + st.wDay; }
std::wstring HourMin(const SYSTEMTIME& st, bool h24) { return FormatDateTime(st, h24 ? L"HH:mm" : L"h:mm tt"); }
// round button helper: filled (primary) or hover-only
void RoundBtn(Gfx& g, float cx, float cy, float r, wchar_t glyph, bool primary, bool hover, bool pressed, D2D1_COLOR_F fill, D2D1_COLOR_F onFill) {
    if (primary) g.FillCircle(cx, cy, r, pressed ? Alpha(fill, .75f) : fill);
    else if (pressed) g.FillCircle(cx, cy, r, Alpha(g.ink.fg, .17f));
    else if (hover) g.FillCircle(cx, cy, r, g.ink.hover);
    g.Icon(glyph, RectF(cx - r, cy - r, cx + r, cy + r), primary ? r * 0.62f : r * 0.72f, primary ? onFill : g.ink.fg);
}
// Shows a menu at the cursor and returns the chosen id (0 = none). Used from Settings, where there is no widget menu.
int PopupAt(HMENU m) {
    POINT pt; GetCursorPos(&pt);
    HWND owner = GetForegroundWindow();
    if (!owner) owner = g_main;
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    return id;
}
} // namespace

// ======================================================================= Focus timer
class FocusWidget : public Widget {
public:
    int focusMin = 25, breakMin = 5;
    bool longBreaks = true, autoNext = false, sound = true;
    int phase = 0;          // 0 focus, 1 short break, 2 long break
    int streak = 0, streakDay = 0;   // focus sessions finished today
    bool running = false;
    long long endMs = 0, leftMs = -1;   // leftMs: paused remainder (-1 = full length)
    int hover = -1;
    FocusWidget() { key = L"focus"; title = L"Focus Timer"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 250, 270 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 190, 210 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 460, 500 }; }
    std::wstring Description() override { return L"Pomodoro-style focus sessions and breaks"; }
    int Interval() override { return running ? 1000 : 60000; }

    long long Length(int ph) const { return 60000LL * (ph == 0 ? focusMin : ph == 1 ? breakMin : breakMin * 3); }
    long long Left() const { return running ? std::max(0LL, endMs - NowMs()) : (leftMs >= 0 ? leftMs : Length(phase)); }
    void Start() { long long l = Left(); endMs = NowMs() + l; running = true; leftMs = -1; }
    void Pause() { leftMs = Left(); running = false; }
    void Reset() {
        bool fresh = !running && leftMs < 0;
        running = false; leftMs = -1;
        if (fresh && phase != 0) phase = 0;   // a second Reset goes back to focus
    }
    void Advance(bool notify) {
        int was = phase;
        if (phase == 0) {
            int today = TodayKey();
            if (streakDay != today) { streakDay = today; streak = 0; }
            streak++;
            phase = (longBreaks && streak % 4 == 0) ? 2 : 1;
        } else phase = 0;
        leftMs = -1; running = autoNext;
        if (running) endMs = NowMs() + Length(phase);
        if (notify) {
            if (was == 0) ShowToast(L"Focus session done", phase == 2 ? L"Great work! Take a " + std::to_wstring(breakMin * 3) + L"-minute break"
                                                                     : L"Time for a " + std::to_wstring(breakMin) + L"-minute break", 0xE916, sound);
            else ShowToast(L"Break is over", running ? L"Next focus session has started" : L"Ready to focus again?", 0xE916, sound);
        }
        RequestSave();
    }
    void Tick() override {
        if (running && NowMs() >= endMs) { Advance(true); dirty = true; }
    }

    struct Geo { float cx, cy, r, sw, by, gap; };
    Geo Lay(float W, float H) {
        Geo G;
        G.by = H - 34;
        float top = 46, bottom = H - 70;
        G.r = std::max(30.f, std::min(W - 44, bottom - top) / 2);
        G.cx = W / 2; G.cy = (top + bottom) / 2;
        G.sw = std::max(5.f, G.r * 0.085f);
        G.gap = std::min(60.f, W / 4.2f);
        return G;
    }
    D2D1_COLOR_F PhaseColor(const Ink& k) const { return phase == 0 ? k.accent : D2D1_COLOR_F{ .30f, .80f, .58f, 1 }; }
    void Draw(Gfx& g, float W, float H) override {
        Geo G = Lay(W, H);
        D2D1_COLOR_F col = PhaseColor(g.ink);
        const wchar_t* lbl = phase == 0 ? L"Focus" : phase == 1 ? L"Short break" : L"Long break";
        g.Text(lbl, RectF(18, 14, W - 80, 40), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (longBreaks) {   // progress towards the long break
            int today = TodayKey(), done = streakDay == today ? streak % 4 : 0;
            if (streakDay == today && streak > 0 && done == 0 && phase == 2) done = 4;
            for (int i = 0; i < 4; i++) {
                float x = W - 58 + i * 12, y = 27;
                if (i < done) g.FillCircle(x, y, 3.6f, col); else g.StrokeCircle(x, y, 3.3f, g.ink.fg3, 1.3f);
            }
        }
        long long left = Left(), len = Length(phase);
        float r = G.r - G.sw / 2;
        g.StrokeCircle(G.cx, G.cy, r, Alpha(g.ink.fg, .13f), G.sw);
        float frac = len > 0 ? (float)left / (float)len : 0;
        if (frac > 0.002f) g.Arc(G.cx, G.cy, r, 0, 360.f * std::min(1.f, frac), col, G.sw);
        long long sec = (left + 999) / 1000;
        wchar_t tb[16];
        if (sec >= 3600) swprintf(tb, 16, L"%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
        else swprintf(tb, 16, L"%02lld:%02lld", sec / 60, sec % 60);
        float big = std::max(18.f, r * 0.46f);
        g.Text(tb, RectF(G.cx - r, G.cy - big * 0.86f, G.cx + r, G.cy + big * 0.5f), big, DWRITE_FONT_WEIGHT_LIGHT, g.ink.fg,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, FONT_DISPLAY);
        std::wstring sub;
        if (running) {
            SYSTEMTIME st; GetLocalTime(&st);
            FILETIME ft; SystemTimeToFileTime(&st, &ft);
            ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + (ULONGLONG)left * 10000ULL;
            ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
            FileTimeToSystemTime(&ft, &st);
            sub = L"Until " + HourMin(st, LocaleUses24h());
        } else sub = leftMs >= 0 ? L"Paused" : L"Ready";
        float ss = std::max(10.5f, std::min(13.f, r * 0.13f));
        g.Text(sub, RectF(G.cx - r, G.cy + big * 0.52f, G.cx + r, G.cy + big * 0.52f + ss * 1.6f), ss, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_CENTER);
        D2D1_COLOR_F on = phase == 0 ? g.ink.onAccent : D2D1_COLOR_F{ .04f, .12f, .08f, 1 };
        RoundBtn(g, G.cx - G.gap, G.by, 16, 0xE72C, false, hover == 0, Pressed(0), col, on);                  // reset
        RoundBtn(g, G.cx, G.by, 21, running ? 0xE769 : 0xE768, true, hover == 1, Pressed(1), col, on);      // play / pause
        RoundBtn(g, G.cx + G.gap, G.by, 16, 0xE893, false, hover == 2, Pressed(2), col, on);                  // skip
    }
    int Hit(float x, float y) {
        D2D1_SIZE_F b = BaseSize(); Geo G = Lay(b.width, b.height);
        for (int i = 0; i < 3; i++) {
            float cx = G.cx + (i - 1) * G.gap, rr = i == 1 ? 23.f : 19.f;
            if ((x - cx) * (x - cx) + (y - G.by) * (y - G.by) <= rr * rr) return i;
        }
        float dx = x - G.cx, dy = y - G.cy;
        if (dx * dx + dy * dy <= G.r * G.r) return 1;   // clicking the ring also starts / pauses
        return -1;
    }
    bool HitId(float x, float y, int& id) override { id = Hit(x, y); return id >= 0; }
    bool Click(float x, float y) override {
        int h = Hit(x, y);
        if (h == 1) { if (running) Pause(); else Start(); }
        else if (h == 0) Reset();
        else if (h == 2) Advance(false);
        else return false;
        RequestSave();
        return true;
    }
    bool Hover(float x, float y) override { int h = Hit(x, y); if (h == hover) return false; hover = h; return true; }
    void Leave() override { hover = -1; }
    void Options(std::vector<Opt>& o) override {
        static const int fm[] = { 15, 25, 45, 60 }, bm[] = { 5, 10, 15 };
        o.push_back({ Opt::Choice, L"Focus length", L"", { L"15 min", L"25 min", L"45 min", L"60 min" },
                      [this] { for (int i = 0; i < 4; i++) if (fm[i] == focusMin) return i; return 1; },
                      [this](int v) { focusMin = fm[v]; if (phase == 0 && !running) leftMs = -1; RequestSave(); } });
        o.push_back({ Opt::Choice, L"Break length", L"", { L"5 min", L"10 min", L"15 min" },
                      [this] { for (int i = 0; i < 3; i++) if (bm[i] == breakMin) return i; return 0; },
                      [this](int v) { breakMin = bm[v]; if (phase != 0 && !running) leftMs = -1; RequestSave(); } });
        o.push_back({ Opt::Toggle, L"Long break after 4 sessions", L"Three times the break length", {}, [this] { return longBreaks ? 1 : 0; }, [this](int v) { longBreaks = v != 0; RequestSave(); } });
        o.push_back({ Opt::Toggle, L"Start the next session automatically", L"", {}, [this] { return autoNext ? 1 : 0; }, [this](int v) { autoNext = v != 0; RequestSave(); } });
        o.push_back({ Opt::Toggle, L"Sound", L"Plays a chime with the notification card", {}, [this] { return sound ? 1 : 0; }, [this](int v) { sound = v != 0; RequestSave(); } });
        o.push_back({ Opt::Action, L"Today's sessions", std::to_wstring(streakDay == TodayKey() ? streak : 0) + L" finished", { L"Reset" }, nullptr,
                      [this](int) { streak = 0; running = false; leftMs = -1; phase = 0; RequestSave(); } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING, 101, running ? L"Pause" : L"Start");
        AppendMenuW(m, MF_STRING, 102, L"Reset");
        AppendMenuW(m, MF_STRING, 103, phase == 0 ? L"Skip to break" : L"Skip break");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        HMENU fl = CreatePopupMenu();
        for (int v : { 15, 25, 45, 60 }) AppendMenuW(fl, MF_STRING | (v == focusMin ? MF_CHECKED : 0), 110 + v, (std::to_wstring(v) + L" minutes").c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)fl, L"Focus length");
        AddCheck(m, 104, L"Start next session automatically", autoNext);
        AddCheck(m, 105, L"Sound", sound);
    }
    void Command(int id) override {
        if (id == 101) { if (running) Pause(); else Start(); }
        else if (id == 102) Reset();
        else if (id == 103) Advance(false);
        else if (id == 104) autoNext = !autoNext;
        else if (id == 105) sound = !sound;
        else if (id > 110 && id <= 170) { focusMin = id - 110; if (phase == 0 && !running) leftMs = -1; }
    }
    void Load(const JVal& o) override {
        focusMin = Clamp((int)o.num(L"focus", 25), 1, 180); breakMin = Clamp((int)o.num(L"break", 5), 1, 60);
        longBreaks = o.boolean(L"longBreaks", true); autoNext = o.boolean(L"autoNext", false); sound = o.boolean(L"sound", true);
        phase = Clamp((int)o.num(L"phase", 0), 0, 2); streak = (int)o.num(L"streak", 0); streakDay = (int)o.num(L"streakDay", 0);
        running = o.boolean(L"running", false); endMs = (long long)o.num(L"end", 0); leftMs = (long long)o.num(L"left", -1);
        if (running && NowMs() - endMs > 60 * 60000LL) { running = false; leftMs = -1; }   // finished long ago (PC was off): start fresh
    }
    void Save(JVal& o) override {
        o.set(L"focus") = JVal::MakeNum(focusMin); o.set(L"break") = JVal::MakeNum(breakMin);
        o.set(L"longBreaks") = JVal::MakeBool(longBreaks); o.set(L"autoNext") = JVal::MakeBool(autoNext); o.set(L"sound") = JVal::MakeBool(sound);
        o.set(L"phase") = JVal::MakeNum(phase); o.set(L"streak") = JVal::MakeNum(streak); o.set(L"streakDay") = JVal::MakeNum(streakDay);
        o.set(L"running") = JVal::MakeBool(running); o.set(L"end") = JVal::MakeNum((double)endMs); o.set(L"left") = JVal::MakeNum((double)leftMs);
    }
};

// ======================================================================= Sticky note
class NoteWidget : public Widget {
public:
    std::wstring text;
    int color = 1, font = 1;   // colour: glass, yellow, pink, mint, blue, lavender; font: small, medium, large
    float scroll = 0, contentH = 0, areaH = 0;
    bool hover = false;
    NoteWidget() { key = L"note"; title = L"Sticky Note"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 260, 230 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 160, 110 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 720, 800 }; }
    std::wstring Description() override { return L"A quick note on your desktop"; }
    float FontSize() const { return font == 0 ? 13.f : font == 2 ? 18.f : 15.f; }
    static D2D1_COLOR_F Tint(int c) {   // paper colours
        switch (c) {
        case 1: return D2D1_COLOR_F{ 1.00f, .90f, .47f, 1 };
        case 2: return D2D1_COLOR_F{ 1.00f, .75f, .84f, 1 };
        case 3: return D2D1_COLOR_F{ .69f, .94f, .80f, 1 };
        case 4: return D2D1_COLOR_F{ .71f, .85f, 1.00f, 1 };
        case 5: return D2D1_COLOR_F{ .84f, .78f, 1.00f, 1 };
        }
        return D2D1_COLOR_F{ 0, 0, 0, 0 };
    }
    void Draw(Gfx& g, float W, float H) override {
        if (color > 0) {   // a sheet of coloured paper over the glass (a hint of glass shows through), with dark ink
            D2D1_COLOR_F t = Tint(color);
            g.FillRound(RectF(0, 0, W, H), g.ink.radius, Alpha(t, .86f));
            g.FillRound(RectF(W / 2 - 22, 7, W / 2 + 22, 11), 2, D2D1_COLOR_F{ t.r * .78f, t.g * .78f, t.b * .78f, 1 });
            D2D1_COLOR_F ink{ .13f, .11f, .08f, 1 };
            g.ink.fg = ink; g.ink.fg2 = Alpha(ink, .62f); g.ink.fg3 = Alpha(ink, .40f); g.ink.hover = Alpha(ink, .07f); g.ink.shadow = false;
        }
        D2D1_RECT_F area = RectF(18, 20, W - 16, H - 14);
        areaH = area.bottom - area.top;
        float fs = FontSize();
        if (text.empty()) {
            if (hover) g.FillRound(RectF(8, 8, W - 8, H - 8), std::max(4.f, g.ink.radius - 6), g.ink.hover);
            g.Icon(0xE70F, RectF(area.left, area.top, area.left + 22, area.top + 24), 13, g.ink.fg3);
            g.Text(L"Click to write a note", RectF(area.left + 26, area.top, area.right, area.top + 24), fs, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            contentH = 0; scroll = 0;
            return;
        }
        IDWriteTextLayout* lay = g_gr.Layout(text, g_gr.Format(FONT_TEXT, fs, DWRITE_FONT_WEIGHT_NORMAL), area.right - area.left - 6, 100000, 0, 0, true, false);
        DWRITE_TEXT_METRICS tm{};
        if (lay) lay->GetMetrics(&tm);
        contentH = tm.height;
        scroll = Clamp(scroll, 0.f, std::max(0.f, contentH - areaH));
        g.rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
        g.Text(text, RectF(area.left, area.top - scroll, area.right - 6, area.top - scroll + 100000), fs, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_TEXT, false, true);
        g.rt->PopAxisAlignedClip();
        if (contentH > areaH + 1) {   // scrollbar
            float th = std::max(18.f, areaH * areaH / contentH), ty = area.top + (areaH - th) * scroll / (contentH - areaH);
            g.FillRound(RectF(W - 9, ty, W - 6, ty + th), 1.5f, g.ink.fg3);
        }
    }
    void Edit() {
        D2D1_SIZE_F b = BaseSize();
        ShowNoteInput(this, RectF(6, 6, b.width - 6, b.height - 6), text, FontSize(), [this](const std::wstring& s) {
            text = s; RequestSave(); RequestRedraw(this);
        });
    }
    bool HitId(float, float, int& id) override { id = 0; return false; }
    bool Click(float, float) override { Edit(); return false; }
    bool Hover(float, float) override { if (hover) return false; hover = true; return text.empty(); }
    void Leave() override { hover = false; }
    bool Wheel(float, float, int d) override {
        float old = scroll;
        scroll = Clamp(scroll - (d > 0 ? 1 : -1) * FontSize() * 3, 0.f, std::max(0.f, contentH - areaH));
        return old != scroll;
    }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Choice, L"Colour", L"", { L"Glass", L"Yellow", L"Pink", L"Mint", L"Blue", L"Lilac" }, [this] { return color; }, [this](int v) { color = v; } });
        o.push_back({ Opt::Choice, L"Text size", L"", { L"Small", L"Medium", L"Large" }, [this] { return font; }, [this](int v) { font = v; } });
        o.push_back({ Opt::Action, L"Clear note", text.empty() ? L"The note is empty" : std::to_wstring(text.size()) + L" characters", { L"Clear" }, nullptr,
                      [this](int) { text.clear(); scroll = 0; RequestSave(); } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING, 101, L"Edit note");
        AppendMenuW(m, MF_STRING | (text.empty() ? MF_GRAYED : 0), 102, L"Copy text");
        AppendMenuW(m, MF_STRING | (text.empty() ? MF_GRAYED : 0), 103, L"Clear note");
        HMENU c = CreatePopupMenu();
        const wchar_t* names[] = { L"Glass", L"Yellow", L"Pink", L"Mint", L"Blue", L"Lilac" };
        for (int i = 0; i < 6; i++) AppendMenuW(c, MF_STRING | (color == i ? MF_CHECKED : 0), 110 + i, names[i]);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)c, L"Colour");
        HMENU f = CreatePopupMenu();
        const wchar_t* fn[] = { L"Small", L"Medium", L"Large" };
        for (int i = 0; i < 3; i++) AppendMenuW(f, MF_STRING | (font == i ? MF_CHECKED : 0), 120 + i, fn[i]);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"Text size");
    }
    void Command(int id) override {
        if (id == 101) Edit();
        else if (id == 102) CopyText();
        else if (id == 103) { text.clear(); scroll = 0; }
        else if (id >= 110 && id < 116) color = id - 110;
        else if (id >= 120 && id < 123) font = id - 120;
    }
    void CopyText() {
        std::wstring t; for (wchar_t c : text) { if (c == L'\n') t += L'\r'; t += c; }
        if (!OpenClipboard(g_main)) return;
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
        if (h) { memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); }
        CloseClipboard();
    }
    void Load(const JVal& o) override { text = o.str(L"text", L""); color = Clamp((int)o.num(L"color", 1), 0, 5); font = Clamp((int)o.num(L"font", 1), 0, 2); }
    void Save(JVal& o) override { o.set(L"text") = JVal::MakeStr(text); o.set(L"color") = JVal::MakeNum(color); o.set(L"font") = JVal::MakeNum(font); }
};

// ======================================================================= World clocks
namespace {
struct CityDef { const wchar_t* name; const wchar_t* tz; int region; };   // region: 0 Americas, 1 Europe & Africa, 2 Asia, 3 Pacific, 4 other
const CityDef kCities[] = {
    { L"Honolulu", L"Hawaiian Standard Time", 0 }, { L"Los Angeles", L"Pacific Standard Time", 0 }, { L"Vancouver", L"Pacific Standard Time", 0 },
    { L"Denver", L"Mountain Standard Time", 0 }, { L"Chicago", L"Central Standard Time", 0 }, { L"Mexico City", L"Central Standard Time (Mexico)", 0 },
    { L"New York", L"Eastern Standard Time", 0 }, { L"Toronto", L"Eastern Standard Time", 0 }, { L"Bogotá", L"SA Pacific Standard Time", 0 },
    { L"São Paulo", L"E. South America Standard Time", 0 }, { L"Buenos Aires", L"Argentina Standard Time", 0 },
    { L"London", L"GMT Standard Time", 1 }, { L"Dublin", L"GMT Standard Time", 1 }, { L"Lisbon", L"GMT Standard Time", 1 },
    { L"Paris", L"Romance Standard Time", 1 }, { L"Madrid", L"Romance Standard Time", 1 }, { L"Berlin", L"W. Europe Standard Time", 1 },
    { L"Amsterdam", L"W. Europe Standard Time", 1 }, { L"Rome", L"W. Europe Standard Time", 1 }, { L"Stockholm", L"W. Europe Standard Time", 1 },
    { L"Athens", L"GTB Standard Time", 1 }, { L"Istanbul", L"Turkey Standard Time", 1 }, { L"Moscow", L"Russian Standard Time", 1 },
    { L"Cairo", L"Egypt Standard Time", 1 }, { L"Lagos", L"W. Central Africa Standard Time", 1 }, { L"Nairobi", L"E. Africa Standard Time", 1 },
    { L"Johannesburg", L"South Africa Standard Time", 1 },
    { L"Dubai", L"Arabian Standard Time", 2 }, { L"Riyadh", L"Arab Standard Time", 2 }, { L"Karachi", L"Pakistan Standard Time", 2 },
    { L"New Delhi", L"India Standard Time", 2 }, { L"Mumbai", L"India Standard Time", 2 }, { L"Bengaluru", L"India Standard Time", 2 },
    { L"Kathmandu", L"Nepal Standard Time", 2 }, { L"Dhaka", L"Bangladesh Standard Time", 2 }, { L"Bangkok", L"SE Asia Standard Time", 2 },
    { L"Jakarta", L"SE Asia Standard Time", 2 }, { L"Singapore", L"Singapore Standard Time", 2 }, { L"Kuala Lumpur", L"Singapore Standard Time", 2 },
    { L"Hong Kong", L"China Standard Time", 2 }, { L"Beijing", L"China Standard Time", 2 }, { L"Shanghai", L"China Standard Time", 2 },
    { L"Seoul", L"Korea Standard Time", 2 }, { L"Tokyo", L"Tokyo Standard Time", 2 },
    { L"Perth", L"W. Australia Standard Time", 3 }, { L"Brisbane", L"E. Australia Standard Time", 3 }, { L"Sydney", L"AUS Eastern Standard Time", 3 },
    { L"Melbourne", L"AUS Eastern Standard Time", 3 }, { L"Auckland", L"New Zealand Standard Time", 3 },
    { L"UTC", L"UTC", 4 },
};
const int kCityCount = sizeof(kCities) / sizeof(kCities[0]);   // ids 120 + index must stay below 185
const int MAX_CITIES = 8;
HMENU CityMenu() {
    HMENU m = CreatePopupMenu();
    const wchar_t* regions[] = { L"Americas", L"Europe && Africa", L"Asia", L"Australia && Pacific" };
    for (int r = 0; r < 4; r++) {
        HMENU s = CreatePopupMenu();
        for (int i = 0; i < kCityCount; i++) if (kCities[i].region == r) AppendMenuW(s, MF_STRING, 120 + i, kCities[i].name);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)s, regions[r]);
    }
    for (int i = 0; i < kCityCount; i++) if (kCities[i].region == 4) AppendMenuW(m, MF_STRING, 120 + i, kCities[i].name);
    return m;
}
bool FindZone(const std::wstring& key, DYNAMIC_TIME_ZONE_INFORMATION& out) {
    for (DWORD i = 0;; i++) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        DWORD r = EnumDynamicTimeZoneInformation(i, &d);
        if (r != ERROR_SUCCESS) return false;
        if (key == d.TimeZoneKeyName) { out = d; return true; }
    }
}
long long Minutes(const SYSTEMTIME& st) {
    FILETIME f; SystemTimeToFileTime(&st, &f);
    return (long long)((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 600000000ULL);
}
} // namespace

class WorldClockWidget : public Widget {
public:
    struct City { std::wstring name, tz; bool looked = false, ok = false; DYNAMIC_TIME_ZONE_INFORMATION dz{}; int year = 0; TIME_ZONE_INFORMATION tzi{}; };
    std::vector<City> cities;
    int h24 = -1;            // -1 = follow Windows
    bool showDiff = true;
    int hover = -1, menuRow = -1;
    const float rowH = 58, padT = 10;
    WorldClockWidget() {
        key = L"worldclock"; title = L"World Clocks";
        for (auto n : { L"London", L"New York", L"Tokyo" }) for (int i = 0; i < kCityCount; i++) if (!wcscmp(kCities[i].name, n)) cities.push_back({ n, kCities[i].tz });
    }
    bool Use24() { return h24 < 0 ? LocaleUses24h() : h24 == 1; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 300, padT * 2 + rowH * std::max<size_t>(1, cities.size()) }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 200, padT * 2 + rowH }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 640, padT * 2 + rowH * MAX_CITIES }; }
    std::wstring Description() override { return L"The time in other cities"; }
    int Interval() override { return 60000; }
    bool Zone(City& c) {
        if (!c.looked) { c.looked = true; c.ok = FindZone(c.tz, c.dz); }
        return c.ok;
    }
    bool ToZone(City& c, const SYSTEMTIME& utc, SYSTEMTIME& out) {
        if (!Zone(c)) return false;
        if (c.year != utc.wYear) {   // the rules for this year (daylight saving dates change)
            c.year = utc.wYear;
            if (!GetTimeZoneInformationForYear(utc.wYear, &c.dz, &c.tzi)) { c.ok = false; return false; }
        }
        return SystemTimeToTzSpecificLocalTime(&c.tzi, &utc, &out) != FALSE;
    }
    int Rows() { return std::max(1, (int)((BaseSize().height - padT * 2 + 4) / rowH)); }
    void Draw(Gfx& g, float W, float H) override {
        SYSTEMTIME utc, local; GetSystemTime(&utc); GetLocalTime(&local);
        long long lm = Minutes(local);
        bool t24 = Use24();
        if (cities.empty()) {
            g.Icon(0xE774, RectF(18, 0, 40, H), 14, g.ink.fg3);
            g.Text(L"Right-click to add a city", RectF(46, 0, W - 16, H), 14, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return;
        }
        int rows = std::min((int)cities.size(), Rows());
        float rh = std::min(rowH + 14, (H - padT * 2) / rows);
        for (int i = 0; i < rows; i++) {
            City& c = cities[i];
            float y0 = padT + i * rh, y1 = y0 + rh;
            if (hover == i) g.FillRound(RectF(8, y0 + 1, W - 8, y1 - 1), 10, g.ink.hover);
            if (i > 0) g.Line(18, y0, W - 18, y0, Alpha(g.ink.fg, .08f), 1);
            SYSTEMTIME ct = local; bool ok = ToZone(c, utc, ct);
            std::wstring sub;
            if (!ok) sub = L"Unknown time zone";
            else {
                long long diff = Minutes(ct) - lm;
                int cd = ct.wYear * 10000 + ct.wMonth * 100 + ct.wDay, ld = local.wYear * 10000 + local.wMonth * 100 + local.wDay;
                sub = cd == ld ? L"Today" : cd > ld ? L"Tomorrow" : L"Yesterday";
                if (showDiff) {
                    long long a = diff < 0 ? -diff : diff;
                    wchar_t b[32];
                    if (a == 0) wcscpy(b, L"same time");
                    else if (a % 60) swprintf(b, 32, L"%s%lldh %lldm", diff < 0 ? L"−" : L"+", a / 60, a % 60);
                    else swprintf(b, 32, L"%s%lldh", diff < 0 ? L"−" : L"+", a / 60);
                    sub += L" · "; sub += b;
                }
            }
            bool day = ct.wHour >= 6 && ct.wHour < 18;
            float mid = (y0 + y1) / 2;
            g.Icon(day ? 0xE706 : 0xE708, RectF(16, mid - 11, 38, mid + 11), 12, day ? g.ink.accentText : g.ink.fg2);
            float tx = 44;
            std::wstring tm = FormatDateTime(ct, t24 ? L"HH:mm" : L"h:mm");
            float big = std::min(26.f, rh * 0.46f), ampW = 0;
            if (!t24) {
                ampW = g.Measure(L"PM", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 4;
                g.Text(ct.wHour < 12 ? L"AM" : L"PM", RectF(W - 18 - ampW, mid - big * 0.42f, W - 18, mid + 4), 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            float tw = g.Measure(tm, big, DWRITE_FONT_WEIGHT_LIGHT, FONT_DISPLAY);
            g.Text(ok ? tm : L"--:--", RectF(W - 18 - ampW - tw - 6, mid - big * 0.7f, W - 18 - ampW, mid + big * 0.7f), big, DWRITE_FONT_WEIGHT_LIGHT, g.ink.fg,
                   DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, FONT_DISPLAY);
            float right = W - 18 - ampW - tw - 12;
            g.Text(c.name, RectF(tx, mid - 20, right, mid + 1), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.Text(sub, RectF(tx, mid + 1, right, mid + 20), 12, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    int Hit(float, float y) {
        if (cities.empty()) return -1;
        int rows = std::min((int)cities.size(), Rows());
        float rh = std::min(rowH + 14, (BaseSize().height - padT * 2) / rows);
        int r = (int)std::floor((y - padT) / rh);
        return r >= 0 && r < rows ? r : -1;
    }
    bool Hover(float x, float y) override { int h = Hit(x, y); if (h == hover) return false; hover = h; return true; }
    void Leave() override { hover = -1; }
    bool RightClickAt(float x, float y, HMENU m) override {
        menuRow = Hit(x, y);
        if (menuRow < 0) return false;
        AppendMenuW(m, MF_STRING, 186, (L"Rename " + cities[menuRow].name + L"…").c_str());
        AppendMenuW(m, MF_STRING | (menuRow == 0 ? MF_GRAYED : 0), 187, L"Move up");
        AppendMenuW(m, MF_STRING, 185, L"Remove");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        return true;
    }
    void AddCity(int idx) {
        if (idx < 0 || idx >= kCityCount || (int)cities.size() >= MAX_CITIES) return;
        cities.push_back({ kCities[idx].name, kCities[idx].tz });
        if (ch > 0) ch = std::max(ch, DefaultSize().height);   // grow a resized widget so the new city shows
    }
    void RemoveCity(int i) {
        if (i < 0 || i >= (int)cities.size()) return;
        cities.erase(cities.begin() + i);
        if (ch > 0) ch = std::min(ch, DefaultSize().height);
    }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Toggle, L"24-hour time", L"", {}, [this] { return Use24() ? 1 : 0; }, [this](int v) { h24 = v ? 1 : 0; } });
        o.push_back({ Opt::Toggle, L"Show time difference", L"How far ahead or behind your own clock each city is", {}, [this] { return showDiff ? 1 : 0; }, [this](int v) { showDiff = v != 0; } });
        for (int i = 0; i < (int)cities.size(); i++)
            o.push_back({ Opt::Action, cities[i].name, cities[i].tz, { L"Remove" }, nullptr, [this, i](int) { RemoveCity(i); } });
        if ((int)cities.size() < MAX_CITIES)
            o.push_back({ Opt::Action, L"Add a city", L"Up to " + std::to_wstring(MAX_CITIES) + L" cities", { L"Add…" }, nullptr,
                          [this](int) { int id = PopupAt(CityMenu()); if (id >= 120) AddCity(id - 120); } });
    }
    void Menu(HMENU m) override {
        if ((int)cities.size() < MAX_CITIES) AppendMenuW(m, MF_POPUP, (UINT_PTR)CityMenu(), L"Add a city");
        AddCheck(m, 101, L"24-hour time", Use24());
        AddCheck(m, 102, L"Show time difference", showDiff);
    }
    void Command(int id) override {
        if (id == 101) h24 = Use24() ? 0 : 1;
        else if (id == 102) showDiff = !showDiff;
        else if (id >= 120 && id < 120 + kCityCount) AddCity(id - 120);
        else if (id == 185) RemoveCity(menuRow);
        else if (id == 187 && menuRow > 0 && menuRow < (int)cities.size()) std::swap(cities[menuRow], cities[menuRow - 1]);
        else if (id == 186 && menuRow >= 0 && menuRow < (int)cities.size()) {
            int i = menuRow;
            D2D1_SIZE_F b = BaseSize();
            int rows = std::min((int)cities.size(), Rows());
            float rh = std::min(rowH + 14, (b.height - padT * 2) / rows);
            ShowInput(this, RectF(10, padT + i * rh + 6, b.width - 10, padT + (i + 1) * rh - 6), cities[i].name, [this, i](const std::wstring& s) {
                if (i < (int)cities.size() && !s.empty()) { cities[i].name = s; RequestSave(); RequestRedraw(this); }
            });
        }
    }
    void Load(const JVal& o) override {
        h24 = (int)o.num(L"h24", -1); showDiff = o.boolean(L"diff", true);
        if (const JVal* a = o.get(L"cities")) if (a->isArr()) {
            cities.clear();
            for (auto& e : a->a) if (e.isObj() && (int)cities.size() < MAX_CITIES) cities.push_back({ e.str(L"name", L""), e.str(L"tz", L"UTC") });
        }
    }
    void Save(JVal& o) override {
        o.set(L"h24") = JVal::MakeNum(h24); o.set(L"diff") = JVal::MakeBool(showDiff);
        JVal a = JVal::MakeArr();
        for (auto& c : cities) { JVal e = JVal::MakeObj(); e.set(L"name") = JVal::MakeStr(c.name); e.set(L"tz") = JVal::MakeStr(c.tz); a.a.push_back(e); }
        o.set(L"cities") = a;
    }
};

// ======================================================================= Network speed
class NetSpeedWidget : public Widget {
public:
    static const int N = 60;
    float down[N] = {}, up[N] = {};   // bytes per second, ring buffer
    int head = 0, count = 0;
    ULONG64 lastIn = 0, lastOut = 0; ULONGLONG lastAt = 0;
    bool bits = false, graph = true;
    int every = 1;   // seconds
    NetSpeedWidget() { key = L"netspeed"; title = L"Network Speed"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 300, 160 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 180, 92 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 640, 400 }; }
    std::wstring Description() override { return L"Live download and upload speed"; }
    int Interval() override { return every == 1 ? 1000 : every * 1000; }
    static bool Read(ULONG64& in, ULONG64& out) {
        MIB_IF_TABLE2* t = nullptr;
        if (GetIfTable2(&t) != NO_ERROR || !t) return false;
        in = out = 0;
        for (ULONG i = 0; i < t->NumEntries; i++) {
            const MIB_IF_ROW2& r = t->Table[i];
            // real adapters only: no loopback, no filter/QoS shims (they repeat the same bytes), no tunnels
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp) continue;
            if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
            in += r.InOctets; out += r.OutOctets;
        }
        FreeMibTable(t);
        return true;
    }
    void Sample() {
        ULONG64 in, out; ULONGLONG now = GetTickCount64();
        if (!Read(in, out)) return;
        if (lastAt && now > lastAt && now - lastAt < 15000 && in >= lastIn && out >= lastOut) {
            double s = (now - lastAt) / 1000.0;
            if (s >= 0.4) {
                down[head] = (float)((in - lastIn) / s); up[head] = (float)((out - lastOut) / s);
                head = (head + 1) % N; if (count < N) count++;
            } else return;   // too soon (e.g. a resize redraw): keep the previous reading
        }
        lastIn = in; lastOut = out; lastAt = now;
    }
    float Last(const float* a) const { return count ? a[(head + N - 1) % N] : 0; }
    std::wstring Fmt(float bps, std::wstring& unit) const {
        double v = bps; const wchar_t* u;
        if (bits) {
            v *= 8;
            if (v >= 1e9) { v /= 1e9; u = L"Gbps"; } else if (v >= 1e6) { v /= 1e6; u = L"Mbps"; } else { v /= 1e3; u = L"Kbps"; }
        } else {
            if (v >= 1073741824.0) { v /= 1073741824.0; u = L"GB/s"; } else if (v >= 1048576.0) { v /= 1048576.0; u = L"MB/s"; } else { v /= 1024.0; u = L"KB/s"; }
        }
        unit = u;
        wchar_t b[32]; swprintf(b, 32, v < 10 ? L"%.1f" : L"%.0f", v);
        return b;
    }
    void Draw(Gfx& g, float W, float H) override {
        Sample();
        bool stack = W < 250;
        float topH = stack ? 96.f : 66.f;
        bool showGraph = graph && H >= topH + 46;
        if (!showGraph) topH = H;
        D2D1_COLOR_F upCol = Alpha(g.ink.fg, .72f);
        auto block = [&](float x0, float y0, float x1, float y1, bool isDown) {
            std::wstring unit, v = Fmt(Last(isDown ? down : up), unit);
            float big = std::min(28.f, (y1 - y0) * 0.56f);
            g.Icon(isDown ? 0xE896 : 0xE898, RectF(x0, y0, x0 + 18, y0 + big * 1.2f), 12, isDown ? g.ink.accentText : upCol);
            float vw = g.Measure(v, big, DWRITE_FONT_WEIGHT_SEMI_BOLD, FONT_DISPLAY);
            g.Text(v, RectF(x0 + 22, y0, x1, y0 + big * 1.25f), big, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, FONT_DISPLAY);
            g.Text(unit, RectF(x0 + 26 + vw, y0, x1, y0 + big * 1.25f), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.Text(isDown ? L"Download" : L"Upload", RectF(x0 + 22, y0 + big * 1.2f, x1, y0 + big * 1.2f + 16), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2);
        };
        if (stack) {
            float h2 = (std::min(topH, H) - 16) / 2;
            block(16, 10, W - 12, 10 + h2, true);
            block(16, 10 + h2, W - 12, 10 + 2 * h2, false);
        } else {
            float bh = std::min(topH, H) - 18;
            block(16, 12, W / 2, 12 + bh, true);
            block(W / 2 + 6, 12, W - 12, 12 + bh, false);
        }
        if (!showGraph) return;
        D2D1_RECT_F gr = RectF(16, topH + 6, W - 16, H - 14);
        float mx = 32 * 1024.f;   // floor so an idle line stays near the bottom
        for (int i = 0; i < count; i++) { int k = (head + N - 1 - i) % N; mx = std::max(mx, std::max(down[k], up[k])); }
        mx *= 1.15f;
        g.Line(gr.left, gr.bottom, gr.right, gr.bottom, Alpha(g.ink.fg, .12f), 1);
        if (count < 2) return;
        float step = (gr.right - gr.left) / (N - 1);
        auto path = [&](const float* a, bool fill) -> ID2D1PathGeometry* {
            ID2D1PathGeometry* p = nullptr; ID2D1GeometrySink* s = nullptr;
            if (FAILED(g_gr.d2d->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) { SafeRelease(p); return nullptr; }
            int n = count;
            auto pt = [&](int i) {   // i = 0 oldest .. n-1 newest, right-aligned
                int k = (head + N - n + i) % N;
                float x = gr.right - (n - 1 - i) * step, y = gr.bottom - (gr.bottom - gr.top) * std::min(1.f, a[k] / mx);
                return D2D1_POINT_2F{ x, y };
            };
            D2D1_POINT_2F p0 = pt(0);
            if (fill) { s->BeginFigure(D2D1_POINT_2F{ p0.x, gr.bottom }, D2D1_FIGURE_BEGIN_FILLED); s->AddLine(p0); }
            else s->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            for (int i = 1; i < n; i++) s->AddLine(pt(i));
            if (fill) s->AddLine(D2D1_POINT_2F{ gr.right, gr.bottom });
            s->EndFigure(fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            s->Close(); s->Release();
            return p;
        };
        if (ID2D1PathGeometry* f = path(down, true)) { g.br->SetColor(Alpha(g.ink.accent, .22f)); g.rt->FillGeometry(f, g.br); f->Release(); }
        auto line = [&](const float* a, D2D1_COLOR_F c, float w) {
            int n = count;
            for (int i = 1; i < n; i++) {
                int k0 = (head + N - n + i - 1) % N, k1 = (head + N - n + i) % N;
                float x1 = gr.right - (n - 1 - i) * step, x0 = x1 - step, h = gr.bottom - gr.top;
                g.Line(x0, gr.bottom - h * std::min(1.f, a[k0] / mx), x1, gr.bottom - h * std::min(1.f, a[k1] / mx), c, w);
            }
        };
        line(down, g.ink.accent, 1.8f);
        line(up, upCol, 1.4f);
    }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Choice, L"Units", L"", { L"MB/s", L"Mbps" }, [this] { return bits ? 1 : 0; }, [this](int v) { bits = v == 1; } });
        o.push_back({ Opt::Choice, L"Update every", L"", { L"1 s", L"2 s", L"5 s" }, [this] { return every == 1 ? 0 : every == 2 ? 1 : 2; }, [this](int v) { every = v == 0 ? 1 : v == 1 ? 2 : 5; } });
        o.push_back({ Opt::Toggle, L"Show graph", L"The last minute of traffic", {}, [this] { return graph ? 1 : 0; }, [this](int v) { graph = v != 0; } });
    }
    void Menu(HMENU m) override {
        AddCheck(m, 101, L"Show in Mbps", bits);
        AddCheck(m, 102, L"Show graph", graph);
    }
    void Command(int id) override { if (id == 101) bits = !bits; else if (id == 102) graph = !graph; }
    void Load(const JVal& o) override { bits = o.boolean(L"bits", false); graph = o.boolean(L"graph", true); every = Clamp((int)o.num(L"every", 1), 1, 5); }
    void Save(JVal& o) override { o.set(L"bits") = JVal::MakeBool(bits); o.set(L"graph") = JVal::MakeBool(graph); o.set(L"every") = JVal::MakeNum(every); }
};

// ======================================================================= Photo frame
namespace {
bool IsImage(const wchar_t* name) {
    const wchar_t* e = wcsrchr(name, L'.');
    if (!e) return false;
    for (auto x : { L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".heic", L".tif", L".tiff", L".jfif" }) if (!_wcsicmp(e, x)) return true;
    return false;
}
// EXIF orientation -> WIC transform
WICBitmapTransformOptions Orientation(IWICBitmapFrameDecode* fr) {
    IWICMetadataQueryReader* q = nullptr;
    WICBitmapTransformOptions t = WICBitmapTransformRotate0;
    if (SUCCEEDED(fr->GetMetadataQueryReader(&q)) && q) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v)) || SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=274}", &v))) {
            int o = v.vt == VT_UI2 ? v.uiVal : 1;
            switch (o) {
            case 2: t = WICBitmapTransformFlipHorizontal; break;
            case 3: t = WICBitmapTransformRotate180; break;
            case 4: t = WICBitmapTransformFlipVertical; break;
            case 5: t = (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
            case 6: t = WICBitmapTransformRotate90; break;
            case 7: t = (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
            case 8: t = WICBitmapTransformRotate270; break;
            }
        }
        PropVariantClear(&v);
        q->Release();
    }
    return t;
}
// Decodes `path` scaled to cover (fill) or fit inside tw x th pixels, already rotated upright. Premultiplied BGRA.
bool DecodePhoto(const std::wstring& path, int tw, int th, bool fit, std::vector<uint32_t>& px, int& pw, int& ph) {
    IWICImagingFactory* f = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr; IWICBitmapFlipRotator* rot = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f))) break;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT iw = 0, ih = 0; fr->GetSize(&iw, &ih);
        if (!iw || !ih) break;
        WICBitmapTransformOptions o = Orientation(fr);
        bool swap = (o & WICBitmapTransformRotate90) != 0;   // 90 and 270 both have this bit
        double ow = swap ? ih : iw, oh = swap ? iw : ih;
        double s = fit ? std::min(tw / ow, th / oh) : std::max(tw / ow, th / oh);
        UINT sw = (UINT)std::max(1.0, std::round(iw * s)), sh = (UINT)std::max(1.0, std::round(ih * s));   // scale first (small), rotate after
        if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr, sw, sh, WICBitmapInterpolationModeFant))) break;
        if (FAILED(f->CreateFormatConverter(&cv)) || FAILED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        IWICBitmapSource* src = cv;
        if (o != WICBitmapTransformRotate0) {
            if (FAILED(f->CreateBitmapFlipRotator(&rot)) || FAILED(rot->Initialize(cv, o))) break;
            src = rot;
        }
        UINT rw = 0, rh = 0; src->GetSize(&rw, &rh);
        pw = fit ? (int)rw : std::min((int)rw, tw); ph = fit ? (int)rh : std::min((int)rh, th);
        WICRect rc{ ((int)rw - pw) / 2, ((int)rh - ph) / 2, pw, ph };   // centre crop for fill
        px.assign((size_t)pw * ph, 0);
        if (FAILED(src->CopyPixels(&rc, pw * 4, (UINT)(px.size() * 4), (BYTE*)px.data()))) { px.clear(); break; }
        ok = true;
    } while (0);
    SafeRelease(rot); SafeRelease(cv); SafeRelease(sc); SafeRelease(fr); SafeRelease(dec); SafeRelease(f);
    return ok;
}
bool PickPath(bool folder, std::wstring& out) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD fl = 0; d->GetOptions(&fl);
    d->SetOptions(fl | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    d->SetTitle(folder ? L"Choose a folder of photos" : L"Choose a photo");
    if (!folder) {
        COMDLG_FILTERSPEC spec[] = { { L"Images", L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.webp;*.heic;*.tif;*.tiff" }, { L"All files", L"*.*" } };
        d->SetFileTypes(2, spec);
    }
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(pics, nullptr, IID_PPV_ARGS(&si)))) { d->SetDefaultFolder(si); si->Release(); }
        CoTaskMemFree(pics);
    }
    HWND owner = GetForegroundWindow();
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); ok = true; }
            it->Release();
        }
    }
    d->Release();
    return ok;
}
std::wstring BaseName(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(k + 1); }
} // namespace

class PhotoWidget : public Widget {
public:
    std::wstring file, folder, current;   // current = file name shown from the folder
    int fit = 0, every = 2;               // every: manual, 1 min, 10 min, 1 hour, daily
    bool shuffle = true;
    long long lastChange = 0;
    std::vector<std::wstring> list; bool scanned = false;
    // decoded picture (dropped once it is on the GPU-side bitmap)
    std::vector<uint32_t> px; int pw = 0, ph = 0;
    std::wstring shownPath; int shownW = 0, shownH = 0, shownFit = -1; bool failed = false;
    ID2D1Bitmap* bmp = nullptr; int bmpGen = -1;
    bool hover = false;
    PhotoWidget() { key = L"photo"; title = L"Photo Frame"; }
    ~PhotoWidget() override { SafeRelease(bmp); }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 280, 280 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 120, 120 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 900, 900 }; }
    std::wstring Description() override { return L"A favourite photo or a slideshow"; }
    int Interval() override { return folder.empty() || every == 0 ? 3600000 : 60000; }
    long long Period() const { static const long long p[] = { 0, 60, 600, 3600, 86400 }; return p[Clamp(every, 0, 4)] * 1000LL; }

    void Scan() {
        scanned = true; list.clear();
        if (folder.empty()) return;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do { if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) && IsImage(fd.cFileName)) list.push_back(fd.cFileName); } while (FindNextFileW(h, &fd) && list.size() < 5000);
        FindClose(h);
        std::sort(list.begin(), list.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    }
    std::wstring Path() {
        if (folder.empty()) return file;
        if (!scanned) Scan();
        if (list.empty()) return L"";
        if (current.empty() || std::find(list.begin(), list.end(), current) == list.end()) current = list[0];
        return folder + L"\\" + current;
    }
    void Next(int dir = 1) {
        Scan();   // pick up photos added since
        if (list.empty()) return;
        auto it = std::find(list.begin(), list.end(), current);
        int i = it == list.end() ? -1 : (int)(it - list.begin()), n = (int)list.size();
        if (shuffle && n > 2 && dir > 0) {
            static std::mt19937 rng((unsigned)GetTickCount64());
            int j; do j = (int)(rng() % n); while (j == i);
            i = j;
        } else i = ((i < 0 ? 0 : i + dir) % n + n) % n;
        current = list[i];
        lastChange = NowMs();
        dirty = true;
        RequestSave();
    }
    void Tick() override {
        long long p = Period();
        if (!lastChange) lastChange = NowMs();   // first run: show the current photo for a full period
        if (!folder.empty() && p > 0 && NowMs() - lastChange >= p) Next();
    }
    void Draw(Gfx& g, float W, float H) override {
        std::wstring path = Path();
        if (path.empty()) {
            if (hover) g.FillRound(RectF(8, 8, W - 8, H - 8), std::max(4.f, g.ink.radius - 6), g.ink.hover);
            float cy = H / 2;
            g.Icon(0xE91B, RectF(0, cy - 44, W, cy - 6), 26, g.ink.fg2);
            g.Text(folder.empty() ? L"Click to choose a photo" : L"No photos in this folder", RectF(10, cy + 2, W - 10, cy + 24), 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_CENTER);
            g.Text(L"Right-click for a slideshow folder", RectF(10, cy + 24, W - 10, cy + 44), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        float k = scale * size;   // decode at the widget's real pixel size, whatever the preview scale
        int tw = std::max(1, (int)std::ceil(W * k)), th = std::max(1, (int)std::ceil(H * k));
        if (bmp && bmpGen != g_gr.generation) { SafeRelease(bmp); shownPath.clear(); }
        if (path != shownPath || tw != shownW || th != shownH || fit != shownFit) {
            SafeRelease(bmp);
            failed = !DecodePhoto(path, tw, th, fit == 1, px, pw, ph);
            shownPath = path; shownW = tw; shownH = th; shownFit = fit;
        }
        if (!bmp && !px.empty()) {
            D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96 };
            g.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)pw, (UINT32)ph }, px.data(), pw * 4, &bp, &bmp);
            bmpGen = g_gr.generation;
            if (bmp) { px.clear(); px.shrink_to_fit(); }   // keep RAM low: the bitmap holds the picture now
        }
        if (failed || !bmp) {
            g.Icon(0xE7BA, RectF(0, H / 2 - 40, W, H / 2 - 6), 22, g.ink.fg2);
            g.Text(L"Couldn't open this photo", RectF(10, H / 2, W - 10, H / 2 + 22), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_CENTER);
            g.Text(BaseName(path), RectF(10, H / 2 + 22, W - 10, H / 2 + 40), 11, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        float dw = pw / k, dh = ph / k;   // DIP size of the bitmap
        D2D1_RECT_F r = fit == 1 ? RectF((W - dw) / 2, (H - dh) / 2, (W + dw) / 2, (H + dh) / 2) : RectF(0, 0, W, H);
        float rad = fit == 1 ? std::min(10.f, g.ink.radius) : g.ink.radius;
        ID2D1BitmapBrush* bb = nullptr;
        g.rt->CreateBitmapBrush(bmp, &bb);
        if (bb) {
            float sx = (r.right - r.left) / pw, sy = (r.bottom - r.top) / ph;
            D2D1_MATRIX_3X2_F t; t._11 = sx; t._12 = 0; t._21 = 0; t._22 = sy; t._31 = r.left; t._32 = r.top;
            bb->SetTransform(&t);
            bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            D2D1_ROUNDED_RECT rr{ r, rad, rad };
            g.rt->FillRoundedRectangle(&rr, bb);
            bb->Release();
        }
        g.StrokeRound(RectF(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), rad, D2D1_COLOR_F{ 1, 1, 1, .18f }, 1);
    }
    void Release() { SafeRelease(bmp); px.clear(); px.shrink_to_fit(); shownPath.clear(); }
    void ChoosePhoto() { std::wstring p; if (PickPath(false, p)) { file = p; folder.clear(); current.clear(); list.clear(); Release(); RequestSave(); } }
    void ChooseFolder() {
        std::wstring p;
        if (!PickPath(true, p)) return;
        folder = p; current.clear(); scanned = false; Release();
        Scan();
        if (!list.empty()) current = list[shuffle ? (int)(GetTickCount64() % list.size()) : 0];
        lastChange = NowMs();
        RequestSave();
    }
    bool HitId(float, float, int& id) override { id = 0; return false; }
    bool Click(float, float) override {
        if (Path().empty() && folder.empty()) { ChoosePhoto(); return true; }
        if (!folder.empty()) { Next(); return true; }
        return false;
    }
    bool Hover(float, float) override { if (hover) return false; hover = true; return Path().empty(); }
    void Leave() override { hover = false; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Action, L"Photo", file.empty() || !folder.empty() ? L"None" : BaseName(file), { L"Choose…" }, nullptr, [this](int) { ChoosePhoto(); } });
        o.push_back({ Opt::Action, L"Slideshow folder", folder.empty() ? L"Off — shows one photo" : folder + L" · " + std::to_wstring(list.size()) + L" photos",
                      { L"Choose…" }, nullptr, [this](int) { ChooseFolder(); } });
        if (!folder.empty()) {
            o.push_back({ Opt::Choice, L"Change photo", L"Clicking the widget always shows the next one", { L"Click", L"1 min", L"10 min", L"1 hour", L"Daily" },
                          [this] { return every; }, [this](int v) { every = v; lastChange = NowMs(); } });
            o.push_back({ Opt::Toggle, L"Shuffle", L"", {}, [this] { return shuffle ? 1 : 0; }, [this](int v) { shuffle = v != 0; } });
            o.push_back({ Opt::Action, L"Stop the slideshow", L"Go back to a single photo", { L"Stop" }, nullptr, [this](int) { folder.clear(); list.clear(); current.clear(); Release(); } });
        }
        o.push_back({ Opt::Choice, L"Display", L"Fill crops the photo to the widget; Fit shows all of it", { L"Fill", L"Fit" }, [this] { return fit; }, [this](int v) { fit = v; } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING, 101, L"Choose photo…");
        AppendMenuW(m, MF_STRING, 102, L"Choose slideshow folder…");
        if (!folder.empty()) { AppendMenuW(m, MF_STRING, 103, L"Next photo"); AppendMenuW(m, MF_STRING, 104, L"Previous photo"); }
        std::wstring p = Path();
        AppendMenuW(m, MF_STRING | (p.empty() ? MF_GRAYED : 0), 105, L"Open photo");
        AddCheck(m, 106, L"Fit whole photo", fit == 1);
    }
    void Command(int id) override {
        if (id == 101) ChoosePhoto();
        else if (id == 102) ChooseFolder();
        else if (id == 103) Next(1);
        else if (id == 104) { bool s = shuffle; shuffle = false; Next(-1); shuffle = s; }
        else if (id == 105) { std::wstring p = Path(); if (!p.empty()) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        else if (id == 106) fit = fit ? 0 : 1;
    }
    void Load(const JVal& o) override {
        file = o.str(L"file", L""); folder = o.str(L"folder", L""); current = o.str(L"current", L"");
        fit = Clamp((int)o.num(L"fit", 0), 0, 1); every = Clamp((int)o.num(L"every", 2), 0, 4); shuffle = o.boolean(L"shuffle", true);
        lastChange = (long long)o.num(L"lastChange", 0);
    }
    void Save(JVal& o) override {
        o.set(L"file") = JVal::MakeStr(file); o.set(L"folder") = JVal::MakeStr(folder); o.set(L"current") = JVal::MakeStr(current);
        o.set(L"fit") = JVal::MakeNum(fit); o.set(L"every") = JVal::MakeNum(every); o.set(L"shuffle") = JVal::MakeBool(shuffle);
        o.set(L"lastChange") = JVal::MakeNum((double)lastChange);
    }
};

std::unique_ptr<Widget> MakeFocus() { return std::unique_ptr<Widget>(new FocusWidget()); }
std::unique_ptr<Widget> MakeNote() { return std::unique_ptr<Widget>(new NoteWidget()); }
std::unique_ptr<Widget> MakeWorldClock() { return std::unique_ptr<Widget>(new WorldClockWidget()); }
std::unique_ptr<Widget> MakeNetSpeed() { return std::unique_ptr<Widget>(new NetSpeedWidget()); }
std::unique_ptr<Widget> MakePhoto() { return std::unique_ptr<Widget>(new PhotoWidget()); }
