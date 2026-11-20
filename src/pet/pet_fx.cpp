// Prism Pet - the flower pot's flight, the cracked glass and the falling pieces, in a small temporary window.
// The window is sized to the effect only (at most 512 x 512 px, 1 MB) and destroyed when it ends.
#include "pet_internal.h"
#include <cmath>

namespace pet {

constexpr float POT_MAX = 3.0f;   // the 35.6-unit pot grows to 106.8 units; the pet is 111.1 units tall
constexpr int BOX = 480;   // effect box; its window is 512 x 512 px (1 MB) at most

static HWND s_wnd = nullptr;
static bool s_flying = false;
static float s_t = 0, s_x0, s_y0, s_x1, s_y1, s_scale = 1, s_k = 1;
static float s_time = 0, s_crackT0 = -1, s_flash = 0;
static V2 s_hit{};
static std::vector<std::vector<V2>> s_lines;
static std::vector<Particle> s_parts;
static Surface s_surf;

static void Release() {
    for (Particle& q : s_parts) SafeRelease(q.geo);
    s_parts.clear(); s_lines.clear();
    s_crackT0 = -1; s_flying = false;
    if (s_wnd) { DestroyWindow(s_wnd); s_wnd = nullptr; }
    s_surf.Free();
}

void FxShutdown() { Release(); }
bool FxActive() { return s_wnd != nullptr; }

void FxThrow(V2 from, V2 to, float scale) {
    Release();
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"PrismPetFx", L"",
                            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!s_wnd) return;
    ShowWindow(s_wnd, SW_SHOWNOACTIVATE);
    s_flying = true; s_t = 0; s_time = 0; s_flash = 0;
    s_x0 = from.x; s_y0 = from.y; s_x1 = to.x; s_y1 = to.y;
    s_scale = scale; s_k = scale / 1.25f;
}

static uint32_t s_seed = 97;
static float Rng() { s_seed = s_seed * 16807 % 2147483647; return (s_seed - 1) / 2147483646.f; }

// Cracks scaled to the pot: 15 jagged radial lines with a few branches, and three broken rings.
static void Impact(float x, float y) {
    s_flash = 1; s_hit = { x, y }; s_crackT0 = s_time; s_seed = 97;
    float k = s_k;
    for (int i = 0; i < 15; i++) {
        float a = i / 15.f * 2 * PI + Rng() * .3f, px = x, py = y, len = (40 + Rng() * 35) * k, L = 0;
        std::vector<V2> pts{ { px, py } };
        while (L < len) {
            float st = (9 + Rng() * 12) * k;
            a += (Rng() - .5f) * .5f; px += std::cos(a) * st; py += std::sin(a) * st; L += st;
            pts.push_back({ px, py });
            if (Rng() < .18f) {
                float b = a + (Rng() < .5f ? .7f : -.7f), bl = (8 + Rng() * 16) * k;
                s_lines.push_back({ { px, py }, { px + std::cos(b) * bl, py + std::sin(b) * bl } });
            }
        }
        s_lines.push_back(std::move(pts));
    }
    for (float R : { 14.f, 30.f, 50.f })
        for (int i = 0; i < 15; i++) {
            if (Rng() < .35f) continue;
            float a1 = i / 15.f * 2 * PI, a2 = (i + 1) / 15.f * 2 * PI, rr = R * k * (.8f + Rng() * .4f);
            float r2 = rr * (.85f + Rng() * .3f);
            s_lines.push_back({ { x + std::cos(a1) * rr, y + std::sin(a1) * rr }, { x + std::cos(a2) * r2, y + std::sin(a2) * r2 } });
        }
    for (int i = 0; i < 10; i++) {   // shards of the pot
        Particle q; q.type = Particle::Shard;
        int n = 4 + (int)(Rng() * 2);
        D2D1_POINT_2F pts[6];
        for (int j = 0; j < n; j++) { float a = j / (float)n * 2 * PI, r = 4 + Rng() * 8; pts[j] = { std::cos(a) * r, std::sin(a) * r }; }
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(g_gr.d2d->CreatePathGeometry(&q.geo)) && SUCCEEDED(q.geo->Open(&sk))) {
            sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED); sk->AddLines(pts + 1, n - 1); sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close(); sk->Release();
        }
        q.x = x + (Rng() - .5f) * 40 * k; q.y = y + (Rng() - .5f) * 30 * k;
        q.vx = (Rng() - .5f) * 180 * k; q.vy = (-120 + Rng() * 120) * k; q.g = 700 * k; q.vr = (Rng() - .5f) * 5; q.max = 2.8f;
        s_parts.push_back(q);
    }
    for (int i = 0; i < 40; i++) {   // soil
        Particle q; q.type = Particle::Soil;
        q.x = x + (Rng() - .5f) * 36 * k; q.y = y + (Rng() - .5f) * 24 * k;
        q.vx = (Rng() - .5f) * 200 * k; q.vy = (-120 + Rng() * 120) * k; q.g = 800 * k; q.size = 1.2f + Rng() * 2; q.max = 2.4f;
        s_parts.push_back(q);
    }
    for (int i = 0; i < 4; i++) {   // leaves
        Particle q; q.type = Particle::Leaf;
        q.x = x + (Rng() - .5f) * 24 * k; q.y = y - 12 * k;
        q.vx = (Rng() - .5f) * 120 * k; q.vy = (-90 + Rng() * 60) * k; q.g = 420 * k; q.vr = (Rng() - .5f) * 6; q.size = 6 + Rng() * 4; q.max = 2.8f;
        s_parts.push_back(q);
    }
}

