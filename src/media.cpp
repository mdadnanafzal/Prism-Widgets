// Media session access through the WinRT ABI (no C++/WinRT needed).
// Interface layouts/IIDs: Windows.Media.Control (GlobalSystemMediaTransportControls*), Windows SDK metadata.
// All async operations are polled from the UI thread, so no threads or callbacks are involved.
#include "media.h"
#include <roapi.h>
#include <winstring.h>
#include <shcore.h>

Media g_media;

namespace {
const GUID IID_SessionManagerStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };
const GUID IID_AsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

// vtable slots (IUnknown 0-2, IInspectable 3-5)
enum {
    STATICS_RequestAsync = 6,
    MANAGER_GetCurrentSession = 6,
    SESSION_SourceAppUserModelId = 6, SESSION_TryGetMediaPropertiesAsync = 7, SESSION_GetTimelineProperties = 8,
    SESSION_GetPlaybackInfo = 9, SESSION_TrySkipNextAsync = 16, SESSION_TrySkipPreviousAsync = 17, SESSION_TryTogglePlayPauseAsync = 20,
    PROPS_Title = 6, PROPS_Subtitle = 7, PROPS_AlbumArtist = 8, PROPS_Artist = 9, PROPS_Thumbnail = 15,
    PINFO_Controls = 6, PINFO_PlaybackStatus = 7,
    CTRL_IsPlayEnabled = 6, CTRL_IsPauseEnabled = 7, CTRL_IsNextEnabled = 12, CTRL_IsPreviousEnabled = 13, CTRL_IsPlayPauseToggleEnabled = 16,
    TL_StartTime = 6, TL_EndTime = 7, TL_Position = 10, TL_LastUpdatedTime = 11,
    STREAMREF_OpenReadAsync = 6,
    ASYNCOP_GetResults = 8, ASYNCINFO_Status = 7,
};

template <typename... A> HRESULT VCall(void* obj, int slot, A... args) {
    if (!obj) return E_POINTER;
    typedef HRESULT(STDMETHODCALLTYPE * Fn)(void*, A...);
    return ((Fn)(*(void***)obj)[slot])(obj, args...);
}
void Rel(void*& p) { if (p) { ((IUnknown*)p)->Release(); p = nullptr; } }
int Status(void* op) {   // 0 started, 1 completed, 2 canceled, 3 error
    void* info = nullptr;
    if (FAILED(((IUnknown*)op)->QueryInterface(IID_AsyncInfo, &info)) || !info) return 3;
    int s = 3; VCall(info, ASYNCINFO_Status, &s);
    Rel(info);
    return s;
}
std::wstring Str(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0; const wchar_t* b = WindowsGetStringRawBuffer(h, &n);
    std::wstring s(b ? b : L"", n);
    WindowsDeleteString(h);
    return s;
}
std::wstring GetStr(void* obj, int slot) { HSTRING h = nullptr; if (FAILED(VCall(obj, slot, &h))) return L""; return Str(h); }
void MediaKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
std::wstring PrettyApp(const std::wstring& id) {
    std::wstring l = id; for (auto& c : l) c = towlower(c);
    struct { const wchar_t* k; const wchar_t* n; } known[] = {
        { L"spotify", L"Spotify" }, { L"msedge", L"Microsoft Edge" }, { L"chrome", L"Chrome" }, { L"firefox", L"Firefox" },
        { L"308046b0af4a39cb", L"Firefox" }, { L"zunemusic", L"Media Player" }, { L"zunevideo", L"Movies & TV" }, { L"vlc", L"VLC" },
        { L"brave", L"Brave" }, { L"opera", L"Opera" }, { L"applemusic", L"Apple Music" }, { L"itunes", L"iTunes" },
        { L"amazonmusic", L"Amazon Music" }, { L"deezer", L"Deezer" }, { L"tidal", L"TIDAL" }, { L"youtube", L"YouTube" }, { L"foobar", L"foobar2000" } };
    for (auto& k : known) if (l.find(k.k) != std::wstring::npos) return k.n;
    std::wstring s = id;
    size_t b = s.rfind(L'!'); if (b != std::wstring::npos) s = s.substr(b + 1);
    b = s.find_last_of(L"\\/"); if (b != std::wstring::npos) s = s.substr(b + 1);
    if (s.size() > 4 && !_wcsicmp(s.c_str() + s.size() - 4, L".exe")) s.resize(s.size() - 4);
    if (!s.empty()) s[0] = towupper(s[0]);
    return s;
}
} // namespace

bool Media::Busy() const { return mgrOp_ || propsOp_ || thumbOp_ || !cmdOps_.empty(); }

