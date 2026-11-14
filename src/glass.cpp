// Wallpaper sampling + software glass renderer.
// The desktop wallpaper is decoded once into a small per-monitor "proxy" image,
// cropped/blurred per widget, then the proxy is freed again to keep RAM tiny.
#include "common.h"

Wallpaper g_wall;

//                id          name             panel blur  rad  tr    tg    tb    ta    sat  bri   refr chroma rim  border shadow size noise
const Theme g_themes[] = {
    { L"liquid",  L"Liquid Glass",   true,  2.5f, 28, 1.f,  1.f,  1.f,  .05f, 1.4f, 1.04f, 18, .35f, .95f, .00f, .30f, 22, 0 },
    { L"frosted", L"Frosted Glass",  true,  16,   22, 1.f,  1.f,  1.f,  .18f, 1.2f, 1.03f, 0,  0,    .35f, .35f, .22f, 24, 0 },
    { L"acrylic", L"Acrylic",        true,  24,   12, .11f, .11f, .13f, .50f, 1.5f, 1.00f, 0,  0,    .12f, .10f, .35f, 22, .03f },
    { L"tinted",  L"Tinted Glass",   true,  10,   24, -1,   -1,   -1,   .30f, 1.3f, 1.00f, 10, .20f, .60f, .15f, .28f, 22, 0 },
    { L"clear",   L"Clear (no panel)", false, 0,  16, 0,    0,    0,    0,    1,    1,     0,  0,    0,    0,    0,    0,  0 },
    { L"dark",    L"Solid Dark",     true,  24,   16, .09f, .09f, .11f, .88f, 1.0f, 1.00f, 0,  0,    .08f, .10f, .40f, 20, .01f },
    { L"light",   L"Solid Light",    true,  24,   16, .97f, .97f, .98f, .86f, 1.0f, 1.00f, 0,  0,    .25f, .50f, .25f, 20, 0 },
};
const int g_themeCount = sizeof(g_themes) / sizeof(g_themes[0]);

const Theme& FindTheme(const std::wstring& id) {
    for (int i = 0; i < g_themeCount; i++) if (id == g_themes[i].id) return g_themes[i];
    return g_themes[0];
}

// ------------------------------------------------------------------ wallpaper
static std::wstring TranscodedWallpaper() {
    PWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
}

void Wallpaper::Release() {
    mons_.clear();
    mons_.shrink_to_fit();
    loaded_ = false;
}

int Wallpaper::MonitorFor(const RECT& r) const {
    int best = -1; long bestA = -1;
    for (size_t i = 0; i < mons_.size(); i++) {
        RECT x; long a = IntersectRect(&x, &r, &mons_[i].rc) ? (x.right - x.left) * (x.bottom - x.top) : 0;
        if (a > bestA) { bestA = a; best = (int)i; }
    }
    if (bestA <= 0 && !mons_.empty()) {
        long cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2; double bd = 1e30;
        for (size_t i = 0; i < mons_.size(); i++) {
            const RECT& m = mons_[i].rc;
            double dx = cx - (m.left + m.right) / 2.0, dy = cy - (m.top + m.bottom) / 2.0;
            if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = (int)i; }
        }
    }
    return best;
}

void Wallpaper::Sample(int mi, float x, float y, float& r, float& g, float& b) const {
    if (mi < 0 || mi >= (int)mons_.size() || mons_[mi].px.empty()) { r = g = b = 0.25f; return; }
    const Mon& m = mons_[mi];
    float sx = (float)m.pw / (float)(m.rc.right - m.rc.left), sy = (float)m.ph / (float)(m.rc.bottom - m.rc.top);
    float u = (x - m.rc.left) * sx - 0.5f, v = (y - m.rc.top) * sy - 0.5f;
    u = Clamp(u, 0.f, (float)(m.pw - 1)); v = Clamp(v, 0.f, (float)(m.ph - 1));
    int x0 = (int)u, y0 = (int)v, x1 = x0 + 1 < m.pw ? x0 + 1 : x0, y1 = y0 + 1 < m.ph ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    uint32_t p00 = m.px[y0 * m.pw + x0], p10 = m.px[y0 * m.pw + x1], p01 = m.px[y1 * m.pw + x0], p11 = m.px[y1 * m.pw + x1];
    auto ch = [&](int sh) {
        float a = ((p00 >> sh) & 255) * (1 - fx) + ((p10 >> sh) & 255) * fx;
        float c = ((p01 >> sh) & 255) * (1 - fx) + ((p11 >> sh) & 255) * fx;
        return (a * (1 - fy) + c * fy) / 255.f;
    };
    r = ch(16); g = ch(8); b = ch(0);
}

