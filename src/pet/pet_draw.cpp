// Prism Pet - draws the robot with Direct2D (CPU target), a port of the prototype's drawPet.
#include "pet_internal.h"

namespace pet {

static const Palette g_palettes[] = {
    { L"pearl",    0xeef1f6, 0xcfd6e2, 0x8d97a8, 0x131821, 0x283243, 0x4ff0b5, 0xc3cbd8 },
    { L"graphite", 0x4b5262, 0x3a404d, 0x2a2f39, 0x0b0e13, 0xdfe5ee, 0xffb347, 0x5a6273 },
    { L"sand",     0xf5eee6, 0xe4d6c8, 0xb39f8d, 0x1c2133, 0x3b2f2b, 0x61e3ff, 0xff8a6e },
};
const Palette& PaletteOf(const std::wstring& id) {
    for (auto& p : g_palettes) if (id == p.id) return p;
    return g_palettes[0];
}

Expr VisorExpr(Eyes eyes, Mouth mouth, bool blink) {
    if (mouth == Mouth::Laugh) return Expr::Laugh;
    if (mouth == Mouth::Yawn) return eyes == Eyes::Closed ? Expr::Sleep : Expr::Yawn;
    switch (eyes) {
    case Eyes::Open:   return blink ? Expr::Blink : (mouth == Mouth::O || mouth == Mouth::Open) ? Expr::Oh : Expr::Open;
    case Eyes::Closed: return Expr::Sleep;
    case Eyes::Happy:  return Expr::Happy;
    case Eyes::Half:   return Expr::Tired;
    case Eyes::Angry:  return Expr::Angry;
    case Eyes::Dizzy:  return Expr::Dizzy;
    case Eyes::O:      return Expr::Surprised;
    }
    return Expr::Open;
}


// ---------------------------------------------------------------- cached D2D resources
static ID2D1PathGeometry *g_laughMouth = nullptr, *g_angryCut[2] = {}, *g_potBody = nullptr, *g_star = nullptr, *g_wedge = nullptr;
static float g_wedgeKey = -1;

// Head bitmaps: the helmet, visor, discs and eyes are about half the robot's shapes but change only with
// the expression, view and gaze, so they are drawn once per state and then blitted. A few states are kept.
struct HeadBmp { ID2D1Bitmap* bmp = nullptr; const void* rt = nullptr; uint32_t key[6] = {}; float cx = 0, cy = 0; unsigned used = 0; };
static HeadBmp g_heads[6];
static unsigned g_headTick = 0;
static void ReleaseHeads() { for (auto& h : g_heads) { SafeRelease(h.bmp); h = HeadBmp{}; } }

void ReleaseDrawCache() {
    ReleaseHeads();
    SafeRelease(g_laughMouth); SafeRelease(g_angryCut[0]); SafeRelease(g_angryCut[1]); SafeRelease(g_potBody); SafeRelease(g_star); SafeRelease(g_wedge); g_wedgeKey = -1;
}

static ID2D1PathGeometry* Polygon(const D2D1_POINT_2F* pts, int n) {
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_gr.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return nullptr; }
    s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    s->AddLines(pts + 1, n - 1);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close(); s->Release();
    return g;
}
// Geometries are factory resources: built once, they survive render-target changes.
static void EnsureGeometry() {
    if (!g_laughMouth && SUCCEEDED(g_gr.d2d->CreatePathGeometry(&g_laughMouth))) {   // flat top, round bottom
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g_laughMouth->Open(&s))) {
            s->BeginFigure({ -4.5f, 0 }, D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine({ 4.5f, 0 });
            s->AddArc({ { -4.5f, 0 }, { 4.5f, 4.5f }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL });
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close(); s->Release();
        }
    }
    // Annoyed eyes: a triangle cut from the top of each pill (left eye: top-right, right eye: top-left).
    const D2D1_POINT_2F cutL[3] = { { -8, -9 }, { 8, -9 }, { 8, -1 } }, cutR[3] = { { -8, -9 }, { 8, -9 }, { -8, -1 } };
    if (!g_angryCut[0]) g_angryCut[0] = Polygon(cutL, 3);
    if (!g_angryCut[1]) g_angryCut[1] = Polygon(cutR, 3);
    const D2D1_POINT_2F pot[4] = { { -9, 0 }, { 9, 0 }, { 6.5f, 15 }, { -6.5f, 15 } };
    if (!g_potBody) g_potBody = Polygon(pot, 4);
    if (!g_star) {
        D2D1_POINT_2F st[10];
        for (int j = 0; j < 10; j++) { float r = j % 2 ? 2.4f : 6, b = j * PI / 5 - PI / 2; st[j] = { std::cos(b) * r, std::sin(b) * r }; }
        g_star = Polygon(st, 10);
    }
}

