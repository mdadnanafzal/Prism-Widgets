#include "events.h"
#include "app.h"
#include "net.h"
#include <algorithm>
#include <map>
#include <set>

Events g_events;

// ------------------------------------------------------------------ dates
static long long DayIndex(int y, int m, int d) {
    SYSTEMTIME s{}; s.wYear = (WORD)y; s.wMonth = (WORD)m; s.wDay = (WORD)d; FILETIME f;
    if (!SystemTimeToFileTime(&s, &f)) return 0;
    return (long long)((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 864000000000ULL);
}
static SYSTEMTIME FromIndex(long long idx) {
    ULONGLONG v = (ULONGLONG)idx * 864000000000ULL; FILETIME f{ (DWORD)v, (DWORD)(v >> 32) }; SYSTEMTIME s{}; FileTimeToSystemTime(&f, &s); return s;
}
int DayKey(int y, int m, int d) { return y * 10000 + m * 100 + d; }
void SplitDay(int day, int& y, int& m, int& d) { y = day / 10000; m = (day / 100) % 100; d = day % 100; }
static long long IndexOf(int day) { int y, m, d; SplitDay(day, y, m, d); return DayIndex(y, m, d); }
static int KeyOf(long long idx) { SYSTEMTIME s = FromIndex(idx); return DayKey(s.wYear, s.wMonth, s.wDay); }
int TodayKey() { SYSTEMTIME s; GetLocalTime(&s); return DayKey(s.wYear, s.wMonth, s.wDay); }
int AddDays(int day, int n) { return KeyOf(IndexOf(day) + n); }
static int DaysInMonth(int y, int m) { static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1]; }
static int Weekday(long long idx) { return (int)((idx + 0) % 7 + 7) % 7; }   // 0 = Monday (1601-01-01 was a Monday)

D2D1_COLOR_F EventColor(unsigned char src, D2D1_COLOR_F accent) {
    if (src == EV_HOLIDAY) return D2D1_COLOR_F{ 1.f, .38f, .36f, 1 };
    if (src == EV_SYNC) return D2D1_COLOR_F{ .22f, .80f, .74f, 1 };
    return accent;
}

