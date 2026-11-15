// Prism Widgets - Settings window ("control centre").
// Custom-drawn with Direct2D (software) in an immediate-mode style: every paint lays out the page and
// registers clickable regions. The window and all its buffers are freed when it is closed.
#include "app.h"
#include <psapi.h>
#include <shellscalingapi.h>

namespace {

D2D1_COLOR_F C(float r, float g, float b, float a = 1) { return D2D1_COLOR_F{ r, g, b, a }; }
D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1_RECT_F{ l, t, r, b }; }
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

struct Pal { D2D1_COLOR_F side, bg, card, cardHi, border, sep, text, text2, text3, hover, track, field, danger; bool dark; };
Pal MakePal(bool dark) {
    Pal p; p.dark = dark;
    if (dark) {
        p.side = C(.125f, .125f, .135f); p.bg = C(.098f, .098f, .106f); p.card = C(.157f, .157f, .168f); p.cardHi = C(.19f, .19f, .20f);
        p.border = C(1, 1, 1, .08f); p.sep = C(1, 1, 1, .07f); p.text = C(.95f, .95f, .96f); p.text2 = C(.95f, .95f, .96f, .62f);
        p.text3 = C(.95f, .95f, .96f, .40f); p.hover = C(1, 1, 1, .06f); p.track = C(1, 1, 1, .18f); p.field = C(1, 1, 1, .06f);
        p.danger = C(1.f, .42f, .40f);
    } else {
        p.side = C(.925f, .925f, .935f); p.bg = C(.965f, .965f, .972f); p.card = C(1, 1, 1); p.cardHi = C(.985f, .985f, .99f);
        p.border = C(0, 0, 0, .09f); p.sep = C(0, 0, 0, .07f); p.text = C(.09f, .09f, .10f); p.text2 = C(.09f, .09f, .10f, .62f);
        p.text3 = C(.09f, .09f, .10f, .42f); p.hover = C(0, 0, 0, .05f); p.track = C(0, 0, 0, .16f); p.field = C(0, 0, 0, .05f);
        p.danger = C(.85f, .2f, .18f);
    }
    return p;
}
bool UiDark() {
    if (g_uiTheme == L"dark") return true;
    if (g_uiTheme == L"light") return false;
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

enum Page { P_WIDGETS, P_DETAIL, P_APPEARANCE, P_GENERAL, P_CUSTOM, P_PERF };
struct Hot { D2D1_RECT_F r; int id; std::function<void()> act; std::function<void(float, bool)> drag; };
struct Preview { std::wstring key; std::vector<uint32_t> px; int w = 0, h = 0; float scale = 0; ID2D1Bitmap* bmp = nullptr; int gen = -1; };

struct State {
    HWND hwnd = nullptr; HDC mem = nullptr; HBITMAP dib = nullptr; HGDIOBJ old = nullptr; int cw = 0, ch = 0;
    float s = 1;                       // DPI scale
    Page page = P_WIDGETS; std::wstring detailKey;
    float scroll = 0, contentH = 0, viewH = 0;
    int hover = -1, pressed = -1, nextId = 0;
    std::vector<Hot> hots;
    D2D1_RECT_F clip{};
    std::vector<Preview> previews;     // widget previews by key; theme previews use key "theme:<id>"
    std::vector<std::pair<std::wstring, float>> need;   // previews to render after this paint
    Pal p; D2D1_COLOR_F accent;
    Gfx* g = nullptr;
    std::function<void(float, bool)> dragFn; D2D1_RECT_F dragRect{}; float dragT = 0;   // active slider drag
} S;

const float SIDE_W = 232;

// A tiny widget used for the theme swatches
class SampleWidget : public Widget {
public:
    SampleWidget() { key = L"__sample"; title = L"Sample"; }
    D2D1_SIZE_F DefaultSize() override { return D2D1_SIZE_F{ 176, 104 }; }
    void Draw(Gfx& g, float W, float) override {
        SYSTEMTIME st; GetLocalTime(&st);
        g.Text(FormatDateTime(st, LocaleUses24h() ? L"HH:mm" : L"h:mm"), R(16, 10, W, 60), 38, DWRITE_FONT_WEIGHT_LIGHT, g.ink.fg,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, FONT_DISPLAY);
        g.Text(FormatDateTime(st, L"dddd"), R(18, 62, W - 30, 82), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, g.ink.fg2);
        g.FillCircle(W - 22, 72, 6, g.ink.accent);
    }
};
SampleWidget g_sample;

// ------------------------------------------------------------------ immediate-mode helpers
int NewId() { return S.nextId++; }
void AddHot(D2D1_RECT_F r, int id, std::function<void()> act) {
    D2D1_RECT_F c{ std::max(r.left, S.clip.left), std::max(r.top, S.clip.top), std::min(r.right, S.clip.right), std::min(r.bottom, S.clip.bottom) };
    if (c.right <= c.left || c.bottom <= c.top) return;
    S.hots.push_back({ c, id, std::move(act), nullptr });
}
void AddDrag(D2D1_RECT_F r, int id, std::function<void(float, bool)> drag) {
    D2D1_RECT_F c{ std::max(r.left, S.clip.left), std::max(r.top, S.clip.top), std::min(r.right, S.clip.right), std::min(r.bottom, S.clip.bottom) };
    if (c.right <= c.left || c.bottom <= c.top) return;
    S.hots.push_back({ c, id, nullptr, std::move(drag) });
    S.hots.back().r = r;   // keep the full track for value mapping
}
bool IsHover(int id) { return S.hover == id; }
void T(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, D2D1_COLOR_F c, int align = 0, bool vcenter = true, bool wrap = false, const wchar_t* fam = FONT_TEXT) {
    S.g->Text(s, r, size, w, c, align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING,
              vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR, fam, false, wrap);
}
float TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL) { return S.g->Measure(s, size, w); }
D2D1_COLOR_F OnAccent() { float l = .2126f * S.accent.r + .7152f * S.accent.g + .0722f * S.accent.b; return l > .62f ? C(.05f, .05f, .05f) : C(1, 1, 1); }

Widget* FindWidget(const std::wstring& key) { for (auto& w : g_widgets) if (w->key == key) return w.get(); return nullptr; }

void Toggle(float right, float cy, bool on, std::function<void()> act) {
    int id = NewId();
    D2D1_RECT_F r = R(right - 42, cy - 12, right, cy + 12);
    if (on) S.g->FillRound(r, 12, S.accent);
    else { S.g->FillRound(r, 12, IsHover(id) ? S.p.hover : S.p.field); S.g->StrokeRound(R(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), 11.5f, S.p.track, 1); }
    float kx = on ? r.right - 12 : r.left + 12;
    S.g->FillCircle(kx, cy, on ? 8.5f : 7.5f, on ? OnAccent() : S.p.text2);
    AddHot(R(r.left - 4, r.top - 4, r.right + 4, r.bottom + 4), id, std::move(act));
}

