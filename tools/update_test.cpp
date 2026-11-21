// Test harness for the updater: fake network, real signature / hash checks and file swap.
#include "../src/update.h"
#include "../src/app.h"
#include "../src/net.h"
#include <cstdio>
#include <fstream>
#include <sstream>
HWND g_main;
std::string g_manifest, g_payload;
bool g_quit = false;
void RequestSave() {}
void UpdateChanged() {}
void TrayBalloon(const std::wstring& t, const std::wstring& x, int p) { printf("  balloon[%d]: %ls\n", p, t.c_str()); }
void QuitForUpdate(const std::wstring& exe) { g_quit = true; printf("  quit-for-update: %ls\n", exe.c_str()); }
void HttpGetAsync(const std::wstring& url, std::function<void(std::string*)> done, size_t maxBytes) {
    std::string s = url.find(L"update.json") != std::wstring::npos ? g_manifest : g_payload;
    if (s.size() > maxBytes) { done(nullptr); return; }
    std::string* b = new std::string(s); done(b); delete b;
}
std::wstring Utf8ToWide(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0); std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n); return w; }
int TodoOpenCount() { return 0; }
float ScaleOf(HMONITOR) { return 1; }
RECT WorkOf(HMONITOR) { return RECT{}; }
static std::string Read(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static const char* St(Updater::State s) { const char* n[] = { "Idle","Checking","UpToDate","Available","Downloading","Ready","Failed" }; return n[s]; }
int main() {
    printf("compare: %d %d %d %d\n", CompareVersions(L"1.8.0", L"1.7.0"), CompareVersions(L"1.7.0", L"1.7.0"), CompareVersions(L"1.10.0", L"1.9.9"), CompareVersions(L"1.7", L"1.7.1"));
    std::string good = Read("manifest.json"); g_payload = Read("newbin.exe");
    // 1. tampered manifest (size changed)
    g_manifest = good; size_t p = g_manifest.find("25612"); g_manifest.replace(p, 5, "25613");
    g_update.Check(true); printf("tampered: %s  %ls\n", St(g_update.state), g_update.Status().c_str());
    // 2. good manifest
    g_manifest = good; g_update.state = Updater::Idle;
    g_update.Check(true); printf("good: %s  %ls  notes=[%ls]\n", St(g_update.state), g_update.Status().c_str(), g_update.notes.c_str());
    // 3. corrupted download
    std::string keep = g_payload; g_payload[100] ^= 1;
    g_update.Install(); printf("corrupt download: %s  %ls\n", St(g_update.state), g_update.Status().c_str());
    // 4. good download: swaps this very exe
    g_payload = keep;
    g_update.Install(); printf("install: %s quit=%d\n", St(g_update.state), g_quit);
    std::string now = Read("update_test.exe");
    printf("exe replaced: %s, old kept: %s\n", now == keep ? "yes" : "NO", GetFileAttributesA("update_test.old.exe") != INVALID_FILE_ATTRIBUTES ? "yes" : "NO");
    return 0;
}