// ------------------------------------------------------------------ iCalendar (.ics) parsing
namespace {
struct IcsTime { bool ok = false, date = false; int day = 0; short h = -1, mi = 0; };
IcsTime ParseIcsTime(const std::wstring& v, bool dateParam) {
    IcsTime t;
    if (v.size() < 8) return t;
    int y = _wtoi(v.substr(0, 4).c_str()), m = _wtoi(v.substr(4, 2).c_str()), d = _wtoi(v.substr(6, 2).c_str());
    if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) return t;
    t.ok = true;
    if (dateParam || v.size() < 15 || v[8] != L'T') { t.date = true; t.day = DayKey(y, m, d); return t; }
    int hh = _wtoi(v.substr(9, 2).c_str()), mm = _wtoi(v.substr(11, 2).c_str());
    if (v.back() == L'Z') {   // UTC -> local
        SYSTEMTIME u{}; u.wYear = (WORD)y; u.wMonth = (WORD)m; u.wDay = (WORD)d; u.wHour = (WORD)hh; u.wMinute = (WORD)mm;
        SYSTEMTIME l{};
        if (SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) { y = l.wYear; m = l.wMonth; d = l.wDay; hh = l.wHour; mm = l.wMinute; }
    }
    // TZID / floating times are treated as local time (calendars are almost always in the user's zone)
    t.day = DayKey(y, m, d); t.h = (short)hh; t.mi = (short)mm;
    return t;
}
std::wstring Unescape(const std::wstring& s) {
    std::wstring o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[++i];
            o += (c == L'n' || c == L'N') ? L' ' : c;
        } else o += s[i];
    }
    return o;
}
struct RawEvent {
    std::wstring uid, summary, rrule, status;
    IcsTime start, end, recurId;
    std::vector<int> exdates;
};
int WeekdayCode(const std::wstring& s) {   // "MO".."SU" -> 0..6
    static const wchar_t* n[] = { L"MO", L"TU", L"WE", L"TH", L"FR", L"SA", L"SU" };
    for (int i = 0; i < 7; i++) if (s == n[i]) return i;
    return -1;
}
void Emit(std::vector<Occ>& out, int day, const RawEvent& e, unsigned char src, int fromDay, int toDay) {
    if (day < fromDay || day > toDay) return;
    Occ o; o.day = day; o.src = src; o.title = e.summary;
    if (!e.start.date) { o.sh = e.start.h; o.sm = e.start.mi; }
    out.push_back(std::move(o));
}
void EmitSpan(std::vector<Occ>& out, long long startIdx, const RawEvent& e, unsigned char src, int fromDay, int toDay) {
    int span = 1;   // multi-day all-day events cover every day (DTEND is exclusive)
    if (e.start.date && e.end.ok && e.end.date) span = (int)std::max(1LL, std::min(31LL, IndexOf(e.end.day) - IndexOf(e.start.day)));
    for (int i = 0; i < span; i++) Emit(out, KeyOf(startIdx + i), e, src, fromDay, toDay);
}
void Expand(const RawEvent& e, const std::set<int>& skip, unsigned char src, int fromDay, int toDay, std::vector<Occ>& out) {
    long long s0 = IndexOf(e.start.day);
    if (e.rrule.empty()) { EmitSpan(out, s0, e, src, fromDay, toDay); return; }
    std::wstring freq; int interval = 1, count = -1, until = 99991231;
    std::vector<std::pair<int, int>> byday;   // (ordinal, weekday)
    size_t p = 0;
    while (p < e.rrule.size()) {
        size_t q = e.rrule.find(L';', p); if (q == std::wstring::npos) q = e.rrule.size();
        std::wstring kv = e.rrule.substr(p, q - p); p = q + 1;
        size_t eq = kv.find(L'='); if (eq == std::wstring::npos) continue;
        std::wstring k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == L"FREQ") freq = v;
        else if (k == L"INTERVAL") interval = std::max(1, _wtoi(v.c_str()));
        else if (k == L"COUNT") count = _wtoi(v.c_str());
        else if (k == L"UNTIL") { IcsTime u = ParseIcsTime(v, false); if (u.ok) until = u.day; }
        else if (k == L"BYDAY") {
            size_t a = 0;
            while (a < v.size()) {
                size_t b = v.find(L',', a); if (b == std::wstring::npos) b = v.size();
                std::wstring tok = v.substr(a, b - a); a = b + 1;
                if (tok.size() < 2) continue;
                int wd = WeekdayCode(tok.substr(tok.size() - 2));
                int ord = tok.size() > 2 ? _wtoi(tok.substr(0, tok.size() - 2).c_str()) : 0;
                if (wd >= 0) byday.push_back({ ord, wd });
            }
        }
    }
    int sy, sm, sd; SplitDay(e.start.day, sy, sm, sd);
    int n = 0, guard = 0;
    auto take = [&](long long idx) -> bool {   // returns false when we're done
        int key = KeyOf(idx);
        if (key > until || key > toDay) return false;
        if (count >= 0 && n >= count) return false;
        n++;
        if (!skip.count(key)) EmitSpan(out, idx, e, src, fromDay, toDay);
        return true;
    };
    if (freq == L"DAILY") {
        for (long long i = s0; guard++ < 4000; i += interval) if (!take(i)) break;
    } else if (freq == L"WEEKLY") {
        long long wk0 = s0 - Weekday(s0);
        std::vector<int> days;
        for (auto& b : byday) days.push_back(b.second);
        if (days.empty()) days.push_back(Weekday(s0));
        std::sort(days.begin(), days.end());
        bool go = true;
        for (long long wk = wk0; go && guard++ < 1500; wk += 7LL * interval)
            for (int wd : days) { long long i = wk + wd; if (i < s0) continue; if (!take(i)) { go = false; break; } }
    } else if (freq == L"MONTHLY") {
        bool go = true;
        for (int k = 0; go && guard++ < 600; k += interval) {
            int y = sy + (sm - 1 + k) / 12, m = (sm - 1 + k) % 12 + 1;
            std::vector<long long> c;
            if (!byday.empty()) {
                for (auto& b : byday) {   // e.g. 2MO = second Monday, -1FR = last Friday
                    long long first = DayIndex(y, m, 1), last = DayIndex(y, m, DaysInMonth(y, m));
                    if (b.first > 0) { long long i = first + ((b.second - Weekday(first) + 7) % 7) + 7LL * (b.first - 1); if (i <= last) c.push_back(i); }
                    else if (b.first < 0) { long long i = last - ((Weekday(last) - b.second + 7) % 7) + 7LL * (b.first + 1); if (i >= first) c.push_back(i); }
                }
            } else if (sd <= DaysInMonth(y, m)) c.push_back(DayIndex(y, m, sd));
            std::sort(c.begin(), c.end());
            for (long long i : c) { if (i < s0) continue; if (!take(i)) { go = false; break; } }
        }
    } else if (freq == L"YEARLY") {
        for (int y = sy; guard++ < 200; y += interval) {
            if (sm == 2 && sd == 29 && DaysInMonth(y, 2) < 29) continue;
            if (!take(DayIndex(y, sm, sd))) break;
        }
    } else EmitSpan(out, s0, e, src, fromDay, toDay);
}
} // namespace

