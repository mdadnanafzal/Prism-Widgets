// Prism Pet - the step runner and every scene, ported from the browser prototype's SCENES.
// Distances are in local units (scaled by W.st.scale) or prototype pixels (scaled by K()).
#include "pet_internal.h"
#include <cmath>
#include <dwmapi.h>

namespace pet {

World W;
SceneCtx C;

using Mod = std::function<void(Pose&)>;
using Upd = std::function<void(float u, float h, float t)>;

static float Rand() {   // xorshift; looks are all it needs
    static uint32_t s = GetTickCount() | 1;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xffffff) / 16777216.f;
}
static V2 Rot(V2 v, float t) { float c = std::cos(t), s = std::sin(t); return { v.x * c - v.y * s, v.x * s + v.y * c }; }
static float StandY() { return W.st.StandY(); }

// ---------------------------------------------------------------- runner
bool RunStep(Scene& s, size_t& i, float h) {
    if (i >= s.size()) return false;
    Step& st = s[i];
    if (!st.started) {
        st.started = true; st.t = 0;
        if (st.begin) st.begin(st);
        if (C.reduced && st.motion) st.dur = 0;   // still poses: jump straight to the end of tweens and walks
    }
    st.t += h;
    float u = st.dur > 0 ? std::clamp(st.t / st.dur, 0.f, 1.f) : 1.f;
    if (st.update) st.update(st, u, h, st.t);
    bool done = st.until ? st.until(st) : st.t >= st.dur;
    if (done) i++;
    return i < s.size();
}

void Spawn(const Particle& p) { if (W.parts.size() < 120) W.parts.push_back(p); }
static void Dust(float x, float y, float vx, float vy, float max, float size) {
    Particle q; q.type = Particle::Dust; q.x = x; q.y = y; q.vx = vx * K(); q.vy = vy * K(); q.max = max; q.size = size * K();
    Spawn(q);
}
void Say(const wchar_t* s, uint32_t col) {
    V2 hw = ToWorld(W.p, Kinematics(W.p).head, W.st.scale);
    Particle q; q.type = Particle::Text; q.text = s; q.col = col;
    q.x = hw.x + W.p.face * 10 * K(); q.y = hw.y - 34 * K(); q.vy = -26 * K(); q.max = 1.3f;
    Spawn(q);
}

void TickWorld(float h) {
    float prev = W.time;
    W.time += h;
    Cursor& c = W.cur;
    float vx = (c.x - c.px) / h, vy = (c.y - c.py) / h;
    c.ax = Lerp(c.ax, std::clamp((vx - c.vx) / h, -9e4f, 9e4f), .3f);
    c.ay = Lerp(c.ay, std::clamp((vy - c.vy) / h, -9e4f, 9e4f), .3f);
    c.vx = vx; c.vy = vy; c.px = c.x; c.py = c.y;
    for (Particle& q : W.parts) { q.life += h; q.vy += q.g * h; q.x += q.vx * h; q.y += q.vy * h; q.rot += q.vr * h; }
    W.parts.erase(std::remove_if(W.parts.begin(), W.parts.end(), [](const Particle& q) { return q.life >= q.max; }), W.parts.end());
    if (W.zzz && std::floor(prev / .8f) != std::floor(W.time / .8f)) {
        V2 hw = ToWorld(W.p, Kinematics(W.p).head, W.st.scale);
        Particle z; z.type = Particle::Z; z.x = hw.x + 6 * K(); z.y = hw.y - 18 * K(); z.vx = 14 * K(); z.vy = -22 * K(); z.max = 2;
        Spawn(z);
    }
}

void PutAwayProps() { W.laptop = W.phone = W.bed = false; W.pot = Pot::None; W.zzz = false; W.hanging = false; }

// ---------------------------------------------------------------- step helpers
static Step Act(std::function<void()> fn) { Step s; s.begin = [fn](Step&) { fn(); }; return s; }
static Step Wait(float dur, Upd fn = nullptr) {
    Step s; s.dur = dur;
    if (fn) s.update = [fn](Step&, float u, float h, float t) { fn(u, h, t); };
    return s;
}
static Step Fps(int n) { return Act([n] { W.fps = n; }); }
static Step Look(Mod m) { return Act([m] { m(W.p); }); }
// Tween the pose toward `mod` applied to a copy of the current pose (evaluated when the step starts).
static Step Tw(Mod mod, float dur, Ease e = Ease::InOut, Upd onUpd = nullptr) {
    Step s; s.dur = dur; s.motion = true;
    s.begin = [mod](Step& st) {
        st.a = W.p; st.b = W.p; mod(st.b);
        W.p.face = st.b.face; W.p.eyes = st.b.eyes; W.p.mouth = st.b.mouth;
    };
    s.update = [e, onUpd](Step& st, float u, float h, float t) {
        Pose r = LerpPose(st.a, st.b, Eased(e, u));
        r.face = W.p.face; r.eyes = W.p.eyes; r.mouth = W.p.mouth;
        W.p = r;
        if (onUpd) onUpd(u, h, t);
    };
    return s;
}
// Pose modifiers: standing (front / side) with extra changes.
static Mod T(Mod extra = nullptr) { return [extra](Pose& q) { ToStand(q, W.st); if (extra) extra(q); }; }
static Mod TS(Mod extra = nullptr) { return [extra](Pose& q) { ToStandSide(q, W.st); if (extra) extra(q); }; }

// Walk cycle to x (px): turns side-on as it starts. speed in prototype px/s; yo: extra px on y over the walk.
static Step Walk(std::function<float()> to, float speed = 115, float yo0 = 0, float yo1 = 0) {
    Step s; s.motion = true;
    s.begin = [to, speed](Step& st) {
        st.f[0] = W.p.x; st.f[1] = W.p.v; st.f[2] = to();
        st.dur = std::max(.05f, std::fabs(st.f[2] - st.f[0]) / (speed * K() * C.walkSpeed));
        W.p.face = st.f[2] >= st.f[0] ? 1.f : -1.f; W.idle = false; W.fps = 30;
    };
    s.update = [yo0, yo1](Step& st, float u, float, float t) {
        W.p.x = Lerp(st.f[0], st.f[2], u);
        WalkPose(W.p, std::fabs(W.p.x - st.f[0]) / (W.st.scale * 17), W.st);
        W.p.v = st.f[1] * std::max(0.f, 1 - t / .15f);
        W.p.y += Lerp(yo0, yo1, u);
    };
    return s;
}
static Step Walk(float x, float speed = 115) { return Walk([x] { return x; }, speed); }

static void Append(Scene& a, Scene b) { for (auto& s : b) a.push_back(std::move(s)); }
static Scene StandUp(float d = .45f) {
    return { Tw([](Pose& q) { q.t = q.v > .5f ? 0 : .25f; Crouch(q, 12, W.st); }, .3f), Tw(T(), d) };
}

// ---------------------------------------------------------------- physics
static void HangPose() {
    Pose& p = W.p; Hang& H = W.hang;
    p.rot = H.th; p.t = 0; p.h = .25f + .25f * std::sin(H.th);
    p.aF1 = PI - .75f; p.aF2 = 0; p.aB1 = H.th * .9f + Lerp(.15f, -.2f, p.v); p.aB2 = .3f;
    float lag = H.th - H.th2, kick = H.kick ? .35f * std::sin(W.time * 9) : 0;
    p.lF1 = lag + .12f + kick; p.lF2 = -.3f; p.lB1 = lag - .1f - kick; p.lB2 = -.2f;
    V2 hf = Kinematics(p).hF, hw = Rot({ hf.x * p.face * W.st.scale, hf.y * W.st.scale }, p.rot);
    p.x = W.cur.x - hw.x; p.y = W.cur.y - hw.y;
}
// Pendulum hanging from the cursor by hand F, pushed by the cursor's acceleration; the legs lag behind.
void HangStep(float h) {
    Hang& H = W.hang; const Cursor& c = W.cur;
    float Lp = 60 * W.st.scale, g = 1400 * K(), ax = std::clamp(c.ax, -7000 * K(), 7000 * K()), ay = std::clamp(c.ay, -7000 * K(), 7000 * K());
    float al = -(g + ay) / Lp * std::sin(H.th) + ax / Lp * std::cos(H.th) - 1.5f * H.w;
    H.w += al * h; H.th = std::clamp(H.th + H.w * h, -1.7f, 1.7f);
    H.w2 += (40 * (H.th - H.th2) - 7 * H.w2) * h; H.th2 += H.w2 * h;
    HangPose();
}

