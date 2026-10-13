#include "GsmtcSource.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using media::MediaSessionInfo;
using media::PlaybackStatus;

namespace {

constexpr int64_t kEmitCoalesceMs = 60;
constexpr uint32_t kMaxThumbBytes = 8 * 1024 * 1024;

int64_t ticksToMs(Windows::Foundation::TimeSpan t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

PlaybackStatus mapStatus(GlobalSystemMediaTransportControlsSessionPlaybackStatus s) {
    using S = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (s) {
    case S::Playing: return PlaybackStatus::Playing;
    case S::Paused: return PlaybackStatus::Paused;
    case S::Stopped:
    case S::Closed: return PlaybackStatus::Stopped;
    case S::Changing:
    case S::Opened: return PlaybackStatus::Buffering;
    default: return PlaybackStatus::Unknown;
    }
}

std::string friendlyFallback(const std::string& aumid) {
    static const std::map<std::string, std::string> known = {
        {"cloudmusic.exe", "网易云音乐"}, {"qqmusic.exe", "QQ音乐"},  {"spotify.exe", "Spotify"},
        {"msedge", "Microsoft Edge"},     {"chrome", "Google Chrome"}, {"firefox.exe", "Firefox"},
        {"308046b0af4a39cb", "Firefox"},  {"kugou.exe", "酷狗音乐"},   {"kwmusic.exe", "酷我音乐"},
        {"foobar2000.exe", "foobar2000"}, {"vlc.exe", "VLC"},          {"potplayermini64.exe", "PotPlayer"}};
    std::string lower = text::toLowerAscii(aumid);
    if (auto it = known.find(lower); it != known.end()) return it->second;
    auto bang = aumid.find('!');
    std::string s = bang != std::string::npos ? aumid.substr(bang + 1) : aumid;
    auto under = s.find('_');
    if (bang != std::string::npos && under != std::string::npos) s = s.substr(0, under);
    if (s.size() > 4 && text::toLowerAscii(s.substr(s.size() - 4)) == ".exe") s.resize(s.size() - 4);
    return s;
}

} // namespace

GsmtcSource::GsmtcSource(IScheduler& sched, ArtworkCache& art, std::function<bool(const std::string&)> isOwn,
                         SnapshotFn onSnapshot)
    : sched_(sched), art_(art), isOwn_(std::move(isOwn)), onSnapshot_(std::move(onSnapshot)), emitTimer_(&sched) {}

GsmtcSource::~GsmtcSource() {
    *alive_ = false;
    try {
        if (manager_) {
            manager_.SessionsChanged(sessionsTok_);
            manager_.CurrentSessionChanged(currentTok_);
        }
        for (auto& [k, e] : entries_) unsubscribe(e);
    } catch (...) {
    }
}

void GsmtcSource::start() {
    std::weak_ptr<bool> alive = alive_;
    [](GsmtcSource* self, std::weak_ptr<bool> alive) -> fire_and_forget {
        co_await resume_background();
        GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        try {
            mgr = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        } catch (hresult_error const& e) {
            log::error("gsmtc", "session manager unavailable: {}", text::toUtf8(e.message()));
        }
        if (!mgr) co_return;
        self->sched_.post([self, alive, mgr] {
            if (alive.expired()) return;
            self->manager_ = mgr;
            self->sessionsTok_ = mgr.SessionsChanged([self, alive](auto&&, auto&&) {
                self->sched_.post([self, alive] {
                    if (!alive.expired()) self->rebuild();
                });
            });
            self->currentTok_ = mgr.CurrentSessionChanged([self, alive](auto&&, auto&&) {
                self->sched_.post([self, alive] {
                    if (!alive.expired()) self->rebuild();
                });
            });
            self->rebuild();
            log::info("gsmtc", "watching Windows media sessions");
        });
    }(this, alive);
}

GsmtcSource::Entry* GsmtcSource::find(const std::string& key) {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

std::string GsmtcSource::appName(const std::string& aumid) {
    if (auto it = appNames_.find(aumid); it != appNames_.end()) return it->second;
    std::string name;
    try {
        auto info = Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(text::toWide(aumid));
        if (info) name = text::toUtf8(info.DisplayInfo().DisplayName());
    } catch (...) {
    }
    if (name.empty()) name = friendlyFallback(aumid);
    appNames_[aumid] = name;
    return name;
}

void GsmtcSource::rebuild() {
    if (!manager_) return;
    std::map<std::string, Session> now;
    std::map<std::string, int> dup;
    std::string current;
    try {
        auto cur = manager_.GetCurrentSession();
        for (auto const& s : manager_.GetSessions()) {
            std::string aumid = text::toUtf8(s.SourceAppUserModelId());
            if (isOwn_ && isOwn_(aumid)) continue;
            int n = ++dup[aumid];
            std::string key = n == 1 ? aumid : aumid + "#" + std::to_string(n);
            now.insert_or_assign(key, s);
            if (cur && cur.SourceAppUserModelId() == s.SourceAppUserModelId() && n == 1) current = key;
        }
    } catch (hresult_error const& e) {
        log::warn("gsmtc", "enumerating sessions failed: {}", text::toUtf8(e.message()));
        return;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!now.count(it->first)) {
            unsubscribe(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [key, s] : now) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            // Same key may now refer to a different session object (app restarted).
            if (it->second.session != s) {
                unsubscribe(it->second);
                it->second.session = s;
                subscribe(key, it->second);
            }
            continue;
        }
        Entry& e = entries_[key];
        e.session = s;
        e.aumid = text::toUtf8(s.SourceAppUserModelId());
        e.info.key = key;
        e.info.appId = e.aumid;
        e.info.appName = appName(e.aumid);
        subscribe(key, e);
    }
    currentKey_ = current;
    scheduleEmit();
}

void GsmtcSource::subscribe(const std::string& key, Entry& e) {
    std::weak_ptr<bool> alive = alive_;
    auto post = [this, alive, key](void (GsmtcSource::*fn)(const std::string&)) {
        return [this, alive, key, fn](auto&&, auto&&) {
            sched_.post([this, alive, key, fn] {
                if (!alive.expired()) (this->*fn)(key);
            });
        };
    };
    try {
        e.propsTok = e.session.MediaPropertiesChanged(post(&GsmtcSource::fetchProperties));
        e.playTok = e.session.PlaybackInfoChanged(post(&GsmtcSource::updatePlayback));
        e.timeTok = e.session.TimelinePropertiesChanged(post(&GsmtcSource::updateTimeline));
    } catch (hresult_error const& ex) {
        log::warn("gsmtc", "subscribe failed for {}: {}", key, text::toUtf8(ex.message()));
    }
    updatePlayback(key);
    updateTimeline(key);
    fetchProperties(key);
}

void GsmtcSource::unsubscribe(Entry& e) {
    try {
        if (e.session) {
            e.session.MediaPropertiesChanged(e.propsTok);
            e.session.PlaybackInfoChanged(e.playTok);
            e.session.TimelinePropertiesChanged(e.timeTok);
        }
    } catch (...) {
    }
}

void GsmtcSource::updatePlayback(const std::string& key) {
    Entry* e = find(key);
    if (!e) return;
    try {
        auto info = e->session.GetPlaybackInfo();
        auto& m = e->info;
        PlaybackStatus before = m.status;
        // Re-anchor the position before the status changes so extrapolation stays continuous.
        if (m.positionMs) {
            m.positionMs = m.positionAt(sched_.nowMs());
            m.positionAtMs = sched_.nowMs();
        }
        m.status = mapStatus(info.PlaybackStatus());
        if (auto rate = info.PlaybackRate()) m.rate = rate.Value();
        auto c = info.Controls();
        uint32_t a = 0;
        if (c.IsPlayEnabled()) a |= media::action::Play;
        if (c.IsPauseEnabled()) a |= media::action::Pause;
        if (c.IsPlayPauseToggleEnabled()) a |= media::action::Toggle;
        if (c.IsNextEnabled()) a |= media::action::Next;
        if (c.IsPreviousEnabled()) a |= media::action::Previous;
        if (c.IsPlaybackPositionEnabled()) a |= media::action::Seek;
        if (c.IsStopEnabled()) a |= media::action::Stop;
        m.actions = a;
        if (auto t = info.PlaybackType()) {
            auto v = t.Value();
            m.kind = v == Windows::Media::MediaPlaybackType::Video ? media::MediaKind::Video
                     : v == Windows::Media::MediaPlaybackType::Music ? media::MediaKind::Music
                                                                       : media::MediaKind::Other;
        }
        if (before != m.status) updateTimeline(key);
    } catch (hresult_error const&) {
        return;
    }
    scheduleEmit();
}

void GsmtcSource::updateTimeline(const std::string& key) {
    Entry* e = find(key);
    if (!e) return;
    try {
        auto tl = e->session.GetTimelineProperties();
        int64_t start = ticksToMs(tl.StartTime());
        int64_t end = ticksToMs(tl.EndTime());
        int64_t pos = ticksToMs(tl.Position()) - start;
        auto& m = e->info;
        m.durationMs = end - start > 0 ? std::optional<int64_t>(end - start) : std::nullopt;
        // Position was valid at LastUpdatedTime; bring it to "now" if playing.
        auto last = tl.LastUpdatedTime();
        int64_t ageMs = 0;
        if (last.time_since_epoch().count() > 0) {
            ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(winrt::clock::now() - last).count();
            if (ageMs < 0 || ageMs > 6 * 3600 * 1000) ageMs = 0;
        }
        if (m.status == PlaybackStatus::Playing) pos += static_cast<int64_t>(static_cast<double>(ageMs) * m.rate);
        if (m.durationMs && pos > *m.durationMs) pos = *m.durationMs;
        m.positionMs = pos < 0 ? 0 : pos;
        m.positionAtMs = sched_.nowMs();
    } catch (hresult_error const&) {
        return;
    }
    scheduleEmit();
}

void GsmtcSource::fetchProperties(const std::string& key) {
    Entry* e = find(key);
    if (!e) return;
    uint64_t gen = ++e->propsGen;
    std::weak_ptr<bool> alive = alive_;
    [](GsmtcSource* self, std::weak_ptr<bool> alive, std::string key, Session s, uint64_t gen) -> fire_and_forget {
        co_await resume_background();
        std::string title, artist, album, albumArtist;
        std::optional<media::ArtworkRef> art;
        try {
            auto props = co_await s.TryGetMediaPropertiesAsync();
            if (!props) co_return;
            title = text::toUtf8(props.Title());
            artist = text::toUtf8(props.Artist());
            album = text::toUtf8(props.AlbumTitle());
            albumArtist = text::toUtf8(props.AlbumArtist());
            if (auto thumb = props.Thumbnail()) {
                auto stream = co_await thumb.OpenReadAsync();
                uint64_t size = stream.Size();
                if (size > 0 && size <= kMaxThumbBytes) {
                    Buffer buf(static_cast<uint32_t>(size));
                    auto got = co_await stream.ReadAsync(buf, static_cast<uint32_t>(size), InputStreamOptions::None);
                    art = self->art_.ingest(ByteView(got.data(), got.Length()));
                }
            }
        } catch (hresult_error const& e) {
            log::debug("gsmtc", "properties for {} unavailable: {}", key, text::toUtf8(e.message()));
            co_return;
        }
        self->sched_.post([self, alive, key, gen, title, artist, album, albumArtist, art] {
            if (alive.expired()) return;
            Entry* e = self->find(key);
            if (!e || e->propsGen != gen) return;
            auto& m = e->info;
            bool changed = m.title != title || m.artist != artist || m.album != album;
            m.title = title;
            m.artist = artist;
            m.album = album;
            m.albumArtist = albumArtist;
            m.artwork = art;
            media::sanitize(m);
            m.updateTrackId();
            if (changed) self->updateTimeline(key);
            self->scheduleEmit();
        });
    }(this, alive, key, e->session, gen);
}

void GsmtcSource::scheduleEmit() {
    if (emitTimer_.active()) return;
    emitTimer_.start(kEmitCoalesceMs, [this] { emit(); });
}

void GsmtcSource::emit() {
    media::MediaSnapshot next;
    // Order: current session, then playing ones, then the rest.
    std::vector<const Entry*> order;
    for (auto& [k, e] : entries_) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(), [&](const Entry* a, const Entry* b) {
        auto rank = [&](const Entry* e) {
            if (e->info.key == currentKey_) return 0;
            return e->info.status == PlaybackStatus::Playing ? 1 : 2;
        };
        return rank(a) < rank(b);
    });
    for (auto* e : order) {
        if (next.sessions.size() >= media::kMaxSessions) break;
        next.sessions.push_back(e->info);
    }
    // Active session: a playing one wins (Windows' current first); when nothing plays,
    // stay on the previously active session instead of following Windows' "current
    // session", which often jumps to an unrelated paused app after a pause.
    const Entry* cur = currentKey_.empty() ? nullptr : find(currentKey_);
    const Entry* prev = snap_.active ? find(*snap_.active) : nullptr;
    if (cur && cur->info.isPlaying()) {
        next.active = currentKey_;
    } else if (auto it = std::find_if(next.sessions.begin(), next.sessions.end(),
                                      [](const MediaSessionInfo& s) { return s.isPlaying(); });
               it != next.sessions.end()) {
        next.active = it->key;
    } else if (prev) {
        next.active = prev->info.key;
    } else if (cur) {
        next.active = currentKey_;
    } else if (!next.sessions.empty()) {
        next.active = next.sessions.front().key;
    }
    if (next.equivalent(snap_)) return;
    next.gen = snap_.gen + 1;
    snap_ = std::move(next);
    if (onSnapshot_) onSnapshot_(snap_);
}