void FxTick(float h) {
    if (!s_wnd) return;
    s_time += h;
    s_flash = std::max(0.f, s_flash - h * 5);
    if (s_flying) {
        s_t += h;
        if (s_t >= .75f) { s_flying = false; Impact(s_x1, s_y1); }
    }
    float half = BOX / 2.f;
    for (Particle& q : s_parts) { q.life += h; q.vy += q.g * h; q.x += q.vx * h; q.y += q.vy * h; q.rot += q.vr * h; }
    for (auto it = s_parts.begin(); it != s_parts.end();) {   // pieces that leave the effect box have slid out of view
        bool out = std::fabs(it->x - s_hit.x) > half - 8 || std::fabs(it->y - s_hit.y) > half - 8;
        if (it->life >= it->max || out) { SafeRelease(it->geo); it = s_parts.erase(it); } else ++it;
    }
    bool cracks = s_crackT0 >= 0 && s_time - s_crackT0 < 3.4f;
    if (!s_flying && !cracks && s_parts.empty()) Release();
}

static void Stroke(ID2D1RenderTarget* rt, const std::vector<V2>& l, float off, float w) {
    for (size_t i = 1; i < l.size(); i++) rt->DrawLine({ l[i - 1].x + off, l[i - 1].y + off }, { l[i].x + off, l[i].y + off }, g_gr.br, w, g_gr.round);
}

void FxPresent() {
    if (!s_wnd) return;
    RECT box;
    float u = std::clamp(s_t / .75f, 0.f, 1.f);
    float pcx = Lerp(s_x0, s_x1, u), pcy = Lerp(s_y0, s_y1, u) - 110 * s_k * std::sin(PI * u);
    if (s_flying) {   // a box around the pot, which grows toward the viewer
        float r = 26 * s_scale * std::pow(POT_MAX, u);
        box = { (LONG)(pcx - r), (LONG)(pcy - r), (LONG)(pcx + r) + 1, (LONG)(pcy + r) + 1 };
    } else {
        box = { (LONG)(s_hit.x - BOX / 2), (LONG)(s_hit.y - BOX / 2), (LONG)(s_hit.x + BOX / 2), (LONG)(s_hit.y + BOX / 2) };
    }
    Present(s_surf, s_wnd, box, [&](ID2D1RenderTarget* rt) {
        D2D1_MATRIX_3X2_F base; rt->GetTransform(&base);
        ID2D1SolidColorBrush* br = g_gr.br;
        if (s_flying) {
            float sc = s_scale * std::pow(POT_MAX, u), a = u * 3.2f, c = std::cos(a), s = std::sin(a);
            D2D1_MATRIX_3X2_F m{ c * sc, s * sc, -s * sc, c * sc, pcx + base._31, pcy + base._32 };
            rt->SetTransform(&m);
            DrawPotShape(rt);
            rt->SetTransform(&base);
            return;
        }
        float a = std::clamp(1 - (s_time - s_crackT0 - 2.4f) / 1, 0.f, 1.f);
        if (a > 0) {
            br->SetColor(D2D1_COLOR_F{ 0, 0, 0, .28f * a });
            for (auto& l : s_lines) Stroke(rt, l, 1.2f * s_k, 3 * s_k);
            br->SetColor(D2D1_COLOR_F{ 1, 1, 1, .85f * a });
            for (auto& l : s_lines) Stroke(rt, l, 0, 1.3f * s_k);
            for (float f : { 1.f, .66f, .4f }) {   // bright point where it hit
                D2D1_ELLIPSE e{ { s_hit.x, s_hit.y }, 12 * s_k * f, 12 * s_k * f };
                br->SetColor(D2D1_COLOR_F{ 1, 1, 1, .25f * a }); rt->FillEllipse(&e, br);
            }
        }
        if (s_flash > 0) {   // impact flash, kept inside the effect box
            for (float f : { 1.f, .7f, .45f }) {
                D2D1_ELLIPSE e{ { s_hit.x, s_hit.y }, 90 * s_k * f, 90 * s_k * f };
                br->SetColor(D2D1_COLOR_F{ 1, 1, 1, .18f * s_flash }); rt->FillEllipse(&e, br);
            }
        }
        DrawParticles(rt, s_parts, s_k);
    });
}

}  // namespace pet
