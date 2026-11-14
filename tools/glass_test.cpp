// Dev harness: renders every theme's glass background to raw files so they can be inspected.
#include "../src/common.h"
#include <cstdio>
int TodoOpenCount() { return 0; }
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!g_wall.Ensure()) { printf("wallpaper load failed\n"); return 1; }
    const int W = 420, H = 280, M = 32;
    for (int i = 0; i < g_themeCount; i++) {
        std::vector<uint32_t> buf(W * H);
        float lum = 0;
        D2D1_COLOR_F acc{ 0.04f, 0.52f, 1.f, 1.f };
        DWORD t0 = GetTickCount();
        RenderBackground(g_themes[i], 900, 300, W, H, M, 1.25f, acc, buf.data(), &lum);
        DWORD t1 = GetTickCount();
        wchar_t name[64]; swprintf(name, 64, L"theme_%d.raw", i);
        FILE* f = _wfopen(name, L"wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
        printf("%ls lum=%.2f ms=%lu\n", g_themes[i].id, lum, t1 - t0);
    }
    return 0;
}
