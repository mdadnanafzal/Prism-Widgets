// Built-in widgets (clock, calendar, to-do, system) + JSON custom widgets.
#include "widgets.h"
#include "media.h"

Graphics g_gr;

static D2D1_POINT_2F P(float x, float y) { return D2D1_POINT_2F{ x, y }; }

// ======================================================================= Graphics
bool Graphics::Init() {
    D2D1_FACTORY_OPTIONS opt{ D2D1_DEBUG_LEVEL_NONE };
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)&d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) return false;
    D2D1_STROKE_STYLE_PROPERTIES sp{ D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                     D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f };
    d2d->CreateStrokeStyle(&sp, nullptr, 0, &round);
    return true;
}
bool Graphics::EnsureTarget() {
    if (rt) return true;
    D2D1_RENDER_TARGET_PROPERTIES p{};
    p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;   // CPU raster: no GPU driver / VRAM footprint
    p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    p.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    p.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    if (FAILED(d2d->CreateDCRenderTarget(&p, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_COLOR_F white{ 1, 1, 1, 1 };
    rt->CreateSolidColorBrush(&white, nullptr, &br);
    generation++;
    return br != nullptr;
}
void Graphics::DropTarget() { SafeRelease(br); SafeRelease(rt); }
IDWriteTextFormat* Graphics::Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w) {
    size = std::round(size * 4) / 4;
    for (auto& f : fmts_) if (f.size == size && f.weight == (int)w && f.fam == family) return f.f;
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &tf)))
        return nullptr;
    if (fmts_.size() > 64) ClearFormats();
    fmts_.push_back({ family, size, (int)w, tf });
    return tf;
}
void Graphics::ClearFormats() { for (auto& f : fmts_) SafeRelease(f.f); fmts_.clear(); }

// ======================================================================= Gfx
void Gfx::Text(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, D2D1_COLOR_F c,
               DWRITE_TEXT_ALIGNMENT a, DWRITE_PARAGRAPH_ALIGNMENT v, const wchar_t* family, bool strike, bool wrap) {
    if (s.empty()) return;
    IDWriteTextFormat* f = g_gr.Format(family, size, w);
    if (!f) return;
    IDWriteTextLayout* L = nullptr;
    if (FAILED(g_gr.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(1.f, r.right - r.left), std::max(1.f, r.bottom - r.top), &L))) return;
    L->SetTextAlignment(a);
    L->SetParagraphAlignment(v);
    L->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* sign = nullptr;
    if (!wrap && SUCCEEDED(g_gr.dw->CreateEllipsisTrimmingSign(f, &sign))) {
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        L->SetTrimming(&tr, sign);
    }
    if (strike) { DWRITE_TEXT_RANGE rg{ 0, (UINT32)s.size() }; L->SetStrikethrough(TRUE, rg); }
    if (ink.shadow) {
        const float offs[3][3] = { { 0, 1.5f, .30f }, { 0, .5f, .25f }, { 1, 1, .18f } };
        for (auto& o : offs) {
            br->SetColor(D2D1_COLOR_F{ 0, 0, 0, o[2] * c.a });
            rt->DrawTextLayout(P(r.left + o[0], r.top + o[1]), L, br);
        }
    }
    br->SetColor(c);
    rt->DrawTextLayout(P(r.left, r.top), L, br);
    SafeRelease(sign);
    L->Release();
}
float Gfx::Measure(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, const wchar_t* family) {
    IDWriteTextFormat* f = g_gr.Format(family, size, w);
    IDWriteTextLayout* L = nullptr;
    if (!f || FAILED(g_gr.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 1000.f, &L))) return 0;
    DWRITE_TEXT_METRICS m{}; L->GetMetrics(&m); L->Release();
    return m.widthIncludingTrailingWhitespace;
}
void Gfx::FillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    br->SetColor(c); D2D1_ROUNDED_RECT rr{ r, rad, rad }; rt->FillRoundedRectangle(&rr, br);
}
void Gfx::StrokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float width) {
    br->SetColor(c); D2D1_ROUNDED_RECT rr{ r, rad, rad }; rt->DrawRoundedRectangle(&rr, br, width, nullptr);
}
void Gfx::FillCircle(float x, float y, float r, D2D1_COLOR_F c) {
    br->SetColor(c); D2D1_ELLIPSE e{ P(x, y), r, r }; rt->FillEllipse(&e, br);
}
void Gfx::StrokeCircle(float x, float y, float r, D2D1_COLOR_F c, float width) {
    br->SetColor(c); D2D1_ELLIPSE e{ P(x, y), r, r }; rt->DrawEllipse(&e, br, width, nullptr);
}
void Gfx::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    br->SetColor(c); rt->DrawLine(P(x0, y0), P(x1, y1), br, width, g_gr.round);
}
void Gfx::Arc(float cx, float cy, float r, float startDeg, float sweepDeg, D2D1_COLOR_F c, float width) {
    if (sweepDeg <= 0.05f) return;
    if (sweepDeg >= 359.9f) { StrokeCircle(cx, cy, r, c, width); return; }
    auto pt = [&](float deg) { float a = (deg - 90) * 3.14159265f / 180; return P(cx + r * std::cos(a), cy + r * std::sin(a)); };
    ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* sk = nullptr;
    if (FAILED(g_gr.d2d->CreatePathGeometry(&pg))) return;
    if (SUCCEEDED(pg->Open(&sk))) {
        // Polyline approximation (2.5 deg steps): renders identically on every D2D implementation.
        int n = std::max(2, (int)std::ceil(sweepDeg / 2.5f));
        std::vector<D2D1_POINT_2F> pts(n);
        for (int i = 0; i < n; i++) pts[i] = pt(startDeg + sweepDeg * (i + 1) / n);
        sk->BeginFigure(pt(startDeg), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddLines(pts.data(), (UINT32)n);
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
        sk->Close(); sk->Release();
        br->SetColor(c);
        rt->DrawGeometry(pg, br, width, g_gr.round);
    }
    pg->Release();
}
void Gfx::Icon(wchar_t glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
    bool sh = ink.shadow; ink.shadow = false;
    Text(std::wstring(1, glyph), r, size, DWRITE_FONT_WEIGHT_NORMAL, c, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, FONT_ICONS);
    ink.shadow = sh;
}

static bool In(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void AddCheck(HMENU m, int id, const wchar_t* text, bool on) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, text); }

