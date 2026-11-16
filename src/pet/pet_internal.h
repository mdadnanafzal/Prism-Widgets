// Prism Pet - declarations shared between the pet's own source files.
#pragma once
#include "../app.h"
#include "pet_rig.h"

namespace pet {

// Colour slots used by every part; switching themes only swaps the palette.
struct Palette { const wchar_t* id; uint32_t shell, shade, joint, visor, line, glow, disc; };
const Palette& PaletteOf(const std::wstring& id);

// Visor expressions (eye and mouth shapes).
enum class Expr : uint8_t { Open, Blink, Happy, Laugh, Angry, Tired, Sleep, Surprised, Dizzy, Yawn, Oh };
Expr VisorExpr(Eyes eyes, Mouth mouth, bool blink);

// Draws the robot with its pelvis at (p.x, p.y) in render-target px; `scale` = px per local unit.
void DrawRobot(ID2D1RenderTarget* rt, const Pose& p, float scale, const Palette& pal, D2D1_COLOR_F glow, Expr expr);
void ReleaseDrawCache();

// pet_brain.cpp
void Relayout();

// pet_input.cpp
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

}  // namespace pet
