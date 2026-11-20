// Prism Pet - skeleton math. A 1:1 port of the browser prototype's rig so its poses keep working.
#include "pet_rig.h"
#include <cmath>
#include <algorithm>

namespace pet {

static float Lerp(float a, float b, float u) { return a + (b - a) * u; }
static V2 Rot(V2 v, float t) { float c = std::cos(t), s = std::sin(t); return { v.x * c - v.y * s, v.x * s + v.y * c }; }
static V2 Dir(float a, float l) { return { std::sin(a) * l, std::cos(a) * l }; }
static V2 Add(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }

Pose Stand(const Stage& s) {
    Pose p; p.y = s.StandY(); p.v = 1;
    p.aF1 = .13f; p.aF2 = -.06f; p.aB1 = -.13f; p.aB2 = .06f;
    p.lF1 = .07f; p.lB1 = -.07f;
    return p;
}
Pose StandSide(const Stage& s) {
    Pose p; p.y = s.StandY(); p.v = 0;
    p.aF1 = .1f; p.aF2 = .18f; p.aB1 = -.1f; p.aB2 = .18f;
    p.lF1 = .04f; p.lB1 = -.04f;
    return p;
}

void ToStand(Pose& p, const Stage& s) {
    Pose q = Stand(s);
    q.x = p.x; q.face = p.face; q.eyes = p.eyes; q.mouth = p.mouth;
    p = q;
}
void ToStandSide(Pose& p, const Stage& s) {
    Pose q = StandSide(s);
    q.x = p.x; q.face = p.face; q.eyes = p.eyes; q.mouth = p.mouth;
    p = q;
}
Pose LerpPose(const Pose& a, const Pose& b, float u) {
    Pose r = b;
    float Pose::*f[] = { &Pose::x, &Pose::y, &Pose::rot, &Pose::t, &Pose::h, &Pose::v, &Pose::gy, &Pose::aF1, &Pose::aF2,
                         &Pose::aB1, &Pose::aB2, &Pose::lF1, &Pose::lF2, &Pose::lB1, &Pose::lB2 };
    for (auto m : f) r.*m = Lerp(a.*m, b.*m, u);
    return r;
}
float Eased(Ease e, float u) {
    switch (e) {
    case Ease::InOut: return u < .5f ? 2 * u * u : 1 - std::pow(-2 * u + 2, 2.f) / 2;
    case Ease::Out: return 1 - std::pow(1 - u, 3.f);
    case Ease::In: return u * u * u;
    default: return u;
    }
}

V2 ShoulderOf(const Pose& p, Side side) { return Rot({ (side == F ? SHX : -SHX) * p.v, -L_TORSO + 2 * p.v }, p.t); }
V2 HipOf(const Pose& p, Side side) { return { (side == F ? HPX : -HPX) * p.v + (side == F ? 2.f : -2.f) * (1 - p.v), 0 }; }

Fk Kinematics(const Pose& p) {
    Fk k;
    k.sh = Rot({ 0, -L_TORSO }, p.t);
    k.shF = ShoulderOf(p, F); k.shB = ShoulderOf(p, B);
    k.hipF = HipOf(p, F); k.hipB = HipOf(p, B);
    k.head = Add(k.sh, Rot({ 0, -(L_NECK + L_HR) }, p.t + p.h));
    auto arm = [&](V2 s, float a1, float a2, V2& el, V2& hand) {
        el = Add(s, Dir(a1 - p.t, L_UA)); hand = Add(el, Dir(a1 + a2 - p.t, L_FA));
    };
    auto leg = [&](V2 hp, float l1, float l2, V2& kn, V2& foot) {
        kn = Add(hp, Dir(l1, L_TH)); foot = Add(kn, Dir(l1 + l2, L_SH));
    };
    arm(k.shF, p.aF1, p.aF2, k.eF, k.hF);
    arm(k.shB, p.aB1, p.aB2, k.eB, k.hB);
    leg(k.hipF, p.lF1, p.lF2, k.kF, k.fF);
    leg(k.hipB, p.lB1, p.lB2, k.kB, k.fB);
    return k;
}

V2 ToWorld(const Pose& p, V2 v, float scale) { V2 r = Rot({ v.x * p.face * scale, v.y * scale }, p.rot); return { p.x + r.x, p.y + r.y }; }
V2 ToLocal(const Pose& p, V2 w, float scale) { V2 r = Rot({ w.x - p.x, w.y - p.y }, -p.rot); return { r.x / (p.face * scale), r.y / scale }; }

// 2-bone IK: returns the angle at the root (A) and the inner angle at the middle joint (C).
static void TwoBone(float l1, float l2, float dx, float dy, float& base, float& A, float& C) {
    float d = std::clamp(std::hypot(dx, dy), .01f, l1 + l2 - .01f);
    base = std::atan2(dx, dy);
    A = std::acos(std::clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.f, 1.f));
    C = std::acos(std::clamp((l1 * l1 + l2 * l2 - d * d) / (2 * l1 * l2), -1.f, 1.f));
}
void ArmIK(Pose& p, Side side, V2 tg) {
    V2 s = ShoulderOf(p, side);
    float bend = (p.v > .5f && side == F) ? -1.f : 1.f, base, A, C;
    TwoBone(L_UA, L_FA, tg.x - s.x, tg.y - s.y, base, A, C);
    (side == F ? p.aF1 : p.aB1) = base - bend * A + p.t;
    (side == F ? p.aF2 : p.aB2) = bend * (PI - C);
}
void LegIK(Pose& p, Side side, V2 tg) {
    V2 hp = HipOf(p, side);
    float bend = (p.v > .5f && side == B) ? -1.f : 1.f, base, A, C;
    TwoBone(L_TH, L_SH, tg.x - hp.x, tg.y - hp.y, base, A, C);
    (side == F ? p.lF1 : p.lB1) = base + bend * A;
    (side == F ? p.lF2 : p.lB2) = -bend * (PI - C);
}

