#include "common.h"
#include <psapi.h>
#include <lmcons.h>

SysInfo g_sys;
int TodoOpenCount(); // widgets.cpp

std::wstring ReadTextFile(const std::wstring& path, bool* ok) {
    if (ok) *ok = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    if (sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(f); return L""; }
    std::string buf((size_t)sz.QuadPart, '\0');
    DWORD rd = 0;
    if (!buf.empty()) ReadFile(f, &buf[0], (DWORD)buf.size(), &rd, nullptr);
    CloseHandle(f);
    buf.resize(rd);
    size_t off = (buf.size() >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(buf.size() - off), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(buf.size() - off), &w[0], n);
    if (ok) *ok = true;
    return w;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, buf.data(), (DWORD)buf.size(), &wr, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { dir = p; CoTaskMemFree(p); }
        dir += L"\\" APP_ID;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}
std::wstring CustomWidgetDir() {
    std::wstring d = AppDataDir() + L"\\widgets";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::wstring ExePath() {
    wchar_t b[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return std::wstring(b, n);
}

// ------------------------------------------------------------- date / time
bool LocaleUses24h() {
    wchar_t f[80] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, f, 80);
    return wcschr(f, L'H') != nullptr;
}
int LocaleFirstDayOfWeek() {
    wchar_t f[8] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, f, 8);
    return Clamp(_wtoi(f), 0, 6);
}
static std::wstring DatePic(const SYSTEMTIME& st, const wchar_t* pic) {
    wchar_t b[128] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, pic, b, 128, nullptr);
    return b;
}
// Tokens: yyyy yy MMMM MMM MM M dddd ddd dd d HH H hh h mm m ss s tt t; 'literal'
std::wstring FormatDateTime(const SYSTEMTIME& st, const std::wstring& fmt) {
    std::wstring o;
    size_t i = 0, n = fmt.size();
    auto run = [&](wchar_t c) { size_t k = i; while (k < n && fmt[k] == c) k++; return k - i; };
    wchar_t b[32];
    while (i < n) {
        wchar_t c = fmt[i];
        if (c == L'\'') { size_t j = fmt.find(L'\'', i + 1); if (j == std::wstring::npos) j = n; o += fmt.substr(i + 1, j - i - 1); i = j + 1; continue; }
        size_t r = 0;
        switch (c) {
        case L'y': case L'M': case L'd': {
            r = run(c); if (c == L'y' && r != 2) r = r < 4 ? r : 4;
            std::wstring pic(r > 4 ? 4 : r, c);
            o += DatePic(st, pic.c_str()); break;
        }
        case L'H': r = run(c); swprintf(b, 32, r >= 2 ? L"%02d" : L"%d", st.wHour); o += b; break;
        case L'h': { r = run(c); int h = st.wHour % 12; if (!h) h = 12; swprintf(b, 32, r >= 2 ? L"%02d" : L"%d", h); o += b; break; }
        case L'm': r = run(c); swprintf(b, 32, r >= 2 ? L"%02d" : L"%d", st.wMinute); o += b; break;
        case L's': r = run(c); swprintf(b, 32, r >= 2 ? L"%02d" : L"%d", st.wSecond); o += b; break;
        case L't': r = run(c); o += st.wHour < 12 ? (r >= 2 ? L"AM" : L"A") : (r >= 2 ? L"PM" : L"P"); break;
        default: o += c; r = 1;
        }
        i += r;
    }
    return o;
}

// ------------------------------------------------------------- system info
static ULONGLONG ft2u(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
void SysInfo::Update() {
    ULONGLONG now = GetTickCount64();
    if (lastSample && now - lastSample < 900) return;
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG i = ft2u(fi), k = ft2u(fk), u = ft2u(fu);
        if (lastSample) {
            ULONGLONG di = i - lastIdle.QuadPart, dk = k - lastKernel.QuadPart, du = u - lastUser.QuadPart;
            ULONGLONG tot = dk + du;
            cpu = tot ? (float)(100.0 * (double)(tot - di) / (double)tot) : 0.f;
            cpu = Clamp(cpu, 0.f, 100.f);
        }
        lastIdle.QuadPart = i; lastKernel.QuadPart = k; lastUser.QuadPart = u;
    }
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        ram = (float)ms.dwMemoryLoad;
        ramTotalGB = ms.ullTotalPhys / 1073741824.0;
        ramUsedGB = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
    }
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && ps.BatteryLifePercent != 255 && !(ps.BatteryFlag & 128)) {
        battery = ps.BatteryLifePercent; charging = ps.ACLineStatus == 1;
    } else battery = -1;
    lastSample = now;
}

static std::wstring Fmt1(double v, int dec) { wchar_t b[32]; swprintf(b, 32, dec ? L"%.1f" : L"%.0f", v); return b; }

