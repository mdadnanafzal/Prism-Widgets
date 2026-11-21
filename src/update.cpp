// One-click updates (see update.h). The manifest is signed with an ECDSA P-256 key whose private half
// stays with the maintainer (tools/release.py); the public half is below. A download must match the
// signed size and SHA-256 before it may replace the running exe.
#include "update.h"
#include "app.h"
#include "net.h"
#include <bcrypt.h>

Updater g_update;

namespace {
// Public key (P-256, X then Y) matching .release/update-signing-key.pem
const unsigned char kPubKey[64] = {
    0x77,0x16,0x23,0x1e,0xe0,0x31,0x90,0xc8,0x8e,0x79,0x7d,0x82,0x83,0x85,0xcd,0x81,0xf9,0x9d,0xd0,0x7d,0xe2,0xb2,0x1b,0xf0,0x81,0x01,0x12,0xd0,0xbc,0xc1,0x7a,0x63,
    0x0f,0x71,0xa8,0x1d,0x20,0xb2,0xcd,0x35,0x8e,0x1e,0x1c,0x9f,0xf2,0xf4,0x3d,0x20,0xc5,0xc4,0x94,0x0a,0x83,0x13,0xff,0x94,0x9e,0xd4,0x86,0xb0,0xbc,0xc4,0x10,0x53 };

long long NowUnix() { FILETIME f; GetSystemTimeAsFileTime(&f); return (long long)(((((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL); }

bool Sha256(const void* data, size_t n, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            ok = BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0;
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}
std::string Hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
bool Base64(const std::string& in, std::string& out) {
    int val = 0, bits = -8; out.clear();
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52; else if (c == '+' || c == '-') v = 62; else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || c == ' ' || c == '\n' || c == '\r') continue; else return false;
        val = (val << 6) | v; bits += 6;
        if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return true;
}
std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
// ECDSA P-256 over SHA-256 of msg; sig is r||s (64 bytes)
bool VerifySignature(const std::string& msg, const std::string& sig) {
    if (sig.size() != 64) return false;
    unsigned char hash[32]; if (!Sha256(msg.data(), msg.size(), hash)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob; hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; hdr->cbKey = 32;
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), kPubKey, 64);
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0) == 0) {
        ok = BCryptVerifySignature(key, nullptr, hash, 32, (PUCHAR)sig.data(), 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
std::wstring Ago(long long t) {
    long long d = NowUnix() - t;
    if (d < 90) return L"just now";
    if (d < 3600) return std::to_wstring(d / 60) + L" min ago";
    if (d < 86400 * 2) return std::to_wstring(d / 3600) + L" h ago";
    return std::to_wstring(d / 86400) + L" days ago";
}
std::wstring OldExePath(const std::wstring& exe) { size_t d = exe.rfind(L'.'); return exe.substr(0, d) + L".old.exe"; }
} // namespace

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    const wchar_t *p = a.c_str(), *q = b.c_str();
    for (int i = 0; i < 4; i++) {
        long x = wcstol(p, (wchar_t**)&p, 10), y = wcstol(q, (wchar_t**)&q, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*p == L'.') p++;
        if (*q == L'.') q++;
    }
    return 0;
}

void Updater::LoadSettings(const JVal* o) {
    if (!o || !o->isObj()) return;
    autoCheck = o->boolean(L"auto", true);
    lastCheck = (long long)o->num(L"lastCheck", 0);
    notified = o->str(L"notified", L"");
}
void Updater::SaveSettings(JVal& o) const {
    o = JVal::MakeObj();
    o.set(L"auto") = JVal::MakeBool(autoCheck);
    o.set(L"lastCheck") = JVal::MakeNum((double)lastCheck);
    o.set(L"notified") = JVal::MakeStr(notified);
}

void Updater::Start(bool justUpdated) {
    std::wstring old = OldExePath(ExePath());
    if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old.c_str()))
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);   // still locked: Windows removes it at next restart
    DeleteFileW((ExePath() + L".new").c_str());
    if (justUpdated) TrayBalloon(L"Prism Widgets was updated", L"You're now on version " APP_VERSION L". Your widgets and settings are just as you left them.", 0);
    SetTimer(g_main, UPDATE_TIMER_ID, 90 * 1000, nullptr);   // first look shortly after sign-in, then every few hours
}
void Updater::OnTimer() {
    SetTimer(g_main, UPDATE_TIMER_ID, 3 * 3600 * 1000, nullptr);
    if (autoCheck && state != Checking && state != Downloading && NowUnix() - lastCheck > 20 * 3600) Check(false);
}

