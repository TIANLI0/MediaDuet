#include "NeteaseCompat.h"

#include "md/handoff/Handoff.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <shellapi.h>
#include <shlobj.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace md::platform {

namespace fs = std::filesystem;
using namespace winrt;

namespace {

constexpr int64_t kScanMs = 5'000;          // look for the NetEase window (cheap FindWindow)
constexpr wchar_t kClass[] = L"OrpheusBrowserHost";

std::mutex g_hookMutex;
std::map<HWINEVENTHOOK, NeteaseCompat*> g_hooks;

std::string windowTitle(HWND h) {
    wchar_t buf[512];
    int n = GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
    return text::toUtf8(std::wstring_view(buf, n > 0 ? n : 0));
}

fs::path queueFile() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        out = fs::path(p) / L"NetEase" / L"CloudMusic" / L"webdata" / L"file" / L"playingList";
        CoTaskMemFree(p);
    }
    return out;
}

} // namespace

struct NeteaseCompat::Hook {
    static void CALLBACK proc(HWINEVENTHOOK h, DWORD, HWND hwnd, LONG obj, LONG, DWORD, DWORD) {
        if (obj != OBJID_WINDOW) return;
        NeteaseCompat* self = nullptr;
        {
            std::lock_guard lock(g_hookMutex);
            auto it = g_hooks.find(h);
            if (it != g_hooks.end()) self = it->second;
        }
        if (self && hwnd == self->hwnd_) self->onTitle();
    }
};

NeteaseCompat::NeteaseCompat(IScheduler& sched, ArtworkCache& art, std::function<void()> changed)
    : sched_(sched), art_(art), changed_(std::move(changed)), scanTimer_(&sched) {}

NeteaseCompat::~NeteaseCompat() {
    *alive_ = false;
    detach();
}

void NeteaseCompat::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled) scan();
    else {
        scanTimer_.cancel();
        detach();
        if (changed_) changed_();
    }
}

void NeteaseCompat::setSmtcPresent(bool present) {
    if (smtcPresent_ == present) return;
    smtcPresent_ = present;
    if (changed_) changed_();
}

void NeteaseCompat::setAudible(bool audible) {
    if (audible_ == audible) return;
    int64_t now = sched_.nowMs();
    if (audible_) posMs_ += now - posAt_; // accumulate the audible stretch that just ended
    posAt_ = now;
    audible_ = audible;
    if (hwnd_ && changed_) changed_();
}

void NeteaseCompat::scan() {
    if (!enabled_) return;
    if (hwnd_ && !IsWindow(hwnd_)) detach();
    if (!hwnd_) {
        HWND h = FindWindowW(kClass, nullptr);
        if (h) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            attach(h, pid);
        }
    }
    scanTimer_.start(kScanMs, [this] { scan(); });
}

void NeteaseCompat::attach(HWND hwnd, DWORD pid) {
    hwnd_ = hwnd;
    pid_ = pid;
    hook_ = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, Hook::proc, pid, 0,
                            WINEVENT_OUTOFCONTEXT);
    {
        std::lock_guard lock(g_hookMutex);
        g_hooks[hook_] = this;
    }
    log::info("netease", "attached to NetEase Cloud Music (pid {})", pid);
    onTitle();
}

void NeteaseCompat::detach() {
    if (hook_) {
        UnhookWinEvent(hook_);
        std::lock_guard lock(g_hookMutex);
        g_hooks.erase(hook_);
    }
    bool had = hwnd_ != nullptr;
    hook_ = nullptr;
    hwnd_ = nullptr;
    pid_ = 0;
    title_.clear();
    artist_.clear();
    songId_ = 0;
    if (had && changed_) changed_();
}

void NeteaseCompat::onTitle() {
    std::string t = windowTitle(hwnd_);
    std::string title, artist;
    // "Title - Artist"; artists rarely contain " - ", titles sometimes do.
    auto sep = t.rfind(" - ");
    if (sep != std::string::npos && sep > 0) {
        title = text::trim(t.substr(0, sep));
        artist = text::trim(t.substr(sep + 3));
    }
    if (title == title_ && artist == artist_) return;
    title_ = title;
    artist_ = artist;
    album_.clear();
    durationMs_.reset();
    artwork_.reset();
    songId_ = 0;
    posMs_ = 0;
    posAt_ = sched_.nowMs();
    uint64_t gen = ++gen_;
    if (changed_) changed_();
    if (!title.empty()) enrich(title, artist, gen);
}

