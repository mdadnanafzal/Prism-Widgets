#pragma once
// Tiny async HTTP(S) GET: runs on a short-lived worker thread, the callback runs on the UI thread.
#include "common.h"
#include <functional>

#define WM_NET (WM_APP + 11)

// url: https://, http:// or webcal:// . body == nullptr on failure. Responses are capped at `maxBytes`.
void HttpGetAsync(const std::wstring& url, std::function<void(std::string* body)> done, size_t maxBytes = 8u << 20);
void NetDispatch(LPARAM lp);   // call from the main window for WM_NET
std::wstring Utf8ToWide(const std::string& s);