// ---------------------------------------------------------------- canvas-style helpers
// Transforms compose like the browser canvas: each call applies on top of the current one.
struct Canvas {
    ID2D1RenderTarget* rt; ID2D1SolidColorBrush* br; ID2D1StrokeStyle* round;
    const Palette& pal; D2D1_COLOR_F glow;
    D2D1_MATRIX_3X2_F m;

    void Set(const D2D1_MATRIX_3X2_F& n) { m = n; rt->SetTransform(&m); }
    void Pre(float a, float b, float c, float d, float e, float f) {   // m = L * m (row vectors)
        D2D1_MATRIX_3X2_F n;
        n._11 = a * m._11 + b * m._21; n._12 = a * m._12 + b * m._22;
        n._21 = c * m._11 + d * m._21; n._22 = c * m._12 + d * m._22;
        n._31 = e * m._11 + f * m._21 + m._31; n._32 = e * m._12 + f * m._22 + m._32;
        Set(n);
    }
    void Translate(float x, float y) { Pre(1, 0, 0, 1, x, y); }
    void Rotate(float a) { float c = std::cos(a), s = std::sin(a); Pre(c, s, -s, c, 0, 0); }
    void Scale(float x, float y) { Pre(x, 0, 0, y, 0, 0); }

    ID2D1SolidColorBrush* B(D2D1_COLOR_F c) { br->SetColor(c); return br; }
    // Filled rounded rect with an optional outline (lw > 0), like the prototype's shape().
    // Outlined shapes: the outline is a slightly larger fill underneath, not a stroke. Same look as
    // fill + centred stroke, and fills cost about half as much as strokes on the software target.
    void RR(float x, float y, float w, float h, float r, D2D1_COLOR_F fill, float lw = 0) {
        r = std::min(r, std::min(w, h) / 2);   // the canvas shrinks radii that don't fit; D2D would turn it into an ellipse
        float o = lw / 2;
        if (lw > 0) {
            D2D1_ROUNDED_RECT out{ { x - o, y - o, x + w + o, y + h + o }, r + o, r + o };
            rt->FillRoundedRectangle(&out, B(Hex(pal.line)));
        }
        float ri = std::max(0.f, r - o);
        D2D1_ROUNDED_RECT in{ { x + o, y + o, x + w - o, y + h - o }, ri, ri };
        rt->FillRoundedRectangle(&in, B(fill));
    }
    void Ellipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F fill, float lw = 0) {
        float o = lw / 2;
        if (lw > 0) { D2D1_ELLIPSE out{ { cx, cy }, rx + o, ry + o }; rt->FillEllipse(&out, B(Hex(pal.line))); }
        D2D1_ELLIPSE in{ { cx, cy }, rx - o, ry - o };
        rt->FillEllipse(&in, B(fill));
    }
    void Line(V2 a, V2 b, D2D1_COLOR_F c, float w) { rt->DrawLine({ a.x, a.y }, { b.x, b.y }, B(c), w, round); }
    // Thick round-capped limb with an outline: the outline colour slightly wider underneath.
    // One stroked two-segment path per pass: cheaper than four round-capped lines on the software target.
    void Limb(V2 a, V2 b, V2 c, float w, D2D1_COLOR_F col) {
        D2D1_COLOR_F ln = Hex(pal.line);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g_gr.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure({ a.x, a.y }, D2D1_FIGURE_BEGIN_HOLLOW);
            D2D1_POINT_2F pts[2] = { { b.x, b.y }, { c.x, c.y } };
            s->AddLines(pts, 2); s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
            rt->DrawGeometry(g, B(ln), w + 4.2f, round); rt->DrawGeometry(g, B(col), w, round);
        }
        SafeRelease(g);
    }
    void Quad(V2 p0, V2 p1, V2 p2, D2D1_COLOR_F c, float w) {   // quadratic curve as short round-capped segments
        V2 prev = p0;
        for (int i = 1; i <= 8; i++) {
            float t = i / 8.f, u = 1 - t;
            V2 q{ u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y };
            Line(prev, q, c, w); prev = q;
        }
    }
    // Stand-in for the canvas shadowBlur: a soft halo of stacked faint ellipses behind a glowing part.
    // A radial gradient brush looks smoother but costs about 2 MB on the software target.
    void Glow(float cx, float cy, float rx, float ry) {
        for (float f : { 1.f, .72f, .48f }) {
            D2D1_ELLIPSE e{ { cx, cy }, rx * f, ry * f };
            rt->FillEllipse(&e, B(Alpha(glow, .14f)));
        }
    }
    void Pill(float x, float y, float w, float h, D2D1_COLOR_F c) { RR(x - w / 2, y - h / 2, w, h, w / 2, c); }
};