void GsmtcSource::execute(const media::MediaCommand& cmd, ResultFn done) {
    std::string key = cmd.key.value_or(snap_.active.value_or(""));
    Entry* e = find(key);
    if (!e) {
        done(false, "no_session");
        return;
    }
    if (!e->info.supports(cmd.action) &&
        !(cmd.action == media::CommandAction::Toggle &&
          (e->info.supports(media::CommandAction::Play) || e->info.supports(media::CommandAction::Pause)))) {
        done(false, "action_unsupported");
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    [](GsmtcSource* self, std::weak_ptr<bool> alive, Session s, media::MediaCommand cmd, ResultFn done,
       bool playing, std::optional<int64_t> durationMs) -> fire_and_forget {
        co_await resume_background();
        bool ok = false;
        try {
            using A = media::CommandAction;
            switch (cmd.action) {
            case A::Play: ok = co_await s.TryPlayAsync(); break;
            case A::Pause: ok = co_await s.TryPauseAsync(); break;
            case A::Toggle: ok = co_await s.TryTogglePlayPauseAsync(); break;
            case A::Next: ok = co_await s.TrySkipNextAsync(); break;
            case A::Previous: ok = co_await s.TrySkipPreviousAsync(); break;
            case A::Stop: ok = co_await s.TryStopAsync(); break;
            case A::Seek: {
                int64_t p = std::max<int64_t>(0, cmd.positionMs.value_or(0));
                if (durationMs) p = std::min(p, *durationMs);
                auto start = s.GetTimelineProperties().StartTime();
                ok = co_await s.TryChangePlaybackPositionAsync((start + std::chrono::milliseconds(p)).count());
                break;
            }
            }
        } catch (hresult_error const& e) {
            log::warn("gsmtc", "command failed: {}", text::toUtf8(e.message()));
        }
        (void)playing;
        self->sched_.post([alive, done, ok] {
            if (!alive.expired()) done(ok, ok ? "" : "player_refused");
        });
    }(this, alive, e->session, cmd, std::move(done), e->info.isPlaying(), e->info.durationMs);
}

} // namespace md::platform
