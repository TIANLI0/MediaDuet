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

#include <cmath>
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
// NetEase's player: a few small reads per poll. Its position advances in 10 ms steps every ~30 ms,
// so a reading is exact; a seek or a stall shows within one poll.
constexpr int64_t kPollPlayingMs = 500;
constexpr int64_t kPollIdleMs = 1'000;
constexpr int64_t kResyncMs = 200;   // re-anchor when the reading leaves the extrapolation by more
constexpr int kMaxReadFailures = 3;  // then fall back to the estimate
constexpr int64_t kQueueCheckMs = 2'000; // the play queue file, while NetEase's player is read
// A NetEase that just started has no player to read yet: try again a few times.
constexpr int kMaxMemTries = 6;
constexpr int64_t kMemRetryMs = 3'000;
// How long after a web command NetEase may still raise its window.
constexpr int64_t kGuardMs = 2'500;
constexpr int64_t kGuardPollMs = 16;

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

/** NetEase's main window: of its OrpheusBrowserHost windows the one with a title ("Song - Artist");
 *  the others (desktop lyrics, mini player, tray helpers) ignore media commands. */
HWND mainWindow() {
    HWND first = nullptr, h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kClass, nullptr)) != nullptr) {
        if (!first) first = h;
        if (GetWindowTextLengthW(h) > 0) return h;
    }
    return first;
}

DWORD windowPid(HWND h) {
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid;
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
    : sched_(sched), art_(art), changed_(std::move(changed)), scanTimer_(&sched), pollTimer_(&sched),
      memRetryTimer_(&sched), guardTimer_(&sched) {}

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
    if (hwnd_ && GetWindowTextLengthW(hwnd_) == 0) {
        // Attached to a helper window (NetEase was starting): move to the main one once it is up.
        HWND main = mainWindow();
        if (main && main != hwnd_ && windowPid(main) == pid_ && GetWindowTextLengthW(main) > 0) {
            hwnd_ = main;
            onTitle();
        }
    }
    if (!hwnd_) {
        if (HWND h = mainWindow()) attach(h, windowPid(h));
    }
    if (hwnd_) checkQueueFile();
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
    memTries_ = 0;
    openMemory(pid);
}

void NeteaseCompat::retryMemory() {
    if (!hwnd_ || mem_ || memTries_ >= kMaxMemTries) return;
    memRetryTimer_.start(kMemRetryMs, [this] {
        if (hwnd_ && !mem_) openMemory(pid_);
    });
}

void NeteaseCompat::openMemory(DWORD pid) {
    ++memTries_;
    uint64_t gen = ++memGen_;
    std::weak_ptr<bool> alive = alive_;
    [](NeteaseCompat* self, std::weak_ptr<bool> alive, DWORD pid, uint64_t gen) -> fire_and_forget {
        co_await resume_background();
        std::string why;
        std::shared_ptr<NeteaseMemory> mem = NeteaseMemory::open(pid, &why);
        self->sched_.post([self, alive, gen, mem, why] {
            if (alive.expired() || gen != self->memGen_) return;
            if (!mem) {
                log::info("netease", "player not readable ({}, try {}): position is estimated", why, self->memTries_);
                self->retryMemory();
                return;
            }
            log::info("netease", "reading the playback position from NetEase's player");
            self->mem_ = mem;
            self->memFailures_ = 0;
            self->memStatus_ = media::PlaybackStatus::Unknown;
            self->poll();
        });
    }(this, alive, pid, gen);
}

void NeteaseCompat::poll() {
    if (!mem_) return;
    int64_t now = sched_.nowMs();
    if (auto r = mem_->read()) {
        memFailures_ = 0;
        using media::PlaybackStatus;
        PlaybackStatus status = r->status == static_cast<int>(media::netease::PlayerStatus::Playing) ? PlaybackStatus::Playing
                                : r->status == static_cast<int>(media::netease::PlayerStatus::Paused) ? PlaybackStatus::Paused
                                                                                                       : PlaybackStatus::Stopped;
        int64_t pos = std::llround(r->positionSec * 1000.0);
        std::optional<int64_t> dur;
        if (r->durationSec > 0) dur = std::llround(r->durationSec * 1000.0);
        int64_t predicted = memPosMs_ + (memStatus_ == PlaybackStatus::Playing ? now - memAt_ : 0);
        if (memSeekUntil_ > 0) {
            // NetEase has not jumped yet: keep showing the target instead of the old position.
            if (now < memSeekUntil_ && r->songId == memId_ && std::llabs(pos - predicted) > kResyncMs) {
                pollTimer_.start(150, [this] { poll(); });
                return;
            }
            memSeekUntil_ = 0;
        }
        if (status != memStatus_ || dur != memDurationMs_ || r->songId != memId_ || std::llabs(pos - predicted) > kResyncMs) {
            memStatus_ = status;
            memPosMs_ = pos;
            memAt_ = now;
            memDurationMs_ = dur;
            memId_ = r->songId;
            if (changed_) changed_();
        }
    } else if (++memFailures_ >= kMaxReadFailures) {
        log::warn("netease", "NetEase's player stopped answering: position is estimated again");
        mem_.reset();
        memStatus_ = media::PlaybackStatus::Unknown;
        memTries_ = 0;
        retryMemory();
        if (changed_) changed_();
        return;
    }
    if (now - queueCheckedAt_ >= kQueueCheckMs) checkQueueFile();
    pollTimer_.start(memStatus_ == media::PlaybackStatus::Playing ? kPollPlayingMs : kPollIdleMs, [this] { poll(); });
}