// ---------------------------------------------------------------- robot parts
static void Disc(Canvas& c, float x, float s) {   // ear disc with a glowing core
    D2D1_MATRIX_3X2_F keep = c.m;
    c.Translate(x, 2); c.Scale(s, 1);
    c.Ellipse(0, 0, 7.5f, 11, Hex(c.pal.disc), 2.4f);
    c.Glow(0, 0, 7.5f, 10);
    c.Ellipse(0, 0, 3.6f, 6, c.glow);
    c.Set(keep);
}

static void Eyes(Canvas& c, Expr e, const float xs[2], const float sc[2]) {
    D2D1_COLOR_F g = c.glow;
    for (int i = 0; i < 2; i++) {
        float x = xs[i], s = sc[i], w = 7 * s;
        bool any = e != Expr::Yawn && e != Expr::Oh;   // the prototype draws only the mouth for these two
        if (any) c.Glow(x, e == Expr::Open || e == Expr::Angry ? -1.f : 1.f, 4.5f * s + 3.5f, e == Expr::Open || e == Expr::Angry ? 11.f : 7.f);
        switch (e) {
        case Expr::Open:  c.Pill(x, -1, w, 14, g); break;
        case Expr::Blink: c.Pill(x, 1, w + 2, 2.8f, g); break;
        case Expr::Happy: case Expr::Laugh: c.Quad({ x - 4.5f * s, 2 }, { x, -8 }, { x + 4.5f * s, 2 }, g, 3.2f); break;
        case Expr::Angry: {
            c.Pill(x, 0, w, 12, g);
            if (g_angryCut[i]) {   // the canvas erases this triangle; the visor is opaque, so paint it in visor colour
                D2D1_MATRIX_3X2_F keep = c.m;
                c.Translate(x, 0);
                c.rt->FillGeometry(g_angryCut[i], c.B(Hex(c.pal.visor)));
                c.Set(keep);
            }
            break;
        }
        case Expr::Tired: c.Pill(x, 4, w + 1.5f, 3.4f, g); c.Pill(x, -2, w - 1, 2, Alpha(g, .55f)); break;
        case Expr::Sleep: c.Quad({ x - 4.5f * s, 0 }, { x, 6 }, { x + 4.5f * s, 0 }, g, 3.2f); break;
        case Expr::Surprised: { D2D1_ELLIPSE el{ { x, -1 }, 5 * s, 5 * s }; c.rt->DrawEllipse(&el, c.B(g), 2.6f); break; }
        case Expr::Dizzy: {
            float r = 4.5f * s;
            c.Line({ x - r, -1 - r }, { x + r, -1 + r }, g, 2.6f); c.Line({ x + r, -1 - r }, { x - r, -1 + r }, g, 2.6f);
            break;
        }
        default: break;
        }
    }
    float mx = (xs[0] + xs[1]) / 2;
    if (e == Expr::Laugh && g_laughMouth) {
        c.Glow(mx, 9, 8, 7);
        D2D1_MATRIX_3X2_F keep = c.m;
        c.Translate(mx, 7);
        c.rt->FillGeometry(g_laughMouth, c.B(g));
        c.Set(keep);
    }
    if (e == Expr::Yawn) { c.Glow(mx, 8, 7, 8); c.Ellipse(mx, 8, 3, 4, g); }
    if (e == Expr::Oh) { c.Glow(mx, 9, 6, 6.5f); c.Ellipse(mx, 9, 2.2f, 2.6f, g); }
}

