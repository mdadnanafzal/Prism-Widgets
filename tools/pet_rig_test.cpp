// Headless check of the pet rig against numbers taken from the browser prototype.
// Build: g++ -std=c++17 tools/pet_rig_test.cpp src/pet/pet_rig.cpp -o build/pet_rig_test.exe
#include "../src/pet/pet_rig.h"
#include <cmath>
#include <cstdio>

using namespace pet;
static int g_fail = 0;
static void Near(const char* what, float got, float want, float tol = 2e-3f) {
    if (std::isfinite(got) && std::fabs(got - want) <= tol) return;
    printf("FAIL %s: got %.4f want %.4f\n", what, got, want); g_fail++;
}
static void NearV(const char* what, V2 got, float x, float y) { Near(what, got.x, x); Near(what, got.y, y); }

int main() {
    const float PS = 1.25f;
    Stage st{ PS, 672 };

    Fk k = Kinematics(Stand(st));   // front view
    NearV("stand head", k.head, 0, -56); NearV("stand shF", k.shF, 13, -28); NearV("stand hF", k.hF, 15.9237f, .8391f);
    NearV("stand hB", k.hB, -15.9237f, .8391f); NearV("stand kF", k.kF, 6.9792f, 13.9657f); NearV("stand fB", k.fB, -7.8885f, 26.9339f);

    k = Kinematics(StandSide(st));   // side view
    NearV("side hipF", k.hipF, 2, 0); NearV("side hF", k.hF, 5.3665f, -1.6202f); NearV("side hB", k.hB, -.3787f, -1.1197f);
    NearV("side fF", k.fF, 3.0797f, 26.9784f);

    Pose q = Stand(st);   // a blended, leaning pose
    q.t = .2f; q.h = -.1f; q.v = .4f; q.aF1 = 1.1f; q.aF2 = .7f; q.aB1 = -.6f; q.aB2 = .3f;
    q.lF1 = .5f; q.lF2 = -.8f; q.lB1 = -.3f; q.lB2 = .2f;
    k = Kinematics(q);
    NearV("mix head", k.head, 8.5557f, -55.2721f); NearV("mix shF", k.shF, 10.8975f, -27.5849f); NearV("mix hF", k.hF, 36.6414f, -18.6695f);
    NearV("mix hB", k.hB, -16.7675f, -6.9143f); NearV("mix fF", k.fF, 6.4702f, 24.7055f); NearV("mix fB", k.fB, -9.0351f, 26.3098f);

    q = Stand(st);   // pot throw targets (front) and foot tap
    ArmIK(q, F, { 8, -34 }); ArmIK(q, B, { -8, -34 }); LegIK(q, F, { 11, 24 }); LegIK(q, B, { -9, 27 });
    float ikF[8] = { -1.2703f, -2.6005f, 1.2703f, 2.6005f, .621f, -.8654f, -.1369f, .0545f };
    float got[8] = { q.aF1, q.aF2, q.aB1, q.aB2, q.lF1, q.lF2, q.lB1, q.lB2 };
    for (int i = 0; i < 8; i++) Near("ik front", got[i], ikF[i]);

    q = StandSide(st); q.t = -.04f;   // typing targets (side)
    ArmIK(q, F, { 12, -9 }); ArmIK(q, B, { 7, -9 });
    Near("ik side aF1", q.aF1, -.0037f); Near("ik side aF2", q.aF2, 1.0927f); Near("ik side aB1", q.aB1, -.3214f); Near("ik side aB2", q.aB2, 1.364f);

    q = Stand(st); Crouch(q, 12, st);
    Near("crouch y", q.y - st.StandY(), 15); Near("crouch lF1", q.lF1, 1.1129f); Near("crouch lF2", q.lF2, -1.9389f);
    Near("crouch lB1", q.lB1, -1.1129f); Near("crouch lB2", q.lB2, 1.9389f);

    q = StandSide(st); WalkPose(q, 1.3f, st);
    Near("walk bob", (q.y - st.StandY()) / PS, 2.4986f); Near("walk lF1", q.lF1, .4336f); Near("walk lF2", q.lF2, -.2541f);
    Near("walk aB1", q.aB1, .5318f); Near("walk aB2", q.aB2, .5909f);

    // Unreachable and degenerate targets clamp without NaN, and a reached hand lands on its target.
    q = Stand(st);
    V2 far[] = { { 0, 0 }, { 200, -300 }, { -13, -28 }, { 13, -28 } };
    for (V2 t : far) { ArmIK(q, F, t); ArmIK(q, B, t); LegIK(q, F, t); LegIK(q, B, t);
        Near("finite aF1", q.aF1, q.aF1); Near("finite aB2", q.aB2, q.aB2); Near("finite lF1", q.lF1, q.lF1); Near("finite lB2", q.lB2, q.lB2); }
    q = Stand(st); ArmIK(q, B, { -6, -19 }); NearV("reach hB", Kinematics(q).hB, -6, -19);
    q = StandSide(st); LegIK(q, F, { 5, 22 }); NearV("reach fF", Kinematics(q).fF, 5, 22);
    // Front view: the near elbow points outward (+x), the far one outward (-x).
    q = Stand(st); ArmIK(q, F, { 8, -10 }); ArmIK(q, B, { -8, -10 });
    k = Kinematics(q);
    if (!(k.eF.x > (k.shF.x + k.hF.x) / 2 && k.eB.x < (k.shB.x + k.hB.x) / 2)) { printf("FAIL elbows not outward\n"); g_fail++; }

    // Visible parts of an edge: covered stretches are cut out.
    auto sp = Uncovered(0, 100, { { 20, 30 }, { 25, 40 }, { 90, 120 } });
    if (sp.size() != 2 || sp[0].l != 0 || sp[0].r != 20 || sp[1].l != 40 || sp[1].r != 90) { printf("FAIL Uncovered\n"); g_fail++; }

    // Ledge route (y grows downward; taskbar at 1000; screen 0..1920; step past an end = 10).
    // Above ledge A near its right end: right end of A, onto B, B's left end (less walking than B's right), down.
    std::vector<Ledge> L{ { 700, 1200, 450 }, { 1000, 1500, 650 }, { 1350, 1850, 820 } };
    auto r1 = PlanLedgeRoute(L, 1150, 300, 1000, 10, 0, 1920);
    bool ok1 = r1.size() == 3 && r1[0].off == 1150 && r1[0].dropTo == 450 && r1[1].off == 1210 && r1[1].dropTo == 650
               && r1[2].off == 990 && r1[2].dropTo == 1000;
    if (!ok1) { printf("FAIL staircase:"); for (auto& s : r1) printf(" (%.0f -> %.0f)", s.off, s.dropTo); printf("\n"); g_fail++; }
    // Above A near its left end: the left end is nearer and goes straight to the taskbar.
    auto r2 = PlanLedgeRoute(L, 750, 300, 1000, 10, 0, 1920);
    if (!(r2.size() == 2 && r2[1].off == 690 && r2[1].dropTo == 1000)) { printf("FAIL nearer end\n"); g_fail++; }
    // A ledge against the left screen side can only be left by its right end.
    std::vector<Ledge> L2{ { 0, 400, 500 } };
    auto r3 = PlanLedgeRoute(L2, 50, 300, 1000, 10, 0, 1920);
    if (!(r3.size() == 2 && r3[1].off == 410)) { printf("FAIL screen side\n"); g_fail++; }
    // Nothing below: straight down.
    auto r4 = PlanLedgeRoute(L, 300, 300, 1000, 10, 0, 1920);
    if (!(r4.size() == 1 && r4[0].dropTo == 1000)) { printf("FAIL straight down\n"); g_fail++; }

    // Pet height (sole to helmet top, helmet drawn at 0.9 scale) for the flower-pot size rule.
    float top = -(L_TORSO + L_NECK + L_HR) - 24.5f * .9f, height = STANDH - top;
    printf("pet height %.1f units, pot at 3.0x %.1f units (%.1f%% of the pet)\n", height, 35.6f * 3, 100 * 35.6f * 3 / height);

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