void Updater::Fail(const std::wstring& why) {
    error = why; state = Failed;
    UpdateChanged();
}

void Updater::Check(bool manual) {
    if (state == Checking || state == Downloading) return;
    manual_ = manual; state = Checking; error.clear();
    UpdateChanged();
    HttpGetAsync(UPDATE_MANIFEST_URL, [this](std::string* body) { OnManifest(body); }, 64 * 1024);
}

void Updater::OnManifest(std::string* body) {
    if (!body) { Fail(L"Couldn't reach the update server. Check your internet connection."); return; }
    JVal j;
    if (!JsonParse(Utf8ToWide(*body), j) || !j.isObj()) { Fail(L"The update information couldn't be read."); return; }
    std::wstring v = j.str(L"version", L""), u = j.str(L"url", L""), h = j.str(L"sha256", L""), n = j.str(L"notes", L"");
    long long sz = (long long)j.num(L"size", 0);
    std::string sig;
    if (v.empty() || u.compare(0, 8, L"https://") || h.size() != 64 || sz <= 0 || sz > (64 << 20) || !Base64(Narrow(j.str(L"sig", L"")), sig)) {
        Fail(L"The update information is incomplete."); return;
    }
    for (auto& c : h) c = towlower(c);
    std::string msg = "prism-update|" + Narrow(v) + "|" + Narrow(h) + "|" + std::to_string(sz);
    if (!VerifySignature(msg, sig)) { Fail(L"The update isn't signed by Prism Widgets, so it was ignored."); return; }
    lastCheck = NowUnix();
    RequestSave();
    if (CompareVersions(v, APP_VERSION) <= 0) { state = UpToDate; UpdateChanged(); return; }
    version = v; url = u; sha = h; size = sz; notes = n;
    state = Available;
    if (notified != version) {
        notified = version;
        TrayBalloon(L"Prism Widgets " + version + L" is available", L"Click here to see what's new and update in one click.", 1);
    }
    UpdateChanged();
}

void Updater::Install() {
    if (state != Available && !(state == Failed && !version.empty())) return;
    state = Downloading; error.clear();
    UpdateChanged();
    HttpGetAsync(url, [this](std::string* body) { OnDownload(body); }, (size_t)size + 1024);
}

void Updater::OnDownload(std::string* body) {
    if (!body) { Fail(L"The download didn't finish. Try again in a moment."); return; }
    unsigned char h[32];
    if ((long long)body->size() != size || !Sha256(body->data(), body->size(), h) || Hex(h, 32) != Narrow(sha)) {
        Fail(L"The downloaded file didn't match the signed update, so it wasn't installed."); return;
    }
    std::wstring exe = ExePath(), fresh = exe + L".new", old = OldExePath(exe);
    HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, body->data(), (DWORD)body->size(), &wr, nullptr) && wr == body->size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { DeleteFileW(fresh.c_str()); Fail(L"Couldn't save the update next to the app. Is the folder read-only?"); return; }
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {   // a running exe can be renamed, not overwritten
        DeleteFileW(fresh.c_str()); Fail(L"Windows didn't allow replacing the app in this folder."); return;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fresh.c_str()); Fail(L"Couldn't put the new version in place."); return;
    }
    state = Ready;
    UpdateChanged();
    QuitForUpdate(exe);
}

std::wstring Updater::Status() const {
    switch (state) {
    case Checking: return L"Checking for updates…";
    case Available: return L"Version " + version + L" is available";
    case Downloading: return L"Downloading version " + version + L"…";
    case Ready: return L"Restarting into version " + version + L"…";
    case Failed: return error;
    case UpToDate: return L"You're up to date · checked " + Ago(lastCheck);
    default: return lastCheck ? L"Last checked " + Ago(lastCheck) : L"Not checked yet";
    }
}
