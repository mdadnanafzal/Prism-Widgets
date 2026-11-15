#pragma once
// Weather: Open-Meteo forecast (free, no API key) + location (auto via IP, or a city the user picks).
// Network calls run on a short-lived worker thread; results come back to the UI thread as WM_WEATHER.
#include "common.h"

#define WM_WEATHER        (WM_APP + 6)
#define WEATHER_TIMER_ID  50

struct WeatherHour { long long t = 0; float temp = 0; int code = 0; int pop = -1; bool day = true; };
struct WeatherDay  { long long t = 0; int code = 0; float tmax = 0, tmin = 0; int pop = -1; };

struct WeatherData {
    bool valid = false;
    long long fetched = 0;       // unix seconds (UTC)
    int utcOffset = 0;           // seconds, location's time zone
    bool fahrenheit = false;
    float temp = 0, feels = 0, wind = 0; int humidity = 0, code = 0; bool day = true;
    std::vector<WeatherHour> hours;
    std::vector<WeatherDay> days;
};

class Weather {
public:
    WeatherData data;
    // settings (saved in config.json -> "weather")
    bool autoLoc = true;
    std::wstring place;
    double lat = 0, lon = 0; bool haveLoc = false; long long locTime = 0;
    int units = -1;              // -1 automatic (region), 0 Celsius, 1 Fahrenheit

    bool busy = false;
    std::wstring status;         // "" when fine, otherwise a short message for the widget

    void LoadSettings(const JVal* o);
    void SaveSettings(JVal& o) const;
    void Ensure();               // first use: load the disk cache and fetch if stale
    void Refresh(bool force);
    void OnResult(WPARAM job, LPARAM body);
    void AskCity();              // input box -> search -> pick
    void SetAuto(bool on);
    void SetUnits(int u);
    bool Fahrenheit() const;
    std::wstring UpdatedText() const;
    void Shutdown();
private:
    bool started_ = false;
    int failures_ = 0;
    void Start(int job, const std::wstring& host, const std::wstring& path);
    void FetchForecast();
    bool Parse(const std::wstring& json, WeatherData& out);
    void LoadCache();
    void SaveCache(const std::string& raw);
    void Schedule(UINT ms);
};
extern Weather g_weather;

std::wstring WeatherText(int code);   // "Partly cloudy" ...
void WeatherChanged();                // app.cpp: redraw weather widgets + Settings