void ParseIcs(const std::wstring& ics, unsigned char src, int fromDay, int toDay, std::vector<Occ>& out) {
    // unfold lines
    std::vector<std::wstring> lines;
    size_t p = 0;
    while (p < ics.size()) {
        size_t q = ics.find(L'\n', p); if (q == std::wstring::npos) q = ics.size();
        std::wstring ln = ics.substr(p, q - p); p = q + 1;
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (!ln.empty() && (ln[0] == L' ' || ln[0] == L'\t') && !lines.empty()) lines.back() += ln.substr(1);
        else lines.push_back(std::move(ln));
    }
    std::vector<RawEvent> evs;
    RawEvent cur; bool in = false;
    for (auto& ln : lines) {
        if (ln == L"BEGIN:VEVENT") { in = true; cur = RawEvent(); continue; }
        if (ln == L"END:VEVENT") { if (in && cur.start.ok) evs.push_back(cur); in = false; continue; }
        if (!in) continue;
        // NAME;PARAMS:VALUE   (a ':' inside a quoted parameter doesn't end the name)
        size_t colon = std::wstring::npos; bool quote = false;
        for (size_t i = 0; i < ln.size(); i++) { if (ln[i] == L'"') quote = !quote; else if (ln[i] == L':' && !quote) { colon = i; break; } }
        if (colon == std::wstring::npos) continue;
        std::wstring head = ln.substr(0, colon), val = ln.substr(colon + 1);
        size_t semi = head.find(L';');
        std::wstring name = head.substr(0, semi), params = semi == std::wstring::npos ? L"" : head.substr(semi);
        bool isDate = params.find(L"VALUE=DATE") != std::wstring::npos && params.find(L"VALUE=DATE-TIME") == std::wstring::npos;
        if (name == L"SUMMARY") cur.summary = Unescape(val);
        else if (name == L"UID") cur.uid = val;
        else if (name == L"DTSTART") cur.start = ParseIcsTime(val, isDate);
        else if (name == L"DTEND") cur.end = ParseIcsTime(val, isDate);
        else if (name == L"RRULE") cur.rrule = val;
        else if (name == L"STATUS") cur.status = val;
        else if (name == L"RECURRENCE-ID") cur.recurId = ParseIcsTime(val, isDate);
        else if (name == L"EXDATE") {
            size_t a = 0;
            while (a < val.size()) {
                size_t b = val.find(L',', a); if (b == std::wstring::npos) b = val.size();
                IcsTime x = ParseIcsTime(val.substr(a, b - a), isDate); if (x.ok) cur.exdates.push_back(x.day);
                a = b + 1;
            }
        }
    }
    // moved / edited instances replace the master's occurrence on their original date
    std::map<std::wstring, std::set<int>> overridden;
    for (auto& e : evs) if (e.recurId.ok && !e.uid.empty()) overridden[e.uid].insert(e.recurId.day);
    for (auto& e : evs) {
        if (e.status == L"CANCELLED") continue;
        if (e.summary.empty()) continue;
        std::set<int> skip(e.exdates.begin(), e.exdates.end());
        if (!e.recurId.ok) { auto it = overridden.find(e.uid); if (it != overridden.end()) skip.insert(it->second.begin(), it->second.end()); }
        RawEvent x = e;
        if (e.recurId.ok) x.rrule.clear();
        Expand(x, skip, src, fromDay, toDay, out);
        if (out.size() > 20000) break;   // sanity limit
    }
}

