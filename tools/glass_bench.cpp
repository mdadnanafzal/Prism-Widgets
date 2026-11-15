// Dev benchmark: average RenderBackground time per theme for a typical widget at 150% scaling.
#include "../src/common.h"
#include <cstdio>
int TodoOpenCount() { return 0; }
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!g_wall.Ensure()) { printf("wallpaper load failed\n"); return 1; }
    const float s = 1.5f; const int M = (int)(32 * s), W = (int)(330 * s) + 2 * M, H = (int)(160 * s) + 2 * M;
    std::vector<uint32_t> buf((size_t)W * H);
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    D2D1_COLOR_F acc{ 0.04f, 0.52f, 1.f, 1.f };
    double total = 0;
    for (int i = 0; i < g_themeCount; i++) {
        float lum; int N = 12;
        RenderBackground(g_themes[i], 600, 300, W, H, M, s, acc, buf.data(), &lum);   // warm-up
        QueryPerformanceCounter(&a);
        for (int k = 0; k < N; k++) RenderBackground(g_themes[i], 600 + k * 7, 300, W, H, M, s, acc, buf.data(), &lum);
        QueryPerformanceCounter(&b);
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / N;
        total += ms;
        printf("%-8ls %6.2f ms  (%dx%d)\n", g_themes[i].id, ms, W, H);
        if (i == 0) { FILE* o = _wfopen(L"bench_liquid.raw", L"wb"); fwrite(buf.data(), 4, buf.size(), o); fclose(o); }
        if (i == 1) { FILE* o = _wfopen(L"bench_frosted.raw", L"wb"); fwrite(buf.data(), 4, buf.size(), o); fclose(o); }
    }
    printf("average  %6.2f ms\n", total / g_themeCount);
    return 0;
}