static void Flail(float t) {
    Pose& p = W.p;
    p.aF1 = 2 + 1.2f * std::sin(t * 14); p.aB1 = -1.5f + 1.2f * std::sin(t * 12 + 1); p.aF2 = .6f; p.aB2 = .6f;
    p.lF1 = .6f * std::sin(t * 11); p.lB1 = -.6f * std::sin(t * 11); p.lF2 = -.6f; p.lB2 = -.5f;
    p.eyes = Eyes::Dizzy; p.mouth = Mouth::Open; p.t = 0; p.h = 0;
}
// Ballistic tumble with bounces off the work area's edges, until it comes to rest on the taskbar edge.
static Step Tumble(std::function<void(float)> extra = nullptr) {
    Step s;
    s.begin = [](Step& st) { st.f[0] = 0; W.fps = 30; W.idle = false; };
    s.until = [](Step& st) { return st.f[0] != 0; };
    s.update = [extra](Step& st, float, float h, float t) {
        Phys& P = W.phys; Pose& p = W.p; const RECT& wa = W.work;
        if (extra) extra(h);
        p.v = Lerp(p.v, 1, .08f);
        P.vy += 1500 * K() * h; p.x += P.vx * h; p.y += P.vy * h; p.rot += P.w * h;
        Flail(t);
        float lie = std::fabs(std::sin(p.rot)), restY = wa.bottom - W.st.scale * (22 + 11 * (1 - lie));
        if (p.x < wa.left + U(28)) { p.x = wa.left + U(28); P.vx = std::fabs(P.vx) * .5f; P.w *= -.7f; }
        if (p.x > wa.right - U(28)) { p.x = wa.right - U(28); P.vx = -std::fabs(P.vx) * .5f; P.w *= -.7f; }
        if (p.y < wa.top + 50 * K()) { p.y = wa.top + 50 * K(); P.vy = std::fabs(P.vy) * .4f; }
        if (p.y >= restY && P.vy > 0) {
            p.y = restY;
            if (P.vy < 300 * K() || t > 4) st.f[0] = 1;
            for (int i = 0; i < 5; i++) Dust(p.x + (Rand() - .5f) * 30 * K(), wa.bottom - 4 * K(), (Rand() - .5f) * 80, -30 - Rand() * 40, .7f, 5);
            P.vy = -P.vy * .38f; P.vx *= .6f; P.w *= .5f;
        }
    };
    return s;
}
// Lie on its back (flipping `face` so it is never face-down) on the current floor.
static Step Settle() {
    Step s; s.dur = .35f;
    s.begin = [](Step& st) {
        Pose& p = W.p;
        float r = std::atan2(std::sin(p.rot), std::cos(p.rot));
        st.f[0] = r >= 0 ? PI / 2 : -PI / 2;
        p.face = st.f[0] > 0 ? -1.f : 1.f; p.rot = r; st.a = p;
        W.dizzyUntil = W.time + 1.9f;
    };
    s.update = [](Step& st, float u, float, float) {
        Pose tg = st.a;
        tg.rot = st.f[0]; tg.y = W.st.floor - U(22); tg.t = 0; tg.h = 0; tg.v = 0; tg.gy = 0;
        tg.aF1 = .1f; tg.aF2 = .3f; tg.aB1 = -.1f; tg.aB2 = .3f; tg.lF1 = .05f; tg.lF2 = 0; tg.lB1 = -.05f; tg.lB2 = 0;
        Pose r = LerpPose(st.a, tg, Eased(Ease::Out, u));
        r.face = W.p.face; r.eyes = Eyes::Dizzy; r.mouth = Mouth::Flat;
        W.p = r;
    };
    return s;
}
static Scene GetUp() {
    Scene s;
    s.push_back(Wait(1.1f));
    s.push_back(Tw([](Pose& q) {
        q.rot = 0; q.v = 0; q.t = .2f; q.h = .1f; q.y = W.st.floor - U(6);
        q.lF1 = 1.45f; q.lF2 = -.25f; q.lB1 = 1.35f; q.lB2 = -.2f; q.aF1 = .5f; q.aF2 = .5f; q.aB1 = -.3f; q.aB2 = .4f;
        q.eyes = Eyes::Half; q.mouth = Mouth::Flat;
    }, .6f));
    Append(s, StandUp(.4f));
    s.push_back(Wait(.6f, [](float u, float, float t) { W.p.h = .28f * std::sin(t * 22) * (1 - u); }));
    s.push_back(Look([](Pose& q) { q.eyes = Eyes::Open; q.mouth = Mouth::Smile; }));
    return s;
}
static Step LandOnTaskbar() { return Act([] { W.st.floor = (float)W.work.bottom; }); }

// ---------------------------------------------------------------- scenes
Scene SceneIdle(std::function<bool()> until) {
    Scene s;
    s.push_back(Act([] { W.fps = 12; W.idle = true; }));
    Step loop; loop.until = [until](Step&) { return until(); };
    s.push_back(loop);
    s.push_back(Act([] { W.idle = false; }));
    return s;
}

Scene SceneWalk(float toX) {
    if (std::fabs(toX - W.p.x) < 2) return {};
    return { Walk(toX), Tw(T(), .25f) };
}

Scene SceneTickle() {
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Tw([](Pose& q) {
        q.t = 0; q.h = .1f; q.gy = 1.5f; q.eyes = Eyes::Happy; q.mouth = Mouth::Laugh; q.rot = 0; q.v = 1;
        Crouch(q, 5, W.st); ArmIK(q, F, { -4, -12 }); ArmIK(q, B, { 4, -8 });
    }, .22f, Ease::Out));
    Step sq; sq.dur = 1.9f;   // squirm
    sq.begin = [](Step& st) { st.f[0] = W.p.x; st.f[1] = 0; };
    sq.update = [](Step& st, float, float, float t) {
        Pose& p = W.p;
        p.x = st.f[0] + 2.2f * K() * std::sin(t * 40); p.rot = .08f * std::sin(t * 26); p.h = .14f * std::sin(t * 17);
        p.y = StandY() + U(5) + U(1.5f) * std::sin(t * 34);
        ArmIK(p, F, { -4, -12 }); ArmIK(p, B, { 4, -8 });
        if (t > st.f[1] * .65f) { st.f[1]++; Say(L"hehe"); }
    };
    s.push_back(sq);
    s.push_back(Tw(T([](Pose& q) { q.eyes = Eyes::Happy; q.mouth = Mouth::Smile; }), .5f));
    s.push_back(Wait(.4f));
    s.push_back(Look([](Pose& q) { q.eyes = Eyes::Open; }));
    s.push_back(Fps(15));
    return s;
}

