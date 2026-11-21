#pragma once
// One-click updates: a signed manifest (update.json) on GitHub says which version is newest;
// the new exe is downloaded, checked against the manifest's SHA-256, and swapped in place.
#include "common.h"

#define UPDATE_TIMER_ID 60
#define UPDATE_MANIFEST_URL L"https://raw.githubusercontent.com/mdadnanafzal/Prism-Widgets/main/update.json"
#define UPDATE_PAGE_URL     L"https://prism-widgets-landing-page.vercel.app/"

int CompareVersions(const std::wstring& a, const std::wstring& b);   // <0, 0, >0

class Updater {
public:
    enum State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    State state = Idle;
    bool autoCheck = true;
    long long lastCheck = 0;          // unix seconds of the last successful check
    std::wstring notified;            // version the tray balloon already announced
    std::wstring error;               // last failure, in words
    std::wstring version, url, sha, notes;
    long long size = 0;

    void LoadSettings(const JVal* o);
    void SaveSettings(JVal& o) const;
    void Start(bool justUpdated);     // timers, leftover cleanup
    void OnTimer();
    void Check(bool manual);
    void Install();
    std::wstring Status() const;      // one line for Settings
private:
    bool manual_ = false;
    void Fail(const std::wstring& why);
    void OnManifest(std::string* body);
    void OnDownload(std::string* body);
};
extern Updater g_update;

// host services (app.cpp)
void UpdateChanged();                                              // refresh Settings
void TrayBalloon(const std::wstring& title, const std::wstring& text, int purpose);   // purpose 1 = update available
void QuitForUpdate(const std::wstring& exe);                       // save, start exe --updated, exit
