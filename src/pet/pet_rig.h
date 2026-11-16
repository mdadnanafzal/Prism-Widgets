// Prism Pet - skeleton, forward/inverse kinematics and stock poses (pure math, no Windows).
#pragma once
#include <cstdint>

namespace pet {

constexpr float PI = 3.14159265358979f;

// Bone lengths in local units (DIP before the pet size setting).
constexpr float L_TORSO = 30, L_NECK = 4, L_HR = 22 /*helmet half-height*/, L_UA = 15, L_FA = 14, L_TH = 14, L_SH = 13;
constexpr float LEG = 27;      // hip to foot point
constexpr float STANDH = 33;   // hip to sole: pelvis height above the floor when standing
constexpr float SHX = 13, HPX = 6;   // front-view shoulder and hip half-spacing (blended by v)

struct V2 { float x, y; };
enum Side { F, B };   // F = near/right side, B = far/left side

enum class Eyes : uint8_t { Open, Closed, Happy, Half, Angry, Dizzy, O };
enum class Mouth : uint8_t { Smile, Flat, O, Open, Laugh, Yawn };

// Angles: 0 = straight down, positive swings toward +x (forward in the side view, outward for F
// in the front view). Elbow and knee angles are relative to the upper segment.
struct Pose {
    float x = 0, y = 0;   // pelvis, window px
    float rot = 0;        // whole body
    float face = 1;       // +1 / -1 mirror
    float t = 0, h = 0;   // torso lean, head tilt
    float v = 1;          // view: 0 = side (three-quarter), 1 = front
    float gy = 0;         // eye gaze offset on the visor
    float aF1 = 0, aF2 = 0, aB1 = 0, aB2 = 0;   // arms: upper, elbow
    float lF1 = 0, lF2 = 0, lB1 = 0, lB2 = 0;   // legs: thigh, knee
    Eyes eyes = Eyes::Open;
    Mouth mouth = Mouth::Smile;
};

// Where the pet stands: px per local unit, and the floor's y in window px.
struct Stage {
    float scale = 1, floor = 0;
    float StandY() const { return floor - STANDH * scale; }
};

struct Fk { V2 sh, shF, shB, hipF, hipB, head, eF, hF, eB, hB, kF, fF, kB, fB; };

Pose Stand(const Stage& s);       // standing, facing the user
Pose StandSide(const Stage& s);   // standing, side-on
V2 ShoulderOf(const Pose& p, Side side);
V2 HipOf(const Pose& p, Side side);
Fk Kinematics(const Pose& p);     // joint positions in local units
V2 ToWorld(const Pose& p, V2 local, float scale);
V2 ToLocal(const Pose& p, V2 world, float scale);
void ArmIK(Pose& p, Side side, V2 target);   // elbow points back in the side view, outward in the front view
void LegIK(Pose& p, Side side, V2 target);   // knee points forward in the side view, outward in the front view
void Crouch(Pose& p, float depth, const Stage& s);   // depth in local units
void WalkPose(Pose& p, float phase, const Stage& s);

}  // namespace pet
