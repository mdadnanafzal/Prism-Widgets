#pragma once
// Shared state + services between the widget host (app.cpp) and the Settings window (settings.cpp).
#include "widgets.h"

struct AccentPreset { const wchar_t* name; const wchar_t* hex; };

extern HINSTANCE g_inst;
extern HWND g_main;
extern std::vector<std::unique_ptr<Widget>> g_widgets;
extern std::wstring g_theme, g_accent, g_textMode, g_uiTheme;
extern bool g_quickDrag, g_snap, g_pauseFullscreen;
extern bool g_arranging;          // Arrange mode active
extern Widget* g_arrangeSel;      // selected widget in Arrange mode
extern const AccentPreset g_accents[];
extern const int g_accentCount;
extern const float g_sizes[];
extern const wchar_t* g_sizeNames[];

D2D1_COLOR_F Accent();
Ink  MakeInk(const Theme& t, float lum);
const Theme& ThemeFor(Widget* w);
bool IsStartup();
void SetStartup(bool on);

// After changing a widget's enabled/size/theme/options: show/hide, relayout, redraw and save.
void ApplyWidget(Widget* w);
// After changing a global setting: redraw every widget (optionally rebuilding the glass) and save.
void ApplyGlobal(bool rebuildGlass);
void ResetPositions();
void CreateNewWidget();
void ReloadCustom();
void FreeMemoryNow();
void QuitApp();
void OpenDataFolder();

// Renders a widget (desktop look, real wallpaper) into premultiplied BGRA pixels for previews.
bool RenderPreview(Widget* w, const Theme& t, float scale, std::vector<uint32_t>& px, int& W, int& H);

// Live performance numbers (Settings -> Performance)
struct PerfStats {
    double renderLast = 0, renderAvg = 0, renderMax = 0; ULONGLONG maxAt = 0; unsigned renders = 0;
    double clickLast = -1, clickAvg = -1;       // input -> screen updated, ms
    double dragRate = 0;                        // screen updates per second during the last drag / resize
    bool responsive = false;                    // full speed (true) or efficiency mode (false)
};
extern PerfStats g_perf;
void Interactive();                             // user is interacting: run at full speed for a few seconds
void PerfDragBegin();
void PerfDragStep();
void PerfDragEnd();
void PerfClick(DWORD msgTime, LONGLONG qpcStart);
void MoveWidgetFast(Widget* w);                 // slide the window; glass is refreshed when the mouse rests
void ReleaseCaches();
void MediaKick(UINT ms = 120);                  // poll the media session soon (after a button press)                           // drop rebuildable caches (no working-set trim)

// Placement engine (app.cpp)
float ScaleOf(HMONITOR m);
RECT  WorkOf(HMONITOR m);
RECT  PanelRect(Widget* w);                                   // panel rect in physical screen px
void  ClampWidget(Widget* w, int& x, int& y, const POINT* pref); // keep fully inside a monitor's work area
void  ResolveOverlaps(Widget* fixed, bool updateAnchors);     // push widgets apart; `fixed` never moves
void  UpdateAnchor(Widget* w);                                // remember position relative to nearest corner
void  LayoutWidget(Widget* w);
void  RenderWidget(Widget* w, bool contentSame = false);   // contentSame: only glass/position/frame changed
void  RenderSoon(Widget* w);                              // A2: at most one redraw per display frame
void  FlushFrame();
void  ScheduleRelease();

// Arrange mode (arrange.cpp)
void StartArrange();
void EndArrange();
void ArrangeSnap(Widget* w, int& x, int& y, POINT cursor);   // grid + alignment guides + equal spacing
void ArrangeClearGuides();
void ArrangeSelect(Widget* w);
extern std::wstring g_arrangeBadge;                           // live size readout while resizing
int  ArrangeZone(Widget* w, float lx, float ly);              // resize/scale handle under a local point, -1 = none
LPCWSTR ArrangeZoneCursor(int zone);
void ArrangeResizeBegin(Widget* w, int zone, POINT p);
void ArrangeResizeMove(Widget* w, POINT p, bool alt);
void ArrangeResizeEnd(Widget* w);
void ArrangeResetZone(Widget* w, int zone);                   // double-click a handle
void PreviewWidget(Widget* w);
void ShowInputAt(RECT screen, float scale, const std::wstring& init, const wchar_t* cue, std::function<void(const std::wstring&)> done);                                // live size change without moving neighbours

// Settings window
void RegisterSettingsClass();
void OpenSettings(Widget* focus = nullptr);
void SettingsRefresh(bool widgetsChanged);   // called when state changes outside the window
void CloseSettings();