bool Wallpaper::LoadInto(Mon& m, const std::wstring& path, int pos, const RECT& span, COLORREF bg) {
    int mw = m.rc.right - m.rc.left, mh = m.rc.bottom - m.rc.top;
    float ps = std::min(1.0f, 1600.0f / (float)std::max(mw, mh));
    m.pw = std::max(1, (int)std::ceil(mw * ps)); m.ph = std::max(1, (int)std::ceil(mh * ps));
    uint32_t bgc = 0xFF000000u | (GetRValue(bg) << 16) | (GetGValue(bg) << 8) | GetBValue(bg);
    m.px.assign((size_t)m.pw * m.ph, bgc);
    if (path.empty()) return true;

    IWICImagingFactory* f = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f))) break;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT iw = 0, ih = 0; fr->GetSize(&iw, &ih);
        if (!iw || !ih) break;

        // Area the image is laid out in (monitor-local physical px)
        double ax = 0, ay = 0, aw = mw, ah = mh;
        if (pos == DWPOS_SPAN) { ax = span.left - m.rc.left; ay = span.top - m.rc.top; aw = span.right - span.left; ah = span.bottom - span.top; }
        double s = 1, dw, dh;
        bool tile = pos == DWPOS_TILE;
        switch (pos) {
        case DWPOS_FIT:     s = std::min(aw / iw, ah / ih); dw = iw * s; dh = ih * s; break;
        case DWPOS_STRETCH: dw = aw; dh = ah; break;
        case DWPOS_CENTER:
        case DWPOS_TILE:    dw = iw; dh = ih; break;
        default:            s = std::max(aw / iw, ah / ih); dw = iw * s; dh = ih * s; break; // FILL / SPAN
        }
        double dx = tile ? ax : ax + (aw - dw) / 2, dy = tile ? ay : ay + (ah - dh) / 2;
        UINT sw = (UINT)std::max(1.0, std::round(dw * ps)), sh = (UINT)std::max(1.0, std::round(dh * ps));
        if (sw > 16384 || sh > 16384) break;

        if (FAILED(f->CreateBitmapScaler(&sc))) break;
        if (FAILED(sc->Initialize(fr, sw, sh, WICBitmapInterpolationModeFant))) break;
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;

        if (!tile) {
            int ox = (int)std::round(dx * ps), oy = (int)std::round(dy * ps);
            int x0 = std::max(0, ox), y0 = std::max(0, oy);
            int x1 = std::min(m.pw, ox + (int)sw), y1 = std::min(m.ph, oy + (int)sh);
            if (x1 > x0 && y1 > y0) {
                WICRect rc{ x0 - ox, y0 - oy, x1 - x0, y1 - y0 };
                uint8_t* dst = (uint8_t*)&m.px[(size_t)y0 * m.pw + x0];
                UINT stride = m.pw * 4, size = (UINT)(((size_t)(y1 - y0 - 1) * m.pw + (x1 - x0)) * 4);
                if (FAILED(cv->CopyPixels(&rc, stride, size, dst))) break;
            }
        } else {
            std::vector<uint32_t> t((size_t)sw * sh);
            if (FAILED(cv->CopyPixels(nullptr, sw * 4, (UINT)(t.size() * 4), (BYTE*)t.data()))) break;
            for (int y = 0; y < m.ph; y++) for (int x = 0; x < m.pw; x++) m.px[(size_t)y * m.pw + x] = t[(size_t)(y % sh) * sw + (x % sw)];
        }
        for (auto& p : m.px) p |= 0xFF000000u; // ignore image alpha
        ok = true;
    } while (0);
    SafeRelease(cv); SafeRelease(sc); SafeRelease(fr); SafeRelease(dec); SafeRelease(f);
    return ok;
}