Scene SceneDragHang(std::function<bool()> released) {
    Scene s;
    s.push_back(Act([] {
        W.hang = Hang{}; W.hanging = true; W.fps = 30; W.idle = false;
        W.p.eyes = Eyes::Open; W.p.mouth = Mouth::O;
    }));
    Step hang; hang.until = [released](Step&) { return released(); };
    hang.update = [](Step&, float, float h, float) { HangStep(h); W.p.eyes = Eyes::Open; W.p.mouth = Mouth::O; };
    s.push_back(hang);
    s.push_back(Act([] {   // let go: it lands right there, on an invisible floor under its feet
        W.hanging = false;
        W.p.x = std::clamp(W.p.x, W.work.left + U(30), W.work.right - U(30));
        W.st.floor = std::min((float)W.work.bottom, W.p.y + U(STANDH + 8));
    }));
    Step land; land.dur = .26f;
    land.begin = [](Step& st) { st.a = W.p; };
    land.update = [](Step& st, float u, float, float) {
        Pose& p = W.p; const Pose& f = st.a;
        p.rot = Lerp(f.rot, 0, Eased(Ease::Out, u)); p.y = Lerp(f.y, StandY(), Eased(Ease::In, u));
        p.aF1 = Lerp(f.aF1, 2.4f, u); p.aF2 = .3f; p.aB1 = Lerp(f.aB1, -2.2f, u); p.aB2 = .3f;
        p.lF1 = Lerp(f.lF1, .15f, u); p.lB1 = Lerp(f.lB1, -.1f, u); p.lF2 = -.2f; p.lB2 = -.15f; p.mouth = Mouth::Open;
    };
    s.push_back(land);
    s.push_back(Act([] { for (int i = 0; i < 6; i++) Dust(W.p.x + (Rand() - .5f) * 36 * K(), W.st.floor - 3 * K(), (Rand() - .5f) * 90, -20 - Rand() * 30, .6f, 4); }));
    s.push_back(Tw([](Pose& q) {
        q.rot = 0; q.t = 0; q.h = .1f; q.v = 1; q.gy = 0; q.aF1 = 1.2f; q.aF2 = -.4f; q.aB1 = -1.2f; q.aB2 = .4f;
        q.eyes = Eyes::Dizzy; q.mouth = Mouth::Flat; Crouch(q, 12, W.st);
    }, .18f, Ease::Out));
    s.push_back(Wait(.6f));
    s.push_back(Look([](Pose& q) { q.eyes = Eyes::Open; }));
    s.push_back(Tw(T(), .4f));
    s.push_back(Tw([](Pose& q) { q.t = 0; q.h = 0; q.gy = 2; Crouch(q, 5, W.st); ArmIK(q, F, { 11, -2 }); ArmIK(q, B, { -11, -3 }); }, .3f));
    s.push_back(Wait(2, [](float, float h, float t) {   // dust itself off
        Pose& p = W.p;
        float v = .5f + .5f * std::sin(t * 2 * PI * 2.4f), w2 = .5f + .5f * std::sin(t * 2 * PI * 2.4f + PI);
        p.gy = 2; p.h = .08f * std::sin(t * 3);
        ArmIK(p, F, { Lerp(11, 5, v), Lerp(-2, -17, v) }); ArmIK(p, B, { Lerp(-11, -5, w2), Lerp(-3, -16, w2) });
        if (Rand() < h * 9) { V2 w = ToWorld(p, Kinematics(p).hF, W.st.scale); Dust(w.x, w.y, (Rand() - .5f) * 50, -15 - Rand() * 25, .7f, 4); }
    }));
    s.push_back(Tw(T([](Pose& q) { q.mouth = Mouth::Smile; }), .35f));
    s.push_back(Fps(15));
    return s;
}

// ---- coding: sit on the floor edge at home with a laptop that comes up from behind it
static Mod SitOnEdge() {
    return [](Pose& q) {
        ToStandSide(q, W.st);
        q.y = W.st.floor - U(5); q.t = -.04f; q.h = .08f;
        q.lF1 = 1.5f; q.lF2 = -1.5f; q.lB1 = 1.38f; q.lB2 = -1.3f; q.aF1 = .25f; q.aF2 = .9f; q.aB1 = .2f; q.aB2 = .9f;
    };
}
static void ArmToLaptop(float dx = 2) { ArmIK(W.p, F, { W.lap.x + dx, W.lap.y - 4 }); }

Scene SceneCodingEnter() {
    Scene s;
    Append(s, SceneGoHome());
    s.push_back(Tw(TS(), .25f));
    s.push_back(Look([](Pose& q) { q.face = C.faceIn; }));
    s.push_back(Wait(.2f));
    s.push_back(Tw([](Pose& q) { q.t = .25f; Crouch(q, 12, W.st); }, .35f));
    s.push_back(Tw(SitOnEdge(), .45f));
    s.push_back(Tw([](Pose& q) { q.t = .5f; q.h = .35f; ArmIK(q, F, { 20, 0 }); }, .4f));
    s.push_back(Act([] { W.lap = Laptop{}; W.laptop = true; }));
    s.push_back(Wait(.6f, [](float u, float, float) { W.lap.y = Lerp(30, 5, Eased(Ease::Out, u)); ArmToLaptop(); }));
    s.push_back(Wait(.55f, [](float u, float, float) {
        float v = Eased(Ease::InOut, u);
        W.lap.x = Lerp(18, 1, v); W.lap.y = Lerp(5, -5, v); W.p.t = Lerp(.5f, -.04f, v); W.p.h = Lerp(.35f, .3f, v); ArmToLaptop();
    }));
    s.push_back(Wait(.5f, [](float u, float, float) {
        W.lap.lid = Lerp(0, 1.85f, Eased(Ease::InOut, u));
        ArmIK(W.p, F, { Lerp(4, 13, u), -9 }); ArmIK(W.p, B, { Lerp(-3, 8, u), -9 });
    }));
    return s;
}

static void Typing(Pose& p) {
    p.t = -.04f;
    ArmIK(p, F, { 12 + Rand() * 5, -9 - Rand() * 1.5f }); ArmIK(p, B, { 7 + Rand() * 4, -9 - Rand() * 1.5f });
    p.h = .3f + (Rand() - .5f) * .04f;
}
// Types until `until`, scratching its helmet now and then.
Scene SceneTyping(std::function<bool()> until) {
    Scene s;
    s.push_back(Fps(12));
    Step loop;
    loop.begin = [](Step& st) { st.f[0] = 4 + Rand() * 8; st.f[1] = 0; };   // f0: next scratch, f1: said "?"
    loop.until = [until](Step&) { return until(); };
    loop.update = [](Step& st, float, float, float t) {
        Pose& p = W.p;
        float at = st.f[0];
        if (t > at && t < at + 1.5f) {
            Typing(p);
            Fk k = Kinematics(p);
            ArmIK(p, F, { k.head.x - 19 + 2 * std::sin(t * 50), k.head.y + 5 + 2 * std::cos(t * 50) });
            p.h = .05f; p.eyes = Eyes::Half; p.mouth = Mouth::Flat;
            if (!st.f[1] && t > at + .2f) { st.f[1] = 1; Say(L"?"); }
        } else {
            if (t >= at + 1.5f) { st.f[0] = t + 8 + Rand() * 12; st.f[1] = 0; }
            Typing(p); p.eyes = Eyes::Open; p.mouth = Mouth::Smile;
        }
    };
    s.push_back(loop);
    return s;
}

Scene SceneCodingExit() {
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Tw([](Pose& q) { q.t = -.04f; q.h = .2f; ArmIK(q, F, { 5, -7 }); ArmIK(q, B, { 2, -6 }); }, .35f));
    s.push_back(Wait(.45f, [](float u, float, float) { W.lap.lid = Lerp(1.85f, 0, Eased(Ease::InOut, u)); }));
    s.push_back(Wait(.5f, [](float u, float, float) {
        float v = Eased(Ease::InOut, u);
        W.lap.x = Lerp(1, 18, v); W.lap.y = Lerp(-5, 5, v); W.p.t = Lerp(-.04f, .5f, v); ArmToLaptop();
    }));
    s.push_back(Wait(.45f, [](float u, float, float) {
        W.lap.y = Lerp(5, 30, Eased(Ease::In, u)); ArmIK(W.p, F, { W.lap.x + 2, std::min(W.lap.y - 4, 2.f) });
    }));
    s.push_back(Act([] { W.laptop = false; }));
    s.push_back(Fps(24));
    s.push_back(Tw([](Pose& q) {
        q.t = .5f; q.h = .55f; q.aF1 = .6f; q.aF2 = .5f; q.aB1 = .5f; q.aB2 = .6f; q.eyes = Eyes::Half; q.mouth = Mouth::Flat;
    }, .7f));
    Step tired; tired.dur = 2.4f;   // sits there tired, sighs
    tired.begin = [](Step& st) { st.f[0] = 0; };
    tired.update = [](Step& st, float, float, float t) {
        W.p.t = .5f + .03f * std::sin(t * 1.8f);
        if (t > .5f && !st.f[0]) { st.f[0] = 1; Say(L"sigh…", 0xcfd6e6); }
    };
    s.push_back(tired);
    s.push_back(Fps(30));
    s.push_back(Tw([](Pose& q) { q.t = .1f; q.h = .1f; q.eyes = Eyes::Open; }, .4f));
    Append(s, StandUp(.45f));
    s.push_back(Look([](Pose& q) { q.mouth = Mouth::Smile; }));
    s.push_back(Fps(15));
    return s;
}