NeteaseCompat::QueueMatch NeteaseCompat::lookupQueue(const std::string& title, const std::string& artist) {
    QueueMatch m;
    try {
        std::ifstream in(queueFile(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        auto j = nlohmann::json::parse(ss.str(), nullptr, false);
        if (!j.is_object() || !j.contains("list") || !j["list"].is_array()) return m;
        for (auto& e : j["list"]) {
            if (!e.is_object() || !e.contains("track") || !e["track"].is_object()) continue;
            auto& tr = e["track"];
            if (tr.value("name", std::string()) != title) continue;
            std::string names;
            if (tr.contains("artists") && tr["artists"].is_array())
                for (auto& a : tr["artists"])
                    if (a.is_object()) names += (names.empty() ? "" : "/") + a.value("name", std::string());
            // Artist separators differ between the window title, SMTC ("A/B", "A, B") and the queue.
            if (!artist.empty() && !names.empty() && names != artist && names.find(artist) == std::string::npos &&
                artist.find(names) == std::string::npos) {
                auto first = names.substr(0, names.find('/'));
                if (artist.find(first) == std::string::npos) continue;
            }
            try {
                m.id = std::stoll(tr.value("id", std::string("0")));
            } catch (...) {
            }
            if (tr.contains("duration") && tr["duration"].is_number()) m.durationMs = tr["duration"].get<int64_t>();
            if (tr.contains("album") && tr["album"].is_object()) {
                m.album = tr["album"].value("name", std::string());
                m.pic = tr["album"].value("picUrl", std::string());
            }
            break;
        }
    } catch (...) {
    }
    return m;
}

void NeteaseCompat::supplement(media::MediaSessionInfo& s) {
    if (!enabled_) return; // Settings > Compatibility > NetEase Cloud Music
    if (s.durationMs && *s.durationMs > 0) {
        if (s.key == supSession_) supKey_.clear(); // NetEase reports a timeline after all
        return;
    }
    int64_t now = sched_.nowMs();
    std::string id = s.title + '\x1f' + s.artist;
    bool playing = s.isPlaying();
    if (id != supKey_ || s.key != supSession_) {
        // New track (or first sight): it starts from the beginning as far as we can tell.
        supKey_ = id;
        supSession_ = s.key;
        supPlaying_ = playing;
        supPos_ = 0;
        supAt_ = now;
        supDuration_.reset();
        supId_ = 0;
        uint64_t gen = ++supGen_;
        if (!s.title.empty()) {
            std::weak_ptr<bool> alive = alive_;
            [](NeteaseCompat* self, std::weak_ptr<bool> alive, std::string title, std::string artist,
               uint64_t gen) -> fire_and_forget {
                co_await resume_background();
                auto m = lookupQueue(title, artist);
                self->sched_.post([self, alive, gen, m] {
                    if (alive.expired() || gen != self->supGen_ || m.durationMs <= 0) return;
                    self->supDuration_ = m.durationMs;
                    log::debug("netease", "duration from the play queue: {} ms", m.durationMs);
                    if (self->changed_) self->changed_();
                });
            }(this, alive, s.title, s.artist, gen);
        }
    } else if (playing != supPlaying_) {
        if (supPlaying_) supPos_ += now - supAt_; // the playing stretch that just ended
        supAt_ = now;
        supPlaying_ = playing;
    }
    s.durationMs = supDuration_;
    if (supId_ > 0) s.ids.netease = std::to_string(supId_);
    int64_t pos = supPos_;
    if (supDuration_) pos = std::min(pos, *supDuration_);
    s.positionMs = pos;
    s.positionAtMs = supAt_;
}

void NeteaseCompat::enrich(const std::string& title, const std::string& artist, uint64_t gen) {
    std::weak_ptr<bool> alive = alive_;
    [](NeteaseCompat* self, std::weak_ptr<bool> alive, std::string title, std::string artist,
       uint64_t gen) -> fire_and_forget {
        co_await resume_background();
        auto match = lookupQueue(title, artist);
        int64_t id = match.id, dur = match.durationMs;
        std::string album = match.album, pic = match.pic;
        std::optional<media::ArtworkRef> art;
        if (!pic.empty() && pic.rfind("http", 0) == 0) {
            try {
                Windows::Web::Http::HttpClient client;
                auto buf = co_await client.GetBufferAsync(Windows::Foundation::Uri(text::toWide(pic + "?param=512y512")));
                art = self->art_.ingest(ByteView(buf.data(), buf.Length()));
            } catch (...) {
            }
        }
        self->sched_.post([self, alive, gen, id, dur, album, art] {
            if (alive.expired() || gen != self->gen_) return;
            self->songId_ = id;
            if (dur > 0) self->durationMs_ = dur;
            self->album_ = album;
            self->artwork_ = art;
            if (self->changed_) self->changed_();
        });
    }(this, alive, title, artist, gen);
}

std::optional<media::MediaSessionInfo> NeteaseCompat::session() const {
    if (!enabled_ || smtcPresent_ || !hwnd_ || title_.empty()) return std::nullopt;
    media::MediaSessionInfo s;
    s.key = kKey;
    s.appId = "cloudmusic.exe";
    s.appName = "\xE7\xBD\x91\xE6\x98\x93\xE4\xBA\x91\xE9\x9F\xB3\xE4\xB9\x90"; // 网易云音乐
    s.title = title_;
    s.artist = artist_;
    s.album = album_;
    s.durationMs = durationMs_;
    s.status = audible_ ? media::PlaybackStatus::Playing : media::PlaybackStatus::Paused;
    s.positionMs = posMs_;
    s.positionAtMs = posAt_;
    s.actions = media::action::Toggle | media::action::Next | media::action::Previous | media::action::Play |
                media::action::Pause;
    s.artwork = artwork_;
    if (songId_ > 0) s.ids.netease = std::to_string(songId_);
    media::sanitize(s);
    s.updateTrackId();
    return s;
}

bool NeteaseCompat::seek(const std::string& key, int64_t positionMs) {
    // NetEase's own web command (orpheus://, handled by `cloudmusic.exe --webcmd`, forwarded to
    // the running instance): its media session has no timeline to seek with.
    std::wstring uri = text::toWide(handoff::neteaseDesktopSeek(positionMs));
    auto h = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNOACTIVATE);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        log::warn("netease", "seek command failed ({})", reinterpret_cast<INT_PTR>(h));
        return false;
    }
    int64_t now = sched_.nowMs();
    if (key == kKey) {
        posMs_ = std::max<int64_t>(positionMs, 0);
        posAt_ = now;
    } else if (key == supSession_) {
        supPos_ = std::max<int64_t>(positionMs, 0);
        supAt_ = now;
    }
    if (changed_) changed_();
    return true;
}

bool NeteaseCompat::execute(media::CommandAction action) {
    if (!hwnd_ || !IsWindow(hwnd_)) return false;
    int cmd = 0;
    switch (action) {
    case media::CommandAction::Play:
        if (audible_) return true;
        cmd = APPCOMMAND_MEDIA_PLAY_PAUSE;
        break;
    case media::CommandAction::Pause:
        if (!audible_) return true;
        cmd = APPCOMMAND_MEDIA_PLAY_PAUSE;
        break;
    case media::CommandAction::Toggle: cmd = APPCOMMAND_MEDIA_PLAY_PAUSE; break;
    case media::CommandAction::Next: cmd = APPCOMMAND_MEDIA_NEXTTRACK; break;
    case media::CommandAction::Previous: cmd = APPCOMMAND_MEDIA_PREVIOUSTRACK; break;
    default: return false;
    }
    SendMessageTimeoutW(hwnd_, WM_APPCOMMAND, reinterpret_cast<WPARAM>(hwnd_), MAKELPARAM(0, cmd | FAPPCOMMAND_KEY),
                        SMTO_ABORTIFHUNG, 500, nullptr);
    return true;
}

} // namespace md::platform
