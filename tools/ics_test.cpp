#include "../src/events.h"
#include <cstdio>
static std::wstring W(const char* s){ std::wstring r; while(*s) r+= (wchar_t)(unsigned char)*s++; return r; }
static void Dump(const char* name, const char* ics, int from, int to) {
    std::vector<Occ> o; ParseIcs(W(ics), EV_SYNC, from, to, o);
    std::sort(o.begin(), o.end(), [](const Occ&a,const Occ&b){ return a.day<b.day || (a.day==b.day && a.sh<b.sh); });
    printf("== %s (%zu)\n", name, o.size());
    for (auto& e : o) printf("  %d %s%02d:%02d %ls\n", e.day, e.sh<0?"allday ":"", e.sh<0?0:e.sh, e.sm, e.title.c_str());
}
int main() {
    Dump("holidays", "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261108\r\nDTEND;VALUE=DATE:20261109\r\nSUMMARY:Diwali/Deepavali\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261225\r\nDTEND;VALUE=DATE:20261226\r\nSUMMARY:Christmas\\, Day\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261230\r\nDTEND;VALUE=DATE:20270102\r\nSUMMARY:Long\r\n weekend folded\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", 20261101, 20270131);
    Dump("weekly standup MO,WE x5 with exdate", "BEGIN:VEVENT\r\nDTSTART:20261102T093000\r\nDTEND:20261102T100000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=5\r\nEXDATE:20261104T093000\r\nSUMMARY:Standup\r\nEND:VEVENT\r\n", 20261101, 20261231);
    Dump("monthly 2nd TU until", "BEGIN:VEVENT\r\nDTSTART:20261110T140000\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU;UNTIL=20270301T000000Z\r\nSUMMARY:Review\r\nEND:VEVENT\r\n", 20261101, 20270331);
    Dump("yearly birthday", "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:19900220\r\nRRULE:FREQ=YEARLY\r\nSUMMARY:Bday\r\nEND:VEVENT\r\n", 20261101, 20280301);
    Dump("daily interval 3 + override + cancelled", "BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261115T080000\r\nRRULE:FREQ=DAILY;INTERVAL=3;COUNT=4\r\nSUMMARY:Gym\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:a\r\nRECURRENCE-ID:20261118T080000\r\nDTSTART:20261118T190000\r\nSUMMARY:Gym (evening)\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nDTSTART:20261120T100000\r\nSTATUS:CANCELLED\r\nSUMMARY:Gone\r\nEND:VEVENT\r\n", 20261101, 20261231);
    Dump("utc time", "BEGIN:VEVENT\r\nDTSTART:20261116T043000Z\r\nSUMMARY:UTC call\r\nEND:VEVENT\r\n", 20261101, 20261231);
    Dump("monthly bymonthday 31", "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261031\r\nRRULE:FREQ=MONTHLY;COUNT=4\r\nSUMMARY:Rent\r\nEND:VEVENT\r\n", 20261001, 20270331);
}
