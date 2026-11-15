#include "weather.h"
#include "app.h"
#include <winhttp.h>

Weather g_weather;

namespace {
enum Job { J_LOCATE = 1, J_LOCATE2, J_FORECAST, J_GEOCODE };

struct Request { int job; std::wstring host, path; };

std::string* HttpGet(const std::wstring& host, const std::wstring& path) {
    std::string* out = nullptr;
    HINTERNET s = WinHttpOpen(L"PrismWidgets/" APP_VERSION, 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"PrismWidgets/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return nullptr;
    WinHttpSetTimeouts(s, 8000, 8000, 12000, 15000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            out = new std::string();
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                size_t old = out->size(); out->resize(old + avail);
                DWORD rd = 0;
                if (!WinHttpReadData(r, &(*out)[old], avail, &rd)) { delete out; out = nullptr; break; }
                out->resize(old + rd);
                if (out->size() > 512 * 1024) break;   // sanity limit
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}

DWORD WINAPI Worker(LPVOID p) {
    Request* q = (Request*)p;
    std::string* body = HttpGet(q->host, q->path);
    if (!PostMessageW(g_main, WM_WEATHER, (WPARAM)q->job, (LPARAM)body)) delete body;
    delete q;
    return 0;
}

std::wstring FromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring UrlEncode(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    std::wstring o; wchar_t b[8];
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (wchar_t)c;
        else { swprintf(b, 8, L"%%%02X", c); o += b; }
    }
    return o;
}
long long NowUnix() {
    FILETIME f; GetSystemTimeAsFileTime(&f);
    return (long long)(((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL);
}
std::wstring CachePath() { return AppDataDir() + L"\\weather.json"; }
float NumAt(const JVal* a, size_t i, float def) {
    if (!a || !a->isArr() || i >= a->a.size() || a->a[i].type != JVal::Num) return def;
    return (float)a->a[i].n;
}
} // namespace

std::wstring WeatherText(int c) {
    switch (c) {
    case 0: return L"Clear";
    case 1: return L"Mainly clear";
    case 2: return L"Partly cloudy";
    case 3: return L"Overcast";
    case 45: case 48: return L"Fog";
    case 51: case 53: case 55: return L"Drizzle";
    case 56: case 57: return L"Freezing drizzle";
    case 61: return L"Light rain";
    case 63: return L"Rain";
    case 65: return L"Heavy rain";
    case 66: case 67: return L"Freezing rain";
    case 71: return L"Light snow";
    case 73: return L"Snow";
    case 75: return L"Heavy snow";
    case 77: return L"Snow grains";
    case 80: case 81: return L"Showers";
    case 82: return L"Heavy showers";
    case 85: case 86: return L"Snow showers";
    case 95: return L"Thunderstorm";
    case 96: case 99: return L"Thunderstorm, hail";
    }
    return L"—";
}

bool Weather::Fahrenheit() const {
    if (units >= 0) return units == 1;
    wchar_t m[4] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, m, 4);   // "1" = US system
    return m[0] == L'1';
}

void Weather::LoadSettings(const JVal* o) {
    if (!o || !o->isObj()) return;
    autoLoc = o->boolean(L"auto", true);
    place = o->str(L"place", L"");
    haveLoc = o->get(L"lat") != nullptr;
    lat = o->num(L"lat", 0); lon = o->num(L"lon", 0);
    locTime = (long long)o->num(L"locTime", 0);
    units = (int)o->num(L"units", -1);
}
void Weather::SaveSettings(JVal& o) const {
    o = JVal::MakeObj();
    o.set(L"auto") = JVal::MakeBool(autoLoc);
    o.set(L"place") = JVal::MakeStr(place);
    if (haveLoc) { o.set(L"lat") = JVal::MakeNum(std::round(lat * 10000) / 10000); o.set(L"lon") = JVal::MakeNum(std::round(lon * 10000) / 10000); }
    o.set(L"locTime") = JVal::MakeNum((double)locTime);
    o.set(L"units") = JVal::MakeNum(units);
}

void Weather::Schedule(UINT ms) { SetTimer(g_main, WEATHER_TIMER_ID, ms, nullptr); }

void Weather::Start(int job, const std::wstring& host, const std::wstring& path) {
    busy = true;
    Request* q = new Request{ job, host, path };
    HANDLE t = CreateThread(nullptr, 64 * 1024, Worker, q, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (t) CloseHandle(t);
    else { delete q; busy = false; status = L"Couldn't start the update"; }
}

void Weather::Ensure() {
    if (started_) return;
    started_ = true;
    LoadCache();
    Refresh(false);
}

void Weather::Refresh(bool force) {
    if (!started_ || busy) return;   // nothing happens until a Weather widget is actually shown
    long long now = NowUnix();
    if (!force && data.valid && data.fahrenheit == Fahrenheit() && now - data.fetched < 25 * 60) {
        Schedule((UINT)std::max<long long>(60, 30 * 60 - (now - data.fetched)) * 1000);
        return;
    }
    if (autoLoc && (!haveLoc || now - locTime > 24 * 3600 || force)) {
        status = data.valid ? L"" : L"Finding your location…";
        Start(J_LOCATE, L"get.geojs.io", L"/v1/ip/geo.json");
        WeatherChanged();
        return;
    }
    if (!haveLoc) { status = L"Click here to choose your city."; WeatherChanged(); return; }
    FetchForecast();
}

void Weather::FetchForecast() {
    wchar_t q[1024];
    bool f = Fahrenheit();
    swprintf(q, 1024,
             L"/v1/forecast?latitude=%.4f&longitude=%.4f"
             L"&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
             L"&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
             L"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             L"&timezone=auto&forecast_days=7&forecast_hours=26&timeformat=unixtime%ls",
             lat, lon, f ? L"&temperature_unit=fahrenheit&wind_speed_unit=mph" : L"");
    if (!data.valid) status = L"Getting the weather…";
    Start(J_FORECAST, L"api.open-meteo.com", q);
    WeatherChanged();
}

bool Weather::Parse(const std::wstring& json, WeatherData& d) {
    JVal j;
    if (!JsonParse(json, j) || !j.isObj()) return false;
    const JVal* cur = j.get(L"current");
    const JVal* hr = j.get(L"hourly");
    const JVal* dy = j.get(L"daily");
    if (!cur || !cur->isObj()) return false;
    d = WeatherData();
    d.utcOffset = (int)j.num(L"utc_offset_seconds", 0);
    d.temp = (float)cur->num(L"temperature_2m", 0);
    d.feels = (float)cur->num(L"apparent_temperature", d.temp);
    d.humidity = (int)cur->num(L"relative_humidity_2m", 0);
    d.code = (int)cur->num(L"weather_code", 0);
    d.day = cur->num(L"is_day", 1) != 0;
    d.wind = (float)cur->num(L"wind_speed_10m", 0);
    const JVal* tu = j.get(L"current_units");
    d.fahrenheit = tu && tu->str(L"temperature_2m", L"").find(L'F') != std::wstring::npos;
    if (hr && hr->isObj()) {
        const JVal *t = hr->get(L"time"), *tp = hr->get(L"temperature_2m"), *wc = hr->get(L"weather_code"), *pp = hr->get(L"precipitation_probability"), *dd = hr->get(L"is_day");
        if (t && t->isArr())
            for (size_t i = 0; i < t->a.size(); i++)
                d.hours.push_back({ (long long)t->a[i].n, NumAt(tp, i, 0), (int)NumAt(wc, i, 0), (int)NumAt(pp, i, -1), NumAt(dd, i, 1) != 0 });
    }
    if (dy && dy->isObj()) {
        const JVal *t = dy->get(L"time"), *wc = dy->get(L"weather_code"), *mx = dy->get(L"temperature_2m_max"), *mn = dy->get(L"temperature_2m_min"), *pp = dy->get(L"precipitation_probability_max");
        if (t && t->isArr())
            for (size_t i = 0; i < t->a.size(); i++)
                d.days.push_back({ (long long)t->a[i].n, (int)NumAt(wc, i, 0), NumAt(mx, i, 0), NumAt(mn, i, 0), (int)NumAt(pp, i, -1) });
    }
    d.valid = true;
    return true;
}

void Weather::LoadCache() {
    bool ok = false;
    std::wstring t = ReadTextFile(CachePath(), &ok);
    if (!ok) return;
    JVal c;
    if (!JsonParse(t, c) || !c.isObj()) return;
    // only reuse data for the same place
    if (std::fabs(c.num(L"lat", 999) - lat) > 0.05 || std::fabs(c.num(L"lon", 999) - lon) > 0.05) return;
    WeatherData d;
    if (Parse(c.str(L"raw", L""), d)) { d.fetched = (long long)c.num(L"fetched", 0); data = d; }
}
void Weather::SaveCache(const std::string& raw) {
    JVal c = JVal::MakeObj();
    c.set(L"fetched") = JVal::MakeNum((double)data.fetched);
    c.set(L"lat") = JVal::MakeNum(lat); c.set(L"lon") = JVal::MakeNum(lon);
    c.set(L"raw") = JVal::MakeStr(FromUtf8(raw));
    WriteTextFile(CachePath(), JsonWrite(c));
}

std::wstring Weather::UpdatedText() const {
    if (!data.valid) return L"Not updated yet";
    long long m = (NowUnix() - data.fetched) / 60;
    if (m < 1) return L"Updated just now";
    if (m < 60) return L"Updated " + std::to_wstring(m) + L" min ago";
    if (m < 48 * 60) return L"Updated " + std::to_wstring(m / 60) + L" h ago";
    return L"Updated " + std::to_wstring(m / 1440) + L" days ago";
}

void Weather::OnResult(WPARAM job, LPARAM lp) {
    std::string* body = (std::string*)lp;
    std::unique_ptr<std::string> holder(body);   // freed on every path
    std::wstring text = body ? FromUtf8(*body) : L"";
    busy = false;
    if (job == J_LOCATE || job == J_LOCATE2) {
        JVal j; bool ok = body && JsonParse(text, j) && j.isObj() && j.get(L"latitude") && j.get(L"longitude");
        if (ok && job == J_LOCATE2 && j.get(L"success") && !j.boolean(L"success", false)) ok = false;
        if (ok) {
            lat = j.num(L"latitude", 0); lon = j.num(L"longitude", 0);
            std::wstring city = j.str(L"city", L"");
            place = city.empty() ? j.str(L"region", L"Your area") : city;
            haveLoc = true; locTime = NowUnix();
            RequestSave();
            FetchForecast();
        } else if (job == J_LOCATE) {
            Start(J_LOCATE2, L"ipwho.is", L"/");                  // second provider
        } else {
            status = haveLoc ? L"" : L"Couldn't detect your location. Click here to choose a city.";
            if (haveLoc) FetchForecast(); else WeatherChanged();
        }
    } else if (job == J_FORECAST) {
        WeatherData d;
        if (body && Parse(text, d)) {
            d.fetched = NowUnix();
            data = d;
            SaveCache(*body);
            status.clear();
            failures_ = 0;
            Schedule(30 * 60 * 1000);
        } else {
            failures_++;
            status = data.valid ? L"" : L"Can't reach the weather service. Click to try again.";
            Schedule((UINT)std::min(30, 5 * failures_) * 60 * 1000);   // retry: 5, 10, 15 ... 30 min
        }
        WeatherChanged();
    } else if (job == J_GEOCODE) {
        JVal j;
        std::vector<std::pair<std::wstring, std::pair<double, double>>> res;
        std::vector<std::wstring> names;
        if (body && JsonParse(text, j)) {
            const JVal* r = j.get(L"results");
            if (r && r->isArr())
                for (auto& e : r->a) {
                    std::wstring nm = e.str(L"name", L""), a1 = e.str(L"admin1", L""), co = e.str(L"country", L"");
                    std::wstring full = nm; if (!a1.empty() && a1 != nm) full += L", " + a1; if (!co.empty()) full += L", " + co;
                    res.push_back({ full, { e.num(L"latitude", 0), e.num(L"longitude", 0) } });
                    names.push_back(nm);
                }
        }
        if (res.empty()) {
            MessageBoxW(nullptr, body ? L"No places found with that name. Try a nearby bigger city." : L"Couldn't search for places. Check your internet connection.",
                        L"Weather location", MB_OK | MB_ICONINFORMATION);
            WeatherChanged();
            return;
        }
        int pick = 0;
        if (res.size() > 1) {   // let the user choose between places with the same name
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Choose a place:");
            for (size_t i = 0; i < res.size(); i++) AppendMenuW(m, MF_STRING, 100 + i, res[i].first.c_str());
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(g_main);
            int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
            DestroyMenu(m);
            PostMessageW(g_main, WM_NULL, 0, 0);
            if (!id) return;
            pick = id - 100;
        }
        autoLoc = false;
        place = names[pick];
        lat = res[pick].second.first; lon = res[pick].second.second; haveLoc = true; locTime = NowUnix();
        data.valid = false;           // different place: don't show the old weather
        RequestSave();
        FetchForecast();
    }
}

void Weather::AskCity() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    float s = ScaleOf(hm);
    RECT wa = WorkOf(hm);
    int w = (int)(320 * s), h = (int)(44 * s);
    int x = std::max((int)wa.left + 8, std::min((int)pt.x - w / 2, (int)wa.right - w - 8));
    int y = std::max((int)wa.top + 8, std::min((int)pt.y - h / 2, (int)wa.bottom - h - 8));
    ShowInputAt(RECT{ x, y, x + w, y + h }, s, autoLoc ? L"" : place, L"Type a city, e.g. Pune, then Enter", [this](const std::wstring& city) {
        if (city.empty()) return;
        Start(J_GEOCODE, L"geocoding-api.open-meteo.com", L"/v1/search?count=6&language=en&format=json&name=" + UrlEncode(city));
    });
}

void Weather::SetAuto(bool on) {
    autoLoc = on;
    if (on) { locTime = 0; data.valid = false; }
    RequestSave();
    Refresh(true);
}
void Weather::SetUnits(int u) {
    units = u;
    RequestSave();
    if (data.valid && data.fahrenheit != Fahrenheit()) Refresh(true);
    WeatherChanged();
}

void Weather::Shutdown() { KillTimer(g_main, WEATHER_TIMER_ID); }