// ======================================================================= Clock
class ClockWidget : public Widget {
public:
    bool analog = false, seconds = false, showDate = true; int h24 = -1; // -1 = follow Windows
    ClockWidget() { key = L"clock"; title = L"Clock"; }
    bool Use24() { return h24 < 0 ? LocaleUses24h() : h24 == 1; }
    D2D1_SIZE_F DefaultSize() override { return analog ? D2D1_SIZE_F{ 210, 210 } : D2D1_SIZE_F{ 330, 150 }; }
    D2D1_SIZE_F MinSize() override { return analog ? D2D1_SIZE_F{ 130, 130 } : D2D1_SIZE_F{ 170, 80 }; }
    D2D1_SIZE_F MaxSize() override { return analog ? D2D1_SIZE_F{ 440, 440 } : D2D1_SIZE_F{ 680, 330 }; }
    void SetStyle(bool a) { if (a != analog) { analog = a; cw = ch = 0; } }   // each style has its own natural shape
    int Interval() override { return seconds ? 1000 : 60000; }
    void Draw(Gfx& g, float W, float H) override {
        SYSTEMTIME st; GetLocalTime(&st);
        if (analog) { DrawAnalog(g, W, H, st); return; }
        bool t24 = Use24();
        std::wstring tm = FormatDateTime(st, t24 ? L"HH:mm" : L"h:mm");
        // adaptive layout: the time fills the space above the date and shrinks to fit the width
        bool date = showDate && H >= 104;
        float dateH = date ? 44.f : 0.f, left = std::min(24.f, W * 0.08f);
        float big = std::max(24.f, (H - dateH) / 1.48f);
        auto extra = [&](float b) {
            float e = 0;
            if (seconds) e = std::max(e, g.Measure(L"88", b * 0.37f, DWRITE_FONT_WEIGHT_NORMAL, FONT_DISPLAY));
            if (!t24) e = std::max(e, g.Measure(L"PM", std::max(11.f, b * 0.21f), DWRITE_FONT_WEIGHT_SEMI_BOLD));
            return e > 0 ? e + 8 : 0.f;
        };
        float tw = g.Measure(tm, big, DWRITE_FONT_WEIGHT_LIGHT, FONT_DISPLAY), avail = W - 2 * left;
        float need = tw + extra(big);
        if (need > avail && need > 0) { big *= avail / need; tw = g.Measure(tm, big, DWRITE_FONT_WEIGHT_LIGHT, FONT_DISPLAY); }
        float top = std::max(2.f, (H - dateH - big * 1.33f) / 2 + (date ? 4.f : 0.f));
        g.Text(tm, RectF(left, top, W, top + big * 1.4f), big, DWRITE_FONT_WEIGHT_LIGHT, g.ink.fg,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_DISPLAY);
        float sx = left + tw + 8;
        if (!t24) { float ap = std::max(11.f, big * 0.21f); g.Text(st.wHour < 12 ? L"AM" : L"PM", RectF(sx, top + big * 0.36f, W, top + big * 0.36f + ap * 1.5f), ap, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2); }
        if (seconds) {
            wchar_t b[8]; swprintf(b, 8, L"%02d", st.wSecond);
            float sz = big * 0.37f, sTop = top + big * 1.06f - sz * 1.06f;
            g.Text(b, RectF(sx, sTop, W, sTop + sz * 1.4f), sz, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_DISPLAY);
        }
        if (date) {
            const wchar_t* fmt = W >= 430 ? L"dddd, d MMMM yyyy" : W >= 250 ? L"dddd, d MMMM" : L"ddd, d MMM";
            g.Text(FormatDateTime(st, fmt), RectF(left + 3, H - 46, W - left, H - 16), 16, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2);
        }
    }
    void DrawAnalog(Gfx& g, float W, float H, const SYSTEMTIME& st) {
        float cx = W / 2, cy = H / 2, R = std::min(W, H) / 2 - std::min(22.f, std::min(W, H) * 0.1f);
        for (int i = 0; i < 60; i++) {
            float a = i * 6 * 3.14159265f / 180, s = std::sin(a), c = -std::cos(a);
            bool hour = i % 5 == 0, q = i % 15 == 0;
            if (!hour) { g.FillCircle(cx + s * R, cy + c * R, 0.9f, g.ink.fg3); continue; }
            float len = q ? 12 : 8;
            g.Line(cx + s * R, cy + c * R, cx + s * (R - len), cy + c * (R - len), q ? g.ink.fg : g.ink.fg2, q ? 3.f : 2.f);
        }
        if (showDate) {
            std::wstring d = FormatDateTime(st, L"ddd d");
            for (auto& ch : d) ch = towupper(ch);
            g.Text(d, RectF(cx - 50, cy + R * 0.32f, cx + 50, cy + R * 0.32f + 20), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        float sec = st.wSecond, mn = st.wMinute + sec / 60.f, hr = (st.wHour % 12) + mn / 60.f;
        auto hand = [&](float frac, float len, float tail, float wdt, D2D1_COLOR_F c) {
            float a = frac * 2 * 3.14159265f, s = std::sin(a), co = -std::cos(a);
            g.Line(cx - s * tail, cy - co * tail, cx + s * len, cy + co * len, c, wdt);
        };
        hand(hr / 12.f, R * 0.52f, 0, 6, g.ink.fg);
        hand(mn / 60.f, R * 0.78f, 0, 4, g.ink.fg);
        if (seconds) hand(sec / 60.f, R * 0.88f, R * 0.16f, 1.6f, g.ink.accent);
        g.FillCircle(cx, cy, 5, g.ink.accent);
        g.FillCircle(cx, cy, 2, g.ink.onAccent);
    }
    std::wstring Description() override { return L"Digital or analog time with the date"; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Choice, L"Style", L"", { L"Digital", L"Analog" }, [this] { return analog ? 1 : 0; }, [this](int v) { SetStyle(v == 1); } });
        o.push_back({ Opt::Toggle, L"Show seconds", L"Redraws every second instead of once a minute", {}, [this] { return seconds ? 1 : 0; }, [this](int v) { seconds = v != 0; } });
        o.push_back({ Opt::Toggle, L"24-hour time", L"", {}, [this] { return Use24() ? 1 : 0; }, [this](int v) { h24 = v ? 1 : 0; } });
        o.push_back({ Opt::Toggle, L"Show date", L"", {}, [this] { return showDate ? 1 : 0; }, [this](int v) { showDate = v != 0; } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING | (!analog ? MF_CHECKED : 0), 101, L"Digital");
        AppendMenuW(m, MF_STRING | (analog ? MF_CHECKED : 0), 102, L"Analog");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AddCheck(m, 103, L"Show seconds", seconds);
        AddCheck(m, 104, L"24-hour time", Use24());
        AddCheck(m, 105, L"Show date", showDate);
    }
    void Command(int id) override {
        if (id == 101) SetStyle(false); else if (id == 102) SetStyle(true);
        else if (id == 103) seconds = !seconds;
        else if (id == 104) h24 = Use24() ? 0 : 1;
        else if (id == 105) showDate = !showDate;
    }
    void Load(const JVal& o) override {
        analog = o.str(L"style", L"digital") == L"analog"; seconds = o.boolean(L"seconds", false);
        showDate = o.boolean(L"date", true); h24 = (int)o.num(L"h24", -1);
    }
    void Save(JVal& o) override {
        o.set(L"style") = JVal::MakeStr(analog ? L"analog" : L"digital"); o.set(L"seconds") = JVal::MakeBool(seconds);
        o.set(L"date") = JVal::MakeBool(showDate); o.set(L"h24") = JVal::MakeNum(h24);
    }
};

// ======================================================================= Calendar
class CalendarWidget : public Widget {
public:
    int offset = 0;       // months from today
    int hoverPart = -1;   // 0 prev, 1 next, 2 header, 10+ day cell
    int firstDay = -1;    // -1 = locale
    CalendarWidget() { key = L"calendar"; title = L"Calendar"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 300, 300 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 150, 120 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 600, 600 }; }
    static bool Compact(float W, float H) { return W < 236 || H < 228; }
    static bool Weeks(float W) { return W >= 370; }
    struct GridGeo { float gx, cw, gy, ch; };
    static GridGeo Grid(float W, float H) {
        GridGeo r; r.gx = Weeks(W) ? 44.f : 12.f; r.cw = (W - r.gx - 12) / 7; r.gy = 82; r.ch = (H - r.gy - 12) / 6; return r;
    }
    static long long DayIndex(int y, int m, int d) {
        SYSTEMTIME s{}; s.wYear = (WORD)y; s.wMonth = (WORD)m; s.wDay = (WORD)d; FILETIME f; SystemTimeToFileTime(&s, &f);
        return (long long)((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 864000000000ULL);
    }
    static SYSTEMTIME FromIndex(long long idx) {
        ULONGLONG v = (ULONGLONG)idx * 864000000000ULL; FILETIME f{ (DWORD)v, (DWORD)(v >> 32) }; SYSTEMTIME s; FileTimeToSystemTime(&f, &s); return s;
    }
    static int IsoWeek(long long idx) {   // ISO-8601 week of the day with this index
        SYSTEMTIME s = FromIndex(idx);
        int dow = (s.wDayOfWeek + 6) % 7;
        long long thu = idx - dow + 3;
        SYSTEMTIME t = FromIndex(thu);
        return (int)((thu - DayIndex(t.wYear, 1, 1)) / 7) + 1;
    }
    void DrawCompact(Gfx& g, float W, float H) {
        SYSTEMTIME now; GetLocalTime(&now);
        std::wstring wd = FormatDateTime(now, L"dddd"); for (auto& c : wd) c = towupper(c);
        g.Text(wd, RectF(16, 12, W - 12, 32), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.accentText);
        float big = Clamp(std::min(H - 70, W * 0.6f), 30.f, 140.f);
        g.Text(std::to_wstring(now.wDay), RectF(14, 28, W - 10, 28 + big * 1.3f), big, DWRITE_FONT_WEIGHT_LIGHT, g.ink.fg,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_DISPLAY);
        g.Text(FormatDateTime(now, W >= 190 ? L"MMMM yyyy" : L"MMM yyyy"), RectF(16, H - 36, W - 12, H - 14), 13.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2);
    }
    int FirstDay() { return firstDay < 0 ? LocaleFirstDayOfWeek() : firstDay; } // 0 = Monday
    static int DaysIn(int y, int m) { static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1]; }
    static int Dow(int y, int m, int d) { // 0 = Monday
        SYSTEMTIME s{}; s.wYear = (WORD)y; s.wMonth = (WORD)m; s.wDay = (WORD)d; FILETIME f; SystemTimeToFileTime(&s, &f); FileTimeToSystemTime(&f, &s);
        return (s.wDayOfWeek + 6) % 7;
    }
    void View(int& y, int& m) {
        SYSTEMTIME st; GetLocalTime(&st);
        int t = st.wYear * 12 + (st.wMonth - 1) + offset; y = t / 12; m = t % 12 + 1;
    }
    D2D1_RECT_F Prev(float W) { return RectF(W - 76, 14, W - 46, 44); }
    D2D1_RECT_F Next(float W) { return RectF(W - 44, 14, W - 14, 44); }
    void Draw(Gfx& g, float W, float H) override {
        if (Compact(W, H)) { DrawCompact(g, W, H); return; }
        SYSTEMTIME now; GetLocalTime(&now);
        int y, m; View(y, m);
        SYSTEMTIME first{}; first.wYear = (WORD)y; first.wMonth = (WORD)m; first.wDay = 1;
        g.Text(FormatDateTime(first, L"MMMM yyyy"), RectF(20, 16, W - 80, 44), 17, DWRITE_FONT_WEIGHT_SEMI_BOLD,
               offset == 0 ? g.ink.fg : g.ink.accentText);
        if (hoverPart == 0) g.FillRound(Prev(W), 8, g.ink.hover);
        if (hoverPart == 1) g.FillRound(Next(W), 8, g.ink.hover);
        g.Icon(0xE76B, Prev(W), 11, g.ink.fg2);
        g.Icon(0xE76C, Next(W), 11, g.ink.fg2);
        GridGeo G = Grid(W, H);
        float gx = G.gx, cw = G.cw, gy = G.gy, ch = G.ch;
        int fd = FirstDay();
        for (int i = 0; i < 7; i++) {
            int dow = (fd + i) % 7; // 0 Monday
            SYSTEMTIME s{}; s.wYear = 2024; s.wMonth = 1; s.wDay = (WORD)(1 + dow); // 2024-01-01 is a Monday
            std::wstring nm = FormatDateTime(s, L"ddd"); if (nm.size() > 2) nm = nm.substr(0, 2);
            g.Text(nm, RectF(gx + i * cw, 56, gx + (i + 1) * cw, 76), 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        int lead = (Dow(y, m, 1) - fd + 7) % 7;
        if (Weeks(W)) {   // ISO week numbers (taken from each row's Thursday)
            g.Text(L"Wk", RectF(10, 56, gx - 4, 76), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_CENTER);
            long long first = DayIndex(y, m, 1);
            int thuCol = (3 - fd + 7) % 7;
            for (int r = 0; r < 6; r++)
                g.Text(std::to_wstring(IsoWeek(first + r * 7 + thuCol - lead)), RectF(10, gy + r * ch, gx - 4, gy + (r + 1) * ch), 11,
                       DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        int dim = DaysIn(y, m), py = m == 1 ? y - 1 : y, pm = m == 1 ? 12 : m - 1, pdim = DaysIn(py, pm);
        for (int c = 0; c < 42; c++) {
            int day = c - lead + 1; bool cur = day >= 1 && day <= dim;
            int shown = cur ? day : (day < 1 ? pdim + day : day - dim);
            float x0 = gx + (c % 7) * cw, y0 = gy + (c / 7) * ch, ccx = x0 + cw / 2, ccy = y0 + ch / 2;
            bool today = cur && y == now.wYear && m == now.wMonth && day == now.wDay;
            float rr = std::min(cw, ch) / 2 - 2;
            if (today) g.FillCircle(ccx, ccy, rr, g.ink.accent);
            else if (hoverPart == 10 + c) g.FillCircle(ccx, ccy, rr, g.ink.hover);
            g.Text(std::to_wstring(shown), RectF(x0, y0, x0 + cw, y0 + ch), 13.5f,
                   today ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                   today ? g.ink.onAccent : (cur ? g.ink.fg : g.ink.fg3),
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    int Part(float x, float y) {
        float W = BaseSize().width, H = BaseSize().height;
        if (Compact(W, H)) return -1;
        if (In(Prev(W), x, y)) return 0;
        if (In(Next(W), x, y)) return 1;
        if (y >= 14 && y < 44 && x < W - 80) return 2;
        GridGeo G = Grid(W, H);
        float gx = G.gx, cw = G.cw, gy = G.gy, ch = G.ch;
        if (x >= gx && x < W - 12 && y >= gy && y < H - 12) return 10 + (int)((y - gy) / ch) * 7 + (int)((x - gx) / cw);
        return -1;
    }
    bool Click(float x, float y) override {
        int p = Part(x, y);
        if (p == 0) { offset--; return true; }
        if (p == 1) { offset++; return true; }
        if (p == 2 && offset) { offset = 0; return true; }
        return false;
    }
    bool Wheel(float, float, int d) override { if (Compact(BaseSize().width, BaseSize().height)) return false; offset += d > 0 ? -1 : 1; return true; }
    bool Hover(float x, float y) override { int p = Part(x, y); if (p == hoverPart) return false; hoverPart = p; return true; }
    void Leave() override { hoverPart = -1; }
    std::wstring Description() override { return L"Month view with today highlighted"; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Choice, L"Week starts on", L"Automatic follows your Windows region settings", { L"Automatic", L"Monday", L"Sunday" },
                      [this] { return firstDay < 0 ? 0 : firstDay == 0 ? 1 : 2; }, [this](int v) { firstDay = v == 0 ? -1 : v == 1 ? 0 : 6; } });
        o.push_back({ Opt::Action, L"Go to today", L"", { L"Today" }, nullptr, [this](int) { offset = 0; } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING | (offset == 0 ? MF_GRAYED : 0), 101, L"Go to today");
        AddCheck(m, 102, L"Week starts on Monday", FirstDay() == 0);
        AddCheck(m, 103, L"Week starts on Sunday", FirstDay() == 6);
    }
    void Command(int id) override { if (id == 101) offset = 0; else if (id == 102) firstDay = 0; else if (id == 103) firstDay = 6; }
    void Load(const JVal& o) override { firstDay = (int)o.num(L"firstDay", -1); }
    void Save(JVal& o) override { o.set(L"firstDay") = JVal::MakeNum(firstDay); }
};

// ======================================================================= To-do
struct TodoItem { std::wstring text; bool done = false; };
static std::vector<TodoItem> g_todos;
static std::wstring TodoPath() { return AppDataDir() + L"\\todo.json"; }
void LoadTodos() {
    g_todos.clear();
    JVal v; bool ok = false; std::wstring t = ReadTextFile(TodoPath(), &ok);
    if (!ok) {  // first run
        g_todos = { { L"Right-click a widget for options", false }, { L"Right-click a widget \u2192 Arrange widgets to move them", false }, { L"Try the Liquid Glass theme", true } };
        return;
    }
    if (JsonParse(t, v) && v.isArr())
        for (auto& e : v.a) if (e.isObj()) g_todos.push_back({ e.str(L"text", L""), e.boolean(L"done", false) });
}
static void SaveTodos() {
    JVal a = JVal::MakeArr();
    for (auto& t : g_todos) { JVal o = JVal::MakeObj(); o.set(L"text") = JVal::MakeStr(t.text); o.set(L"done") = JVal::MakeBool(t.done); a.a.push_back(o); }
    WriteTextFile(TodoPath(), JsonWrite(a));
}
int TodoOpenCount() { int n = 0; for (auto& t : g_todos) if (!t.done) n++; return n; }

class TodoWidget : public Widget {
public:
    int scroll = 0, hover = -1; // hover: row index, -2 = add row
    bool hideDone = false;
    const float top = 56, rowH = 34;
    TodoWidget() { key = L"todo"; title = L"To-Do"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 300, 330 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 220, 160 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 600, 900 }; }
    std::vector<int> Visible() { std::vector<int> v; for (int i = 0; i < (int)g_todos.size(); i++) if (!hideDone || !g_todos[i].done) v.push_back(i); return v; }
    int Rows() { return (int)((BaseSize().height - 52 - top) / rowH); }
    D2D1_RECT_F AddRect() { float W = BaseSize().width, H = BaseSize().height; return RectF(12, H - 48, W - 12, H - 12); }
    void Draw(Gfx& g, float W, float H) override {
        g.Text(L"To Do", RectF(20, 16, W - 100, 44), 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg);
        int open = TodoOpenCount();
        g.Text(open ? std::to_wstring(open) + L" left" : L"All done", RectF(W - 120, 20, W - 20, 44), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_TRAILING);
        auto vis = Visible();
        int rows = Rows();
        scroll = Clamp(scroll, 0, std::max(0, (int)vis.size() - rows));
        if (vis.empty()) g.Text(L"Nothing here yet", RectF(20, top + 30, W - 20, top + 60), 14, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (int r = 0; r < rows && scroll + r < (int)vis.size(); r++) {
            int i = vis[scroll + r]; auto& t = g_todos[i];
            float y0 = top + r * rowH, cy = y0 + rowH / 2;
            if (hover == scroll + r) g.FillRound(RectF(12, y0 + 1, W - 12, y0 + rowH - 1), 9, g.ink.hover);
            if (t.done) { g.FillCircle(32, cy, 9, g.ink.accent); g.Icon(0xE73E, RectF(22, cy - 10, 42, cy + 10), 10, g.ink.onAccent); }
            else g.StrokeCircle(32, cy, 8.5f, g.ink.fg2, 1.5f);
            g.Text(t.text, RectF(52, y0, W - 22, y0 + rowH), 14, DWRITE_FONT_WEIGHT_NORMAL, t.done ? g.ink.fg3 : g.ink.fg,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, FONT_TEXT, t.done);
        }
        if ((int)vis.size() > rows) { // scrollbar
            float trackT = top + 4, trackH = rows * rowH - 8, th = std::max(16.f, trackH * rows / vis.size());
            float ty = trackT + (trackH - th) * scroll / std::max(1, (int)vis.size() - rows);
            g.FillRound(RectF(W - 9, ty, W - 6, ty + th), 1.5f, g.ink.fg3);
        }
        D2D1_RECT_F ar = AddRect();
        if (hover == -2) g.FillRound(ar, 9, g.ink.hover);
        g.Icon(0xE710, RectF(22, ar.top, 42, ar.bottom), 12, g.ink.accentText);
        g.Text(L"Add a task", RectF(52, ar.top, W - 20, ar.bottom), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.accentText,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    int Hit(float x, float y) {
        if (In(AddRect(), x, y)) return -2;
        float W = BaseSize().width;
        if (x < 12 || x > W - 12 || y < top) return -1;
        int r = (int)((y - top) / rowH);
        if (r >= Rows()) return -1;
        int idx = scroll + r;
        return idx < (int)Visible().size() ? idx : -1;
    }
    int ItemAt(int hit) { auto v = Visible(); return hit >= 0 && hit < (int)v.size() ? v[hit] : -1; }
    bool Click(float x, float y) override {
        int h = Hit(x, y);
        if (h == -2) {
            ShowInput(this, AddRect(), L"", [this](const std::wstring& s) {
                if (s.empty()) return;
                g_todos.push_back({ s, false }); SaveTodos();
                scroll = 1 << 20; // jump to end
                RequestRedraw(this);
            });
            return true;
        }
        int i = ItemAt(h);
        if (i >= 0) { g_todos[i].done = !g_todos[i].done; SaveTodos(); return true; }
        return false;
    }
    int menuItem = -1;
    bool RightClickAt(float x, float y, HMENU m) override {
        menuItem = ItemAt(Hit(x, y));
        if (menuItem < 0) return false;
        AppendMenuW(m, MF_STRING, 150, L"Edit task");
        AppendMenuW(m, MF_STRING, 151, L"Delete task");
        AppendMenuW(m, MF_STRING, 152, L"Move up");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        return true;
    }
    bool Wheel(float, float, int d) override { int old = scroll; scroll -= d > 0 ? 1 : -1; scroll = Clamp(scroll, 0, std::max(0, (int)Visible().size() - Rows())); return old != scroll; }
    bool Hover(float x, float y) override { int h = Hit(x, y); if (h == hover) return false; hover = h; return true; }
    void Leave() override { hover = -1; }
    std::wstring Description() override { return L"Quick tasks you can tick off"; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Toggle, L"Hide completed tasks", L"", {}, [this] { return hideDone ? 1 : 0; }, [this](int v) { hideDone = v != 0; } });
        int done = (int)g_todos.size() - TodoOpenCount();
        o.push_back({ Opt::Action, L"Clear completed tasks", done ? std::to_wstring(done) + L" finished task(s) will be removed" : L"No finished tasks",
                      { L"Clear" }, nullptr, [this](int) { Command(101); } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING, 101, L"Clear completed");
        AddCheck(m, 102, L"Hide completed", hideDone);
    }
    void Command(int id) override {
        if (id == 101) { std::vector<TodoItem> k; for (auto& t : g_todos) if (!t.done) k.push_back(t); g_todos.swap(k); SaveTodos(); }
        else if (id == 102) hideDone = !hideDone;
        else if (id == 151 && menuItem >= 0 && menuItem < (int)g_todos.size()) { g_todos.erase(g_todos.begin() + menuItem); SaveTodos(); }
        else if (id == 152 && menuItem > 0 && menuItem < (int)g_todos.size()) { std::swap(g_todos[menuItem], g_todos[menuItem - 1]); SaveTodos(); }
        else if (id == 150 && menuItem >= 0 && menuItem < (int)g_todos.size()) {
            auto v = Visible(); int row = -1;
            for (int r = 0; r < (int)v.size(); r++) if (v[r] == menuItem) row = r - scroll;
            if (row < 0 || row >= Rows()) row = 0;
            int idx = menuItem;
            ShowInput(this, RectF(12, top + row * rowH, BaseSize().width - 12, top + (row + 1) * rowH), g_todos[idx].text,
                      [this, idx](const std::wstring& s) {
                          if (idx < (int)g_todos.size() && !s.empty()) { g_todos[idx].text = s; SaveTodos(); RequestRedraw(this); }
                      });
        }
    }
    void Load(const JVal& o) override { hideDone = o.boolean(L"hideDone", false); }
    void Save(JVal& o) override { o.set(L"hideDone") = JVal::MakeBool(hideDone); }
};

// ======================================================================= Media (now playing)
class MediaWidget : public Widget {
public:
    bool showArt = true, showProgress = true;
    int hover = -1;                 // 0 prev, 1 play/pause, 2 next
    ID2D1Bitmap* bmp = nullptr; int bmpGen = -1, bmpVer = -1;
    MediaWidget() { key = L"media"; title = L"Media Controls"; }
    ~MediaWidget() override { SafeRelease(bmp); }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 360, 156 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 200, 130 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 620, 480 }; }
    int Interval() override { return g_media.Busy() ? 250 : (g_media.st.playing ? 1000 : 2000); }
    std::wstring Description() override { return L"Now playing, with play/pause, next and previous"; }
    // Adaptive layout: 0 normal (art left), 1 narrow (small art), 2 tall album card (art on top)
    struct Geo { int mode; D2D1_RECT_F art; float x0, x1, yApp, yTitle, yArtist, titleSz, yProg, yBtn, gap, bx0, bx1; bool prog; };
    Geo Lay(float W, float H) {
        Geo G{};
        G.mode = (H >= 250 && W <= H * 1.35f) ? 2 : (W >= 280 ? 0 : 1);
        G.titleSz = G.mode == 1 ? 14.5f : 16.5f;
        G.yBtn = H - (G.mode == 1 ? 30.f : 34.f);
        if (G.mode == 2) {
            float side = showArt ? std::max(0.f, std::min(W - 28, H - 176)) : 0;
            G.art = RectF((W - side) / 2, 14, (W + side) / 2, 14 + side);
            G.x0 = 18; G.x1 = W - 18;
            float ty = showArt ? 14 + side + 10 : 18;
            G.yApp = ty; G.yTitle = ty + 16; G.yArtist = ty + 40;
            G.bx0 = G.x0; G.bx1 = G.x1;
        } else if (G.mode == 0) {
            float side = std::min(std::min(H - 28, W * 0.42f), 230.f);
            G.art = RectF(14, (H - side) / 2, 14 + side, (H + side) / 2);
            G.x0 = showArt ? 28 + side : 22; G.x1 = W - 18;
            G.yApp = 14; G.yTitle = 30; G.yArtist = 54;
            G.bx0 = G.x0; G.bx1 = W - 16;
        } else {
            G.art = RectF(14, 14, 54, 54);
            G.x0 = showArt ? 64 : 18; G.x1 = W - 16;
            G.yApp = 14; G.yTitle = 28; G.yArtist = 48;
            G.bx0 = 16; G.bx1 = W - 16;
        }
        G.yProg = G.yBtn - 38;
        G.prog = showProgress && G.yProg >= G.yArtist + 26;
        G.gap = std::min(58.f, (G.bx1 - G.bx0) / 3.2f);
        return G;
    }
    D2D1_POINT_2F Btn(const Geo& G, int i) { return D2D1_POINT_2F{ (G.bx0 + G.bx1) / 2 + (i - 1) * G.gap, G.yBtn }; }
    static std::wstring Time(long long t) {
        long long sec = t / 10000000; wchar_t b[32];
        if (sec >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
        else swprintf(b, 32, L"%lld:%02lld", sec / 60, sec % 60);
        return b;
    }
    void Draw(Gfx& g, float W, float H) override {
        g_media.Pump();
        const MediaState& m = g_media.st;
        Geo G = Lay(W, H);
        if (showArt && G.art.right - G.art.left > 4) {
            D2D1_RECT_F ar = G.art;
            float rad = std::min(14.f, (ar.right - ar.left) * 0.12f);
            if (!m.art.empty()) {
                if (bmp && (bmpGen != g_gr.generation || bmpVer != m.artVersion)) SafeRelease(bmp);
                if (!bmp) {
                    D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96 };
                    g.rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)m.artW, (UINT32)m.artH }, m.art.data(), m.artW * 4, &bp, &bmp);
                    bmpGen = g_gr.generation; bmpVer = m.artVersion;
                }
            } else SafeRelease(bmp);
            if (bmp) {
                ID2D1BitmapBrush* bb = nullptr;
                g.rt->CreateBitmapBrush(bmp, &bb);
                if (bb) {
                    float k = (ar.right - ar.left) / m.artW;
                    D2D1_MATRIX_3X2_F t; t._11 = k; t._12 = 0; t._21 = 0; t._22 = k; t._31 = ar.left; t._32 = ar.top;
                    bb->SetTransform(&t);
                    D2D1_ROUNDED_RECT rr{ ar, rad, rad };
                    g.rt->FillRoundedRectangle(&rr, bb);
                    bb->Release();
                }
                g.StrokeRound(ar, rad, D2D1_COLOR_F{ 1, 1, 1, .12f }, 1);
            } else {
                g.FillRound(ar, rad, Alpha(g.ink.accent, .28f));
                g.StrokeRound(ar, rad, Alpha(g.ink.fg, .10f), 1);
                g.Icon(0xE8D6, ar, std::max(14.f, (ar.right - ar.left) * 0.31f), Alpha(g.ink.fg, .75f));
            }
        }
        float x0 = G.x0, x1 = G.x1;
        std::wstring app = m.hasSession ? m.app : (m.available ? L"Media" : L"Media keys");
        for (auto& c : app) c = towupper(c);
        g.Text(app, RectF(x0, G.yApp, x1 - 20, G.yApp + 16), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg3);
        if (m.playing) {   // tiny equaliser glyph
            float ey = G.yApp + 12;
            for (int i = 0; i < 3; i++) { float h = (i == 1 ? 9.f : 6.f); g.FillRound(RectF(x1 - 14 + i * 5, ey - h, x1 - 11 + i * 5, ey), 1, g.ink.accentText); }
        }
        bool has = m.hasSession && !m.title.empty();
        g.Text(has ? m.title : L"Nothing playing", RectF(x0, G.yTitle, x1, G.yTitle + G.titleSz * 1.45f), G.titleSz, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg);
        g.Text(has ? m.artist : L"Spotify, YouTube, Media Player…", RectF(x0, G.yArtist, x1, G.yArtist + 20), 13, DWRITE_FONT_WEIGHT_NORMAL, has ? g.ink.fg2 : g.ink.fg3);
        if (G.prog && m.hasSession && m.end > 0) {
            long long pos = g_media.Position();
            float f = Clamp((float)((double)pos / (double)m.end), 0.f, 1.f), by = G.yProg;
            float px0 = G.mode == 0 ? x0 : G.bx0, px1 = G.mode == 0 ? x1 : G.bx1;
            g.FillRound(RectF(px0, by, px1, by + 4), 2, Alpha(g.ink.fg, .16f));
            if (f > 0) g.FillRound(RectF(px0, by, px0 + std::max(4.f, (px1 - px0) * f), by + 4), 2, g.ink.fg);
            g.Text(Time(pos), RectF(px0, by + 7, px0 + 80, by + 21), 10.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3);
            g.Text(Time(m.end), RectF(px1 - 80, by + 7, px1, by + 21), 10.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        const wchar_t glyphs[3] = { 0xE892, (wchar_t)(m.playing ? 0xE769 : 0xE768), 0xE893 };
        bool en[3] = { !m.hasSession || m.canPrev, !m.hasSession || m.canPlayPause, !m.hasSession || m.canNext };
        for (int i = 0; i < 3; i++) {
            D2D1_POINT_2F c = Btn(G, i);
            if (i == 1) {
                g.FillCircle(c.x, c.y, 21, hover == 1 ? Alpha(g.ink.accent, .85f) : g.ink.accent);
                g.Icon(glyphs[i], RectF(c.x - 20, c.y - 20, c.x + 20 + (m.playing ? 0 : 1.5f), c.y + 20), 16, g.ink.onAccent);
            } else {
                if (hover == i && en[i]) g.FillCircle(c.x, c.y, 18, g.ink.hover);
                g.Icon(glyphs[i], RectF(c.x - 18, c.y - 18, c.x + 18, c.y + 18), 15, en[i] ? g.ink.fg : g.ink.fg3);
            }
        }
    }
    int Hit(float x, float y) {
        D2D1_SIZE_F bs = BaseSize(); Geo G = Lay(bs.width, bs.height);
        for (int i = 0; i < 3; i++) { D2D1_POINT_2F c = Btn(G, i); float dx = x - c.x, dy = y - c.y; if (dx * dx + dy * dy <= 22 * 22) return i; }
        return -1;
    }
    bool Click(float x, float y) override {
        int h = Hit(x, y);
        if (h == 0) g_media.Prev(); else if (h == 1) g_media.PlayPause(); else if (h == 2) g_media.Next();
        return h >= 0;
    }
    bool Hover(float x, float y) override { int h = Hit(x, y); if (h == hover) return false; hover = h; return true; }
    void Leave() override { hover = -1; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Toggle, L"Show album art", L"", {}, [this] { return showArt ? 1 : 0; }, [this](int v) { showArt = v != 0; } });
        o.push_back({ Opt::Toggle, L"Show progress bar", L"When the app reports the track position", {}, [this] { return showProgress ? 1 : 0; }, [this](int v) { showProgress = v != 0; } });
    }
    void Menu(HMENU m) override {
        AddCheck(m, 101, L"Show album art", showArt);
        AddCheck(m, 102, L"Show progress bar", showProgress);
    }
    void Command(int id) override { if (id == 101) showArt = !showArt; else if (id == 102) showProgress = !showProgress; }
    void Load(const JVal& o) override { showArt = o.boolean(L"art", true); showProgress = o.boolean(L"progress", true); }
    void Save(JVal& o) override { o.set(L"art") = JVal::MakeBool(showArt); o.set(L"progress") = JVal::MakeBool(showProgress); }
};

// ======================================================================= System
class SystemWidget : public Widget {
public:
    SystemWidget() { key = L"system"; title = L"System Monitor"; }
    std::wstring Description() override { return L"CPU, memory and battery (or disk) rings"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 330, 150 }; }
    D2D1_SIZE_F MinSize() override { return D2D1_SIZE_F{ 170, 120 }; }
    D2D1_SIZE_F MaxSize() override { return D2D1_SIZE_F{ 640, 440 }; }
    int Interval() override { return 2000; }
    void Draw(Gfx& g, float W, float H) override {
        g_sys.Update();
        float vals[3]; const wchar_t* labels[3]; wchar_t sub[3][48];
        vals[0] = g_sys.cpu; labels[0] = L"CPU"; swprintf(sub[0], 48, L"");
        vals[1] = g_sys.ram; labels[1] = L"Memory"; swprintf(sub[1], 48, L"%.1f / %.0f GB", g_sys.ramUsedGB, g_sys.ramTotalGB);
        if (g_sys.battery >= 0) { vals[2] = g_sys.battery; labels[2] = g_sys.charging ? L"Charging" : L"Battery"; swprintf(sub[2], 48, L""); }
        else {
            ULARGE_INTEGER fr{}, tot{}, tf{}; vals[2] = 0;
            if (GetDiskFreeSpaceExW(L"C:\\", &fr, &tot, &tf) && tot.QuadPart) vals[2] = (float)(100.0 * (tot.QuadPart - tf.QuadPart) / tot.QuadPart);
            labels[2] = L"Disk C:"; swprintf(sub[2], 48, L"%.0f GB free", tf.QuadPart / 1073741824.0);
        }
        if (W >= H * 1.5f) {   // rings in a row
            float colW = (W - 24) / 3;
            float r = std::max(16.f, std::min(colW / 2 - 12, (H - 72) / 2));
            float sw = std::max(4.f, r * 0.17f), cy = (H - 44) / 2 + 6;
            for (int i = 0; i < 3; i++) {
                float cx = 12 + colW * (i + 0.5f);
                g.StrokeCircle(cx, cy, r, Alpha(g.ink.fg, 0.14f), sw);
                g.Arc(cx, cy, r, 0, 360.f * Clamp(vals[i], 0.f, 100.f) / 100.f, g.ink.accent, sw);
                wchar_t v[16]; swprintf(v, 16, L"%.0f%%", vals[i]);
                g.Text(v, RectF(cx - r, cy - r, cx + r, cy + r), std::max(11.f, r * 0.44f), DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g.Text(labels[i], RectF(cx - colW / 2, cy + r + 8, cx + colW / 2, cy + r + 26), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (sub[i][0] && cy + r + 42 <= H) g.Text(sub[i], RectF(cx - colW / 2, cy + r + 25, cx + colW / 2, cy + r + 42), 11, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        } else {               // list with bars
            float rowH = (H - 24) / 3;
            for (int i = 0; i < 3; i++) {
                float y0 = 12 + i * rowH, by = y0 + rowH - 14;
                wchar_t v[16]; swprintf(v, 16, L"%.0f%%", vals[i]);
                g.Text(labels[i], RectF(18, y0, W - 70, by - 2), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (sub[i][0] && W >= 240) g.Text(sub[i], RectF(18 + g.Measure(labels[i], 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 8, y0, W - 70, by - 2), 11, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg3, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g.Text(v, RectF(W - 80, y0, W - 18, by - 2), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g.FillRound(RectF(18, by, W - 18, by + 6), 3, Alpha(g.ink.fg, .14f));
                float f = Clamp(vals[i], 0.f, 100.f) / 100.f;
                if (f > 0) g.FillRound(RectF(18, by, 18 + std::max(6.f, (W - 36) * f), by + 6), 3, g.ink.accent);
            }
        }
    }
};

// ======================================================================= Custom (JSON)
class CustomWidget : public Widget {
public:
    struct Elem {
        std::wstring type, text, font, color, fill, stroke, track, align, valign, click, value, image;
        float x = 0, y = 0, w = 0, h = 0, r = 0, x2 = 0, y2 = 0, size = 14, radius = 0, thick = 2, max = 100;
        int weight = 400; bool wrap = false; wchar_t glyph = 0;
        ID2D1Bitmap* bmp = nullptr; int bmpGen = -1; float bmpScale = 0;
    };
    std::wstring file, error, jsonTheme; float W = 260, H = 120; int refresh = 60000;
    std::vector<Elem> els; int hoverEl = -1;

    CustomWidget(const std::wstring& f) : file(f) {
        key = L"custom:" + f; title = f;
        size_t dot = title.rfind(L'.'); if (dot != std::wstring::npos) title = title.substr(0, dot);
        Parse();
    }
    ~CustomWidget() override { for (auto& e : els) SafeRelease(e.bmp); }
    bool IsCustom() override { return true; }
    std::wstring DefaultTheme() override { return jsonTheme; }
    std::wstring FilePath() override { return CustomWidgetDir() + L"\\" + file; }
    static int Weight(const std::wstring& s) {
        if (s.empty()) return 400;
        if (iswdigit(s[0])) return _wtoi(s.c_str());
        if (s == L"thin") return 100; if (s == L"light") return 300; if (s == L"medium") return 500;
        if (s == L"semibold") return 600; if (s == L"bold") return 700; if (s == L"black") return 900;
        return 400;
    }
    void Parse() {
        bool ok = false; std::wstring t = ReadTextFile(FilePath(), &ok);
        JVal j; std::wstring err;
        if (!ok) { error = L"Cannot read file"; return; }
        if (!JsonParse(t, j, &err) || !j.isObj()) { error = err.empty() ? L"Root must be an object" : err; return; }
        title = j.str(L"name", title.c_str());
        W = (float)Clamp(j.num(L"width", 260), 40.0, 2000.0); H = (float)Clamp(j.num(L"height", 120), 30.0, 2000.0);
        refresh = (int)j.num(L"refresh", 60000); if (refresh != 0 && refresh < 1000) refresh = 1000;
        theme = jsonTheme = j.str(L"theme", L"");
        const JVal* a = j.get(L"elements");
        if (a && a->isArr()) for (auto& o : a->a) {
            if (!o.isObj()) continue;
            Elem e;
            e.type = o.str(L"type", L"text"); e.text = o.str(L"text", L""); e.font = o.str(L"font", L"");
            e.color = o.str(L"color", L"fg"); e.fill = o.str(L"fill", L""); e.stroke = o.str(L"stroke", L"");
            e.track = o.str(L"track", L""); e.align = o.str(L"align", L"left"); e.valign = o.str(L"valign", L"top");
            e.click = o.str(L"click", L""); e.value = o.str(L"value", L"0"); e.image = o.str(L"path", L"");
            e.x = (float)o.num(L"x", 0); e.y = (float)o.num(L"y", 0); e.w = (float)o.num(L"w", 0); e.h = (float)o.num(L"h", 0);
            e.r = (float)o.num(L"r", 20); e.x2 = (float)o.num(L"x2", e.x); e.y2 = (float)o.num(L"y2", e.y);
            e.size = (float)o.num(L"size", 14); e.radius = (float)o.num(L"radius", 8); e.thick = (float)o.num(L"thickness", 2);
            e.max = (float)o.num(L"max", 100); e.weight = Weight(o.str(L"weight", L"")); e.wrap = o.boolean(L"wrap", false);
            std::wstring gl = o.str(L"glyph", L""); if (!gl.empty()) e.glyph = (wchar_t)wcstoul(gl.c_str(), nullptr, 16);
            if (e.w <= 0) e.w = W - e.x - 12; if (e.h <= 0) e.h = e.type == L"text" || e.type == L"button" ? e.size * 1.5f : 10;
            els.push_back(e);
        }
    }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ W, H }; }
    int Interval() override { return refresh <= 0 ? 3600000 : refresh; }
    D2D1_COLOR_F Col(Gfx& g, const std::wstring& s, D2D1_COLOR_F def) {
        if (s.empty()) return def;
        if (s == L"fg") return g.ink.fg; if (s == L"fg2") return g.ink.fg2; if (s == L"fg3") return g.ink.fg3;
        if (s == L"accent") return g.ink.accent; if (s == L"onaccent") return g.ink.onAccent; if (s == L"hover") return g.ink.hover;
        bool ok; D2D1_COLOR_F c = ParseColor(s, &ok); return ok ? c : def;
    }
    D2D1_RECT_F Bounds(const Elem& e) {
        if (e.type == L"circle" || e.type == L"ring") return RectF(e.x - e.r, e.y - e.r, e.x + e.r, e.y + e.r);
        return RectF(e.x, e.y, e.x + e.w, e.y + e.h);
    }
    void DrawImage(Gfx& g, Elem& e) {
        float sc = scale * size;
        if (e.bmp && (e.bmpGen != g_gr.generation || e.bmpScale != sc)) SafeRelease(e.bmp);
        if (!e.bmp) {
            std::wstring path = e.image;
            if (path.size() < 2 || path[1] != L':') path = CustomWidgetDir() + L"\\" + path;
            IWICImagingFactory* f = nullptr; IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr;
            IWICBitmapScaler* s = nullptr; IWICFormatConverter* c = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
                SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
                SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateBitmapScaler(&s)) &&
                SUCCEEDED(s->Initialize(fr, std::max(1u, (UINT)(e.w * sc)), std::max(1u, (UINT)(e.h * sc)), WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(f->CreateFormatConverter(&c)) &&
                SUCCEEDED(c->Initialize(s, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                g.rt->CreateBitmapFromWicBitmap(c, nullptr, &e.bmp);
            SafeRelease(c); SafeRelease(s); SafeRelease(fr); SafeRelease(d); SafeRelease(f);
            e.bmpGen = g_gr.generation; e.bmpScale = sc;
        }
        if (e.bmp) { D2D1_RECT_F r = Bounds(e); g.rt->DrawBitmap(e.bmp, &r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr); }
    }
    void Draw(Gfx& g, float, float) override {
        if (!error.empty()) {
            g.Text(L"\u26A0 " + title, RectF(14, 12, W - 14, 34), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.ink.fg);
            g.Text(error, RectF(14, 34, W - 14, H - 8), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_TEXT, false, true);
            return;
        }
        for (int i = 0; i < (int)els.size(); i++) {
            Elem& e = els[i];
            D2D1_RECT_F b = Bounds(e);
            auto align = e.align == L"center" ? DWRITE_TEXT_ALIGNMENT_CENTER : e.align == L"right" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
            auto valign = e.valign == L"middle" || e.valign == L"center" ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : e.valign == L"bottom" ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            const wchar_t* fam = e.font.empty() ? (e.size >= 28 ? FONT_DISPLAY : FONT_TEXT) : e.font.c_str();
            float v = (float)_wtof(ExpandVars(e.value).c_str()), frac = e.max > 0 ? Clamp(v / e.max, 0.f, 1.f) : 0;
            if (e.type == L"text") {
                g.Text(ExpandVars(e.text), b, e.size, (DWRITE_FONT_WEIGHT)e.weight, e.color == L"accent" ? g.ink.accentText : Col(g, e.color, g.ink.fg), align, valign, fam, false, e.wrap);
            } else if (e.type == L"icon") {
                g.Icon(e.glyph, b, e.size, Col(g, e.color, g.ink.fg));
            } else if (e.type == L"rect") {
                if (!e.fill.empty() || e.stroke.empty()) g.FillRound(b, e.radius, Col(g, e.fill.empty() ? e.color : e.fill, g.ink.hover));
                if (!e.stroke.empty()) g.StrokeRound(b, e.radius, Col(g, e.stroke, g.ink.fg3), e.thick);
            } else if (e.type == L"circle") {
                if (!e.fill.empty() || e.stroke.empty()) g.FillCircle(e.x, e.y, e.r, Col(g, e.fill.empty() ? e.color : e.fill, g.ink.accent));
                if (!e.stroke.empty()) g.StrokeCircle(e.x, e.y, e.r, Col(g, e.stroke, g.ink.fg3), e.thick);
            } else if (e.type == L"line") {
                g.Line(e.x, e.y, e.x2, e.y2, Col(g, e.color, g.ink.fg3), e.thick);
            } else if (e.type == L"bar") {
                float rad = std::min(e.radius, e.h / 2);
                g.FillRound(b, rad, Col(g, e.track, Alpha(g.ink.fg, .14f)));
                if (frac > 0) g.FillRound(RectF(b.left, b.top, b.left + std::max(e.h, e.w * frac), b.bottom), rad, Col(g, e.color == L"fg" ? L"accent" : e.color, g.ink.accent));
            } else if (e.type == L"ring") {
                g.StrokeCircle(e.x, e.y, e.r, Col(g, e.track, Alpha(g.ink.fg, .14f)), e.thick);
                g.Arc(e.x, e.y, e.r, 0, 360 * frac, Col(g, e.color == L"fg" ? L"accent" : e.color, g.ink.accent), e.thick);
            } else if (e.type == L"button") {
                D2D1_COLOR_F f = Col(g, e.fill, Alpha(g.ink.fg, .10f));
                if (hoverEl == i) f = Alpha(f, 1.8f);
                g.FillRound(b, e.radius, f);
                if (e.glyph) {
                    float gs = e.size * 1.15f;
                    if (e.text.empty()) g.Icon(e.glyph, b, gs, Col(g, e.color, g.ink.fg));
                    else {
                        g.Icon(e.glyph, RectF(b.left + 8, b.top, b.left + 8 + gs * 1.6f, b.bottom), gs, Col(g, e.color, g.ink.fg));
                        g.Text(ExpandVars(e.text), RectF(b.left + 10 + gs * 1.6f, b.top, b.right - 8, b.bottom), e.size, (DWRITE_FONT_WEIGHT)e.weight,
                               Col(g, e.color, g.ink.fg), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, fam);
                    }
                } else g.Text(ExpandVars(e.text), b, e.size, (DWRITE_FONT_WEIGHT)e.weight, Col(g, e.color, g.ink.fg), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, fam);
            } else if (e.type == L"image") {
                DrawImage(g, e);
            }
        }
    }
    int HitEl(float x, float y) {
        for (int i = (int)els.size() - 1; i >= 0; i--) if (!els[i].click.empty() && In(Bounds(els[i]), x, y)) return i;
        return -1;
    }
    bool Click(float x, float y) override { int i = HitEl(x, y); if (i < 0) return false; Launch(ExpandVars(els[i].click)); return false; }
    bool Hover(float x, float y) override { int i = HitEl(x, y); if (i == hoverEl) return false; hoverEl = i; return true; }
    void Leave() override { hoverEl = -1; }
    std::wstring Description() override { return error.empty() ? L"Custom widget \u00B7 " + file : L"Error: " + error; }
    void Options(std::vector<Opt>& o) override {
        o.push_back({ Opt::Action, L"Widget file", file, { L"Edit" }, nullptr, [this](int) { Command(101); } });
        o.push_back({ Opt::Action, L"Widgets folder", L"Add or remove .json files here", { L"Open" }, nullptr, [this](int) { Command(102); } });
    }
    void Menu(HMENU m) override {
        AppendMenuW(m, MF_STRING, 101, L"Edit widget file");
        AppendMenuW(m, MF_STRING, 102, L"Open widgets folder");
    }
    void Command(int id) override {
        if (id == 101) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + FilePath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        if (id == 102) ShellExecuteW(nullptr, L"open", CustomWidgetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
};

std::unique_ptr<Widget> MakeClock() { return std::unique_ptr<Widget>(new ClockWidget()); }
std::unique_ptr<Widget> MakeCalendar() { return std::unique_ptr<Widget>(new CalendarWidget()); }
std::unique_ptr<Widget> MakeTodo() { return std::unique_ptr<Widget>(new TodoWidget()); }
std::unique_ptr<Widget> MakeMedia() { return std::unique_ptr<Widget>(new MediaWidget()); }
std::unique_ptr<Widget> MakeSystem() { return std::unique_ptr<Widget>(new SystemWidget()); }
std::unique_ptr<Widget> MakeCustom(const std::wstring& f) { return std::unique_ptr<Widget>(new CustomWidget(f)); }

// ======================================================================= samples
static const wchar_t* kSampleGreeting = LR"JSON({
  // Custom widget example. Edit me! Changes appear instantly.
  // Variables: {time:HH:mm} {date:dddd, d MMMM} {greeting} {user} {cpu} {ram}
  //            {battery} {disk:C} {diskfree:C} {uptime} {countdown:2027-01-01} {todo}
  "name": "Greeting",
  "width": 340,
  "height": 120,
  "refresh": 60000,
  "elements": [
    { "type": "text", "x": 22, "y": 18, "text": "{greeting},", "size": 15, "color": "fg2" },
    { "type": "text", "x": 22, "y": 38, "text": "{user}", "size": 32, "weight": "semibold" },
    { "type": "text", "x": 22, "y": 84, "text": "You have {todo} tasks left today", "size": 13, "color": "fg3" }
  ]
})JSON";

static const wchar_t* kSampleStats = LR"JSON({
  "name": "System Bars",
  "width": 300,
  "height": 150,
  "refresh": 2000,
  "elements": [
    { "type": "text", "x": 20, "y": 16, "text": "Performance", "size": 15, "weight": "semibold" },
    { "type": "text", "x": 20, "y": 50, "w": 120, "text": "CPU", "size": 12.5, "color": "fg2" },
    { "type": "text", "x": 180, "y": 50, "w": 100, "text": "{cpu}%", "size": 12.5, "align": "right" },
    { "type": "bar",  "x": 20, "y": 70, "w": 260, "h": 6, "value": "{cpu}", "max": 100, "color": "accent" },
    { "type": "text", "x": 20, "y": 92, "w": 120, "text": "Memory", "size": 12.5, "color": "fg2" },
    { "type": "text", "x": 140, "y": 92, "w": 140, "text": "{ramused} / {ramtotal} GB", "size": 12.5, "align": "right" },
    { "type": "bar",  "x": 20, "y": 112, "w": 260, "h": 6, "value": "{ram}", "max": 100, "color": "#FF8C42" }
  ]
})JSON";

static const wchar_t* kSampleLauncher = LR"JSON({
  // Buttons launch apps, folders or links. "glyph" = Segoe Fluent Icons code point.
  "name": "Launcher",
  "width": 300,
  "height": 96,
  "refresh": 0,
  "elements": [
    { "type": "button", "x": 16,  "y": 18, "w": 60, "h": 60, "radius": 14, "glyph": "E774", "size": 20, "click": "https://www.google.com" },
    { "type": "button", "x": 86,  "y": 18, "w": 60, "h": 60, "radius": 14, "glyph": "EC50", "size": 20, "click": "explorer.exe" },
    { "type": "button", "x": 156, "y": 18, "w": 60, "h": 60, "radius": 14, "glyph": "E713", "size": 20, "click": "ms-settings:" },
    { "type": "button", "x": 226, "y": 18, "w": 60, "h": 60, "radius": 14, "glyph": "E70B", "size": 20, "click": "notepad.exe" }
  ]
})JSON";

static const wchar_t* kSampleCountdown = LR"JSON({
  "name": "Countdown",
  "width": 220,
  "height": 130,
  "refresh": 60000,
  "elements": [
    { "type": "text", "x": 0, "y": 18, "w": 220, "text": "New Year 2027", "size": 13, "color": "fg2", "align": "center" },
    { "type": "text", "x": 0, "y": 36, "w": 220, "text": "{countdown:2027-01-01}", "size": 48, "weight": "light", "align": "center" },
    { "type": "text", "x": 0, "y": 98, "w": 220, "text": "days to go", "size": 12, "color": "accent", "weight": "semibold", "align": "center" }
  ]
})JSON";

void WriteSampleWidgets() {
    std::wstring d = CustomWidgetDir();
    struct { const wchar_t* n; const wchar_t* c; } s[] = {
        { L"Greeting.json", kSampleGreeting }, { L"System Bars.json", kSampleStats },
        { L"Launcher.json", kSampleLauncher }, { L"Countdown.json", kSampleCountdown } };
    for (auto& x : s) { std::wstring p = d + L"\\" + x.n; if (!FileExists(p)) WriteTextFile(p, x.c); }
}