void Crouch(Pose& q, float d, const Stage& s) {
    q.y = s.StandY() + d * s.scale;
    LegIK(q, F, { Lerp(3, 9, q.v), LEG - d });
    LegIK(q, B, { Lerp(-3, -9, q.v), LEG - d });
}

void WalkPose(Pose& p, float ph, const Stage& s) {
    float sF = std::sin(ph), sB = std::sin(ph + PI);
    p.lF1 = .45f * sF; p.lB1 = .45f * sB;
    p.lF2 = -.95f * std::max(0.f, std::cos(ph)); p.lB2 = -.95f * std::max(0.f, std::cos(ph + PI));
    p.aF1 = -.5f * sF + .05f; p.aB1 = -.5f * sB + .05f;
    p.aF2 = .35f + .25f * std::max(0.f, -sF); p.aB2 = .35f + .25f * std::max(0.f, -sB);
    p.t = .07f; p.h = -.03f; p.rot = 0; p.gy = 0;
    p.y = s.StandY() + s.scale * LEG * (1 - std::cos(.45f * std::fabs(sF)));
}


std::vector<Span> Uncovered(float l, float r, const std::vector<Span>& covers) {
    std::vector<Span> out{ { l, r } };
    for (const Span& c : covers) {
        std::vector<Span> next;
        for (const Span& s : out) {
            if (c.r <= s.l || c.l >= s.r) { next.push_back(s); continue; }
            if (c.l > s.l) next.push_back({ s.l, c.l });
            if (c.r < s.r) next.push_back({ c.r, s.r });
        }
        out.swap(next);
    }
    return out;
}

std::vector<LedgeStep> PlanLedgeRoute(const std::vector<Ledge>& L, float x0, float y0, float bottom, float m, float left, float right) {
    auto landing = [&](float x, float y) {   // highest ledge below y with room for the feet at x, -1 = the taskbar
        int best = -1;
        for (int i = 0; i < (int)L.size(); i++)
            if (L[i].y > y + 4 && x >= L[i].l + m * .5f && x <= L[i].r - m * .5f && (best < 0 || L[i].y < L[best].y)) best = i;
        return best;
    };
    const float fallCost = .3f;   // falls are quick: they count a little, walking counts fully
    // Dijkstra over exits: node 2i+e = leaving ledge i by its left (e = 0) or right (e = 1) end.
    int n = 2 * (int)L.size(), done = n;   // node n = on the taskbar
    std::vector<float> dist(n + 1, 1e30f); std::vector<int> prev(n + 1, -2), visited(n + 1, 0);
    auto offOf = [&](int node) { const Ledge& e = L[node / 2]; return node % 2 ? e.r + m : e.l - m; };
    auto relax = [&](int from, float base, float x, float y) {   // standing at (x, y) after a landing
        int j = landing(x, y);
        if (j < 0) {   // reached the taskbar
            float c = base + fallCost * (bottom - y);
            if (c < dist[done]) { dist[done] = c; prev[done] = from; }
            return;
        }
        float fall = fallCost * (L[j].y - y);
        for (int e = 0; e < 2; e++) {
            int q = 2 * j + e; float end = e ? L[j].r : L[j].l, off = offOf(q);
            if (off < left + m || off > right - m) continue;   // a screen side: no way down there
            float c = base + fall + std::fabs(x - end) + m;
            if (c < dist[q]) { dist[q] = c; prev[q] = from; }
        }
    };
    relax(-1, 0, x0, y0);
    for (;;) {
        int u = -1;
        for (int i = 0; i <= n; i++) if (!visited[i] && dist[i] < 1e29f && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0 || u == done) break;
        visited[u] = 1;
        relax(u, dist[u], offOf(u), L[u / 2].y);
    }
    std::vector<LedgeStep> route;
    if (prev[done] == -2) return { { x0, bottom } };   // no route (all ends blocked): straight down
    std::vector<int> exits;
    for (int v = prev[done]; v >= 0; v = prev[v]) exits.push_back(v);
    std::reverse(exits.begin(), exits.end());
    float x = x0, y = y0;
    auto fallFrom = [&](float fx, float fy) { int j = landing(fx, fy); return j < 0 ? bottom : L[j].y; };
    route.push_back({ x, fallFrom(x, y) });   // the first drop, straight down
    for (int v : exits) { x = offOf(v); y = L[v / 2].y; route.push_back({ x, fallFrom(x, y) }); }
    return route;
}

}  // namespace pet