void NeteaseCompat::checkQueueFile() {
    queueCheckedAt_ = sched_.nowMs();
    if (queueLoading_) return;
    fs::path path = queueFile();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        if (queue_) {
            queue_.reset();
            queueStamp_ = 0;
            if (changed_) changed_();
        }
        return;
    }
    uint64_t stamp = ((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^
                     ((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
    if (stamp == queueStamp_) return;
    queueLoading_ = true;
    std::weak_ptr<bool> alive = alive_;
    [](NeteaseCompat* self, std::weak_ptr<bool> alive, fs::path path, uint64_t stamp) -> fire_and_forget {
        co_await resume_background();
        std::shared_ptr<const media::netease::PlayingList> list;
        try {
            std::ifstream in(path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            if (auto l = media::netease::parsePlayingList(ss.str())) list = std::make_shared<const media::netease::PlayingList>(std::move(*l));
        } catch (...) {
        }
        self->sched_.post([self, alive, stamp, list] {
            if (alive.expired()) return;
            self->queueLoading_ = false;
            if (!list) return; // NetEase is writing it: read again at the next check
            self->queueStamp_ = stamp;
            if (self->queue_ && self->queue_->rev == list->rev) return;
            self->queue_ = list;
            log::debug("netease", "play queue: {} entries (rev {})", list->entries.size(), list->rev);
            if (self->changed_) self->changed_();
        });
    }(this, alive, std::move(path), stamp);
}

int64_t NeteaseCompat::activeSongId(const std::string& key) const {
    if (mem_ && memId_ > 0) return memId_;
    return key == kKey ? songId_ : supId_;
}

void NeteaseCompat::applyQueue(media::MediaSessionInfo& s, int64_t activeId) const {
    if (!queue_ || queue_->entries.empty()) return;
    media::QueueHint h;
    h.count = static_cast<int>(queue_->entries.size());
    h.rev = queue_->rev;
    if (activeId > 0)
        for (auto& e : queue_->entries)
            if (e.id == activeId) {
                h.activeId = activeId;
                break;
            }
    s.queue = h;
}

std::optional<media::MediaQueue> NeteaseCompat::queue(const std::string& key) const {
    if (!handlesQueue(key)) return std::nullopt;
    int64_t active = activeSongId(key);
    return media::netease::queueWindow(*queue_, key, active > 0 ? std::optional<int64_t>(active) : std::nullopt);
}

bool NeteaseCompat::playQueueItem(const std::string& key, int64_t id) {
    if (!handlesQueue(key) || id <= 0) return false;
    // NetEase's own web command plays the song; it stays in the queue it is part of.
    if (!runQuietly(text::toWide(handoff::neteaseDesktopPlay(std::to_string(id))))) return false;
    if (mem_) pollTimer_.start(300, [this] { poll(); });
    return true;
}

void NeteaseCompat::applyMemory(media::MediaSessionInfo& s, int64_t shownId) const {
    if (!mem_ || memStatus_ == media::PlaybackStatus::Unknown) return;
    // The player switches songs a moment before the title follows: keep the old track's values meanwhile.
    if (shownId > 0 && memId_ > 0 && shownId != memId_) return;
    if (memStatus_ != media::PlaybackStatus::Stopped) s.status = memStatus_;
    s.positionMs = memPosMs_;
    s.positionAtMs = memAt_;
    if (memDurationMs_) s.durationMs = memDurationMs_;
    if (memId_ > 0) s.ids.netease = std::to_string(memId_);
    s.actions |= media::action::Seek;
}

void NeteaseCompat::detach() {
    if (hook_) {
        UnhookWinEvent(hook_);
        std::lock_guard lock(g_hookMutex);
        g_hooks.erase(hook_);
    }
    bool had = hwnd_ != nullptr;
    ++memGen_;
    pollTimer_.cancel();
    memRetryTimer_.cancel();
    mem_.reset();
    memStatus_ = media::PlaybackStatus::Unknown;
    memId_ = 0;
    memDurationMs_.reset();
    hook_ = nullptr;
    guardTimer_.cancel();
    if (guard_.locked) LockSetForegroundWindow(LSFW_UNLOCK);
    guard_ = {};
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
                    if (alive.expired() || gen != self->supGen_) return;
                    if (m.id > 0) self->supId_ = m.id;
                    if (m.durationMs <= 0) return;
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
    applyMemory(s, supId_);
    applyQueue(s, activeSongId(s.key));
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
    applyMemory(s, songId_);
    applyQueue(s, activeSongId(kKey));
    media::sanitize(s);
    s.updateTrackId();
    return s;
}

bool NeteaseCompat::seek(const std::string& key, int64_t positionMs) {
    // NetEase's own web command (orpheus://, handled by `cloudmusic.exe --webcmd`, forwarded to
    // the running instance): its media session has no timeline to seek with.
    if (!runQuietly(text::toWide(handoff::neteaseDesktopSeek(positionMs)))) return false;
    int64_t now = sched_.nowMs();
    if (key == kKey) {
        posMs_ = std::max<int64_t>(positionMs, 0);
        posAt_ = now;
    } else if (key == supSession_) {
        supPos_ = std::max<int64_t>(positionMs, 0);
        supAt_ = now;
    }
    if (mem_) {
        // Show the target right away; the next readings confirm it (NetEase takes ~100-300 ms).
        memPosMs_ = std::max<int64_t>(positionMs, 0);
        if (memDurationMs_) memPosMs_ = std::min(memPosMs_, *memDurationMs_);
        memAt_ = now;
        memSeekUntil_ = now + 3'000;
        pollTimer_.start(150, [this] { poll(); });
    }
    if (changed_) changed_();
    return true;
}

bool NeteaseCompat::runQuietly(const std::wstring& uri) {
    // `cloudmusic.exe --webcmd` hands the command to the running instance, which then shows and
    // activates its window. Remember how the window was and put it back if the command raised it;
    // while we hold the foreground (the PC window), nobody else may take it meanwhile.
    HWND fg = GetForegroundWindow();
    bool netFront = hwnd_ && fg && windowPid(fg) == pid_;
    if (hwnd_ && !netFront) {
        if (!guard_.until) {
            guard_.foreground = fg;
            guard_.visible = IsWindowVisible(hwnd_) != FALSE;
            guard_.iconic = IsIconic(hwnd_) != FALSE;
        }
        if (!guard_.locked && fg && windowPid(fg) == GetCurrentProcessId())
            guard_.locked = LockSetForegroundWindow(LSFW_LOCK) != FALSE;
        guard_.until = sched_.nowMs() + kGuardMs;
    }
    auto h = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNOACTIVATE);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        log::warn("netease", "web command failed ({})", reinterpret_cast<INT_PTR>(h));
        return false;
    }
    if (guard_.until) guardWindow();
    return true;
}

void NeteaseCompat::guardWindow() {
    if (hwnd_ && IsWindow(hwnd_)) {
        if (!guard_.visible && IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);   // it was in the tray
        } else if (guard_.visible && guard_.iconic && !IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        }
        // The window the user was in gets the focus back.
        HWND fg = GetForegroundWindow();
        if (guard_.foreground && fg != guard_.foreground && windowPid(fg) == pid_ && IsWindow(guard_.foreground))
            SetForegroundWindow(guard_.foreground);
    }
    if (hwnd_ && sched_.nowMs() < guard_.until) {
        guardTimer_.start(kGuardPollMs, [this] { guardWindow(); });
        return;
    }
    if (guard_.locked) LockSetForegroundWindow(LSFW_UNLOCK);
    guard_ = {};
}

bool NeteaseCompat::execute(media::CommandAction action) {
    if (hwnd_ && !IsWindow(hwnd_)) detach();   // NetEase restarted: do not wait for the next scan
    if (!hwnd_) {
        if (HWND h = mainWindow()) attach(h, windowPid(h));
    }
    if (!hwnd_) return false;
    bool playing = mem_ && memStatus_ != media::PlaybackStatus::Unknown ? memStatus_ == media::PlaybackStatus::Playing : audible_;
    int cmd = 0;
    switch (action) {
    case media::CommandAction::Play:
        if (playing) return true;
        cmd = APPCOMMAND_MEDIA_PLAY_PAUSE;
        break;
    case media::CommandAction::Pause:
        if (!playing) return true;
        cmd = APPCOMMAND_MEDIA_PLAY_PAUSE;
        break;
    case media::CommandAction::Toggle: cmd = APPCOMMAND_MEDIA_PLAY_PAUSE; break;
    case media::CommandAction::Next: cmd = APPCOMMAND_MEDIA_NEXTTRACK; break;
    case media::CommandAction::Previous: cmd = APPCOMMAND_MEDIA_PREVIOUSTRACK; break;
    default: return false;
    }
    log::info("netease", "media command {} ({})", cmd, playing ? "playing" : "paused");
    SendMessageTimeoutW(hwnd_, WM_APPCOMMAND, reinterpret_cast<WPARAM>(hwnd_), MAKELPARAM(0, cmd | FAPPCOMMAND_KEY),
                        SMTO_ABORTIFHUNG, 500, nullptr);
    return true;
}

} // namespace md::platform