// ---- browser: lean on the nearest screen edge and scroll a phone
static Scene PhoneOut(V2 pocket, V2 at, float lean) {
    Scene s;
    s.push_back(Tw([pocket](Pose& q) { ArmIK(q, B, pocket); }, .35f));
    s.push_back(Act([pocket, at, lean] { W.ph = Phone{ 0, lean, at, pocket }; W.phone = true; }));
    s.push_back(Tw([at](Pose& q) { q.h = .3f; ArmIK(q, B, at); }, .45f));
    s.push_back(Tw([at](Pose& q) { ArmIK(q, F, { at.x + 2, at.y + 2 }); }, .25f));
    return s;
}
static Scene PhoneAway() {
    Scene s;
    s.push_back(Look([](Pose& q) { q.eyes = Eyes::Open; q.mouth = Mouth::Smile; }));
    s.push_back(Tw([](Pose& q) { q.h = 0; ArmIK(q, B, W.ph.pocket); ArmIK(q, F, { 5, -10 }); }, .45f));
    s.push_back(Act([] { W.phone = false; }));
    return s;
}
static float EdgeSide() { return W.p.x - W.work.left < W.work.right - W.p.x ? 1.f : -1.f; }   // +1: left edge, it faces right
static float EdgeX(float side, float inset) { return side > 0 ? W.work.left + U(inset) : W.work.right - U(inset); }

Scene SceneBrowserEnter() {
    float side = EdgeSide();
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Walk([side] { return EdgeX(side, 36); }));
    s.push_back(Tw(TS(), .2f));
    s.push_back(Look([side](Pose& q) { q.face = side; }));
    s.push_back(Wait(.15f));
    s.push_back(Tw(TS([side](Pose& q) {
        q.x = EdgeX(side, 36); q.t = -.2f; q.h = .02f; q.y = StandY() + U(2);
        q.lF1 = .32f; q.lF2 = -.05f; q.lB1 = .18f; q.lB2 = 0; q.aF1 = .1f; q.aB1 = -.2f;
    }), .5f));
    Append(s, PhoneOut({ -6, -2 }, { 9, -21 }, -.2f));
    return s;
}

// Scrolls until `until`, laughing now and then.
Scene SceneScrolling(std::function<bool()> until) {
    Scene s;
    s.push_back(Fps(15));
    Step loop;
    loop.begin = [](Step& st) { st.f[0] = 3 + Rand() * 6; st.f[1] = 0; };   // f0: next laugh, f1: 1 while laughing
    loop.until = [until](Step&) { return until(); };
    loop.update = [](Step& st, float, float h, float t) {
        Pose& p = W.p; Phone& ph = W.ph;
        ph.off += h * 14 * (std::sin(t * 1.3f) > .2f ? 1 : 0);
        float at = st.f[0];
        if (t > at && t < at + 1.1f) {
            if (!st.f[1]) { st.f[1] = 1; Say(L"haha"); W.fps = 30; }
            p.h = -.25f + .05f * std::sin(t * 30); p.t = ph.lean + (-.06f + .03f * std::sin(t * 30));
            p.eyes = Eyes::Happy; p.mouth = Mouth::Laugh;
        } else {
            if (st.f[1]) { st.f[1] = 0; st.f[0] = t + 6 + Rand() * 10; W.fps = 15; }
            p.h = .3f; p.t = ph.lean; p.eyes = Eyes::Open; p.mouth = Mouth::Smile;
        }
        ArmIK(p, B, ph.at); ArmIK(p, F, { ph.at.x + 2, ph.at.y + 2 + (Rand() < .5f ? 1.4f : 0) });
    };
    s.push_back(loop);
    return s;
}

static Step LaughOnce(float dur = 1.1f) {
    Step s; s.dur = dur;
    s.begin = [](Step&) { Say(L"haha"); };
    s.update = [](Step&, float, float, float t) {
        Pose& p = W.p;
        p.h = -.25f + .05f * std::sin(t * 30); p.eyes = Eyes::Happy; p.mouth = Mouth::Laugh;
        if (W.phone) {
            p.t = W.ph.lean - .05f + .03f * std::sin(t * 30);
            ArmIK(p, B, W.ph.at); ArmIK(p, F, { W.ph.at.x + 2, W.ph.at.y + 2 });
        }
    };
    return s;
}

Scene SceneBrowserExit() {
    Scene s;
    s.push_back(Fps(30));
    s.push_back(LaughOnce());
    Append(s, PhoneAway());
    s.push_back(Tw(T([](Pose& q) { q.x = EdgeX(EdgeSide(), 46); }), .5f));
    s.push_back(Fps(15));
    return s;
}

Scene SceneCodingToBoth() {
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Tw([](Pose& q) { q.t = -.04f; q.h = .2f; ArmIK(q, F, { 5, -7 }); ArmIK(q, B, { 2, -6 }); }, .3f));
    s.push_back(Wait(.6f, [](float u, float, float) {   // sets the open laptop down beside it
        float v = Eased(Ease::InOut, u);
        W.lap.x = Lerp(1, 20, v); W.lap.y = Lerp(-5, 5, v); W.p.t = Lerp(-.04f, .45f, v); ArmToLaptop(3);
    }));
    s.push_back(Tw([](Pose& q) { q.t = -.32f; q.h = 0; q.aF1 = .3f; q.aF2 = .6f; q.aB1 = .1f; q.aB2 = .6f; }, .55f));
    Append(s, PhoneOut({ -7, -3 }, { 7, -20 }, -.32f));
    return s;
}

Scene SceneBothToCoding() {
    Scene s;
    s.push_back(Fps(30));
    Append(s, PhoneAway());
    s.push_back(Tw([](Pose& q) { q.t = .45f; q.h = .3f; }, .45f, Ease::InOut, [](float, float, float) { ArmToLaptop(3); }));
    s.push_back(Wait(.6f, [](float u, float, float) {   // laptop back on its lap
        float v = Eased(Ease::InOut, u);
        W.lap.x = Lerp(20, 1, v); W.lap.y = Lerp(5, -5, v); W.p.t = Lerp(.45f, -.04f, v); ArmToLaptop(3);
    }));
    s.push_back(Wait(.3f, [](float u, float, float) { ArmIK(W.p, F, { Lerp(4, 13, u), -9 }); ArmIK(W.p, B, { Lerp(-3, 8, u), -9 }); }));
    return s;
}

