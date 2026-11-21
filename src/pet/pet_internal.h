// Prism Pet - declarations shared between the pet's own source files.
#pragma once
#include "../app.h"
#include "pet.h"
#include "pet_rig.h"
#include <functional>

namespace pet {

// ---------------------------------------------------------------- look
// Colour slots used by every part; switching themes only swaps the palette.
struct Palette { const wchar_t* id; uint32_t shell, shade, joint, visor, line, glow, disc; };
const Palette& PaletteOf(const std::wstring& id);
inline D2D1_COLOR_F Hex(uint32_t c, float a = 1) { return { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a }; }
inline float Lerp(float a, float b, float u) { return a + (b - a) * u; }

// Visor expressions (eye and mouth shapes).
enum class Expr : uint8_t { Open, Blink, Happy, Laugh, Angry, Tired, Sleep, Surprised, Dizzy, Yawn, Oh };
Expr VisorExpr(Eyes eyes, Mouth mouth, bool blink);

// ---------------------------------------------------------------- world state (screen px)
struct Particle {
    enum Type : uint8_t { Dust, Text, Z, Shard, Soil, Leaf } type = Dust;
    float x = 0, y = 0, vx = 0, vy = 0, g = 0, life = 0, max = 1, size = 6, rot = 0, vr = 0;
    const wchar_t* text = nullptr; uint32_t col = 0xffffff;
    ID2D1PathGeometry* geo = nullptr;   // shard outline (owned by pet_fx.cpp)
};
struct Laptop { float x = 18, y = 30, lid = 0; };              // local units, relative to the pelvis
struct Phone { float off = 0, lean = 0; V2 at{}, pocket{}; };   // held in hand B; local units
enum class Pot : uint8_t { None, HandB, Both };
struct Bed { float x = 0, floor = 0, dy = 0; int dir = 1; };    // screen px: left end, floor; dir: +1 = pillow on the left
struct Hang { float th = 0, w = 0, th2 = 0, w2 = 0; bool kick = false; };
struct Phys { float vx = 0, vy = 0, w = 0; };
struct Cursor { float x = 0, y = 0, px = 0, py = 0, vx = 0, vy = 0, ax = 0, ay = 0; };

struct World {
    Pose p; Stage st;
    RECT work{};                 // work area of the pet's monitor (taskbar edge = work.bottom)
    float time = 0, seed = 0, dizzyUntil = -1;
    int fps = 15;                // frame rate the current step asks for
    bool idle = false, zzz = false;
    bool laptop = false, phone = false, bed = false;
    Laptop lap; Phone ph; Bed bd; Pot pot = Pot::None;
    bool hanging = false; Hang hang;
    Phys phys; Cursor cur;
    std::vector<Particle> parts;
    const Palette* pal = nullptr; D2D1_COLOR_F glow{};
    bool portal = false; float portalX = 0, portalFloor = 0, portalOpen = 0;   // screen px; open 0..1
    float shrink = 1;            // 1 = normal size; smaller while it goes through a portal
};
extern World W;

// px per prototype pixel: the prototype ran at 1.25 px per local unit
inline float K() { return W.st.scale / 1.25f; }
inline float U(float units) { return units * W.st.scale; }   // local units to px

// ---------------------------------------------------------------- drawing (pet_draw.cpp)
void DrawPet(ID2D1RenderTarget* rt, const World& w, const Pose& p, Expr expr);   // bed, robot, props, particles, stars
void DrawRobot(ID2D1RenderTarget* rt, const Pose& p, float scale, const Palette& pal, D2D1_COLOR_F glow, Expr expr,
               const World* props = nullptr);
void DrawPotShape(ID2D1RenderTarget* rt);   // in the current transform, local units
void DrawParticles(ID2D1RenderTarget* rt, const std::vector<Particle>& parts, float k);
void ReleaseDrawCache();

// ---------------------------------------------------------------- scenes (pet_scenes.cpp)
struct Step {
    float dur = 0;
    bool motion = false;          // a tween or walk: instant when Windows animations are off
    std::function<void(Step&)> begin;
    std::function<void(Step&, float u, float h, float t)> update;
    std::function<bool(Step&)> until;
    Pose a, b; float f[6] = {};   // scratch
    bool started = false; float t = 0;
};
using Scene = std::vector<Step>;

struct SceneCtx {             // where things are, in screen px
    float homeX = 0;
    float faceIn = -1;        // facing toward the middle of the screen when sitting at home
    bool reduced = false;     // Windows animation effects are off
    float walkSpeed = 1;      // walk speed setting (all walks)
    int wayDown = 0;          // from above the taskbar: 0 climb down the screen edge, 1 jump down, 2 walk across the screen, 3 window ledges, 4 portal
    bool portalHome = true;   // on another screen: go back through a portal (else that screen becomes home)
};
extern SceneCtx C;

bool RunStep(Scene& s, size_t& i, float h);   // false when the scene has finished
void Spawn(const Particle& p);
void Say(const wchar_t* s, uint32_t col = 0xffffff);
void TickWorld(float h);                     // particles, z bubbles, cursor derivatives
void HangStep(float h);

Scene SceneIdle(std::function<bool()> until);
Scene SceneWalk(float toX);
Scene SceneTickle();
Scene SceneDragHang(std::function<bool()> released);   // hangs until released, then lands and dusts off
Scene SceneCodingEnter();
Scene SceneTyping(std::function<bool()> until);
Scene SceneCodingExit();
Scene SceneBrowserEnter();
Scene SceneScrolling(std::function<bool()> until);
Scene SceneBrowserExit();
Scene SceneCodingToBoth();
Scene SceneBothToCoding();
Scene ScenePacing(float range);
Scene SceneFlowerPot();
Scene SceneGrab(std::function<int()> hold);          // hold(): 0 = keep hanging, 1 = let go, 2 = yanked (W.phys set)
Scene SceneKnockedOver(float dir);
Scene SceneThrown();                                  // W.phys already set
Scene SceneSleep(std::function<bool()> until);
Scene SceneWakeUp();
Scene SceneGoHome();                                  // to the home spot: portal or adopt the screen, get down, walk
bool OnHomeScreen();                                  // pet_brain.cpp
void AdoptThisScreen();                               // pet_brain.cpp: this screen's bottom-right becomes home
void HomeAfterPortal(float& x, float& floorY);        // pet_brain.cpp: home spot on the home screen, screen px
void SnapAsleep();                                    // in bed, no animation
void PutAwayProps();

// ---------------------------------------------------------------- brain (pet_brain.cpp)
void Relayout();
void OnTimer(UINT id);
void OnSettingChange();
void BeginDrag();
void EndDrag();
void OnClick();
void OnYank(float vx, float vy);
void OnContact(float vx, float vy);
void SetFrameRate(int fps);
bool Dragging();
RECT PetHitBox();          // screen px
bool KeepAtBottom();       // desktop-only layer: the window stays under every other window
HWND PetWindow();

// ---------------------------------------------------------------- effects (pet_fx.cpp)
void FxThrow(V2 from, V2 to, float scale);
bool FxActive();
void FxTick(float h);
void FxPresent();
void FxShutdown();

// ---------------------------------------------------------------- input (pet_input.cpp)
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
enum class HookMode : int { Off, Grab, Collide };
void HookSet(HookMode m);   // starts or stops the mouse-hook thread
bool HookInstalled();
void HookSetHitBox(RECT r);
constexpr UINT WM_PET_YANK = WM_APP + 21, WM_PET_CONTACT = WM_APP + 22;
constexpr UINT T_PET_FRAME = 60, T_PET_TICK = 61, T_PET_PROXIMITY = 62, T_PET_LEAVE_CODE = 63, T_PET_LEAVE_WEB = 64;

// A reusable DIB for one layered window, so animation frames don't allocate. Freed when the window hides.
// It has its own DC render target, bound once: rebinding the shared target costs about 0.5 ms a frame.
struct Surface {
    HDC mem = nullptr; HBITMAP dib = nullptr; HGDIOBJ old = nullptr; int w = 0, h = 0;
    ID2D1DCRenderTarget* rt = nullptr; ID2D1SolidColorBrush* br = nullptr;
    POINT org{}; bool placed = false;   // window position; it stays put while `box` fits inside
    int oversized = 0;                  // frames in a row the surface was much bigger than needed
    BYTE alpha = 255;                   // whole-window opacity (see-through modes)
    void Free();
};
// Renders `draw` (in screen px) into a layered window that covers the screen rect `box` (premultiplied BGRA).
// anchor (moving content): the window is centred on this point every frame; it must move smoothly (the
// pet's body, not its bounding box, which changes shape as the limbs swing). Null: the window stays put
// and only re-centres when the content reaches its edge.
void Present(Surface& s, HWND hwnd, RECT box, const std::function<void(ID2D1RenderTarget*)>& draw, const POINT* anchor = nullptr);

}  // namespace pet