// ------------------------------------------------------------- variables
std::wstring ExpandVars(const std::wstring& s) {
    if (s.find(L'{') == std::wstring::npos) return s;
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring o;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'{') { o += s[i++]; continue; }
        size_t j = s.find(L'}', i);
        if (j == std::wstring::npos) { o += s.substr(i); break; }
        std::wstring tok = s.substr(i + 1, j - i - 1), name = tok, arg;
        size_t c = tok.find(L':');
        if (c != std::wstring::npos) { name = tok.substr(0, c); arg = tok.substr(c + 1); }
        for (auto& ch : name) ch = towlower(ch);
        std::wstring v;
        if (name == L"time") v = FormatDateTime(st, arg.empty() ? (LocaleUses24h() ? L"HH:mm" : L"h:mm tt") : arg);
        else if (name == L"date") v = FormatDateTime(st, arg.empty() ? L"dddd, d MMMM" : arg);
        else if (name == L"cpu") { g_sys.Update(); v = Fmt1(g_sys.cpu, 0); }
        else if (name == L"ram") { g_sys.Update(); v = Fmt1(g_sys.ram, 0); }
        else if (name == L"ramused") { g_sys.Update(); v = Fmt1(g_sys.ramUsedGB, 1); }
        else if (name == L"ramtotal") { g_sys.Update(); v = Fmt1(g_sys.ramTotalGB, 1); }
        else if (name == L"battery") { g_sys.Update(); v = g_sys.battery < 0 ? L"--" : Fmt1(g_sys.battery, 0); }
        else if (name == L"charging") { g_sys.Update(); v = g_sys.charging ? (arg.empty() ? L"⚡" : arg) : L""; }
        else if (name == L"disk" || name == L"diskfree") {
            std::wstring root = (arg.empty() ? L"C" : arg.substr(0, 1)) + L":\\";
            ULARGE_INTEGER fr{}, tot{}, tf{};
            if (GetDiskFreeSpaceExW(root.c_str(), &fr, &tot, &tf) && tot.QuadPart)
                v = name == L"disk" ? Fmt1(100.0 * (double)(tot.QuadPart - tf.QuadPart) / (double)tot.QuadPart, 0)
                                    : Fmt1(tf.QuadPart / 1073741824.0, 0);
            else v = L"--";
        }
        else if (name == L"uptime") {
            ULONGLONG sec = GetTickCount64() / 1000; wchar_t b[64];
            swprintf(b, 64, L"%llud %lluh %llum", sec / 86400, (sec / 3600) % 24, (sec / 60) % 60); v = b;
        }
        else if (name == L"greeting") {
            int h = st.wHour;
            v = h < 5 ? L"Good night" : h < 12 ? L"Good morning" : h < 17 ? L"Good afternoon" : h < 22 ? L"Good evening" : L"Good night";
        }
        else if (name == L"user") {
            wchar_t b[UNLEN + 1]; DWORD n = UNLEN + 1;
            if (GetUserNameW(b, &n)) v = b;
        }
        else if (name == L"countdown") {
            int y = 0, m = 0, d = 0;
            if (swscanf(arg.c_str(), L"%d-%d-%d", &y, &m, &d) == 3) {
                SYSTEMTIME t{}; t.wYear = (WORD)y; t.wMonth = (WORD)m; t.wDay = (WORD)d;
                SYSTEMTIME today = st; today.wHour = today.wMinute = today.wSecond = today.wMilliseconds = 0;
                FILETIME a, b2; SystemTimeToFileTime(&t, &a); SystemTimeToFileTime(&today, &b2);
                long long diff = ((long long)ft2u(a) - (long long)ft2u(b2)) / 864000000000LL;
                v = std::to_wstring(diff < 0 ? 0 : diff);
            }
        }
        else if (name == L"env") {
            wchar_t b[1024]; DWORD n = GetEnvironmentVariableW(arg.c_str(), b, 1024);
            if (n && n < 1024) v = b;
        }
        else if (name == L"file") {
            std::wstring t = ReadTextFile(arg);
            size_t e = t.find_first_of(L"\r\n");
            v = t.substr(0, e);
        }
        else if (name == L"todo") v = std::to_wstring(TodoOpenCount());
        else { o += L'{'; i++; continue; }  // unknown: keep literal brace
        o += v;
        i = j + 1;
    }
    return o;
}

void Launch(const std::wstring& target) {
    if (target.empty()) return;
    std::wstring file = target, args;
    // "program.exe" args  -> split on first quoted token
    if (target[0] == L'"') {
        size_t q = target.find(L'"', 1);
        if (q != std::wstring::npos) { file = target.substr(1, q - 1); args = target.substr(q + 1); }
    }
    while (!args.empty() && args[0] == L' ') args.erase(0, 1);
    ShellExecuteW(nullptr, L"open", file.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
}

D2D1_COLOR_F ParseColor(const std::wstring& s, bool* ok) {
    if (ok) *ok = false;
    D2D1_COLOR_F c{ 1, 1, 1, 1 };
    if (s.size() >= 7 && s[0] == L'#') {
        unsigned v = wcstoul(s.c_str() + 1, nullptr, 16);
        if (s.size() >= 9) { c.r = ((v >> 24) & 255) / 255.f; c.g = ((v >> 16) & 255) / 255.f; c.b = ((v >> 8) & 255) / 255.f; c.a = (v & 255) / 255.f; }
        else { c.r = ((v >> 16) & 255) / 255.f; c.g = ((v >> 8) & 255) / 255.f; c.b = (v & 255) / 255.f; }
        if (ok) *ok = true;
    }
    return c;
}

D2D1_COLOR_F WindowsAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        // stored as 0xAABBGGRR
        return D2D1_COLOR_F{ (v & 255) / 255.f, ((v >> 8) & 255) / 255.f, ((v >> 16) & 255) / 255.f, 1.f };
    }
    return D2D1_COLOR_F{ 0.0f, 0.47f, 0.84f, 1.f };
}

// Return freed pages to the OS so the process stays tiny in Task Manager.
void TrimMemory() {
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}
