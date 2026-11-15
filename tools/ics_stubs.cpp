#include "../src/events.h"
#include "../src/net.h"
#include "../src/app.h"
HWND g_main;
void EventsChanged() {}
void RequestSave() {}
int TodoOpenCount() { return 0; }
float ScaleOf(HMONITOR) { return 1; }
RECT WorkOf(HMONITOR) { return RECT{}; }
void HttpGetAsync(const std::wstring&, std::function<void(std::string*)>, size_t) {}
void ShowInputAt(RECT, float, const std::wstring&, const wchar_t*, std::function<void(const std::wstring&)>) {}
std::wstring Utf8ToWide(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0); std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n); return w; }