namespace { struct EnumCtx { std::vector<RECT> rcs; }; }
static BOOL CALLBACK EnumMon(HMONITOR, HDC, LPRECT r, LPARAM p) { ((EnumCtx*)p)->rcs.push_back(*r); return TRUE; }

bool Wallpaper::Ensure() {
    if (loaded_) return true;
    mons_.clear();
    RECT virt{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    std::wstring fallback = TranscodedWallpaper();
    if (!FileExists(fallback)) fallback.clear();

    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_IDesktopWallpaper, (void**)&dw)) && dw) {
        UINT cnt = 0; dw->GetMonitorDevicePathCount(&cnt);
        DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL; dw->GetPosition(&pos);
        COLORREF bg = RGB(30, 30, 30); dw->GetBackgroundColor(&bg);
        for (UINT i = 0; i < cnt; i++) {
            LPWSTR id = nullptr;
            if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
            RECT r{};
            if (SUCCEEDED(dw->GetMonitorRECT(id, &r)) && !IsRectEmpty(&r)) {
                LPWSTR wp = nullptr; dw->GetWallpaper(id, &wp);
                std::wstring path = wp ? wp : L""; if (wp) CoTaskMemFree(wp);
                if (path.empty() || !FileExists(path)) path = fallback;
                Mon m; m.rc = r;
                LoadInto(m, path, (int)pos, virt, bg);
                mons_.push_back(std::move(m));
            }
            CoTaskMemFree(id);
        }
        dw->Release();
    }
    if (mons_.empty()) {
        EnumCtx c; EnumDisplayMonitors(nullptr, nullptr, EnumMon, (LPARAM)&c);
        for (auto& r : c.rcs) { Mon m; m.rc = r; LoadInto(m, fallback, DWPOS_FILL, virt, GetSysColor(COLOR_DESKTOP)); mons_.push_back(std::move(m)); }
    }
    loaded_ = !mons_.empty();
    return loaded_;
}

std::wstring Wallpaper::Signature() {
    std::wstring sig;
    auto addFile = [&](const std::wstring& p) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        sig += p;
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
            sig += L"|" + std::to_wstring(a.ftLastWriteTime.dwLowDateTime) + L"|" + std::to_wstring(a.nFileSizeLow);
        sig += L";";
    };
    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_IDesktopWallpaper, (void**)&dw)) && dw) {
        UINT cnt = 0; dw->GetMonitorDevicePathCount(&cnt);
        DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL; dw->GetPosition(&pos);
        sig += std::to_wstring((int)pos) + L";";
        for (UINT i = 0; i < cnt; i++) {
            LPWSTR id = nullptr; if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
            LPWSTR wp = nullptr; dw->GetWallpaper(id, &wp);
            RECT r{}; dw->GetMonitorRECT(id, &r);
            sig += std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom) + L":";
            if (wp) { addFile(wp); CoTaskMemFree(wp); }
            CoTaskMemFree(id);
        }
        dw->Release();
    }
    addFile(TranscodedWallpaper());
    return sig;
}

