#pragma once
// Calendar events: public holidays (Google public holiday calendars), the user's own events,
// and an optional synced Google / Outlook calendar via its iCal (.ics) link.
#include "common.h"
#include <functional>

#define EVENTS_TIMER_ID 51
enum EvSource : unsigned char { EV_HOLIDAY = 0, EV_MINE = 1, EV_SYNC = 2 };

struct Occ {                 // one occurrence on one day
    int day = 0;             // yyyymmdd
    short sh = -1, sm = 0;   // local start time, sh = -1 means all day
    unsigned char src = EV_MINE;
    int mine = -1;           // index into Events::my for own events
    std::wstring title;
};
struct MyEvent { std::wstring id, title; int y = 0, m = 0, d = 0; short sh = -1, sm = 0; bool yearly = false; };

int  DayKey(int y, int m, int d);
int  TodayKey();
int  AddDays(int day, int n);
void SplitDay(int day, int& y, int& m, int& d);
D2D1_COLOR_F EventColor(unsigned char src, D2D1_COLOR_F accent);
void ParseIcs(const std::wstring& ics, unsigned char src, int fromDay, int toDay, std::vector<Occ>& out);

class Events {
public:
    bool enabled = true, holidays = true, mine = true;
    std::wstring region;                 // ISO-2 code; empty = Windows region
    std::wstring icsUrl;
    std::vector<MyEvent> my;
    std::wstring holStatus, syncStatus;
    long long holTime = 0, syncTime = 0;
    int holCount = 0, syncCount = 0;

    void LoadSettings(const JVal* o);
    void SaveSettings(JVal& o) const;
    void Ensure();                       // first use: load caches, refresh if stale
    void Refresh(bool force);
    bool Started() const { return started_; }
    int  Mask(int day) const;            // bit (1 << source) for each source present that day
    bool IsHoliday(int day) const;
    void ForDay(int day, std::vector<const Occ*>& out) const;
    void Upcoming(int fromDay, int days, size_t maxN, std::vector<const Occ*>& out) const;

    void AddFromText(int day, const std::wstring& text);
    void EditFromText(int idx, const std::wstring& text);
    void Delete(int idx);
    void ToggleYearly(int idx);
    std::wstring TextOf(int idx) const;

    void SetEnabled(bool on);
    void SetHolidays(bool on);
    void SetMine(bool on);
    void SetRegion(const std::wstring& iso);
    void SetIcsUrl(const std::wstring& url);
    std::wstring RegionCode() const;
    std::wstring RegionName() const;
    void ChooseRegion();                 // popup menu of countries
    void AskIcsUrl();
    std::wstring SyncSummary() const;
    std::wstring HolidaySummary() const;
private:
    bool started_ = false, busyHol_ = false, busySync_ = false;
    std::vector<Occ> holOcc_, syncOcc_, occ_;
    void Rebuild();
    void FetchHolidays(bool alt);
    void FetchSync();
    void LoadMine();
    void SaveMine() const;
};
extern Events g_events;
void EventsChanged();   // app.cpp (posted): redraw calendars, Settings and the day popover

// Day popover (daypop.cpp)
void ShowDayPopup(int day, RECT anchor);
void RefreshDayPopup();
void CloseDayPopup();
