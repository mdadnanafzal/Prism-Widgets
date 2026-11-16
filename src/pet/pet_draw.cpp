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

static float Lerp(float a, float b, float u) { return a + (b - a) * u; }
static D2D1_COLOR_F Hex(uint32_t c, float a = 1) { return { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a }; }

// ---------------------------------------------------------------- cached D2D resources
static ID2D1PathGeometry *g_laughMouth = nullptr, *g_angryCut[2] = {};

void ReleaseDrawCache() {
    SafeRelease(g_laughMouth); SafeRelease(g_angryCut[0]); SafeRelease(g_angryCut[1]);
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
    void RR(float x, float y, float w, float h, float r, D2D1_COLOR_F fill, float lw = 0) {
        r = std::min(r, std::min(w, h) / 2);   // the canvas shrinks radii that don't fit; D2D would turn it into an ellipse
        D2D1_ROUNDED_RECT rr{ { x, y, x + w, y + h }, r, r };
        rt->FillRoundedRectangle(&rr, B(fill));
        if (lw > 0) rt->DrawRoundedRectangle(&rr, B(Hex(pal.line)), lw);
    }
    void Ellipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F fill, float lw = 0) {
        D2D1_ELLIPSE e{ { cx, cy }, rx, ry };
        rt->FillEllipse(&e, B(fill));
        if (lw > 0) rt->DrawEllipse(&e, B(Hex(pal.line)), lw);
    }
    void Line(V2 a, V2 b, D2D1_COLOR_F c, float w) { rt->DrawLine({ a.x, a.y }, { b.x, b.y }, B(c), w, round); }
    // Thick round-capped limb with an outline: the outline colour slightly wider underneath.
    void Limb(V2 a, V2 b, V2 c, float w, D2D1_COLOR_F col) {
        D2D1_COLOR_F ln = Hex(pal.line);
        Line(a, b, ln, w + 4.2f); Line(b, c, ln, w + 4.2f);
        Line(a, b, col, w); Line(b, c, col, w);
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
        for (float f : { 1.f, .8f, .62f, .46f }) {
            D2D1_ELLIPSE e{ { cx, cy }, rx * f, ry * f };
            rt->FillEllipse(&e, B(Alpha(glow, .11f)));
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
        for (int i = 1; i <= 8; i++) {
            float a = (.25f + .37f * i / 8) * PI;
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

void DrawRobot(ID2D1RenderTarget* rt, const Pose& p, float scale, const Palette& pal, D2D1_COLOR_F glow, Expr expr) {
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
        c.Translate(k.head.x, k.head.y); c.Rotate(p.t + p.h); c.Scale(.9f, .9f);
        Head(c, 1 - v, expr, p.gy);
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
    if (v > .5f && !upB) arm(B);
    if (!upF) arm(F);
    c.Set(base);
}

}  // namespace pet
