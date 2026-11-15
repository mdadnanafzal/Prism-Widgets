#include "net.h"
#include "app.h"
#include <winhttp.h>

namespace {
struct NetJob { std::wstring url; std::function<void(std::string*)> done; std::string* body = nullptr; size_t max = 0; };

std::string* Get(const std::wstring& urlIn, size_t maxBytes) {
    std::wstring url = urlIn;
    if (url.compare(0, 9, L"webcal://") == 0) url = L"https://" + url.substr(9);
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return nullptr;
    std::wstring fullPath = path;
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) fullPath.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    std::string* out = nullptr;
    HINTERNET s = WinHttpOpen(L"PrismWidgets/" APP_VERSION, 4 /*AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"PrismWidgets/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return nullptr;
    WinHttpSetTimeouts(s, 8000, 8000, 15000, 20000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
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
                if (out->size() > maxBytes) { delete out; out = nullptr; break; }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}

DWORD WINAPI Worker(LPVOID p) {
    NetJob* j = (NetJob*)p;
    j->body = Get(j->url, j->max);
    if (!PostMessageW(g_main, WM_NET, 0, (LPARAM)j)) { delete j->body; delete j; }
    return 0;
}
} // namespace

void HttpGetAsync(const std::wstring& url, std::function<void(std::string*)> done, size_t maxBytes) {
    NetJob* j = new NetJob{ url, std::move(done), nullptr, maxBytes };
    HANDLE t = CreateThread(nullptr, 64 * 1024, Worker, j, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (t) CloseHandle(t);
    else { auto cb = std::move(j->done); delete j; if (cb) cb(nullptr); }
}

void NetDispatch(LPARAM lp) {
    NetJob* j = (NetJob*)lp;
    std::unique_ptr<std::string> body(j->body);
    auto cb = std::move(j->done);
    delete j;
    if (cb) cb(body.get());
}

std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