// ------------------------------------------------------------------ blur
static void BoxBlurPass(std::vector<uint32_t>& img, std::vector<uint32_t>& tmp, int w, int h, int r, bool horiz) {
    int len = horiz ? w : h, lines = horiz ? h : w;
    if (len <= 1 || r <= 0) return;
    std::vector<uint32_t> line(len);
    float inv = 1.0f / (2 * r + 1);
    for (int l = 0; l < lines; l++) {
        for (int i = 0; i < len; i++) line[i] = horiz ? img[(size_t)l * w + i] : img[(size_t)i * w + l];
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) { uint32_t p = line[Clamp(k, 0, len - 1)]; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255; }
        for (int i = 0; i < len; i++) {
            uint32_t v = 0xFF000000u | ((uint32_t)(sr * inv + .5f) << 16) | ((uint32_t)(sg * inv + .5f) << 8) | (uint32_t)(sb * inv + .5f);
            if (horiz) tmp[(size_t)l * w + i] = v; else tmp[(size_t)i * w + l] = v;
            uint32_t a = line[std::min(i + r + 1, len - 1)], b = line[std::max(i - r, 0)];
            sr += (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
            sg += (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
            sb += (int)(a & 255) - (int)(b & 255);
        }
    }
    img.swap(tmp);
}

static inline float Smooth(float e0, float e1, float x) { float t = Clamp((x - e0) / (e1 - e0), 0.f, 1.f); return t * t * (3 - 2 * t); }

// Signed distance to a rounded rectangle (negative inside) + outward normal.
static inline float SdRound(float px, float py, float cx, float cy, float hx, float hy, float r, float* nx, float* ny) {
    float dx = px - cx, dy = py - cy;
    float qx = std::fabs(dx) - (hx - r), qy = std::fabs(dy) - (hy - r);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    float ol = std::sqrt(ox * ox + oy * oy);
    if (nx) {
        if (qx > 0 && qy > 0) { *nx = (ox / ol) * (dx < 0 ? -1.f : 1.f); *ny = (oy / ol) * (dy < 0 ? -1.f : 1.f); }
        else if (qx > qy) { *nx = dx < 0 ? -1.f : 1.f; *ny = 0; }
        else { *nx = 0; *ny = dy < 0 ? -1.f : 1.f; }
    }
    return ol + std::min(std::max(qx, qy), 0.f) - r;
}

void RenderBackground(const Theme& t, int sx, int sy, int w, int h, int margin, float scale,
                      D2D1_COLOR_F accent, uint32_t* out, float* lumOut) {
    RECT wr{ sx, sy, sx + w, sy + h };
    int mon = g_wall.MonitorFor(wr);
    float x0 = (float)margin, y0 = (float)margin, x1 = (float)(w - margin), y1 = (float)(h - margin);
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    float rad = std::min(t.radius * scale, std::min(hx, hy));

    if (!t.panel) {
        // Fully transparent; alpha=1 inside the panel so the widget still receives clicks.
        double lsum = 0; int ln = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            float d = SdRound(x + .5f, y + .5f, cx, cy, hx, hy, rad, nullptr, nullptr);
            out[(size_t)y * w + x] = d < 0 ? 0x01000000u : 0;
            if (d < -4 && ((x | y) & 7) == 0) {
                float r, g, b; g_wall.Sample(mon, sx + x + .5f, sy + y + .5f, r, g, b);
                lsum += .2126 * r + .7152 * g + .0722 * b; ln++;
            }
        }
        if (lumOut) *lumOut = ln ? (float)(lsum / ln) : 0.3f;
        return;
    }

    float sigma = t.blur * scale, refr = t.refraction * scale;
    int br = sigma > 0.5f ? std::max(1, (int)std::round((std::sqrt(4 * sigma * sigma + 1) - 1) / 2)) : 0;
    int pad = 3 * br + (int)std::ceil(refr) + 2;
    int bw = w + 2 * pad, bh = h + 2 * pad;
    std::vector<uint32_t> B((size_t)bw * bh), T((size_t)bw * bh);
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
        float r, g, b; g_wall.Sample(mon, sx - pad + x + .5f, sy - pad + y + .5f, r, g, b);
        B[(size_t)y * bw + x] = 0xFF000000u | ((uint32_t)(r * 255 + .5f) << 16) | ((uint32_t)(g * 255 + .5f) << 8) | (uint32_t)(b * 255 + .5f);
    }
    for (int i = 0; i < 3 && br > 0; i++) { BoxBlurPass(B, T, bw, bh, br, true); BoxBlurPass(B, T, bw, bh, br, false); }
    T.clear(); T.shrink_to_fit();

    auto fetch = [&](float fx, float fy, int sh) -> float {
        fx = Clamp(fx - .5f, 0.f, (float)(bw - 1)); fy = Clamp(fy - .5f, 0.f, (float)(bh - 1));
        int ix = (int)fx, iy = (int)fy, jx = std::min(ix + 1, bw - 1), jy = std::min(iy + 1, bh - 1);
        float ax = fx - ix, ay = fy - iy;
        float a = ((B[(size_t)iy * bw + ix] >> sh) & 255) * (1 - ax) + ((B[(size_t)iy * bw + jx] >> sh) & 255) * ax;
        float c = ((B[(size_t)jy * bw + ix] >> sh) & 255) * (1 - ax) + ((B[(size_t)jy * bw + jx] >> sh) & 255) * ax;
        return (a * (1 - ay) + c * ay) / 255.f;
    };

    float tr = t.tr, tg = t.tg, tb = t.tb;
    if (tr < 0) { tr = accent.r; tg = accent.g; tb = accent.b; }
    float shSize = std::max(1.f, t.shadowSize * scale), shOff = shSize * 0.3f;
    float rimW = 1.3f * scale, glowW = 7.f * scale, zone = std::max(1.f, std::min(rad * 1.15f, std::min(hx, hy)));
    const float lx = -0.55f, ly = -0.835f; // light from top-left
    double lsum = 0; int ln = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float px = x + .5f, py = y + .5f;
            float ds = SdRound(px, py - shOff, cx, cy, hx, hy, rad, nullptr, nullptr);
            float sa = ds <= 0 ? t.shadow : t.shadow * std::pow(Clamp(1 - ds / shSize, 0.f, 1.f), 2.2f);
            float nx = 0, ny = 0;
            float d = SdRound(px, py, cx, cy, hx, hy, rad, &nx, &ny);
            float cov = Clamp(0.5f - d, 0.f, 1.f);
            if (cov <= 0) { out[(size_t)y * w + x] = ((uint32_t)(sa * 255 + .5f)) << 24; continue; }
            float e = std::max(-d, 0.f);
            float bx = px + pad, by = py + pad, r, g, b;
            if (refr > 0) {
                float k = Clamp(1 - e / zone, 0.f, 1.f); k = refr * k * k;
                float c = t.chroma;
                r = fetch(bx + nx * k * (1 + c), by + ny * k * (1 + c), 16);
                g = fetch(bx + nx * k, by + ny * k, 8);
                b = fetch(bx + nx * k * (1 - c), by + ny * k * (1 - c), 0);
            } else {
                uint32_t p = B[(size_t)y * bw + x + (size_t)pad * bw + pad];
                r = ((p >> 16) & 255) / 255.f; g = ((p >> 8) & 255) / 255.f; b = (p & 255) / 255.f;
            }
            float L = .2126f * r + .7152f * g + .0722f * b;
            r = (L + (r - L) * t.saturation) * t.brightness;
            g = (L + (g - L) * t.saturation) * t.brightness;
            b = (L + (b - L) * t.saturation) * t.brightness;
            r = r * (1 - t.ta) + tr * t.ta; g = g * (1 - t.ta) + tg * t.ta; b = b * (1 - t.ta) + tb * t.ta;
            if (t.noise > 0) {
                uint32_t hsh = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u); hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                float n = ((hsh & 255) / 255.f - .5f) * t.noise; r += n; g += n; b += n;
            }
            if (t.rim > 0) {
                float dl = nx * lx + ny * ly;
                float edge = std::exp(-e / rimW);
                float spec = t.rim * edge * (0.16f + 0.8f * std::max(dl, 0.f) + 0.38f * std::max(-dl, 0.f));
                float glow = t.rim * 0.10f * std::exp(-e / glowW) * std::max(dl, 0.f);
                float sheen = t.rim * 0.045f * (1 - Clamp((py - y0) / (y1 - y0), 0.f, 1.f));
                r += spec + glow + sheen; g += spec + glow + sheen; b += spec + glow + sheen;
            }
            if (t.border > 0) {
                float bd = t.border * std::exp(-e / (0.7f * scale));
                r += (1 - r) * bd; g += (1 - g) * bd; b += (1 - b) * bd;
            }
            r = Clamp(r, 0.f, 1.f); g = Clamp(g, 0.f, 1.f); b = Clamp(b, 0.f, 1.f);
            if (e > 6 * scale && ((x | y) & 3) == 0) { lsum += .2126f * r + .7152f * g + .0722f * b; ln++; }
            float a = cov + sa * (1 - cov);
            out[(size_t)y * w + x] = ((uint32_t)(a * 255 + .5f) << 24) | ((uint32_t)(r * cov * 255 + .5f) << 16) |
                                     ((uint32_t)(g * cov * 255 + .5f) << 8) | (uint32_t)(b * cov * 255 + .5f);
        }
    }
    if (lumOut) *lumOut = ln ? (float)(lsum / ln) : 0.5f;
}