// ---- bored: pace back and forth, then arms crossed with a foot tap
Scene ScenePacing(float range) {
    float c = W.p.x, r = (W.work.right - W.work.left) * range / 2;
    float lo = W.work.left + U(40), hi = W.work.right - U(40);
    if (c - r < lo) c = lo + r;
    if (c + r > hi) c = hi - r;
    if (c - r < lo) { c = (lo + hi) / 2; r = (hi - lo) / 2; }   // range wider than the screen
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Walk(c + r, 125));
    for (int i = 0; i < 4; i++) {
        s.push_back(Tw(TS(), .18f)); s.push_back(Walk(c - r, 125));
        s.push_back(Tw(TS(), .18f)); s.push_back(Walk(c + r, 125));
    }
    s.push_back(Tw(TS(), .18f)); s.push_back(Walk(c, 125));
    s.push_back(Tw(T(), .3f));
    s.push_back(Look([](Pose& q) { q.face = C.faceIn; }));
    s.push_back(Tw([](Pose& q) { q.h = 0; q.mouth = Mouth::Flat; q.eyes = Eyes::Half; ArmIK(q, B, { 6, -19 }); ArmIK(q, F, { -6, -18 }); }, .4f));
    s.push_back(Fps(20));
    s.push_back(Wait(3, [](float, float, float t) {
        Pose& p = W.p;
        ArmIK(p, B, { 6, -19 }); ArmIK(p, F, { -6, -18 });
        float tap = std::max(0.f, std::sin(t * 12));
        LegIK(p, F, { 11, LEG - 3 * tap }); LegIK(p, B, { -9, LEG }); p.h = .06f * std::sin(t * 1.5f);
    }));
    s.push_back(Fps(30));
    s.push_back(Tw(T([](Pose& q) { q.eyes = Eyes::Open; q.mouth = Mouth::Smile; }), .45f));
    s.push_back(Fps(15));
    return s;
}

// ---- mischief: throw a flower pot at the screen
Scene SceneFlowerPot() {
    Scene s;
    s.push_back(Fps(30));
    s.push_back(Tw([](Pose& q) { q.h = .12f; q.eyes = Eyes::Angry; q.mouth = Mouth::Flat; }, .3f));
    s.push_back(Wait(.5f));
    s.push_back(Tw([](Pose& q) { ArmIK(q, B, { -17, -6 }); }, .35f));   // reaches behind its back
    s.push_back(Act([] { W.pot = Pot::HandB; }));
    s.push_back(Wait(.15f));
    s.push_back(Tw([](Pose& q) { q.h = 0; ArmIK(q, F, { 8, -10 }); ArmIK(q, B, { -8, -10 }); }, .25f));
    s.push_back(Act([] { W.pot = Pot::Both; }));
    s.push_back(Tw([](Pose& q) { q.h = 0; q.gy = 1.5f; Crouch(q, 8, W.st); ArmIK(q, F, { 9, -2 }); ArmIK(q, B, { -9, -2 }); }, .35f));
    s.push_back(Wait(.15f));
    s.push_back(Tw([](Pose& q) { q.gy = -1.5f; q.mouth = Mouth::Open; Crouch(q, 1, W.st); ArmIK(q, F, { 8, -34 }); ArmIK(q, B, { -8, -34 }); }, .14f, Ease::In));
    s.push_back(Act([] {   // two-handed throw from the chest, toward the middle of the screen
        Fk k = Kinematics(W.p);
        V2 from = ToWorld(W.p, { (k.hF.x + k.hB.x) / 2, (k.hF.y + k.hB.y) / 2 - 6 }, W.st.scale);
        float dir = W.p.x > (W.work.left + W.work.right) / 2.f ? -1.f : 1.f;
        V2 to{ W.p.x + dir * U(240), W.st.floor - U(298) };
        to.x = std::clamp(to.x, W.work.left + U(80), W.work.right - U(80));
        to.y = std::clamp(to.y, W.work.top + U(80), W.work.bottom - U(80));
        W.pot = Pot::None;
        FxThrow(from, to, W.st.scale);
    }));
    s.push_back(Tw([](Pose& q) { q.gy = 0; Crouch(q, 4, W.st); ArmIK(q, F, { 12, -12 }); ArmIK(q, B, { -12, -12 }); }, .4f));
    s.push_back(Wait(.38f));
    s.push_back(Act([] { W.p.eyes = Eyes::Happy; W.p.mouth = Mouth::Laugh; Say(L"ha!"); }));
    s.push_back(Wait(3.1f, [](float u, float, float t) { W.p.y = StandY() - U(3) * std::fabs(std::sin(t * 9)) * (1 - u); }));
    s.push_back(Tw(T([](Pose& q) { q.eyes = Eyes::Open; q.mouth = Mouth::Smile; }), .4f));
    s.push_back(Fps(15));
    return s;
}

// ---- mischief: walk under the cursor, jump, hang on; `hold` decides when it lets go
Scene SceneGrab(std::function<int()> hold) {
    Scene s;
    s.push_back(Fps(30));
    Step chase;   // walk until standing beside the cursor (it may move)
    chase.begin = [](Step& st) { st.f[0] = 0; W.idle = false; };
    chase.update = [](Step& st, float, float h, float) {
        float dir = W.cur.x >= W.p.x ? 1.f : -1.f, tx = W.cur.x - dir * U(30), dx = tx - W.p.x;
        float step = std::min(std::fabs(dx), 115 * K() * C.walkSpeed * h);
        W.p.face = dx >= 0 ? 1.f : -1.f;
        W.p.x += step * W.p.face; st.f[0] += step;
        WalkPose(W.p, st.f[0] / (W.st.scale * 17), W.st);
        W.p.v = 0;
    };
    chase.until = [](Step& st) { float dir = W.cur.x >= W.p.x ? 1.f : -1.f; return std::fabs(W.cur.x - dir * U(30) - W.p.x) < 2 || st.t > 20; };
    s.push_back(chase);
    s.push_back(Tw(T([](Pose& q) { q.face = W.cur.x >= q.x ? 1.f : -1.f; }), .2f));
    s.push_back(Tw([](Pose& q) { q.gy = -2.5f; }, .3f));
    s.push_back(Wait(.3f));
    s.push_back(Tw([](Pose& q) { q.aF1 = .5f; q.aB1 = -.5f; Crouch(q, 10, W.st); }, .25f));
    Step jump; jump.dur = .32f;
    jump.begin = [](Step& st) {
        Pose q = W.p; q.aF1 = PI - .75f; q.aF2 = 0; q.t = 0;
        V2 hf = Kinematics(q).hF;
        st.f[0] = W.p.x; st.f[1] = W.p.y; st.f[2] = W.cur.x - hf.x * W.st.scale * W.p.face; st.f[3] = W.cur.y - hf.y * W.st.scale;
    };
    jump.update = [](Step& st, float u, float, float) {
        float v = Eased(Ease::Out, u); Pose& p = W.p;
        p.x = Lerp(st.f[0], st.f[2], v); p.y = Lerp(st.f[1], st.f[3], v);
        p.aF1 = Lerp(.5f, PI - .75f, v); p.aF2 = 0; p.t = 0;
        p.lF1 = Lerp(p.lF1, .07f, .3f); p.lB1 = Lerp(p.lB1, -.07f, .3f); p.lF2 = Lerp(p.lF2, -.1f, .3f); p.lB2 = Lerp(p.lB2, .1f, .3f);
    };
    s.push_back(jump);
    s.push_back(Act([] { W.hang = Hang{}; W.hang.kick = true; W.hanging = true; }));
    Step hang; hang.until = [hold](Step&) { return hold() != 0; };
    hang.update = [](Step&, float, float h, float) { HangStep(h); W.p.eyes = Eyes::Angry; W.p.mouth = Mouth::Flat; };
    s.push_back(hang);
    s.push_back(Act([hold] { W.hanging = false; if (hold() != 2) W.phys = Phys{ 0, 0, 0 }; }));   // let go: just drops
    s.push_back(Tumble());
    s.push_back(LandOnTaskbar());
    s.push_back(Settle());
    Append(s, GetUp());
    s.push_back(Fps(15));
    return s;
}