long long Media::Position() const {
    long long p = st.pos;
    if (st.playing && st.updated > 0) {
        FILETIME f; GetSystemTimeAsFileTime(&f);
        long long now = ((long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
        if (now > st.updated) p += now - st.updated;
    }
    if (st.end > 0 && p > st.end) p = st.end;
    return p < 0 ? 0 : p;
}

void Media::Poll() {
    lastPoll_ = GetTickCount64();
    void* sess = nullptr;
    if (FAILED(VCall(manager_, MANAGER_GetCurrentSession, &sess))) sess = nullptr;
    Rel(session_);
    session_ = sess;
    if (!sess) {
        st.hasSession = false; st.playing = false; st.title.clear(); st.artist.clear(); st.app.clear();
        st.pos = st.end = st.updated = 0;
        if (!st.art.empty()) { st.art.clear(); st.art.shrink_to_fit(); st.artVersion++; }
        artKey_.clear();
        return;
    }
    st.hasSession = true;
    st.app = PrettyApp(GetStr(sess, SESSION_SourceAppUserModelId));
    void* pi = nullptr;
    if (SUCCEEDED(VCall(sess, SESSION_GetPlaybackInfo, &pi)) && pi) {
        int status = 0; VCall(pi, PINFO_PlaybackStatus, &status);
        st.playing = status == 4;
        void* c = nullptr;
        if (SUCCEEDED(VCall(pi, PINFO_Controls, &c)) && c) {
            bool b = true, p1 = false, p2 = false, p3 = false;
            if (SUCCEEDED(VCall(c, CTRL_IsNextEnabled, &b))) st.canNext = b;
            if (SUCCEEDED(VCall(c, CTRL_IsPreviousEnabled, &b))) st.canPrev = b;
            VCall(c, CTRL_IsPlayEnabled, &p1); VCall(c, CTRL_IsPauseEnabled, &p2); VCall(c, CTRL_IsPlayPauseToggleEnabled, &p3);
            st.canPlayPause = p1 || p2 || p3;
            Rel(c);
        }
        Rel(pi);
    }
    void* tl = nullptr;
    if (SUCCEEDED(VCall(sess, SESSION_GetTimelineProperties, &tl)) && tl) {
        long long s = 0, e = 0, p = 0, u = 0;
        VCall(tl, TL_StartTime, &s); VCall(tl, TL_EndTime, &e); VCall(tl, TL_Position, &p); VCall(tl, TL_LastUpdatedTime, &u);
        st.pos = p - s; st.end = e - s; st.updated = u;
        Rel(tl);
    }
    if (!propsOp_) { void* op = nullptr; if (SUCCEEDED(VCall(sess, SESSION_TryGetMediaPropertiesAsync, &op))) propsOp_ = op; }
}

bool Media::TakeProps() {
    int s = Status(propsOp_);
    if (s == 0) return false;
    void* props = nullptr;
    if (s == 1) VCall(propsOp_, ASYNCOP_GetResults, &props);
    Rel(propsOp_);
    if (!props) return false;
    st.title = GetStr(props, PROPS_Title);
    st.artist = GetStr(props, PROPS_Artist);
    if (st.artist.empty()) st.artist = GetStr(props, PROPS_AlbumArtist);
    if (st.artist.empty()) st.artist = GetStr(props, PROPS_Subtitle);
    std::wstring key = st.app + L"|" + st.title + L"|" + st.artist;
    if (key != artKey_ && key != pendingArtKey_) {
        void* ref = nullptr;
        VCall(props, PROPS_Thumbnail, &ref);
        if (ref) {
            Rel(thumbOp_);
            void* op = nullptr;
            if (SUCCEEDED(VCall(ref, STREAMREF_OpenReadAsync, &op))) { thumbOp_ = op; pendingArtKey_ = key; }
            Rel(ref);
        } else if (!st.art.empty()) { st.art.clear(); st.art.shrink_to_fit(); st.artVersion++; artKey_.clear(); }
    }
    Rel(props);
    return true;
}

bool Media::TakeThumb() {
    int s = Status(thumbOp_);
    if (s == 0) return false;
    void* stream = nullptr;
    if (s == 1) VCall(thumbOp_, ASYNCOP_GetResults, &stream);
    Rel(thumbOp_);
    std::wstring key = pendingArtKey_; pendingArtKey_.clear();
    if (!stream) return false;
    IStream* is = nullptr;
    bool ok = false;
    const UINT SZ = 160;   // decoded once, small: ~100 KB
    std::vector<uint32_t> px;
    if (SUCCEEDED(CreateStreamOverRandomAccessStream((IUnknown*)stream, IID_IStream, (void**)&is)) && is) {
        IWICImagingFactory* f = nullptr; IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr;
        IWICBitmapClipper* cl = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
            SUCCEEDED(f->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnLoad, &d)) && SUCCEEDED(d->GetFrame(0, &fr)) &&
            SUCCEEDED(fr->GetSize(&w, &h)) && w && h) {
            UINT side = std::min(w, h);
            WICRect rc{ (INT)((w - side) / 2), (INT)((h - side) / 2), (INT)side, (INT)side };   // centre square crop
            if (SUCCEEDED(f->CreateBitmapClipper(&cl)) && SUCCEEDED(cl->Initialize(fr, &rc)) &&
                SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(cl, SZ, SZ, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                px.resize((size_t)SZ * SZ);
                ok = SUCCEEDED(cv->CopyPixels(nullptr, SZ * 4, SZ * SZ * 4, (BYTE*)px.data()));
            }
        }
        SafeRelease(cv); SafeRelease(sc); SafeRelease(cl); SafeRelease(fr); SafeRelease(d); SafeRelease(f);
        is->Release();
    }
    Rel(stream);
    if (!ok) return false;
    st.art.swap(px); st.artW = st.artH = SZ; st.artVersion++;
    artKey_ = key;
    return true;
}

bool Media::Pump() {
    bool changed = false;
    if (!inited_) {
        inited_ = true; changed = true;
        const wchar_t* cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING h = nullptr;
        if (SUCCEEDED(WindowsCreateString(cls, (UINT32)wcslen(cls), &h))) {
            if (SUCCEEDED(RoGetActivationFactory(h, IID_SessionManagerStatics, &statics_)) && statics_) {
                void* op = nullptr;
                if (SUCCEEDED(VCall(statics_, STATICS_RequestAsync, &op)) && op) { mgrOp_ = op; st.available = true; }
            }
            WindowsDeleteString(h);
        }
        if (!mgrOp_) st.checked = true;
    }
    // never let a stuck async call keep us polling fast
    if (Busy()) {
        if (!busySince_) busySince_ = GetTickCount64();
        else if (GetTickCount64() - busySince_ > 8000) {
            for (auto& op : cmdOps_) Rel(op);
            cmdOps_.clear(); Rel(propsOp_); Rel(thumbOp_); pendingArtKey_.clear();
            if (mgrOp_) { Rel(mgrOp_); st.available = false; }
            busySince_ = 0;
        }
    } else busySince_ = 0;
    if (mgrOp_) {
        int s = Status(mgrOp_);
        if (s == 1) { VCall(mgrOp_, ASYNCOP_GetResults, &manager_); Rel(mgrOp_); lastPoll_ = 0; changed = true; st.checked = true; }
        else if (s > 1) { Rel(mgrOp_); st.available = false; changed = true; st.checked = true; }
    }
    for (size_t i = 0; i < cmdOps_.size();) {
        if (Status(cmdOps_[i]) != 0) { Rel(cmdOps_[i]); cmdOps_.erase(cmdOps_.begin() + i); lastPoll_ = 0; }
        else i++;
    }
    struct { bool hs, pl, cn, cp; std::wstring t, a, ap; long long e; int av; } before =
        { st.hasSession, st.playing, st.canNext, st.canPrev, st.title, st.artist, st.app, st.end, st.artVersion };
    if (manager_ && GetTickCount64() - lastPoll_ >= 800) Poll();
    if (propsOp_) TakeProps();
    if (thumbOp_ && TakeThumb()) changed = true;
    if (before.hs != st.hasSession || before.pl != st.playing || before.t != st.title || before.a != st.artist ||
        before.ap != st.app || before.cn != st.canNext || before.cp != st.canPrev || before.e != st.end || before.av != st.artVersion)
        changed = true;
    return changed;
}

void Media::Command(int slot, WORD vk) {
    if (session_) {
        void* op = nullptr;
        if (SUCCEEDED(VCall(session_, slot, &op)) && op) { cmdOps_.push_back(op); return; }
    }
    MediaKey(vk);   // no session / API unavailable: send the hardware media key instead
    lastPoll_ = 0;
}
void Media::PlayPause() { Command(SESSION_TryTogglePlayPauseAsync, VK_MEDIA_PLAY_PAUSE); if (st.hasSession) st.playing = !st.playing; }
void Media::Next() { Command(SESSION_TrySkipNextAsync, VK_MEDIA_NEXT_TRACK); }
void Media::Prev() { Command(SESSION_TrySkipPreviousAsync, VK_MEDIA_PREV_TRACK); }

void Media::Shutdown() {
    for (auto& op : cmdOps_) Rel(op);
    cmdOps_.clear();
    Rel(thumbOp_); Rel(propsOp_); Rel(mgrOp_); Rel(session_); Rel(manager_); Rel(statics_);
}
