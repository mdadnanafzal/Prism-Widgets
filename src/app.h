#pragma once
// Shared state + services between the widget host (app.cpp) and the Settings window (settings.cpp).
#include "widgets.h"

struct AccentPreset { const wchar_t* name; const wchar_t* hex; };

extern HINSTANCE g_inst;
extern HWND g_main;
extern std::vector<std::unique_ptr<Widget>> g_widgets;
extern std::wstring g_theme, g_accent, g_textMode, g_uiTheme;
extern bool g_locked, g_snap, g_pauseFullscreen;
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

// Settings window
void RegisterSettingsClass();
void OpenSettings(Widget* focus = nullptr);
void SettingsRefresh(bool widgetsChanged);   // called when state changes outside the window
void CloseSettings();
