// Prism Widgets - lightweight native desktop widgets for Windows 11
// Shared declarations.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

#define APP_NAME     L"Prism Widgets"
#define APP_ID       L"PrismWidgets"
#define APP_VERSION  L"1.7.0"

template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
template <class T> inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- JSON
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<JVal> a;
    std::vector<std::pair<std::wstring, JVal>> o;

    const JVal* get(const wchar_t* key) const;
    JVal& set(const wchar_t* key);                 // object member (created if missing)
    double num(const wchar_t* key, double def) const;
    bool boolean(const wchar_t* key, bool def) const;
    std::wstring str(const wchar_t* key, const wchar_t* def) const;
    bool isObj() const { return type == Obj; }
    bool isArr() const { return type == Arr; }

    static JVal MakeNum(double v) { JVal j; j.type = Num; j.n = v; return j; }
    static JVal MakeStr(const std::wstring& v) { JVal j; j.type = Str; j.s = v; return j; }
    static JVal MakeBool(bool v) { JVal j; j.type = Bool; j.b = v; return j; }
    static JVal MakeObj() { JVal j; j.type = Obj; return j; }
    static JVal MakeArr() { JVal j; j.type = Arr; return j; }
};
bool JsonParse(const std::wstring& text, JVal& out, std::wstring* err = nullptr);
std::wstring JsonWrite(const JVal& v, int indent = 0);

// ---------------------------------------------------------------- util
std::wstring ReadTextFile(const std::wstring& path, bool* ok = nullptr);
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
bool FileExists(const std::wstring& path);
std::wstring AppDataDir();         // %APPDATA%\PrismWidgets
std::wstring CustomWidgetDir();    // %APPDATA%\PrismWidgets\widgets
std::wstring ExePath();
std::wstring FormatDateTime(const SYSTEMTIME& st, const std::wstring& fmt);
bool LocaleUses24h();
int  LocaleFirstDayOfWeek();       // 0 = Monday ... 6 = Sunday
std::wstring ExpandVars(const std::wstring& s);
void Launch(const std::wstring& target);
D2D1_COLOR_F ParseColor(const std::wstring& s, bool* ok);
D2D1_COLOR_F WindowsAccent();
void TrimMemory();

struct SysInfo {
    float cpu = 0, ram = 0, battery = -1; bool charging = false;
    double ramUsedGB = 0, ramTotalGB = 0;
    ULONGLONG lastSample = 0;
    ULARGE_INTEGER lastIdle{}, lastKernel{}, lastUser{};
    void Update();     // throttled (max ~1/s)
};
extern SysInfo g_sys;

// ---------------------------------------------------------------- themes
struct Theme {
    const wchar_t* id;
    const wchar_t* name;
    bool  panel;        // draws a glass panel (false = text directly on wallpaper)
    float blur;         // sigma in DIP
    float radius;       // corner radius in DIP
    float tr, tg, tb, ta; // tint colour + strength (tr<0 => accent tint)
    float saturation;
    float brightness;
    float refraction;   // edge lens displacement in DIP (liquid glass)
    float chroma;       // chromatic aberration amount
    float rim;          // specular rim highlight strength
    float border;       // thin border strength
    float shadow;       // drop shadow opacity
    float shadowSize;   // DIP
    float noise;        // acrylic grain
};
extern const Theme g_themes[];
extern const int g_themeCount;
const Theme& FindTheme(const std::wstring& id);

// ---------------------------------------------------------------- wallpaper
class Wallpaper {
public:
    struct Mon {
        RECT rc{};              // monitor rect in physical screen px
        int pw = 0, ph = 0;     // proxy size
        std::vector<uint32_t> px; // BGRA proxy of what the desktop wallpaper shows
    };
    bool Ensure();              // loads proxies if not loaded
    void Release();
    bool Loaded() const { return loaded_; }
    const std::vector<Mon>& Mons() const { return mons_; }
    float PhysPerProxy(int mon) const {   // physical screen px per proxy px (>= 1 on big screens)
        if (mon < 0 || mon >= (int)mons_.size() || !mons_[mon].pw) return 1.f;
        return (float)(mons_[mon].rc.right - mons_[mon].rc.left) / mons_[mon].pw;
    }
    // Bilinear sample (physical screen coords). Returns 0x00RRGGBB.
    void Sample(int mon, float x, float y, float& r, float& g, float& b) const;
    int  MonitorFor(const RECT& r) const;
    std::wstring Signature();   // cheap check for wallpaper changes
private:
    bool loaded_ = false;
    std::vector<Mon> mons_;
    bool LoadInto(Mon& m, const std::wstring& path, int pos, const RECT& spanRect, COLORREF bg);
};
extern Wallpaper g_wall;

// Render the glass/background layer (premultiplied BGRA, w*h) for a window
// whose top-left is at screen (sx, sy). Panel inset by `margin` px.
void RenderBackground(const Theme& t, int sx, int sy, int w, int h, int margin,
                      float scale, D2D1_COLOR_F accent, uint32_t* out, float* lumOut);