// Slider: t in [0,1]; fn(t, done) is called while dragging and once on release
void Slider(float x0, float x1, float cy, float t, std::function<void(float, bool)> fn) {
    int id = NewId();
    bool active = IsHover(id) || S.pressed == id;
    S.g->FillRound(R(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, S.p.track);
    float kx = x0 + (x1 - x0) * Clamp(t, 0.f, 1.f);
    S.g->FillRound(R(x0, cy - 2.5f, kx, cy + 2.5f), 2.5f, S.accent);
    S.g->FillCircle(kx, cy, active ? 10.f : 9.f, C(1, 1, 1));
    S.g->StrokeCircle(kx, cy, active ? 10.f : 9.f, S.p.dark ? C(0, 0, 0, .35f) : C(0, 0, 0, .18f), 1);
    AddDrag(R(x0 - 10, cy - 14, x1 + 10, cy + 14), id, std::move(fn));
}
float SliderT(const D2D1_RECT_F& r, float x) { return Clamp((x - (r.left + 10)) / std::max(1.f, (r.right - 10) - (r.left + 10)), 0.f, 1.f); }

float SegmentedWidth(const std::vector<std::wstring>& ch) { float w = 0; for (auto& c : ch) w += std::max(56.f, TextW(c, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 26); return w + 4; }
void Segmented(float right, float cy, const std::vector<std::wstring>& ch, int sel, std::function<void(int)> pick) {
    float w = SegmentedWidth(ch), x = right - w;
    S.g->FillRound(R(x, cy - 15, right, cy + 15), 8, S.p.field);
    x += 2;
    for (int i = 0; i < (int)ch.size(); i++) {
        int id = NewId();
        float sw = std::max(56.f, TextW(ch[i], 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 26);
        D2D1_RECT_F r = R(x, cy - 13, x + sw, cy + 13);
        if (i == sel) { S.g->FillRound(r, 6.5f, S.p.dark ? C(1, 1, 1, .16f) : C(1, 1, 1)); S.g->StrokeRound(r, 6.5f, S.p.border, 1); }
        else if (IsHover(id)) S.g->FillRound(r, 6.5f, S.p.hover);
        T(ch[i], r, 13, i == sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, i == sel ? S.p.text : S.p.text2, 1);
        AddHot(r, id, [pick, i] { pick(i); });
        x += sw;
    }
}

float Button(float right, float cy, const std::wstring& text, bool primary, std::function<void()> act, wchar_t glyph = 0) {
    int id = NewId();
    float w = TextW(text, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 30 + (glyph ? 22 : 0);
    D2D1_RECT_F r = R(right - w, cy - 16, right, cy + 16);
    bool hv = IsHover(id);
    if (primary) S.g->FillRound(r, 8, hv ? Alpha(S.accent, .88f) : S.accent);
    else { S.g->FillRound(r, 8, hv ? S.p.cardHi : S.p.field); S.g->StrokeRound(r, 8, S.p.border, 1); }
    D2D1_COLOR_F tc = primary ? OnAccent() : S.p.text;
    float tx = r.left + 15;
    if (glyph) { S.g->Icon(glyph, R(tx - 2, r.top, tx + 18, r.bottom), 13, tc); tx += 22; }
    T(text, R(tx, r.top, r.right - 12, r.bottom), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, tc);
    AddHot(r, id, std::move(act));
    return w;
}

// Grouped rows (macOS-style inset list)
struct RowSpec {
    std::wstring label, hint;
    std::function<void(D2D1_RECT_F)> control;  // draws in the row's rect
    float h = 0;                                // 0 = automatic
    bool stacked = false;                       // control goes under the label, full width
    D2D1_COLOR_F hintColor{ 0, 0, 0, 0 };
};
float Group(float x0, float x1, float y, const std::wstring& title, std::vector<RowSpec> rows) {
    if (!title.empty()) { T(title, R(x0 + 4, y, x1, y + 22), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text2, 0, true); y += 28; }
    float total = 0;
    for (auto& r : rows) { if (!r.h) r.h = r.hint.empty() ? 52 : 64; total += r.h; }
    D2D1_RECT_F card = R(x0, y, x1, y + total);
    S.g->FillRound(card, 12, S.p.card);
    S.g->StrokeRound(R(card.left + .5f, card.top + .5f, card.right - .5f, card.bottom - .5f), 12, S.p.border, 1);
    float ry = y;
    for (size_t i = 0; i < rows.size(); i++) {
        auto& r = rows[i];
        if (i) S.g->Line(x0 + 18, ry, x1 - 18, ry, S.p.sep, 1);
        if (r.stacked) {
            T(r.label, R(x0 + 18, ry + 12, x1 - 18, ry + 34), 14, DWRITE_FONT_WEIGHT_NORMAL, S.p.text, 0, true);
            if (!r.hint.empty()) T(r.hint, R(x0 + 18, ry + 32, x1 - 18, ry + 50), 12, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2, 0, true);
            if (r.control) r.control(R(x0 + 18, ry + (r.hint.empty() ? 40 : 56), x1 - 18, ry + r.h - 14));
        } else {
            float cy = ry + r.h / 2;
            if (r.hint.empty()) T(r.label, R(x0 + 18, ry, x1 - 200, ry + r.h), 14, DWRITE_FONT_WEIGHT_NORMAL, S.p.text, 0, true);
            else {
                T(r.label, R(x0 + 18, cy - 21, x1 - 200, cy + 1), 14, DWRITE_FONT_WEIGHT_NORMAL, S.p.text, 0, true);
                T(r.hint, R(x0 + 18, cy + 1, x1 - 220, cy + 21), 12, DWRITE_FONT_WEIGHT_NORMAL, r.hintColor.a > 0 ? r.hintColor : S.p.text2, 0, true);
            }
            if (r.control) r.control(R(x0 + 18, ry, x1 - 18, ry + r.h));
        }
        ry += r.h;
    }
    return ry + 26;
}

// ------------------------------------------------------------------ previews
Preview* FindPreview(const std::wstring& key) { for (auto& p : S.previews) if (p.key == key) return &p; return nullptr; }
void DropPreviews(const std::wstring& key = L"") {
    for (auto it = S.previews.begin(); it != S.previews.end();) {
        if (key.empty() || it->key == key) { SafeRelease(it->bmp); it = S.previews.erase(it); } else ++it;
    }
}
void DropWidgetPreviews() {
    for (auto it = S.previews.begin(); it != S.previews.end();) {
        if (it->key.compare(0, 6, L"theme:") != 0) { SafeRelease(it->bmp); it = S.previews.erase(it); } else ++it;
    }
}
// Draws the preview for `key` fitted (and centred) in `box`; queues rendering if not ready.
void DrawPreview(const std::wstring& key, float scale, D2D1_RECT_F box) {
    Preview* p = FindPreview(key);
    if (!p || p->scale != scale) {
        bool queued = false; for (auto& n : S.need) if (n.first == key) queued = true;
        if (!queued) S.need.push_back({ key, scale });
        if (!p) {  // placeholder shimmer
            float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
            S.g->FillRound(R(cx - 50, cy - 26, cx + 50, cy + 26), 12, S.p.field);
            return;
        }
    }
    if (p->bmp && p->gen != g_gr.generation) SafeRelease(p->bmp);
    if (!p->bmp && !p->px.empty()) {
        D2D1_BITMAP_PROPERTIES bp{ { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.f * S.s, 96.f * S.s };
        S.g->rt->CreateBitmap(D2D1_SIZE_U{ (UINT32)p->w, (UINT32)p->h }, p->px.data(), p->w * 4, &bp, &p->bmp);
        p->gen = g_gr.generation;
    }
    if (!p->bmp) return;
    float w = p->w / S.s, h = p->h / S.s;
    float k = std::min(1.f, std::min((box.right - box.left) / w, (box.bottom - box.top) / h));
    w *= k; h *= k;
    float x = (box.left + box.right - w) / 2, y = (box.top + box.bottom - h) / 2;
    D2D1_RECT_F d = R(x, y, x + w, y + h);
    S.g->rt->DrawBitmap(p->bmp, &d, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
}
void RenderQueued() {
    auto need = std::move(S.need); S.need.clear();
    for (auto& n : need) {
        Preview pv; pv.key = n.first; pv.scale = n.second;
        bool ok = false;
        if (n.first.compare(0, 6, L"theme:") == 0) {
            for (auto& w : g_widgets) if (w->enabled && w->placed) { g_sample.x = w->x; g_sample.y = w->y; g_sample.placed = true; break; }
            ok = RenderPreview(&g_sample, FindTheme(n.first.substr(6)), n.second, pv.px, pv.w, pv.h);
        } else if (Widget* w = FindWidget(n.first)) ok = RenderPreview(w, ThemeFor(w), n.second, pv.px, pv.w, pv.h);
        DropPreviews(n.first);
        if (ok) S.previews.push_back(std::move(pv));
    }
}

// ------------------------------------------------------------------ actions
void Changed(Widget* w) { ApplyWidget(w); DropPreviews(w->key); }
void GlobalChanged(bool rebuild, bool themes) { ApplyGlobal(rebuild); if (themes) DropPreviews(); else DropWidgetPreviews(); }
void Go(Page p, const std::wstring& key = L"") { S.page = p; S.detailKey = key; S.scroll = 0; S.hover = -1; }

// ------------------------------------------------------------------ chrome
void ApplyFrame() {
    if (!S.hwnd) return;
    Pal p = MakePal(UiDark());
    BOOL dark = p.dark;
    DwmSetWindowAttribute(S.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = RGB((int)(p.side.r * 255), (int)(p.side.g * 255), (int)(p.side.b * 255));
    COLORREF txt = RGB((int)(p.text.r * 255), (int)(p.text.g * 255), (int)(p.text.b * 255));
    DwmSetWindowAttribute(S.hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(S.hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof(txt));
}

void DrawLogo(float x, float y, float sz) {
    ID2D1GradientStopCollection* stops = nullptr; ID2D1LinearGradientBrush* lg = nullptr;
    D2D1_GRADIENT_STOP gs[2] = { { 0, C(.25f, .52f, 1.f) }, { 1, C(.75f, .35f, .95f) } };
    S.g->rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops);
    if (stops) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp{ D2D1_POINT_2F{ x, y }, D2D1_POINT_2F{ x + sz, y + sz } };
        S.g->rt->CreateLinearGradientBrush(&lp, nullptr, stops, &lg);
    }
    D2D1_ROUNDED_RECT rr{ R(x, y, x + sz, y + sz), sz * .26f, sz * .26f };
    if (lg) S.g->rt->FillRoundedRectangle(&rr, lg);
    SafeRelease(lg); SafeRelease(stops);
    float cx = x + sz / 2, cy = y + sz * .54f;
    ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* sk = nullptr;
    if (SUCCEEDED(g_gr.d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&sk))) {
        sk->BeginFigure(D2D1_POINT_2F{ cx, cy - sz * .26f }, D2D1_FIGURE_BEGIN_FILLED);
        D2D1_POINT_2F pts[2] = { { cx + sz * .25f, cy + sz * .18f }, { cx - sz * .25f, cy + sz * .18f } };
        sk->AddLines(pts, 2); sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
        S.g->br->SetColor(C(1, 1, 1, .95f)); S.g->rt->FillGeometry(pg, S.g->br, nullptr);
    }
    SafeRelease(pg);
}

void DrawSidebar(float H) {
    D2D1_RECT_F sr = R(0, 0, SIDE_W, H);
    S.g->br->SetColor(S.p.side);
    S.g->rt->FillRectangle(&sr, S.g->br);
    S.g->Line(SIDE_W - .5f, 0, SIDE_W - .5f, H, S.p.sep, 1);
    DrawLogo(20, 18, 34);
    T(APP_NAME, R(64, 16, SIDE_W - 10, 36), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text);
    T(L"Desktop widgets \u00B7 v" APP_VERSION, R(64, 35, SIDE_W - 10, 53), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text3);

    struct Item { Page p; wchar_t glyph; const wchar_t* name; };
    const Item items[] = { { P_WIDGETS, 0xE80A, L"Widgets" }, { P_APPEARANCE, 0xE790, L"Appearance" }, { P_GENERAL, 0xE713, L"General" },
                           { P_CUSTOM, 0xE943, L"Custom widgets" }, { P_PERF, 0xE9D9, L"Performance" } };
    float y = 78;
    for (auto& it : items) {
        int id = NewId();
        bool sel = S.page == it.p || (it.p == P_WIDGETS && S.page == P_DETAIL);
        D2D1_RECT_F r = R(12, y, SIDE_W - 12, y + 38);
        if (sel) S.g->FillRound(r, 8, S.accent); else if (IsHover(id)) S.g->FillRound(r, 8, S.p.hover);
        D2D1_COLOR_F c = sel ? OnAccent() : S.p.text;
        S.g->Icon(it.glyph, R(22, y, 46, y + 38), 15, c);
        T(it.name, R(54, y, SIDE_W - 16, y + 38), 14, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, c);
        Page pg = it.p;
        AddHot(r, id, [pg] { Go(pg); });
        y += 42;
    }
    // footer: status + quit
    int on = 0; for (auto& w : g_widgets) if (w->enabled) on++;
    S.g->FillCircle(28, H - 74, 4, C(.2f, .82f, .4f));
    T(L"Running \u00B7 " + std::to_wstring(on) + (on == 1 ? L" widget" : L" widgets"), R(40, H - 86, SIDE_W - 12, H - 62), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2);
    int qid = NewId();
    D2D1_RECT_F q = R(12, H - 54, SIDE_W - 12, H - 18);
    if (IsHover(qid)) S.g->FillRound(q, 8, S.p.hover);
    S.g->Icon(0xE7E8, R(22, q.top, 46, q.bottom), 14, S.p.text2);
    T(L"Quit Prism Widgets", R(54, q.top, q.right, q.bottom), 13.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2);
    AddHot(q, qid, [] { QuitApp(); });
}

void PageHeader(float x0, float x1, float& y, const std::wstring& title, const std::wstring& sub) {
    T(title, R(x0, y, x1, y + 38), 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text, 0, true, false, FONT_DISPLAY);
    y += 40;
    if (!sub.empty()) { T(sub, R(x0, y, x1, y + 22), 13.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2); y += 24; }
    y += 18;
}

// ------------------------------------------------------------------ pages
float PageWidgets(float x0, float x1, float y) {
    Button(x1, y + 20, L"Arrange on desktop", true, [] { StartArrange(); }, 0xE7C2);
    PageHeader(x0, x1 - 200, y, L"Widgets", L"Choose what shows on your desktop. Click a widget to customise it.");
    int cols = (x1 - x0) >= 680 ? 3 : 2;
    float gap = 16, cw = (x1 - x0 - gap * (cols - 1)) / cols, chh = 236;
    int n = 0;
    auto cell = [&](int i) { return R(x0 + (i % cols) * (cw + gap), y + (i / cols) * (chh + gap), x0 + (i % cols) * (cw + gap) + cw, y + (i / cols) * (chh + gap) + chh); };
    for (auto& up : g_widgets) {
        Widget* w = up.get();
        D2D1_RECT_F r = cell(n++);
        int id = NewId();
        bool hv = IsHover(id);
        S.g->FillRound(r, 14, hv ? S.p.cardHi : S.p.card);
        S.g->StrokeRound(R(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), 14, hv ? Alpha(S.accent, .7f) : S.p.border, hv ? 1.5f : 1);
        D2D1_RECT_F pv = R(r.left + 8, r.top + 8, r.right - 8, r.top + 168);
        S.g->FillRound(pv, 9, S.p.field);
        DrawPreview(w->key, S.s * 0.56f, R(pv.left + 4, pv.top + 2, pv.right - 4, pv.bottom - 2));
        if (!w->enabled) S.g->FillRound(pv, 9, Alpha(S.p.bg, .45f));
        T(w->title, R(r.left + 16, r.top + 176, r.right - 66, r.top + 200), 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text);
        T(w->Description(), R(r.left + 16, r.top + 199, r.right - 66, r.top + 220), 12, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2);
        std::wstring key = w->key;
        AddHot(r, id, [key] { Go(P_DETAIL, key); });
        Toggle(r.right - 14, r.top + 199, w->enabled, [w] { w->enabled = !w->enabled; ApplyWidget(w); });
    }
    // "new custom widget" tile
    D2D1_RECT_F r = cell(n++);
    int id = NewId();
    S.g->FillRound(r, 14, IsHover(id) ? S.p.hover : C(0, 0, 0, 0));
    ID2D1StrokeStyle* dash = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp{ D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_DASH, 0 };
    g_gr.d2d->CreateStrokeStyle(&sp, nullptr, 0, &dash);
    D2D1_ROUNDED_RECT rr{ R(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 14, 14 };
    S.g->br->SetColor(S.p.track); S.g->rt->DrawRoundedRectangle(&rr, S.g->br, 1.5f, dash);
    SafeRelease(dash);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    S.g->FillCircle(cx, cy - 18, 22, Alpha(S.accent, .15f));
    S.g->Icon(0xE710, R(cx - 20, cy - 38, cx + 20, cy + 2), 16, S.accent);
    T(L"Create custom widget", R(r.left, cy + 12, r.right, cy + 34), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text, 1);
    T(L"Opens a template in Notepad", R(r.left, cy + 34, r.right, cy + 54), 12, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2, 1);
    AddHot(r, id, [] { CreateNewWidget(); });
    int rowsN = (n + cols - 1) / cols;
    return y + rowsN * (chh + gap) + 20;
}

float PageDetail(float x0, float x1, float y) {
    Widget* w = FindWidget(S.detailKey);
    if (!w) { Go(P_WIDGETS); return y; }
    int bid = NewId();
    D2D1_RECT_F back = R(x0 - 6, y - 4, x0 + 96, y + 26);
    if (IsHover(bid)) S.g->FillRound(back, 7, S.p.hover);
    S.g->Icon(0xE76B, R(x0, y - 4, x0 + 16, y + 26), 11, S.accent);
    T(L"Widgets", R(x0 + 20, y - 4, x0 + 100, y + 26), 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.accent);
    AddHot(back, bid, [] { Go(P_WIDGETS); });
    y += 36;
    PageHeader(x0, x1, y, w->title, w->Description());

    // large live preview
    D2D1_RECT_F pv = R(x0, y, x1, y + 270);
    S.g->FillRound(pv, 14, S.p.card);
    S.g->StrokeRound(R(pv.left + .5f, pv.top + .5f, pv.right - .5f, pv.bottom - .5f), 14, S.p.border, 1);
    DrawPreview(w->key, S.s * 0.95f, R(pv.left + 10, pv.top + 6, pv.right - 10, pv.bottom - 6));
    y += 270 + 26;

    std::vector<RowSpec> disp;
    disp.push_back({ L"Show on desktop", L"", [w](D2D1_RECT_F r) { Toggle(r.right, (r.top + r.bottom) / 2, w->enabled, [w] { w->enabled = !w->enabled; Changed(w); }); } });
    disp.push_back({ L"Scale", L"Makes everything bigger or smaller (also: corner handles in Arrange mode)", [w](D2D1_RECT_F r) {
        float cy = (r.top + r.bottom) / 2;
        wchar_t b[16]; swprintf(b, 16, L"%d%%", (int)std::lround(w->size * 100));
        T(b, R(r.right - 52, cy - 12, r.right, cy + 12), 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text, 2);
        Slider(r.right - 290, r.right - 66, cy, (w->size - 0.6f) / 1.4f, [w](float t, bool done) {
            w->size = Clamp(std::round((0.6f + t * 1.4f) * 20) / 20, 0.6f, 2.0f);
            if (done) Changed(w); else PreviewWidget(w);
        });
    } });
    if (w->ResizableW() || w->ResizableH()) {
        disp.push_back({ L"Content size", L"Drag the edge handles in Arrange mode — the layout adapts", [w](D2D1_RECT_F r) {
            float cy = (r.top + r.bottom) / 2;
            D2D1_SIZE_F b = w->BaseSize();
            float bw = Button(r.right, cy, L"Reset", false, [w] { w->cw = w->ch = 0; Changed(w); });
            bw += Button(r.right - bw - 10, cy, L"Resize", true, [] { StartArrange(); }) + 10;
            wchar_t t[48]; swprintf(t, 48, L"%d × %d", (int)std::lround(b.width), (int)std::lround(b.height));
            T(t, R(r.right - bw - 130, cy - 12, r.right - bw - 14, cy + 12), 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text2, 2);
        } });
    } else {
        disp.push_back({ L"Content size", L"This widget has a fixed layout, so it can be scaled but not reshaped", nullptr });
    }
    RowSpec th{ L"Theme", L"Override the global theme for this widget only", [w](D2D1_RECT_F r) {
        float x = r.left, yy = r.top;
        auto chip = [&](const std::wstring& label, const std::wstring& id) {
            int cid = NewId();
            float cwid = TextW(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 26;
            if (x + cwid > r.right) { x = r.left; yy += 36; }
            D2D1_RECT_F c = R(x, yy, x + cwid, yy + 28);
            bool sel = w->theme == id;
            if (sel) S.g->FillRound(c, 14, S.accent);
            else { S.g->FillRound(c, 14, IsHover(cid) ? S.p.hover : S.p.field); S.g->StrokeRound(c, 14, S.p.border, 1); }
            T(label, c, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, sel ? OnAccent() : S.p.text, 1);
            AddHot(c, cid, [w, id] { w->theme = id; Changed(w); });
            x += cwid + 8;
        };
        chip(L"Use global", L"");
        for (int i = 0; i < g_themeCount; i++) chip(g_themes[i].name, g_themes[i].id);
    } };
    th.stacked = true; th.h = 132;
    disp.push_back(th);
    y = Group(x0, x1, y, L"Display", disp);

    std::vector<Opt> opts; w->Options(opts);
    if (!opts.empty()) {
        std::vector<RowSpec> rows;
        for (auto& o : opts) {
            RowSpec rs{ o.label, o.hint, nullptr };
            Opt op = o;
            if (o.kind == Opt::Toggle)
                rs.control = [w, op](D2D1_RECT_F r) { int v = op.get(); Toggle(r.right, (r.top + r.bottom) / 2, v != 0, [w, op, v] { op.set(!v); Changed(w); }); };
            else if (o.kind == Opt::Choice)
                rs.control = [w, op](D2D1_RECT_F r) { Segmented(r.right, (r.top + r.bottom) / 2, op.choices, op.get(), [w, op](int i) { op.set(i); Changed(w); }); };
            else
                rs.control = [w, op](D2D1_RECT_F r) { Button(r.right, (r.top + r.bottom) / 2, op.choices.empty() ? L"Run" : op.choices[0], false, [w, op] { op.set(0); Changed(w); }); };
            if (o.kind == Opt::Action && o.hint.compare(0, 6, L"Error:") == 0) rs.hintColor = S.p.danger;
            rows.push_back(rs);
        }
        y = Group(x0, x1, y, L"Options", rows);
    }
    std::vector<RowSpec> pos;
    pos.push_back({ L"Position", L"Use Arrange mode to move, align and space widgets", [w](D2D1_RECT_F r) {
        float bw = Button(r.right, (r.top + r.bottom) / 2, L"Reset", false, [w] { w->placed = false; w->hasAnchor = false; Changed(w); });
        Button(r.right - bw - 10, (r.top + r.bottom) / 2, L"Arrange", true, [] { StartArrange(); });
    } });
    y = Group(x0, x1, y, L"", pos);
    return y;
}

float PageAppearance(float x0, float x1, float y) {
    PageHeader(x0, x1, y, L"Appearance", L"Glass is rendered from your real wallpaper, so every theme blends into your desktop.");
    RowSpec themes{ L"Theme", L"Applies to all widgets (individual widgets can override it)", [](D2D1_RECT_F r) {
        int cols = (r.right - r.left) >= 600 ? 4 : 3;
        float gap = 12, w = (r.right - r.left - gap * (cols - 1)) / cols, h = 132;
        for (int i = 0; i < g_themeCount; i++) {
            int id = NewId();
            D2D1_RECT_F c = R(r.left + (i % cols) * (w + gap), r.top + (i / cols) * (h + gap), r.left + (i % cols) * (w + gap) + w, r.top + (i / cols) * (h + gap) + h);
            bool sel = g_theme == g_themes[i].id;
            S.g->FillRound(c, 12, S.p.field);
            DrawPreview(std::wstring(L"theme:") + g_themes[i].id, S.s * 0.62f, R(c.left + 4, c.top + 2, c.right - 4, c.bottom - 28));
            T(g_themes[i].name, R(c.left + 6, c.bottom - 28, c.right - 6, c.bottom - 6), 12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, sel ? S.p.text : S.p.text2, 1);
            if (sel) S.g->StrokeRound(R(c.left + 1, c.top + 1, c.right - 1, c.bottom - 1), 11, S.accent, 2.5f);
            else if (IsHover(id)) S.g->StrokeRound(R(c.left + .5f, c.top + .5f, c.right - .5f, c.bottom - .5f), 12, S.p.track, 1.5f);
            const wchar_t* tid = g_themes[i].id;
            AddHot(c, id, [tid] { g_theme = tid; GlobalChanged(true, false); });
        }
    } };
    themes.stacked = true;
    { int cols = (x1 - x0 - 36) >= 600 ? 4 : 3; int rws = (g_themeCount + cols - 1) / cols; themes.h = 56 + rws * 144 + 6; }
    y = Group(x0, x1, y, L"", { themes });

    RowSpec acc{ L"Accent colour", L"Used for highlights such as today's date, ticks and progress rings", [](D2D1_RECT_F r) {
        float x = r.left + 16, cy = r.top + 18;
        auto sw = [&](D2D1_COLOR_F c, const std::wstring& val, const wchar_t* name) {
            int id = NewId();
            bool sel = g_accent == val;
            if (sel) S.g->StrokeCircle(x, cy, 17, c, 2);
            else if (IsHover(id)) S.g->StrokeCircle(x, cy, 17, S.p.track, 2);
            S.g->FillCircle(x, cy, 12.5f, c);
            if (val == L"system") S.g->Icon(0xE7F8, R(x - 12, cy - 12, x + 12, cy + 12), 11, C(1, 1, 1, .9f));
            if (sel) T(name, R(x - 50, cy + 20, x + 50, cy + 38), 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text2, 1);
            std::wstring v = val;
            AddHot(R(x - 18, cy - 18, x + 18, cy + 18), id, [v] { g_accent = v; GlobalChanged(true, true); });
            x += 46;
        };
        sw(WindowsAccent(), L"system", L"Windows");
        for (int i = 0; i < g_accentCount; i++) { bool ok; sw(ParseColor(g_accents[i].hex, &ok), g_accents[i].hex, g_accents[i].name); }
    } };
    acc.stacked = true; acc.h = 120;
    y = Group(x0, x1, y, L"", { acc });

    std::vector<RowSpec> rows;
    rows.push_back({ L"Text on widgets", L"Automatic picks light or dark text from the wallpaper behind each widget", [](D2D1_RECT_F r) {
        int sel = g_textMode == L"light" ? 1 : g_textMode == L"dark" ? 2 : 0;
        Segmented(r.right, (r.top + r.bottom) / 2, { L"Automatic", L"Light", L"Dark" }, sel, [](int i) {
            g_textMode = i == 0 ? L"auto" : i == 1 ? L"light" : L"dark"; GlobalChanged(false, true); });
    } });
    rows.push_back({ L"Settings window", L"", [](D2D1_RECT_F r) {
        int sel = g_uiTheme == L"light" ? 1 : g_uiTheme == L"dark" ? 2 : 0;
        Segmented(r.right, (r.top + r.bottom) / 2, { L"System", L"Light", L"Dark" }, sel, [](int i) {
            g_uiTheme = i == 0 ? L"system" : i == 1 ? L"light" : L"dark"; ApplyFrame(); RequestSave(); });
    } });
    return Group(x0, x1, y, L"", rows);
}

float PageGeneral(float x0, float x1, float y) {
    PageHeader(x0, x1, y, L"General", L"");
    y = Group(x0, x1, y, L"Startup", { { L"Start with Windows", L"Widgets appear when you sign in. This window stays closed.", [](D2D1_RECT_F r) {
        Toggle(r.right, (r.top + r.bottom) / 2, IsStartup(), [] { SetStartup(!IsStartup()); });
    } } });
    std::vector<RowSpec> desk;
    desk.push_back({ L"Arrange widgets", L"Grid, alignment guides and equal spacing. Esc or Done to finish.", [](D2D1_RECT_F r) {
        Button(r.right, (r.top + r.bottom) / 2, L"Arrange", true, [] { StartArrange(); }, 0xE7C2); } });
    desk.push_back({ L"Move widgets without Arrange mode", L"Off: widgets stay put, so clicks never move them by accident", [](D2D1_RECT_F r) {
        Toggle(r.right, (r.top + r.bottom) / 2, g_quickDrag, [] { g_quickDrag = !g_quickDrag; RequestSave(); }); } });
    if (g_quickDrag)
        desk.push_back({ L"Snap while dragging", L"Align to screen edges and other widgets (hold Alt to bypass)", [](D2D1_RECT_F r) {
            Toggle(r.right, (r.top + r.bottom) / 2, g_snap, [] { g_snap = !g_snap; RequestSave(); }); } });
    desk.push_back({ L"Reset all widget positions", L"Stacks visible widgets on the right of your main screen", [](D2D1_RECT_F r) {
        Button(r.right, (r.top + r.bottom) / 2, L"Reset", false, [] { ResetPositions(); }); } });
    y = Group(x0, x1, y, L"Desktop", desk);
    y = Group(x0, x1, y, L"Games & fullscreen", { { L"Pause during fullscreen apps and games", L"Widgets stop updating completely so they never cost you FPS", [](D2D1_RECT_F r) {
        Toggle(r.right, (r.top + r.bottom) / 2, g_pauseFullscreen, [] { g_pauseFullscreen = !g_pauseFullscreen; RequestSave(); }); } } });
    std::vector<RowSpec> data;
    data.push_back({ L"Data folder", L"Settings, to-do list and custom widgets", [](D2D1_RECT_F r) {
        Button(r.right, (r.top + r.bottom) / 2, L"Open", false, [] { OpenDataFolder(); }); } });
    return Group(x0, x1, y, L"Data", data);
}

float PageCustom(float x0, float x1, float y) {
    PageHeader(x0, x1, y, L"Custom widgets", L"Each widget is a small JSON file. Save the file and the widget updates instantly.");
    float bx = x1;
    bx -= Button(bx, y + 16, L"Reload", false, [] { ReloadCustom(); }, 0xE72C) + 10;
    bx -= Button(bx, y + 16, L"Open folder", false, [] { ShellExecuteW(nullptr, L"open", CustomWidgetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }, 0xE838) + 10;
    Button(bx, y + 16, L"New widget", true, [] { CreateNewWidget(); }, 0xE710);
    y += 56;
    std::vector<RowSpec> rows;
    for (auto& up : g_widgets) {
        Widget* w = up.get();
        if (!w->IsCustom()) continue;
        RowSpec rs{ w->title, w->Description(), [w](D2D1_RECT_F r) {
            float cy = (r.top + r.bottom) / 2;
            Toggle(r.right, cy, w->enabled, [w] { w->enabled = !w->enabled; Changed(w); });
            Button(r.right - 58, cy, L"Edit", false, [w] { w->Command(101); });
            std::wstring key = w->key;
            Button(r.right - 128, cy, L"Options", false, [key] { Go(P_DETAIL, key); });
        } };
        if (w->Description().compare(0, 6, L"Error:") == 0) rs.hintColor = S.p.danger;
        rows.push_back(rs);
    }
    if (rows.empty()) rows.push_back({ L"No custom widgets yet", L"Click New widget to start from a template", nullptr });
    y = Group(x0, x1, y, L"Your widgets", rows);

    RowSpec ref{ L"Quick reference", L"", [](D2D1_RECT_F r) {
        const wchar_t* txt =
            L"Elements   text \u00B7 icon \u00B7 rect \u00B7 circle \u00B7 line \u00B7 bar \u00B7 ring \u00B7 button \u00B7 image\n"
            L"Common     x  y  w  h  size  weight  color  align  valign  radius  click\n"
            L"Colours    fg  fg2  fg3  accent  onaccent  #RRGGBB  #RRGGBBAA\n\n"
            L"Variables  {time:HH:mm}  {date:dddd, d MMMM}  {greeting}  {user}\n"
            L"           {cpu}  {ram}  {ramused}  {ramtotal}  {battery}  {charging}\n"
            L"           {disk:C}  {diskfree:C}  {uptime}  {countdown:2027-01-01}  {todo}\n"
            L"           {env:NAME}  {file:C:\\path\\to\\file.txt}\n\n"
            L"\"click\" opens a link, app, folder or ms-settings: page.  Full guide: README.md";
        S.g->FillRound(r, 8, S.p.field);
        T(txt, R(r.left + 14, r.top + 12, r.right - 14, r.bottom - 10), 12, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2, 0, false, true, L"Cascadia Mono");
    } };
    ref.stacked = true; ref.h = 248;
    return Group(x0, x1, y, L"", { ref });
}

struct PMC2 { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
              QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize; ULONG64 SharedCommitUsage; };

float PagePerf(float x0, float x1, float y) {
    PageHeader(x0, x1, y, L"Performance", L"Live resource use of Prism Widgets.");
    PMC2 m{}; m.cb = sizeof(m);
    bool ex2 = GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m)) != 0;
    if (!ex2) { m.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX); GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(PROCESS_MEMORY_COUNTERS_EX)); }
    double memMB = (ex2 && m.PrivateWorkingSetSize ? m.PrivateWorkingSetSize : m.WorkingSetSize) / 1048576.0;
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto f2 = [](FILETIME f) { return (((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    FILETIME nowf; GetSystemTimeAsFileTime(&nowf);
    double cpuS = f2(k) + f2(u), upS = std::max(1.0, f2(nowf) - f2(c));
    SYSTEM_INFO si; GetSystemInfo(&si);
    double avg = 100.0 * cpuS / upS / std::max<DWORD>(1, si.dwNumberOfProcessors);
    int on = 0; for (auto& w : g_widgets) if (w->enabled) on++;

    wchar_t b1[32], b2[32], b3[32];
    swprintf(b1, 32, L"%.1f MB", memMB); swprintf(b2, 32, avg < 0.01 ? L"< 0.01 %%" : L"%.2f %%", avg); swprintf(b3, 32, L"%d", on);
    struct Tile { const wchar_t* label; const wchar_t* val; const wchar_t* sub; wchar_t glyph; } tiles[3] = {
        { L"Memory", b1, L"As shown in Task Manager", 0xE964 }, { L"CPU (average)", b2, L"Since Prism Widgets started", 0xE9D9 }, { L"Widgets on desktop", b3, L"Change on the Widgets page", 0xE80A } };
    float gap = 14, tw = (x1 - x0 - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = R(x0 + i * (tw + gap), y, x0 + i * (tw + gap) + tw, y + 118);
        S.g->FillRound(r, 12, S.p.card); S.g->StrokeRound(R(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), 12, S.p.border, 1);
        S.g->Icon(tiles[i].glyph, R(r.left + 14, r.top + 14, r.left + 34, r.top + 34), 14, S.accent);
        T(tiles[i].label, R(r.left + 40, r.top + 14, r.right - 10, r.top + 34), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text2);
        T(tiles[i].val, R(r.left + 16, r.top + 40, r.right - 10, r.top + 84), 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text, 0, true, false, FONT_DISPLAY);
        T(tiles[i].sub, R(r.left + 16, r.top + 86, r.right - 10, r.top + 106), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text3);
    }
    y += 118 + 22;
    {   // C1: responsiveness numbers
        wchar_t r1[64], r2[64], r3[64];
        if (g_perf.renders) swprintf(r1, 64, L"%.1f ms avg \u00B7 %.0f ms max", g_perf.renderAvg, g_perf.renderMax); else wcscpy(r1, L"\u2014");
        if (g_perf.clickLast >= 0) swprintf(r2, 64, L"%.0f ms last \u00B7 %.0f ms avg", g_perf.clickLast, g_perf.clickAvg); else wcscpy(r2, L"Click a widget button");
        if (g_perf.dragRate > 0) swprintf(r3, 64, L"%.0f updates/s", g_perf.dragRate); else wcscpy(r3, L"Drag a widget in Arrange mode");
        std::vector<RowSpec> live;
        auto val = [](std::wstring v) { return [v](D2D1_RECT_F r) { T(v, r, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text, 2); }; };
        live.push_back({ L"Redraw time", L"How long one widget takes to draw (max over the last minute)", val(r1) });
        live.push_back({ L"Click response", L"From your click to the widget showing the result", val(r2) });
        live.push_back({ L"Drag smoothness", L"Screen updates per second during the last drag or resize", val(r3) });
        live.push_back({ L"Power mode", L"Full speed while you use widgets, efficiency mode a few seconds after", val(g_perf.responsive ? L"Responsive" : L"Efficiency") });
        y = Group(x0, x1, y, L"Responsiveness", live);
    }
    T(L"This Settings window adds a few MB while it is open. It is fully released when you close it; the widgets keep running.",
      R(x0 + 4, y, x1, y + 36), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, S.p.text2, 0, false, true);
    y += 48;
    std::vector<RowSpec> rows;
    auto info = [](const wchar_t* v) { std::wstring s = v; return [s](D2D1_RECT_F r) { T(s, R(r.left, r.top, r.right, r.bottom), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, S.p.text2, 2); }; };
    rows.push_back({ L"Rendering", L"CPU only (Direct2D software) \u2014 no GPU memory, redraws only when content changes", info(L"Software") });
    rows.push_back({ L"Wallpaper cache", L"Decoded only while needed, released after a few seconds idle", info(g_wall.Loaded() ? L"In use" : L"Released") });
    rows.push_back({ L"Idle behaviour", L"Efficiency mode (EcoQoS) when idle; memory returned after 10 idle minutes", info(L"Automatic") });
    rows.push_back({ L"Free memory now", L"Drops caches and trims the working set", [](D2D1_RECT_F r) {
        Button(r.right, (r.top + r.bottom) / 2, L"Free now", false, [] { FreeMemoryNow(); }); } });
    y = Group(x0, x1, y, L"Efficiency", rows);
    std::vector<RowSpec> about;
    about.push_back({ L"Version", L"Native C++ \u00B7 Win32 + Direct2D \u00B7 Windows 11", info(APP_VERSION) });
    about.push_back({ L"Help", L"How to use widgets and write custom ones", [](D2D1_RECT_F r) {
        Button(r.right, (r.top + r.bottom) / 2, L"Open guide", false, [] {
            std::wstring dir = ExePath(); dir = dir.substr(0, dir.rfind(L'\\'));
            std::wstring readme = dir + L"\\README.md";
            if (FileExists(readme)) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + readme + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(nullptr, L"open", CustomWidgetDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }); } });
    return Group(x0, x1, y, L"About", about);
}

// ------------------------------------------------------------------ paint
void EnsureDib(int w, int h) {
    if (S.dib && S.cw == w && S.ch == h) return;
    if (S.dib) { SelectObject(S.mem, S.old); DeleteObject(S.dib); S.dib = nullptr; }
    if (!S.mem) { HDC sc = GetDC(nullptr); S.mem = CreateCompatibleDC(sc); ReleaseDC(nullptr, sc); }
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = std::max(1, w); bi.bmiHeader.biHeight = -std::max(1, h);
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    S.dib = CreateDIBSection(S.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (S.dib) S.old = SelectObject(S.mem, S.dib);
    S.cw = w; S.ch = h;
}

void Paint() {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(S.hwnd, &ps);
    RECT rc; GetClientRect(S.hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    float ns = GetDpiForWindow(S.hwnd) / 96.f;
    if (ns != S.s) { S.s = ns; DropPreviews(); }
    EnsureDib(cw, ch);
    S.p = MakePal(UiDark());
    S.accent = Accent();
    if (S.dib && g_gr.EnsureTarget()) {
        RECT r{ 0, 0, cw, ch };
        if (SUCCEEDED(g_gr.rt->BindDC(S.mem, &r))) {
            g_gr.rt->SetDpi(96.f * S.s, 96.f * S.s);
            g_gr.rt->BeginDraw();
            D2D1_MATRIX_3X2_F id; id._11 = 1; id._12 = 0; id._21 = 0; id._22 = 1; id._31 = 0; id._32 = 0;
            g_gr.rt->SetTransform(&id);
            g_gr.rt->Clear(&S.p.bg);
            Ink ink{}; ink.shadow = false;
            Gfx g{ g_gr.rt, g_gr.br, ink };
            S.g = &g;
            S.hots.clear(); S.nextId = 0;
            float W = cw / S.s, H = ch / S.s;

            // content pane
            S.clip = R(SIDE_W, 0, W, H);
            g_gr.rt->PushAxisAlignedClip(&S.clip, D2D1_ANTIALIAS_MODE_ALIASED);
            float pad = 40, colW = std::min(820.f, W - SIDE_W - 2 * pad);
            float x0 = SIDE_W + std::max(pad, (W - SIDE_W - colW) / 2), x1 = x0 + colW;
            float top = 30 - S.scroll, end = top;
            switch (S.page) {
            case P_WIDGETS: end = PageWidgets(x0, x1, top); break;
            case P_DETAIL: end = PageDetail(x0, x1, top); break;
            case P_APPEARANCE: end = PageAppearance(x0, x1, top); break;
            case P_GENERAL: end = PageGeneral(x0, x1, top); break;
            case P_CUSTOM: end = PageCustom(x0, x1, top); break;
            case P_PERF: end = PagePerf(x0, x1, top); break;
            }
            S.contentH = end + S.scroll + 20; S.viewH = H;
            float maxScroll = std::max(0.f, S.contentH - H);
            if (maxScroll > 0) {  // scrollbar
                float th = std::max(40.f, H * H / S.contentH), ty = (H - th) * (S.scroll / maxScroll);
                S.g->FillRound(R(W - 8, ty + 4, W - 4, ty + th - 4), 2, S.p.track);
            }
            g_gr.rt->PopAxisAlignedClip();

            S.clip = R(0, 0, SIDE_W, H);
            DrawSidebar(H);
            S.g = nullptr;
            if (g_gr.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { g_gr.DropTarget(); DropPreviews(); }
            if (S.scroll > maxScroll) { S.scroll = maxScroll; InvalidateRect(S.hwnd, nullptr, FALSE); }
        }
    }
    BitBlt(hdc, 0, 0, cw, ch, S.mem, 0, 0, SRCCOPY);
    EndPaint(S.hwnd, &ps);
    if (!S.need.empty()) { RenderQueued(); InvalidateRect(S.hwnd, nullptr, FALSE); }
}

void FinishDrag(HWND h) {
    if (!S.dragFn) return;
    auto fn = std::move(S.dragFn); S.dragFn = nullptr;
    S.pressed = -1;
    fn(S.dragT, true);
    if (S.hwnd) InvalidateRect(h, nullptr, FALSE);
}

int HitTest(float x, float y) {
    for (int i = (int)S.hots.size() - 1; i >= 0; i--) if (Inside(S.hots[i].r, x, y)) return S.hots[i].id;
    return -1;
}

LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_MOUSEWHEEL || m == WM_KEYDOWN || (m == WM_ACTIVATE && LOWORD(wp))) Interactive();
    switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        float s = GetDpiForWindow(h) / 96.f; if (s <= 0) s = 1;
        auto* mi = (MINMAXINFO*)lp; mi->ptMinTrackSize.x = (LONG)(840 * s); mi->ptMinTrackSize.y = (LONG)(580 * s);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        DropPreviews(); InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (S.dragFn) {
            if (!(wp & MK_LBUTTON)) { FinishDrag(h); return 0; }   // button released somewhere we didn't see
            S.dragT = SliderT(S.dragRect, GET_X_LPARAM(lp) / S.s); S.dragFn(S.dragT, false); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        int id = HitTest(GET_X_LPARAM(lp) / S.s, GET_Y_LPARAM(lp) / S.s);
        if (id != S.hover) { S.hover = id; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_CAPTURECHANGED: if (S.dragFn && (HWND)lp != h) FinishDrag(h); return 0;
    case WM_MOUSELEAVE: if (S.hover != -1) { S.hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, S.hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / S.s, y = GET_Y_LPARAM(lp) / S.s;
        S.pressed = HitTest(x, y); SetCapture(h);
        for (auto& hh : S.hots) if (hh.id == S.pressed && hh.drag) { S.dragFn = hh.drag; S.dragRect = hh.r; S.dragT = SliderT(S.dragRect, x); S.dragFn(S.dragT, false); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x = GET_X_LPARAM(lp) / S.s, y = GET_Y_LPARAM(lp) / S.s;
        if (S.dragFn) { S.dragT = SliderT(S.dragRect, x); FinishDrag(h); return 0; }
        int id = HitTest(x, y);
        if (id >= 0 && id == S.pressed) {
            std::function<void()> act;
            for (auto& hh : S.hots) if (hh.id == id) act = hh.act;
            if (act) act();
            if (S.hwnd) InvalidateRect(h, nullptr, FALSE);
        }
        S.pressed = -1;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        float maxScroll = std::max(0.f, S.contentH - S.viewH);
        S.scroll = Clamp(S.scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 64.f, 0.f, maxScroll);
        S.hover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER: if (S.page == P_PERF) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) { ApplyFrame(); DropPreviews(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: {
        KillTimer(h, 1);
        DropPreviews();
        if (S.dib) { SelectObject(S.mem, S.old); DeleteObject(S.dib); }
        if (S.mem) DeleteDC(S.mem);
        S = State();
        ReleaseCaches();   // drop what this window built; the deep-idle trim returns memory later
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

void RegisterSettingsClass() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = SettingsProc; wc.hInstance = g_inst; wc.lpszClassName = L"PrismSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
}

void OpenSettings(Widget* focus) {
    if (!S.hwnd) {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(MonitorFromRect(&wa, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
        float s = dx / 96.f;
        int w = std::min((int)(1080 * s), (int)(wa.right - wa.left) - 40), h = std::min((int)(760 * s), (int)(wa.bottom - wa.top) - 40);
        int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
        S = State();
        S.s = s;
        S.hwnd = CreateWindowExW(0, L"PrismSettings", L"Prism Widgets", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
        if (!S.hwnd) return;
        ApplyFrame();
        SetTimer(S.hwnd, 1, 1000, nullptr);
        ShowWindow(S.hwnd, SW_SHOW);
    } else if (IsIconic(S.hwnd)) ShowWindow(S.hwnd, SW_RESTORE);
    if (focus) Go(P_DETAIL, focus->key);
    SetForegroundWindow(S.hwnd);
    InvalidateRect(S.hwnd, nullptr, FALSE);
}

void SettingsRefresh(bool widgetsChanged) {
    if (!S.hwnd) return;
    if (widgetsChanged) DropWidgetPreviews();
    if (S.page == P_DETAIL && !FindWidget(S.detailKey)) Go(P_WIDGETS);
    InvalidateRect(S.hwnd, nullptr, FALSE);
}

void CloseSettings() { if (S.hwnd) DestroyWindow(S.hwnd); }