// A slow cursor bump: it falls over away from the cursor, then gets up.
Scene SceneKnockedOver(float dir) {
    Scene s;
    s.push_back(Act([] { W.fps = 30; W.idle = false; W.p.mouth = Mouth::O; }));
    Step fall; fall.dur = .55f;
    fall.begin = [](Step& st) { st.f[0] = W.p.x; st.f[1] = W.p.y; };
    fall.update = [dir](Step& st, float u, float, float) {
        float v = Eased(Ease::In, u); Pose& p = W.p;
        p.rot = dir * v * PI / 2; p.y = Lerp(st.f[1], W.st.floor - U(22), v); p.x = st.f[0] + dir * U(8) * v;
        p.v = Lerp(1, 0, u); p.aF1 = Lerp(.1f, 2.4f, u); p.aB1 = Lerp(-.1f, 2, u); p.lF1 = Lerp(.04f, .4f, u); p.eyes = Eyes::Open;
    };
    s.push_back(fall);
    s.push_back(Act([] { for (int i = 0; i < 5; i++) Dust(W.p.x + (Rand() - .5f) * 40 * K(), W.st.floor - 4 * K(), (Rand() - .5f) * 80, -30, .6f, 5); }));
    s.push_back(Wait(.9f));
    s.push_back(Settle());
    Append(s, GetUp());
    s.push_back(Fps(15));
    return s;
}

Scene SceneThrown() {
    Scene s;
    s.push_back(Act([] { W.fps = 30; W.idle = false; W.hanging = false; }));
    s.push_back(Tumble());
    s.push_back(LandOnTaskbar());
    s.push_back(Settle());
    Append(s, GetUp());
    s.push_back(Fps(15));
    return s;
}

// ---- sleep and wake-up: a bed on the taskbar edge near home
constexpr float BED_LEN = 123, BED_TH = 10;
void SnapAsleep() {
    PutAwayProps();
    W.st.floor = (float)W.work.bottom;
    float lo = W.work.left + U(6), hi = W.work.right - U(BED_LEN + 6);
    float bx = std::clamp(C.homeX - U(86), lo, std::max(lo, hi));
    int dir = bx - U(17 + 20) >= W.work.left ? 1 : -1;   // step off on the side with room
    W.bd = Bed{ bx, W.st.floor, 0, dir };
    W.bed = true;
    Pose p = StandSide(W.st);
    p.x = dir > 0 ? bx + U(70) : bx + U(BED_LEN - 70); p.y = W.st.floor - U(BED_TH + 22);
    p.rot = -PI / 2 * dir; p.face = (float)dir; p.eyes = Eyes::Closed; p.mouth = Mouth::Flat;
    p.aF1 = .05f; p.aF2 = .2f; p.aB1 = -.05f; p.aB2 = .2f; p.lF1 = p.lF2 = p.lB1 = p.lB2 = 0;
    W.p = p;
    W.zzz = true; W.idle = true; W.fps = 8;
}

Scene SceneSleep(std::function<bool()> until) {
    Scene s;
    s.push_back(Act([] { W.zzz = true; W.idle = true; W.fps = 8; }));
    Step loop; loop.until = [until](Step&) { return until(); };
    s.push_back(loop);
    return s;
}

Scene SceneWakeUp() {
    if (!W.bed) SnapAsleep();
    const Bed b = W.bd;
    float stepOff = b.dir > 0 ? b.x - U(17) : b.x + U(BED_LEN + 17), foot = b.dir > 0 ? b.x + U(3) : b.x + U(BED_LEN - 3);
    float th = U(BED_TH);
    Scene s;
    s.push_back(Wait(1.6f));
    s.push_back(Act([] { W.zzz = false; W.p.eyes = Eyes::Open; W.fps = 30; }));
    s.push_back(Wait(.7f));
    s.push_back(Act([] { W.idle = false; }));
    s.push_back(Tw([th](Pose& q) {   // sits up
        q.rot = 0; q.lF1 = 1.5f; q.lF2 = 0; q.lB1 = 1.45f; q.lB2 = 0; q.t = .15f; q.h = .1f;
        q.aF1 = .3f; q.aF2 = .3f; q.aB1 = .2f; q.aB2 = .3f; q.y = W.st.floor - th - U(5);
    }, .9f));
    s.push_back(Tw([](Pose& q) {   // stretches and yawns
        q.aF1 = PI - .15f; q.aF2 = 0; q.aB1 = PI + .15f; q.aB2 = 0; q.t = -.12f; q.h = -.35f; q.mouth = Mouth::Yawn; q.eyes = Eyes::Closed;
    }, .8f));
    s.push_back(Wait(.9f));
    s.push_back(Tw([](Pose& q) { q.aF1 = .4f; q.aF2 = .4f; q.aB1 = .3f; q.aB2 = .4f; q.t = .05f; q.h = 0; q.mouth = Mouth::Smile; q.eyes = Eyes::Open; }, .5f));
    s.push_back(Tw([th](Pose& q) { q.t = .4f; q.h = .1f; q.aF1 = .6f; q.aB1 = .5f; q.aF2 = .3f; q.aB2 = .3f; Crouch(q, 16, W.st); q.y -= th; }, .5f));
    s.push_back(Tw(T([th](Pose& q) { q.y = StandY() - th; }), .5f));
    s.push_back(Wait(.5f));
    s.push_back(Walk([stepOff] { return stepOff; }, 90, -th, 0));
    s.push_back(Tw(TS(), .25f));
    s.push_back(Look([b](Pose& q) { q.face = (float)b.dir; }));
    s.push_back(Wait(.25f));
    s.push_back(Tw([foot, th](Pose& q) {   // a foot on the bed's edge
        q.t = -.12f; q.aF1 = -.7f; q.aF2 = .3f; q.aB1 = .8f; q.aB2 = .3f; LegIK(q, F, ToLocal(q, { foot, W.st.floor - th }, W.st.scale));
    }, .45f));
    s.push_back(Wait(.8f, [foot, th](float u, float, float) {   // pushes it down behind the taskbar
        W.bd.dy = Lerp(0, U(8), Eased(Ease::InOut, u));
        LegIK(W.p, F, ToLocal(W.p, { foot, W.st.floor - th + W.bd.dy }, W.st.scale)); W.p.mouth = Mouth::Flat;
    }));
    s.push_back(Wait(.35f, [](float u, float, float) { W.bd.dy = Lerp(U(8), U(26), Eased(Ease::In, u)); }));
    s.push_back(Act([] { W.bed = false; }));
    s.push_back(Tw(T([](Pose& q) { q.mouth = Mouth::Smile; }), .35f));
    s.push_back(Wait(.7f, [](float, float h, float t) {   // dusts itself off
        Pose& p = W.p; float c = 1.5f + 2 * std::sin(t * 30);
        ArmIK(p, F, { c, -18 }); ArmIK(p, B, { -c, -18 });
        if (Rand() < h * 12) { V2 w = ToWorld(p, { 0, -18 }, W.st.scale); Dust(w.x, w.y, (Rand() - .5f) * 60, -20 - Rand() * 30, .6f, 4); }
    }));
    s.push_back(Tw(T(), .3f));
    s.push_back(Walk([] { return C.homeX; }));
    s.push_back(Tw(T(), .25f));
    s.push_back(Look([](Pose& q) { q.face = C.faceIn; }));
    s.push_back(Fps(15));
    return s;
}

