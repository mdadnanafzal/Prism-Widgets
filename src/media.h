#pragma once
// "Now playing" via Windows' Global System Media Transport Controls (the same source the
// Windows volume flyout uses). Works with Spotify, browsers (YouTube etc.), Media Player, VLC...
#include "common.h"

struct MediaState {
    bool available = false;     // Windows media session API usable
    bool checked = false;       // first availability check done
    bool hasSession = false;    // something is (or was) playing
    bool playing = false;
    bool canPrev = true, canNext = true, canPlayPause = true;
    std::wstring title, artist, app;
    long long pos = 0, end = 0, updated = 0;   // 100-ns units; updated = FILETIME of last position report
    std::vector<uint32_t> art; int artW = 0, artH = 0; int artVersion = 0;   // premultiplied BGRA
};

class Media {
public:
    MediaState st;
    bool Pump();          // advance async work / poll; true when the state changed
    bool Busy() const;    // async work in flight (poll faster)
    void PlayPause();
    void Next();
    void Prev();
    long long Position() const;   // extrapolated while playing
    void Shutdown();
private:
    void* statics_ = nullptr; void* manager_ = nullptr; void* session_ = nullptr;
    void* mgrOp_ = nullptr; void* propsOp_ = nullptr; void* thumbOp_ = nullptr;
    std::vector<void*> cmdOps_;
    bool inited_ = false;
    ULONGLONG lastPoll_ = 0, busySince_ = 0;
    std::wstring artKey_, pendingArtKey_;
    void Poll();
    bool TakeProps();
    bool TakeThumb();
    void Command(int slot, WORD fallbackVk);
};
extern Media g_media;