// ------------------------------------------------------------------ regions (Google public holiday calendars)
namespace {
struct Region { const wchar_t* iso; const wchar_t* name; const wchar_t* gid; };
const Region kRegions[] = {
    { L"IN", L"India", L"indian" }, { L"US", L"United States", L"usa" }, { L"GB", L"United Kingdom", L"uk" }, { L"CA", L"Canada", L"canadian" },
    { L"AU", L"Australia", L"australian" }, { L"NZ", L"New Zealand", L"new_zealand" }, { L"IE", L"Ireland", L"irish" }, { L"DE", L"Germany", L"german" },
    { L"FR", L"France", L"french" }, { L"IT", L"Italy", L"italian" }, { L"ES", L"Spain", L"spain" }, { L"NL", L"Netherlands", L"dutch" },
    { L"BE", L"Belgium", L"be" }, { L"CH", L"Switzerland", L"ch" }, { L"AT", L"Austria", L"austrian" }, { L"PT", L"Portugal", L"portuguese" },
    { L"SE", L"Sweden", L"swedish" }, { L"NO", L"Norway", L"norwegian" }, { L"DK", L"Denmark", L"danish" }, { L"FI", L"Finland", L"finnish" },
    { L"PL", L"Poland", L"polish" }, { L"GR", L"Greece", L"greek" }, { L"TR", L"Turkey", L"turkish" }, { L"RU", L"Russia", L"russian" },
    { L"AE", L"United Arab Emirates", L"ae" }, { L"SA", L"Saudi Arabia", L"saudiarabian" }, { L"PK", L"Pakistan", L"pk" }, { L"BD", L"Bangladesh", L"bd" },
    { L"LK", L"Sri Lanka", L"lk" }, { L"NP", L"Nepal", L"np" }, { L"SG", L"Singapore", L"singapore" }, { L"MY", L"Malaysia", L"malaysia" },
    { L"ID", L"Indonesia", L"indonesian" }, { L"PH", L"Philippines", L"philippines" }, { L"TH", L"Thailand", L"th" }, { L"VN", L"Vietnam", L"vietnamese" },
    { L"CN", L"China", L"china" }, { L"HK", L"Hong Kong", L"hong_kong" }, { L"TW", L"Taiwan", L"taiwan" }, { L"JP", L"Japan", L"japanese" },
    { L"KR", L"South Korea", L"south_korea" }, { L"ZA", L"South Africa", L"sa" }, { L"NG", L"Nigeria", L"ng" }, { L"KE", L"Kenya", L"ke" },
    { L"EG", L"Egypt", L"eg" }, { L"BR", L"Brazil", L"brazilian" }, { L"MX", L"Mexico", L"mexican" }, { L"AR", L"Argentina", L"ar" },
    { L"CL", L"Chile", L"cl" }, { L"CO", L"Colombia", L"co" },
};
const Region* FindRegion(const std::wstring& iso) { for (auto& r : kRegions) if (iso == r.iso) return &r; return nullptr; }
std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
long long NowUnix() { FILETIME f; GetSystemTimeAsFileTime(&f); return (long long)(((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL); }
long long FileAgeTime(const std::wstring& path) {   // unix time of last write, 0 if missing
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    ULONGLONG v = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return (long long)((v - 116444736000000000ULL) / 10000000ULL);
}
std::wstring HolPath(const std::wstring& code) { return AppDataDir() + L"\\holidays_" + code + L".ics"; }
std::wstring SyncPath() { return AppDataDir() + L"\\calendar_sync.ics"; }
std::wstring MinePath() { return AppDataDir() + L"\\events.json"; }
int WindowFrom() { return AddDays(TodayKey(), -62); }
int WindowTo() { return AddDays(TodayKey(), 400); }
void SaveRaw(const std::wstring& path, const std::string& raw) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(f, raw.data(), (DWORD)raw.size(), &wr, nullptr); CloseHandle(f);
}
std::wstring Ago(long long t) {
    if (!t) return L"never";
    long long m = (NowUnix() - t) / 60;
    if (m < 1) return L"just now";
    if (m < 60) return std::to_wstring(m) + L" min ago";
    if (m < 48 * 60) return std::to_wstring(m / 60) + L" h ago";
    return std::to_wstring(m / 1440) + L" days ago";
}
} // namespace

std::wstring Events::RegionCode() const {
    if (!region.empty()) return region;
    wchar_t g[16] = {};
    if (GetUserDefaultGeoName(g, 16) > 0 && wcslen(g) == 2) return g;
    return L"IN";
}
std::wstring Events::RegionName() const { const Region* r = FindRegion(RegionCode()); return r ? r->name : RegionCode(); }

// ------------------------------------------------------------------ settings + storage
void Events::LoadSettings(const JVal* o) {
    if (!o || !o->isObj()) return;
    enabled = o->boolean(L"enabled", true);
    holidays = o->boolean(L"holidays", true);
    mine = o->boolean(L"mine", true);
    region = o->str(L"region", L"");
    icsUrl = o->str(L"ics", L"");
}
void Events::SaveSettings(JVal& o) const {
    o = JVal::MakeObj();
    o.set(L"enabled") = JVal::MakeBool(enabled);
    o.set(L"holidays") = JVal::MakeBool(holidays);
    o.set(L"mine") = JVal::MakeBool(mine);
    o.set(L"region") = JVal::MakeStr(region);
    o.set(L"ics") = JVal::MakeStr(icsUrl);
}
void Events::LoadMine() {
    my.clear();
    JVal j;
    if (!JsonParse(ReadTextFile(MinePath()), j)) return;
    const JVal* a = j.get(L"events");
    if (!a || !a->isArr()) return;
    for (auto& e : a->a) {
        MyEvent m;
        m.id = e.str(L"id", L""); m.title = e.str(L"title", L"");
        std::wstring d = e.str(L"date", L""), t = e.str(L"time", L"");
        if (swscanf(d.c_str(), L"%d-%d-%d", &m.y, &m.m, &m.d) != 3 || m.title.empty()) continue;
        int hh = -1, mm = 0;
        if (!t.empty() && swscanf(t.c_str(), L"%d:%d", &hh, &mm) == 2) { m.sh = (short)hh; m.sm = (short)mm; }
        m.yearly = e.boolean(L"yearly", false);
        my.push_back(m);
    }
}
void Events::SaveMine() const {
    JVal a = JVal::MakeArr();
    for (auto& m : my) {
        JVal e = JVal::MakeObj();
        wchar_t d[16], t[8];
        swprintf(d, 16, L"%04d-%02d-%02d", m.y, m.m, m.d);
        e.set(L"id") = JVal::MakeStr(m.id); e.set(L"title") = JVal::MakeStr(m.title); e.set(L"date") = JVal::MakeStr(d);
        if (m.sh >= 0) { swprintf(t, 8, L"%02d:%02d", m.sh, m.sm); e.set(L"time") = JVal::MakeStr(t); }
        e.set(L"yearly") = JVal::MakeBool(m.yearly);
        a.a.push_back(e);
    }
    JVal root = JVal::MakeObj(); root.set(L"events") = a;
    WriteTextFile(MinePath(), JsonWrite(root));
}

// ------------------------------------------------------------------ occurrence index
void Events::Rebuild() {
    occ_.clear();
    int from = WindowFrom(), to = WindowTo();
    if (holidays) occ_.insert(occ_.end(), holOcc_.begin(), holOcc_.end());
    if (!icsUrl.empty()) occ_.insert(occ_.end(), syncOcc_.begin(), syncOcc_.end());
    if (mine) {
        for (int i = 0; i < (int)my.size(); i++) {
            const MyEvent& m = my[i];
            auto add = [&](int y) {
                if (m.m == 2 && m.d == 29 && DaysInMonth(y, 2) < 29) return;
                int k = DayKey(y, m.m, m.d);
                if (k < from || k > to) return;
                Occ o; o.day = k; o.sh = m.sh; o.sm = m.sm; o.src = EV_MINE; o.mine = i; o.title = m.title;
                occ_.push_back(std::move(o));
            };
            if (m.yearly) { int y0 = from / 10000, y1 = to / 10000; for (int y = std::max(m.y, y0); y <= y1; y++) add(y); }
            else add(m.y);
        }
    }
    std::stable_sort(occ_.begin(), occ_.end(), [](const Occ& a, const Occ& b) {
        if (a.day != b.day) return a.day < b.day;
        int ta = a.sh < 0 ? -1 : a.sh * 60 + a.sm, tb = b.sh < 0 ? -1 : b.sh * 60 + b.sm;
        if (ta != tb) return ta < tb;
        return a.src < b.src;
    });
}
static std::pair<std::vector<Occ>::const_iterator, std::vector<Occ>::const_iterator> RangeOf(const std::vector<Occ>& v, int day) {
    auto lo = std::lower_bound(v.begin(), v.end(), day, [](const Occ& o, int d) { return o.day < d; });
    auto hi = std::upper_bound(lo, v.end(), day, [](int d, const Occ& o) { return d < o.day; });
    return { lo, hi };
}
int Events::Mask(int day) const {
    if (!enabled) return 0;
    int m = 0; auto r = RangeOf(occ_, day);
    for (auto it = r.first; it != r.second; ++it) m |= 1 << it->src;
    return m;
}
bool Events::IsHoliday(int day) const { return (Mask(day) & (1 << EV_HOLIDAY)) != 0; }
void Events::ForDay(int day, std::vector<const Occ*>& out) const {
    out.clear(); if (!enabled) return;
    auto r = RangeOf(occ_, day);
    for (auto it = r.first; it != r.second; ++it) out.push_back(&*it);
}
void Events::Upcoming(int fromDay, int days, size_t maxN, std::vector<const Occ*>& out) const {
    out.clear(); if (!enabled) return;
    int to = AddDays(fromDay, days);
    auto it = std::lower_bound(occ_.begin(), occ_.end(), fromDay, [](const Occ& o, int d) { return o.day < d; });
    for (; it != occ_.end() && it->day <= to && out.size() < maxN; ++it) out.push_back(&*it);
}

// ------------------------------------------------------------------ loading + refreshing
void Events::Ensure() {
    if (started_ || !enabled) return;
    started_ = true;
    LoadMine();
    int from = WindowFrom(), to = WindowTo();
    std::wstring hp = HolPath(RegionCode());
    if (FileExists(hp)) {
        ParseIcs(ReadTextFile(hp), EV_HOLIDAY, from, to, holOcc_);
        holTime = FileAgeTime(hp); holCount = (int)holOcc_.size();
    }
    if (!icsUrl.empty() && FileExists(SyncPath())) {
        std::wstring t = ReadTextFile(SyncPath());
        ParseIcs(t, EV_SYNC, from, to, syncOcc_);
        syncTime = FileAgeTime(SyncPath()); syncCount = (int)syncOcc_.size();
    }
    Rebuild();
    Refresh(false);
    SetTimer(g_main, EVENTS_TIMER_ID, 30 * 60 * 1000, nullptr);
}

void Events::Refresh(bool force) {
    if (!started_ || !enabled) return;
    long long now = NowUnix();
    if (holidays && !busyHol_ && (force || !holTime || now - holTime > 7 * 24 * 3600)) FetchHolidays(false);
    if (!icsUrl.empty() && !busySync_ && (force || !syncTime || now - syncTime > 30 * 60)) FetchSync();
}

void Events::FetchHolidays(bool alt) {
    std::wstring code = RegionCode();
    const Region* r = FindRegion(code);
    std::wstring gid = (!alt && r) ? r->gid : Lower(code);   // fallback: ISO-style calendar id
    std::wstring url = L"https://calendar.google.com/calendar/ical/en." + gid + L"%23holiday%40group.v.calendar.google.com/public/basic.ics";
    busyHol_ = true; holStatus = L"Updating…";
    HttpGetAsync(url, [this, code, alt, r](std::string* body) {
        busyHol_ = false;
        if (code != RegionCode()) return;   // region changed meanwhile
        if (!body || body->find("BEGIN:VCALENDAR") == std::string::npos) {
            if (!alt && r && Lower(code) != r->gid) { FetchHolidays(true); return; }
            holStatus = L"Couldn't load holidays";
            EventsChanged();
            return;
        }
        SaveRaw(HolPath(code), *body);
        holOcc_.clear();
        ParseIcs(Utf8ToWide(*body), EV_HOLIDAY, WindowFrom(), WindowTo(), holOcc_);
        holTime = NowUnix(); holCount = (int)holOcc_.size(); holStatus.clear();
        Rebuild();
        EventsChanged();
    }, 4u << 20);
}

void Events::FetchSync() {
    std::wstring url = icsUrl;
    busySync_ = true; syncStatus = L"Syncing…";
    HttpGetAsync(url, [this, url](std::string* body) {
        busySync_ = false;
        if (url != icsUrl) return;
        if (!body || body->find("BEGIN:VCALENDAR") == std::string::npos) {
            syncStatus = body ? L"That link isn't an iCal calendar" : L"Couldn't reach the calendar";
            EventsChanged();
            return;
        }
        SaveRaw(SyncPath(), *body);
        syncOcc_.clear();
        ParseIcs(Utf8ToWide(*body), EV_SYNC, WindowFrom(), WindowTo(), syncOcc_);
        syncTime = NowUnix(); syncCount = (int)syncOcc_.size(); syncStatus.clear();
        Rebuild();
        EventsChanged();
    });
}

std::wstring Events::HolidaySummary() const {
    if (!holStatus.empty()) return holStatus;
    return RegionName() + (holTime ? L" · updated " + Ago(holTime) : L"");
}
std::wstring Events::SyncSummary() const {
    if (icsUrl.empty()) return L"Not connected. Paste the secret iCal (.ics) link of a Google or Outlook calendar.";
    if (!syncStatus.empty()) return syncStatus;
    return L"Connected · " + std::to_wstring(syncCount) + L" events · synced " + Ago(syncTime);
}

// ------------------------------------------------------------------ own events
namespace {
// "18:30 Dinner", "6pm Gym", "6:15 pm Call", "Mom's birthday" -> time + title
void ParseEventText(const std::wstring& in, short& sh, short& sm, std::wstring& title) {
    std::wstring s = in; sh = -1; sm = 0;
    while (!s.empty() && iswspace(s[0])) s.erase(0, 1);
    int h = -1, m = 0, used = 0;
    if (swscanf(s.c_str(), L"%d:%d%n", &h, &m, &used) == 2 || (swscanf(s.c_str(), L"%d%n", &h, &used) == 1 && used <= 2)) {
        std::wstring rest = s.substr(used);
        size_t k = 0; while (k < rest.size() && rest[k] == L' ') k++;
        std::wstring ap = Lower(rest.substr(k, 2));
        bool hasAp = ap == L"am" || ap == L"pm";
        bool colon = s.find(L':') != std::wstring::npos && s.find(L':') < (size_t)used;
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59 && (colon || hasAp)) {
            if (hasAp) { if (h == 12) h = 0; if (ap == L"pm") h += 12; rest = rest.substr(k + 2); }
            sh = (short)h; sm = (short)m; s = rest;
        }
    }
    while (!s.empty() && (iswspace(s[0]) || s[0] == L'-' || s[0] == L'–')) s.erase(0, 1);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    title = s;
}
}
void Events::AddFromText(int day, const std::wstring& text) {
    MyEvent e; ParseEventText(text, e.sh, e.sm, e.title);
    if (e.title.empty()) return;
    SplitDay(day, e.y, e.m, e.d);
    std::wstring l = Lower(e.title);
    e.yearly = l.find(L"birthday") != std::wstring::npos || l.find(L"anniversary") != std::wstring::npos;
    e.id = std::to_wstring(NowUnix()) + L"-" + std::to_wstring(GetTickCount() % 100000);
    my.push_back(e);
    SaveMine(); Rebuild(); EventsChanged();
}
void Events::EditFromText(int idx, const std::wstring& text) {
    if (idx < 0 || idx >= (int)my.size()) return;
    short sh, sm; std::wstring t; ParseEventText(text, sh, sm, t);
    if (t.empty()) return;
    my[idx].title = t; my[idx].sh = sh; my[idx].sm = sm;
    SaveMine(); Rebuild(); EventsChanged();
}
void Events::Delete(int idx) { if (idx < 0 || idx >= (int)my.size()) return; my.erase(my.begin() + idx); SaveMine(); Rebuild(); EventsChanged(); }
void Events::ToggleYearly(int idx) { if (idx < 0 || idx >= (int)my.size()) return; my[idx].yearly = !my[idx].yearly; SaveMine(); Rebuild(); EventsChanged(); }
std::wstring Events::TextOf(int idx) const {
    if (idx < 0 || idx >= (int)my.size()) return L"";
    const MyEvent& m = my[idx];
    if (m.sh < 0) return m.title;
    wchar_t b[16]; swprintf(b, 16, L"%02d:%02d ", m.sh, m.sm);
    return b + m.title;
}