// ---------------------------------------------------------------- going home
// Drop to a lower floor: a short hop first (hop) or straight off (stepping off a ledge), arms up,
// then a crouched landing with dust. vx: prototype px/s sideways while falling.
static Scene DropTo(float floorY, float vx, bool hop) {
    Scene s;
    if (hop) s.push_back(Tw([](Pose& q) { q.rot = 0; Crouch(q, 8, W.st); q.aF1 = .5f; q.aB1 = -.5f; }, .18f));
    Step fall;
    fall.begin = [vx, hop](Step& st) {
        st.f[0] = vx * K(); st.f[1] = (hop ? -260 : -40) * K(); st.f[2] = 0;
        W.fps = 30; W.idle = false; W.p.eyes = Eyes::O; W.p.mouth = Mouth::O;
    };
    fall.update = [floorY](Step& st, float, float h, float t) {
        Pose& p = W.p;
        st.f[1] += 1500 * K() * h; p.x += st.f[0] * h; p.y += st.f[1] * h;
        p.rot = 0; p.v = Lerp(p.v, 1, .1f); p.t = 0; p.h = -.1f;
        p.aF1 = 2.3f + .2f * std::sin(t * 18); p.aB1 = -2.3f - .2f * std::sin(t * 18); p.aF2 = .3f; p.aB2 = -.3f;
        p.lF1 = .25f; p.lB1 = -.25f; p.lF2 = -.4f; p.lB2 = .4f;
        float stand = floorY - U(STANDH);
        if (p.y >= stand && st.f[1] > 0) { p.y = stand; st.f[2] = 1; }
    };
    fall.until = [](Step& st) { return st.f[2] != 0; };
    s.push_back(fall);
    s.push_back(Act([floorY] {
        W.st.floor = floorY;
        for (int i = 0; i < 5; i++) Dust(W.p.x + (Rand() - .5f) * 30 * K(), floorY - 3 * K(), (Rand() - .5f) * 80, -20 - Rand() * 30, .6f, 4);
    }));
    s.push_back(Tw([](Pose& q) {
        q.rot = 0; q.v = 1; q.eyes = Eyes::Open; q.mouth = Mouth::Smile; Crouch(q, 12, W.st); q.aF1 = 1; q.aB1 = -1;
    }, .15f, Ease::Out));
    Append(s, StandUp(.3f));
    return s;
}

// Way down 0: walk to the nearest screen edge and climb down it, hand over hand, to the taskbar.
static Scene ClimbDown() {
    float side = W.p.x - W.work.left < W.work.right - W.p.x ? -1.f : 1.f;   // the nearest edge; -1: left
    float wall = side < 0 ? (float)W.work.left : (float)W.work.right;
    // At an edge shared with another screen it climbs a little further in, so its helmet stays on this screen.
    bool shared = MonitorFromPoint(POINT{ (LONG)(wall + side * 4), (LONG)(W.st.floor - U(40)) }, MONITOR_DEFAULTTONULL) != nullptr;
    float inset = shared ? 34.f : 16.f, reach = inset - 3, x = wall - side * U(inset);   // reach: hands, in local units
    Scene s;
    s.push_back(Walk([x] { return x; }));
    s.push_back(Tw(TS([side](Pose& q) { q.face = side; }), .2f));
    s.push_back(Tw([reach](Pose& q) { q.t = .12f; q.h = -.2f; ArmIK(q, F, { reach, -26 }); ArmIK(q, B, { reach - 1, -22 }); }, .3f));
    Step climb; climb.motion = true;
    climb.begin = [](Step& st) {
        st.f[0] = W.p.y; st.f[1] = (float)W.work.bottom - U(STANDH);
        st.dur = std::max(.3f, (st.f[1] - st.f[0]) / (U(60) * C.walkSpeed)); W.fps = 30; W.idle = false;
    };
    climb.update = [reach](Step& st, float u, float, float t) {
        Pose& p = W.p; float ph = t * 7 * C.walkSpeed;
        p.y = Lerp(st.f[0], st.f[1], u); p.t = .12f; p.h = -.15f; p.v = 0; p.rot = 0;
        ArmIK(p, F, { reach, -25 + 6 * std::sin(ph) }); ArmIK(p, B, { reach - 1, -25 - 6 * std::sin(ph) });
        LegIK(p, F, { 9, LEG - 7 + 5 * std::sin(ph + 1) }); LegIK(p, B, { 8, LEG - 7 - 5 * std::sin(ph + 1) });
    };
    s.push_back(climb);
    s.push_back(Act([] { W.st.floor = (float)W.work.bottom; }));
    s.push_back(Tw(T(), .3f));
    return s;
}

// Way down 2: walk across the screen like on glass, around the widgets, straight to home.
// The path is a small A* on a coarse grid of the work area (well under a millisecond, so no thread).
static bool Blocked(const std::vector<uint8_t>& b, int cols, int rows, int cx, int cy) {
    return cx < 0 || cy < 0 || cx >= cols || cy >= rows || b[cy * cols + cx];
}
static std::vector<V2> FindPath(V2 from, V2 to) {
    const RECT wa = W.work;
    float cell = std::max(14.f, U(18));
    int cols = (int)((wa.right - wa.left) / cell) + 1, rows = (int)((wa.bottom - wa.top) / cell) + 1, n = cols * rows;
    std::vector<uint8_t> block(n, 0);
    for (auto& w : g_widgets) {   // keep the pet's body (above its feet) off the widgets
        if (!w->enabled || !w->hwnd) continue;
        RECT r = PanelRect(w.get());
        float x0 = r.left - U(26), x1 = r.right + U(26), y0 = (float)r.top, y1 = r.bottom + U(110);
        for (int cy = 0; cy < rows; cy++) for (int cx = 0; cx < cols; cx++) {
            float px = wa.left + (cx + .5f) * cell, py = wa.top + (cy + .5f) * cell;
            if (px >= x0 && px <= x1 && py >= y0 && py <= y1) block[cy * cols + cx] = 1;
        }
    }
    auto cellOf = [&](V2 p, int& cx, int& cy) {
        cx = std::clamp((int)((p.x - wa.left) / cell), 0, cols - 1); cy = std::clamp((int)((p.y - wa.top) / cell), 0, rows - 1);
    };
    int sx, sy, gx, gy; cellOf(from, sx, sy); cellOf(to, gx, gy);
    block[sy * cols + sx] = block[gy * cols + gx] = 0;
    std::vector<float> g(n, 1e30f); std::vector<int> par(n, -1);
    std::vector<std::pair<float, int>> open;   // min-heap on f
    auto hcost = [&](int cx, int cy) { return std::hypot((float)(cx - gx), (float)(cy - gy)); };
    g[sy * cols + sx] = 0; open.push_back({ hcost(sx, sy), sy * cols + sx });
    bool found = false;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), std::greater<>()); int cur = open.back().second; open.pop_back();
        if (cur == gy * cols + gx) { found = true; break; }
        int cx = cur % cols, cy = cur / cols;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            if ((!dx && !dy) || Blocked(block, cols, rows, cx + dx, cy + dy)) continue;
            if (dx && dy && (Blocked(block, cols, rows, cx + dx, cy) || Blocked(block, cols, rows, cx, cy + dy))) continue;   // no corner cutting
            int nb = (cy + dy) * cols + cx + dx; float ng = g[cur] + (dx && dy ? 1.414f : 1.f);
            if (ng < g[nb]) { g[nb] = ng; par[nb] = cur; open.push_back({ ng + hcost(cx + dx, cy + dy), nb }); std::push_heap(open.begin(), open.end(), std::greater<>()); }
        }
    }
    if (!found) return { from, to };
    std::vector<V2> cells;
    for (int c = gy * cols + gx; c != -1; c = par[c]) cells.push_back({ wa.left + (c % cols + .5f) * cell, wa.top + (c / cols + .5f) * cell });
    std::reverse(cells.begin(), cells.end());
    cells.front() = from; cells.back() = to;
    auto clear = [&](V2 a, V2 b) {   // straight line stays in free cells
        float len = std::hypot(b.x - a.x, b.y - a.y); int steps = std::max(1, (int)(len / (cell * .5f)));
        for (int i = 0; i <= steps; i++) {
            V2 p{ Lerp(a.x, b.x, i / (float)steps), Lerp(a.y, b.y, i / (float)steps) }; int cx, cy; cellOf(p, cx, cy);
            if (block[cy * cols + cx] && !(cx == sx && cy == sy) && !(cx == gx && cy == gy)) return false;
        }
        return true;
    };
    std::vector<V2> path{ cells.front() };   // string pulling: drop corners that can be seen past
    for (size_t i = 1; i + 1 < cells.size(); i++) if (!clear(path.back(), cells[i + 1])) path.push_back(cells[i]);
    path.push_back(cells.back());
    return path;
}
// Feet follow the points (screen px); side-on walk cycle, facing the way it moves.
static Step WalkPath(std::vector<V2> pts) {
    Step s; s.motion = true;
    float total = 0;
    for (size_t i = 1; i < pts.size(); i++) total += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    s.begin = [total](Step& st) { st.f[0] = W.p.v; st.dur = std::max(.05f, total / (115 * K() * C.walkSpeed)); W.idle = false; W.fps = 30; };
    s.update = [pts, total](Step& st, float u, float, float t) {
        float d = u * total, acc = 0;
        V2 at = pts.back(), seg{ 1, 0 };
        for (size_t i = 1; i < pts.size(); i++) {
            float l = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            if (acc + l >= d && l > 0) { float k = (d - acc) / l; at = { Lerp(pts[i - 1].x, pts[i].x, k), Lerp(pts[i - 1].y, pts[i].y, k) }; seg = { pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y }; break; }
            acc += l;
        }
        if (std::fabs(seg.x) > .3f * std::hypot(seg.x, seg.y)) W.p.face = seg.x >= 0 ? 1.f : -1.f;
        W.st.floor = at.y; W.p.x = at.x;
        WalkPose(W.p, d / (W.st.scale * 17), W.st);
        W.p.v = st.f[0] * std::max(0.f, 1 - t / .15f);
    };
    return s;
}

