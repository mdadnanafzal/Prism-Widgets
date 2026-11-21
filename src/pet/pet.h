// Prism Pet - the desktop pet's public API, used by app.cpp, arrange.cpp and settings.cpp.
#pragma once
#include "../common.h"

struct PetSettings {
    bool enabled = true;
    std::wstring theme = L"pearl";     // pearl | graphite | sand
    std::wstring glow = L"theme";      // theme | accent
    float size = .85f;                 // 0.6 - 1.5
    float mischiefMinutes = 30;        // flower pot / cursor grab interval (active time)
    bool pacing = true;
    float pacingMinutes = 5;           // standing still before pacing
    float pacingRange = 1 / 3.f;       // fraction of the screen width
    bool collision = false;            // the cursor can bump the pet
    float walkSpeed = 1;               // all walks: .7 slow, 1 normal, 1.4 fast
    int wayDown = 0;                   // from above the taskbar: 0 climb down, 1 jump down, 2 walk across the screen, 3 window ledges, 4 portal for every trip home
    bool portalHome = true;            // on another screen: portal back to the home screen (false: that screen becomes home)
    bool onTop = true;                 // true: above all windows; false: desktop only, under windows like the widgets
    bool fadeOverApps = true;          // on top: see-through and click-through while a maximized window is behind it
    bool batteryPause = false;         // hide while the PC runs on battery
    bool cursorGrab = false;           // mischief may grab the cursor (else only the flower pot)
    std::vector<std::wstring> codingApps, browserApps;   // process file names
};
PetSettings& PetConfig();
void PetApplySettings();               // after changing PetConfig(): show/hide, redraw, save
void PetResetPosition();               // home = bottom-right of the main screen
void PetSetHomeHere();                 // home = bottom-right of the screen the pet is on
std::wstring PetHomeName();            // the home screen, for Settings

void PetLoadSettings(const JVal* j);   // the "pet" object in config.json (may be null)
void PetSaveSettings(JVal& j);
void PetInit(bool atLogin);            // after LoadConfig; atLogin: start asleep in bed and wake up
void PetShutdown();
void PetArrange(bool on);              // Arrange mode: the pet hides while it runs
void PetPeek(bool on);                 // Peek: lift a desktop-only pet above windows with the widgets
HWND PetHwnd();                        // the pet window while it is shown (else null)
bool PetScreenRect(RECT& r);           // the pet's body on screen (px) while it is shown, so notification cards can avoid it

enum class PetSession { Lock, Unlock, Suspend, Resume, EndSession };
void PetOnSession(PetSession e);

// Main loop: extra handles to wait on (frame timer, watched processes) and their dispatch.
int  PetWaitHandles(HANDLE* out, int max);
void PetOnHandle(HANDLE h);

// Settings window
void PetDrawPreview(ID2D1RenderTarget* rt, float cx, float floorY, float scale);
struct PetStats { double renderMs = 0; int fps = 0; bool visible = false, hook = false; };
PetStats PetGetStats();