// ------------------------------------------------------------------ settings actions
void Events::SetEnabled(bool on) { enabled = on; RequestSave(); if (on) { Ensure(); Rebuild(); } EventsChanged(); }
void Events::SetHolidays(bool on) { holidays = on; RequestSave(); Rebuild(); if (on) Refresh(false); EventsChanged(); }
void Events::SetMine(bool on) { mine = on; RequestSave(); Rebuild(); EventsChanged(); }
void Events::SetRegion(const std::wstring& iso) {
    region = iso; RequestSave();
    holOcc_.clear(); holTime = 0; holCount = 0; holStatus.clear();
    std::wstring hp = HolPath(RegionCode());
    if (FileExists(hp)) {   // cached from before
        std::wstring t = ReadTextFile(hp);
        ParseIcs(t, EV_HOLIDAY, WindowFrom(), WindowTo(), holOcc_);
        holTime = FileAgeTime(hp); holCount = (int)holOcc_.size();
    }
    Rebuild(); Refresh(false); EventsChanged();
}
void Events::SetIcsUrl(const std::wstring& urlIn) {
    std::wstring url = urlIn;
    while (!url.empty() && iswspace(url.back())) url.pop_back();
    while (!url.empty() && iswspace(url[0])) url.erase(0, 1);
    if (!url.empty() && url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    icsUrl = url; RequestSave();
    syncOcc_.clear(); syncTime = 0; syncCount = 0; syncStatus.clear();
    DeleteFileW(SyncPath().c_str());
    Rebuild(); Refresh(true); EventsChanged();
}
void Events::ChooseRegion() {
    HMENU m = CreatePopupMenu();
    std::wstring cur = RegionCode();
    AppendMenuW(m, MF_STRING | (region.empty() ? MF_CHECKED : 0), 1, (L"Automatic (" + RegionName() + L")").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::vector<const Region*> sorted; for (auto& r : kRegions) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [](const Region* a, const Region* b) { return wcscmp(a->name, b->name) < 0; });
    for (size_t i = 0; i < sorted.size(); i++)
        AppendMenuW(m, MF_STRING | (!region.empty() && cur == sorted[i]->iso ? MF_CHECKED : 0) | ((i % 25 == 0 && i) ? MF_MENUBARBREAK : 0), 100 + i, sorted[i]->name);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    PostMessageW(g_main, WM_NULL, 0, 0);
    if (id == 1) SetRegion(L"");
    else if (id >= 100) SetRegion(sorted[id - 100]->iso);
}
void Events::AskIcsUrl() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    float s = ScaleOf(hm); RECT wa = WorkOf(hm);
    int w = (int)(460 * s), h = (int)(44 * s);
    int x = std::max((int)wa.left + 8, std::min((int)pt.x - w / 2, (int)wa.right - w - 8));
    int y = std::max((int)wa.top + 8, std::min((int)pt.y - h / 2, (int)wa.bottom - h - 8));
    ShowInputAt(RECT{ x, y, x + w, y + h }, s, icsUrl, L"Paste the secret iCal link (….ics), then Enter. Empty = disconnect", [this](const std::wstring& u) { SetIcsUrl(u); });
}