// v here is the head's own view value: 0 = front, 1 = three-quarter (the inverse of Pose::v).
static void Head(Canvas& c, float v, Expr expr, float gy) {
    if (v < .6f) Disc(c, 31, std::max(.01f, 1 - v * 1.2f));   // far ear disc, hidden as it turns
    c.RR(-30, -25, 60, 49, 22, Hex(c.pal.shell), 2.6f);
    Disc(c, Lerp(-31, -30, v), Lerp(1, 1.15f, v));
    float vx = Lerp(0, 7, v), vw = Lerp(44, 38, v);
    c.RR(vx - vw / 2, -15, vw, 30, 12, Hex(c.pal.visor));
    {   // visor highlight: arc around (vx-6, -30), r 30, from .25pi to .62pi (inside the visor, so no clip needed)
        V2 prev{ vx - 6 + 30 * std::cos(.25f * PI), -30 + 30 * std::sin(.25f * PI) };
        for (int i = 1; i <= 5; i++) {
            float a = (.25f + .37f * i / 5) * PI;
            V2 q{ vx - 6 + 30 * std::cos(a), -30 + 30 * std::sin(a) };
            c.Line(prev, q, D2D1_COLOR_F{ 1, 1, 1, .10f }, 3); prev = q;
        }
    }
    D2D1_MATRIX_3X2_F keep = c.m;
    c.Translate(0, gy);
    float xs[2] = { vx + Lerp(-10, -8.5f, v), vx + Lerp(10, 9, v) }, sc[2] = { 1, Lerp(1, .72f, v) };
    Eyes(c, expr, xs, sc);
    c.Set(keep);
}

// ---------------------------------------------------------------- props
// Laptop on the lap: an 18-unit base and lid hinged at its far end; it rises from behind the floor line.
static void DrawLaptop(Canvas& c, const D2D1_MATRIX_3X2_F& screen, const Laptop& L, float floorY, float time) {
    D2D1_MATRIX_3X2_F keep = c.m;
    c.Set(screen);
    D2D1_RECT_F clip{ -1e6f, -1e6f, 1e6f, floorY };
    c.rt->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    c.Set(keep);
    c.RR(L.x, L.y - 3, 18, 3, 1.2f, Hex(0x3e4554), 1.2f);
    V2 hg{ L.x + 18, L.y - 3 }, e{ hg.x - 18 * std::cos(L.lid), hg.y - 18 * std::sin(L.lid) };
    c.Line(hg, e, Hex(0x2b313d), 3.2f);
    if (L.lid > .6f) {   // the screen's glow on the inside of the lid
        float nx = -std::sin(L.lid), ny = std::cos(L.lid), flick = .75f + .2f * (.5f + .5f * std::sin(time * 37));
        c.Line({ hg.x + nx * 1.8f, hg.y + ny * 1.8f }, { e.x + nx * 1.8f, e.y + ny * 1.8f }, D2D1_COLOR_F{ .51f, .80f, 1, flick }, 1.6f);
        float key = L.lid + L.x * 100 + L.y * 1e4f;   // the light wedge is rebuilt only while the laptop moves
        if (g_wedgeKey != key) {
            SafeRelease(g_wedge);
            D2D1_POINT_2F wedge[4] = { { hg.x, hg.y }, { e.x, e.y }, { e.x - 14, e.y + 6 }, { hg.x - 16, hg.y } };
            g_wedge = Polygon(wedge, 4); g_wedgeKey = key;
        }
        if (g_wedge) c.rt->FillGeometry(g_wedge, c.B(D2D1_COLOR_F{ .51f, .80f, 1, .10f }));
    }
    c.rt->PopAxisAlignedClip();
}
static void DrawPhone(Canvas& c, const Phone& ph, V2 hand) {
    D2D1_MATRIX_3X2_F keep = c.m;
    c.Translate(hand.x + 1, hand.y - 4); c.Rotate(.35f);
    c.RR(-4, -7, 8, 13, 2, Hex(0x20242e));
    D2D1_RECT_F scr{ -3, -6, 3, 5 };
    c.rt->FillRectangle(&scr, c.B(Hex(0x8fd0ff)));
    for (int i = -1; i < 5; i++) {   // content lines scrolling past, kept inside the screen
        float yy = -6 + std::fmod(std::fmod(i * 4 + ph.off, 16.f) + 16, 16.f) - 2;
        D2D1_RECT_F ln{ -2.2f, std::max(yy, -6.f), 2.2f, std::min(yy + 1.4f, 5.f) };
        if (ln.bottom > ln.top) c.rt->FillRectangle(&ln, c.B(D2D1_COLOR_F{ 30 / 255.f, 60 / 255.f, 110 / 255.f, .55f }));
    }
    c.Set(keep);
}