// Way down 3: the visible parts of open windows' top edges are ledges. It takes the route down with the
// least walking (on each ledge it leaves by one end), then walks home along the taskbar.
static std::vector<Ledge> Ledges() {
    struct Win { RECT r; bool ledge; };
    struct Ctx { std::vector<Win> wins; DWORD pid; } c{ {}, GetCurrentProcessId() };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {   // front to back
        Ctx* c = (Ctx*)lp;
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return TRUE;   // click-through overlays hide nothing
        BOOL cloaked = FALSE; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof cloaked);
        if (cloaked) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        bool own = pid == c->pid;   // Prism windows are never ledges, but its Settings window still hides edges
        if (own) {
            wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
            if (!wcscmp(cls, L"PrismPet") || !wcscmp(cls, L"PrismPetFx")) return TRUE;
        }
        RECT r;
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
        if (r.right <= r.left || r.bottom <= r.top) return TRUE;
        bool ledge = !own && !(ex & WS_EX_TOOLWINDOW) && r.right - r.left >= 160 && r.bottom - r.top >= 80;
        c->wins.push_back({ r, ledge });
        return TRUE;
    }, (LPARAM)&c);
    std::vector<Ledge> out;
    const RECT wa = W.work;
    float minW = std::max(60.f, U(40));   // room to stand
    for (size_t i = 0; i < c.wins.size(); i++) {
        const RECT& r = c.wins[i].r;
        if (!c.wins[i].ledge || r.top <= wa.top + 2 || r.top >= wa.bottom - 20) continue;   // maximised windows, the taskbar
        float y = (float)r.top;
        std::vector<Span> covers;   // windows in front that cover this edge
        for (size_t k = 0; k < i; k++) {
            const RECT& o = c.wins[k].r;
            if (o.top < y - 1 && o.bottom > y) covers.push_back({ (float)o.left, (float)o.right });
        }
        for (Span sp : Uncovered((float)std::max(r.left, wa.left), (float)std::min(r.right, wa.right), covers))
            if (sp.r - sp.l >= minW) out.push_back({ sp.l, sp.r, y });   // only the part on this screen
    }
    return out;
}
static Scene LedgePath() {
    std::vector<LedgeStep> route = PlanLedgeRoute(Ledges(), W.p.x, W.st.floor, (float)W.work.bottom, U(14),
                                                  (float)W.work.left, (float)W.work.right);
    Scene s;
    for (size_t i = 0; i < route.size(); i++) {
        if (i) { float x = route[i].off; s.push_back(Walk([x] { return x; })); }   // to one step past the ledge's end
        Append(s, DropTo(route[i].dropTo, 0, i == 0));
    }
    return s;
}

// On another screen: a portal opens beside it, it walks in, and comes out of another one at home.
static void PortalOpen(float u) { W.portalOpen = u; }
static Scene PortalHome() {
    Scene s;
    s.push_back(Act([] {
        float d = W.p.x > (W.work.left + W.work.right) / 2.f ? -1.f : 1.f;
        W.portal = true; W.portalX = W.p.x + d * U(42); W.portalFloor = W.st.floor; W.portalOpen = 0; W.fps = 30; W.idle = false;
    }));
    s.push_back(Tw(T([](Pose& q) { q.face = W.portalX >= q.x ? 1.f : -1.f; q.eyes = Eyes::O; }), .2f));
    s.push_back(Wait(.4f, [](float u, float, float) { PortalOpen(Eased(Ease::Out, u)); }));
    s.push_back(Look([](Pose& q) { q.eyes = Eyes::Happy; }));
    s.push_back(Walk([] { return W.portalX - (W.portalX > W.p.x ? 1 : -1) * U(14); }));
    Step in; in.dur = .45f;   // pulled in, shrinking into the middle of the ring
    in.begin = [](Step& st) { st.f[0] = W.p.x; st.f[1] = W.p.y; };
    in.update = [](Step& st, float u, float, float) {
        float v = Eased(Ease::In, u);
        W.shrink = Lerp(1, .05f, v); W.p.x = Lerp(st.f[0], W.portalX, v); W.p.y = Lerp(st.f[1], W.portalFloor - U(38), v);
    };
    s.push_back(in);
    s.push_back(Act([] { W.shrink = 0; }));
    s.push_back(Wait(.3f, [](float u, float, float) { PortalOpen(1 - u); }));
    s.push_back(Act([] {   // the other end, beside home on the home screen
        float x, fl; HomeAfterPortal(x, fl);
        W.portalX = x + C.faceIn * U(50); W.portalFloor = fl; W.st.floor = fl;
        W.p.x = W.portalX; W.p.y = fl - U(38); W.p.face = -C.faceIn; W.portalOpen = 0;
    }));
    s.push_back(Wait(.4f, [](float u, float, float) { PortalOpen(Eased(Ease::Out, u)); }));
    Step out; out.dur = .45f;
    out.begin = [](Step& st) { st.f[0] = W.p.y; };
    out.update = [](Step& st, float u, float, float) {
        float v = Eased(Ease::Out, u);
        W.shrink = Lerp(.05f, 1, v); W.p.y = Lerp(st.f[0], W.st.StandY(), v);
    };
    s.push_back(out);
    s.push_back(Act([] { W.shrink = 1; }));
    s.push_back(Tw(T([](Pose& q) { q.eyes = Eyes::Open; }), .2f));
    s.push_back(Wait(.3f, [](float u, float, float) { PortalOpen(1 - u); }));
    s.push_back(Act([] { W.portal = false; }));
    return s;
}

Scene SceneGoHome() {
    Scene s;
    s.push_back(Fps(30));
    bool other = !OnHomeScreen();
    if (other && C.portalHome) Append(s, PortalHome());   // back to the home screen
    else {
        if (other) s.push_back(Act([] { AdoptThisScreen(); }));   // this screen's bottom-right becomes home
        float homeX = other ? W.work.right - U(45) : C.homeX;
        bool up = W.st.floor < W.work.bottom - 2;   // on an invisible floor above the taskbar
        if (C.wayDown == 4) {   // portal: any trip longer than a short step, short and cheap
            if (up || std::fabs(homeX - W.p.x) > U(60)) Append(s, PortalHome());
        } else if (up) {
            switch (C.wayDown) {
            case 1: Append(s, DropTo((float)W.work.bottom, (homeX >= W.p.x ? 1 : -1) * 60.f, true)); break;
            case 2: s.push_back(WalkPath(FindPath({ W.p.x, W.st.floor }, { homeX, (float)W.work.bottom }))); s.push_back(Act([] { W.st.floor = (float)W.work.bottom; })); break;
            case 3: Append(s, LedgePath()); break;
            default: Append(s, ClimbDown()); break;
            }
        }
    }
    s.push_back(Walk([] { return C.homeX; }));
    s.push_back(Tw(T(), .25f));
    return s;
}

}  // namespace pet