void DrawRobot(ID2D1RenderTarget* rt, const Pose& p, float scale, const Palette& pal, D2D1_COLOR_F glow, Expr expr, const World* w) {
    EnsureGeometry();
    Canvas c{ rt, g_gr.br, g_gr.round, pal, glow, {} };
    rt->GetTransform(&c.m);
    D2D1_MATRIX_3X2_F base = c.m;
    c.Translate(p.x, p.y); c.Rotate(p.rot); c.Scale(p.face * scale, scale);

    const float v = p.v;
    const Fk k = Kinematics(p);
    auto leg = [&](Side sd) {
        V2 hp = sd == F ? k.hipF : k.hipB, kn = sd == F ? k.kF : k.kB, f = sd == F ? k.fF : k.fB;
        float a = sd == F ? p.lF1 + p.lF2 : p.lB1 + p.lB2, sg = sd == F ? 1.f : -1.f;
        bool back = sd == B && v < .5f;
        c.Limb(hp, kn, f, 10, Hex(back ? pal.joint : pal.shade));
        float cx = f.x + Lerp(std::cos(a) * 3, sg * 1.5f, v), cy = f.y + Lerp(-std::sin(a) * 3 + 2.2f, 2.5f, v);
        D2D1_MATRIX_3X2_F keep = c.m;   // foot: an ellipse tilted with the shin in the side view
        c.Translate(cx, cy); c.Rotate(Lerp(-a, 0, v));
        c.Ellipse(0, 0, Lerp(9, 8, v), 3.8f, Hex(back ? pal.joint : pal.shell), 2.4f);
        c.Set(keep);
    };
    auto arm = [&](Side sd) {
        bool back = sd == B && v < .5f;
        D2D1_COLOR_F col = Hex(back ? pal.shade : pal.shell);
        V2 hand = sd == F ? k.hF : k.hB;
        c.Limb(sd == F ? k.shF : k.shB, sd == F ? k.eF : k.eB, hand, 8.5f, col);
        c.Ellipse(hand.x, hand.y, 5, 5, col, 2.4f);
    };
    auto body = [&]() {
        D2D1_MATRIX_3X2_F keep = c.m;
        c.Rotate(p.t);
        float bw = Lerp(28, 32, v), ox = Lerp(1, 0, v);
        c.RR(ox - bw / 2, -32, bw, 37, 12, Hex(pal.shade), 2.4f);   // ponytail: one 12 radius; the prototype uses 11 top / 13 bottom (<1 px apart)
        c.RR(ox - bw / 2 + 3, -30, bw - 6, 14, 7, Hex(pal.shell), 1.6f);
        c.RR(-5, -37, 10, 6, 3, Hex(pal.joint));   // neck
        float lx = Lerp(5, 0, v);
        c.Glow(lx, -23, 6.5f, 6.5f);   // chest light
        c.Ellipse(lx, -23, 2.4f, 2.4f, glow);
        c.Set(keep);
    };
    auto head = [&]() {
        D2D1_MATRIX_3X2_F keep = c.m;
        c.Translate(k.head.x, k.head.y); c.Rotate(p.t + p.h);
        if (!w) { c.Scale(.9f, .9f); Head(c, 1 - v, expr, p.gy); c.Set(keep); return; }   // previews draw it directly
        float hs = .9f * scale;   // head px per local unit
        uint32_t key[6] = { (uint32_t)std::lround(hs * 1000), (uint32_t)std::lround((1 - v) * 20), (uint32_t)expr,
                            (uint32_t)std::lround(p.gy * 4 + 100), (uint32_t)(uintptr_t)&pal,
                            (uint32_t)(glow.r * 255) | (uint32_t)(glow.g * 255) << 8 | (uint32_t)(glow.b * 255) << 16 };
        HeadBmp* hb = nullptr;
        for (auto& h : g_heads) if (h.bmp && h.rt == rt && !memcmp(h.key, key, sizeof key)) hb = &h;
        if (!hb) {   // draw this state once into a bitmap
            hb = &g_heads[0];
            for (auto& h : g_heads) if (h.used < hb->used) hb = &h;
            SafeRelease(hb->bmp);
            float qv = key[1] / 20.f, qgy = ((int)key[3] - 100) / 4.f;
            hb->cx = std::ceil(44 * hs) + 2; hb->cy = std::ceil(34 * hs) + 2;
            D2D1_SIZE_F sz{ 2 * hb->cx, 2 * hb->cy };
            ID2D1BitmapRenderTarget* brt = nullptr;
            D2D1_PIXEL_FORMAT pf{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            if (SUCCEEDED(rt->CreateCompatibleRenderTarget(&sz, nullptr, &pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &brt))) {
                ID2D1SolidColorBrush* bb = nullptr; D2D1_COLOR_F white{ 1, 1, 1, 1 };
                brt->CreateSolidColorBrush(&white, nullptr, &bb);
                if (bb) {
                    brt->BeginDraw();
                    D2D1_COLOR_F clear{ 0, 0, 0, 0 }; brt->Clear(&clear);
                    Canvas hc{ brt, bb, c.round, pal, glow, { hs, 0, 0, hs, hb->cx, hb->cy } };
                    brt->SetTransform(&hc.m);
                    Head(hc, qv, expr, qgy);
                    if (SUCCEEDED(brt->EndDraw())) brt->GetBitmap(&hb->bmp);
                    bb->Release();
                }
                brt->Release();
            }
            hb->rt = rt; memcpy(hb->key, key, sizeof key);
        }
        hb->used = ++g_headTick;
        if (hb->bmp) {
            c.Scale(1 / scale, 1 / scale);   // back to px, the bitmap's own units
            D2D1_RECT_F d{ -hb->cx, -hb->cy, hb->cx, hb->cy };
            rt->DrawBitmap(hb->bmp, &d, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        }
        c.Set(keep);
    };

    // Draw order. Side view: far arm, far leg, body, near leg, head, near arm.
    // Front view: legs, body, arms raised beside the helmet (behind it), head, remaining arms.
    bool upB = v > .5f && k.hB.y < k.head.y + 18, upF = v > .5f && k.hF.y < k.head.y + 18;
    if (v <= .5f) { arm(B); leg(B); } else leg(B);
    if (v > .5f) leg(F);
    body();
    if (v <= .5f) leg(F);
    if (upB) arm(B);
    if (upF) arm(F);
    head();
    if (w && w->laptop) DrawLaptop(c, base, w->lap, w->st.floor, w->time);
    if (w && w->phone) DrawPhone(c, w->ph, k.hB);
    if (v > .5f && !upB) arm(B);
    if (w && w->pot != Pot::None) {
        V2 at = w->pot == Pot::HandB ? V2{ k.hB.x, k.hB.y - 6 } : V2{ (k.hF.x + k.hB.x) / 2, (k.hF.y + k.hB.y) / 2 - 6 };
        D2D1_MATRIX_3X2_F keep = c.m;
        c.Translate(at.x, at.y);
        DrawPotShape(rt);
        c.Set(keep);
    }
    if (!upF) arm(F);
    c.Set(base);
}

}  // namespace pet

namespace pet {

// Multiplies `local` onto the target's current transform (row vectors: local applies first).
static D2D1_MATRIX_3X2_F On(const D2D1_MATRIX_3X2_F& l, const D2D1_MATRIX_3X2_F& m) {
    return { l._11 * m._11 + l._12 * m._21, l._11 * m._12 + l._12 * m._22,
             l._21 * m._11 + l._22 * m._21, l._21 * m._12 + l._22 * m._22,
             l._31 * m._11 + l._32 * m._21 + m._31, l._31 * m._12 + l._32 * m._22 + m._32 };
}

void DrawPotShape(ID2D1RenderTarget* rt) {
    EnsureGeometry();
    ID2D1SolidColorBrush* br = g_gr.br;
    D2D1_MATRIX_3X2_F m; rt->GetTransform(&m);
    for (float a : { -.6f, 0.f, .6f }) {   // leaves: translate(0, -2), rotate(a)
        float l = a == 0 ? 16.f : 13.f, c = std::cos(a), s = std::sin(a);
        D2D1_MATRIX_3X2_F n = On({ c, s, -s, c, 0, -2 }, m);
        rt->SetTransform(&n);
        D2D1_ELLIPSE e{ { 0, -l / 2 }, 3.2f, l / 2 };
        br->SetColor(Hex(0x4f8a3a)); rt->FillEllipse(&e, br);
    }
    rt->SetTransform(&m);
    D2D1_ELLIPSE fl{ { 0, -17 }, 3.6f, 3.6f }, fc{ { 0, -17 }, 1.5f, 1.5f };
    br->SetColor(Hex(0xff7aa8)); rt->FillEllipse(&fl, br);
    br->SetColor(Hex(0xffe07a)); rt->FillEllipse(&fc, br);
    if (g_potBody) { br->SetColor(Hex(0xc56a3d)); rt->FillGeometry(g_potBody, br); br->SetColor(Hex(0x7d3c1e)); rt->DrawGeometry(g_potBody, br, 1.2f); }
    D2D1_ROUNDED_RECT rim{ { -10.5f, -2.5f, 10.5f, 2.5f }, 1.5f, 1.5f };
    br->SetColor(Hex(0xd47a4b)); rt->FillRoundedRectangle(&rim, br);
    br->SetColor(Hex(0x7d3c1e)); rt->DrawRoundedRectangle(&rim, br, 1.2f);
}

// Text and "z" bubbles use one cached format and are scaled with the transform.
static void Label(ID2D1RenderTarget* rt, const wchar_t* s, float x, float y, float size, D2D1_COLOR_F col, float alpha) {
    IDWriteTextFormat* f = g_gr.Format(FONT_TEXT, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!f || !s) return;
    D2D1_MATRIX_3X2_F m; rt->GetTransform(&m);
    float k = size / 16;
    D2D1_MATRIX_3X2_F n = On({ k, 0, 0, k, x, y }, m);
    rt->SetTransform(&n);
    UINT32 len = (UINT32)wcslen(s);
    D2D1_RECT_F r{ 0, -18, 200, 6 };   // baseline near y
    ID2D1SolidColorBrush* br = g_gr.br;
    br->SetColor(D2D1_COLOR_F{ 15 / 255.f, 18 / 255.f, 28 / 255.f, .6f * alpha });
    for (V2 o : { V2{ -1.2f, 0 }, V2{ 1.2f, 0 }, V2{ 0, -1.2f }, V2{ 0, 1.2f } }) {   // dark outline
        D2D1_RECT_F ro{ r.left + o.x, r.top + o.y, r.right + o.x, r.bottom + o.y };
        rt->DrawText(s, len, f, &ro, br);
    }
    br->SetColor(Alpha(col, alpha));
    rt->DrawText(s, len, f, &r, br);
    rt->SetTransform(&m);
}

void DrawParticles(ID2D1RenderTarget* rt, const std::vector<Particle>& parts, float k) {
    ID2D1SolidColorBrush* br = g_gr.br;
    D2D1_MATRIX_3X2_F m; rt->GetTransform(&m);
    for (const Particle& q : parts) {
        float f = q.life / q.max;
        switch (q.type) {
        case Particle::Dust: {
            D2D1_ELLIPSE e{ { q.x, q.y }, q.size * (.6f + f * 1.4f), q.size * (.6f + f * 1.4f) };
            br->SetColor(Hex(0xe2d9c8, (1 - f) * .55f)); rt->FillEllipse(&e, br);
            break;
        }
        case Particle::Text: case Particle::Z: {
            float a = f > .7f ? (1 - f) / .3f : std::min(1.f, f * 8);
            bool z = q.type == Particle::Z;
            Label(rt, z ? L"z" : q.text, q.x, q.y, (z ? 12 + f * 10 : 16) * k, z ? Hex(0xdfe6ff) : Hex(q.col), a);
            break;
        }
        default: {   // shard, soil, leaf: spin, fade near the end
            float a = f > .75f ? (1 - f) / .25f : 1;
            float c = std::cos(q.rot), s = std::sin(q.rot);
            D2D1_MATRIX_3X2_F n = On({ c * k, s * k, -s * k, c * k, q.x, q.y }, m);
            rt->SetTransform(&n);
            if (q.type == Particle::Shard && q.geo) {
                br->SetColor(Hex(0xc56a3d, a)); rt->FillGeometry(q.geo, br);
                br->SetColor(Hex(0x7d3c1e, a)); rt->DrawGeometry(q.geo, br, 1.2f);
            } else if (q.type == Particle::Soil) {
                D2D1_ELLIPSE e{ { 0, 0 }, q.size, q.size }; br->SetColor(Hex(0x5b3d26, a)); rt->FillEllipse(&e, br);
            } else if (q.type == Particle::Leaf) {
                D2D1_ELLIPSE e{ { 0, 0 }, q.size * .3f, q.size }; br->SetColor(Hex(0x4f8a3a, a)); rt->FillEllipse(&e, br);
            }
            rt->SetTransform(&m);
        }
        }
    }
}

// The bed: a mattress with a pillow at its head end, sliding down behind the floor line.
static void DrawBed(ID2D1RenderTarget* rt, const Bed& b, float s, float k) {
    D2D1_RECT_F clip{ -1e6f, -1e6f, 1e6f, b.floor };
    rt->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ID2D1SolidColorBrush* br = g_gr.br;
    float len = 123 * s, th = 10 * s, x = b.x, y = b.floor + b.dy;
    D2D1_ROUNDED_RECT mt{ { x, y - th, x + len, y }, 4 * s, 4 * s };
    br->SetColor(Hex(0xe8e3f4)); rt->FillRoundedRectangle(&mt, br);
    br->SetColor(Hex(0xa79ec4)); rt->DrawRoundedRectangle(&mt, br, 1.5f * k);
    br->SetColor(D2D1_COLOR_F{ 150 / 255.f, 140 / 255.f, 190 / 255.f, .5f });
    for (int i = 1; i < 6; i++) rt->DrawLine({ x + i * len / 6, y - th + 2 * k }, { x + i * len / 6, y - 2 * k }, br, 1 * k);
    float px = b.dir > 0 ? x + 3 * s : x + len - 29 * s;
    D2D1_ROUNDED_RECT pw{ { px, y - th - 7 * s, px + 26 * s, y - th + 1 * s }, 4 * s, 4 * s };
    br->SetColor(Hex(0xffffff)); rt->FillRoundedRectangle(&pw, br);
    br->SetColor(Hex(0xbdb5d6)); rt->DrawRoundedRectangle(&pw, br, 1.5f * k);
    rt->PopAxisAlignedClip();
}

// A portal: an upright glowing ring standing on the floor, dark inside.
static void DrawPortal(ID2D1RenderTarget* rt, const World& w) {
    float s = w.st.scale, o = w.portalOpen, cx = w.portalX, cy = w.portalFloor - 38 * s, rx = 22 * s * o, ry = 40 * s * o;
    ID2D1SolidColorBrush* br = g_gr.br;
    for (float f : { 1.4f, 1.2f }) { D2D1_ELLIPSE e{ { cx, cy }, rx * f, ry * f }; br->SetColor(Alpha(w.glow, .12f)); rt->FillEllipse(&e, br); }
    D2D1_ELLIPSE ring{ { cx, cy }, rx, ry }; br->SetColor(Alpha(w.glow, .95f)); rt->FillEllipse(&ring, br);
    D2D1_ELLIPSE in{ { cx, cy }, std::max(0.f, rx - 3 * s * o), std::max(0.f, ry - 3 * s * o) };
    br->SetColor(D2D1_COLOR_F{ .04f, .05f, .09f, .94f }); rt->FillEllipse(&in, br);
    D2D1_ELLIPSE swirl{ { cx, cy }, rx * .55f, ry * .6f }; br->SetColor(Alpha(w.glow, .16f)); rt->FillEllipse(&swirl, br);
}

void DrawPet(ID2D1RenderTarget* rt, const World& w, const Pose& p, Expr expr) {
    float k = w.st.scale / 1.25f;
    if (w.bed) DrawBed(rt, w.bd, w.st.scale, k);
    if (w.portal && w.portalOpen > .01f) DrawPortal(rt, w);
    if (w.shrink > .06f) DrawRobot(rt, p, w.st.scale * w.shrink, *w.pal, w.glow, expr, &w);   // smaller while it goes through a portal
    DrawParticles(rt, w.parts, k);
    if (w.time <= w.dizzyUntil && g_star) {   // dizzy stars circling the helmet
        V2 hd = ToWorld(p, Kinematics(p).head, w.st.scale);
        D2D1_MATRIX_3X2_F m; rt->GetTransform(&m);
        for (int i = 0; i < 3; i++) {
            float a = w.time * 5 + i * 2.09f, x = hd.x + std::cos(a) * 20 * k, y = hd.y - 22 * k + std::sin(a) * 6 * k;
            D2D1_MATRIX_3X2_F n = On({ k, 0, 0, k, x, y }, m);
            rt->SetTransform(&n);
            g_gr.br->SetColor(Hex(0xffd84a)); rt->FillGeometry(g_star, g_gr.br);
        }
        rt->SetTransform(&m);
    }
}

}  // namespace pet
