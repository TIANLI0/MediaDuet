#pragma once

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Handoff ("continue on the other device", spec §8.7): the song a player plays on one device
// continues in a music app on the other one, at the same position, and the source pauses.
namespace md::handoff {

using json = nlohmann::json;

// Music apps a song can be handed to (same ids on both platforms).
enum class App { Netease, QqMusic, Spotify };
const char* toString(App a);
std::optional<App> appFromString(std::string_view s);
// The handoff app a media session belongs to, from its app id (Windows exe name / AUMID,
// Android package); nullopt for any other player.
std::optional<App> appOfSession(std::string_view appId);
json toJson(const std::vector<App>& apps);
// Lenient: unknown ids are skipped, duplicates removed, order kept.
std::vector<App> appsFromJson(const json& j);

struct Song {
    std::string title, artist, album;
    std::optional<int64_t> durationMs;
};
Song songOf(const media::MediaSessionInfo& s);
json toJson(const Song& s);
std::optional<Song> songFromJson(const json& j);
// Search keyword: the title without bracketed suffixes plus the first artist.
std::string searchQuery(const Song& s);

// True when session `s` shows `want`: the titles are equal after lyrics::normalize (or one
// contains the other) and the durations are within 5 s when both are known. Artists are not
// compared: catalogues spell them differently ("冯沁苑(买辣椒也用券)" / "买辣椒也用券").
bool shows(const media::MediaSessionInfo& s, const Song& want);
// The session of `app` in `snap` that shows `want`, preferring one that plays; nullptr if none.
const media::MediaSessionInfo* findTarget(const media::MediaSnapshot& snap, App app, const Song& want);

// ---- Windows desktop clients --------------------------------------------------------------
// NetEase Cloud Music: `orpheus://` + base64(JSON), handled by `cloudmusic.exe --webcmd`.
std::string neteaseDesktopCommand(const json& message);
std::string neteaseDesktopPlay(std::string_view songId);       // {"cmd":"play","type":"song","id":…}
std::string neteaseDesktopSeek(int64_t positionMs);            // {"cmd":"seek","value":seconds}
// QQ Music: `QQMusic.exe /tencentprotocol '<this>'` (numeric song id).
std::string qqDesktopProtocol(int64_t songId);
std::string spotifyTrackUri(std::string_view trackId);         // spotify:track:<id>
std::string spotifySearchUri(const Song& s);                   // spotify:search:<query>
// A Spotify track id (22 base62 chars) from a spotify:track: URI or an open.spotify.com link.
std::optional<std::string> spotifyTrackId(std::string_view uriOrUrl);

// Error codes of handoff.result (spec §8.7).
namespace error {
inline constexpr const char* AppUnavailable = "app_unavailable";
inline constexpr const char* NotFound = "not_found";
inline constexpr const char* LaunchFailed = "launch_failed";
inline constexpr const char* Timeout = "timeout";
inline constexpr const char* NoSession = "no_session";
inline constexpr const char* Busy = "busy";
inline constexpr const char* LinkLost = "link_lost";
inline constexpr const char* Unsupported = "unsupported";
inline constexpr const char* Network = "network"; // the catalogue lookup failed (try again)
} // namespace error

enum class Device { Pc, Phone };
const char* toString(Device d);
inline Device other(Device d) { return d == Device::Pc ? Device::Phone : Device::Pc; }

// What the device that takes over reports (handoff.result of handoff.play).
struct PlayResult {
    bool ok = false;
    // "playing": `key` plays the song; "opened": the app shows the song (or a search for it)
    // but playback could not be confirmed, so the source keeps playing.
    std::string state;
    std::string key;
    std::string error;
};
PlayResult playResultFromJson(const json& j);
json toJson(const PlayResult& r);

struct Outcome {
    bool ok = false;
    std::string state; // playing | opened
    std::string error;
    Device to = Device::Pc;
    std::optional<App> app;
    std::string title;
};
json toJson(const Outcome& o);

// Runs one handoff at a time (the Windows host is the coordinator for both directions):
// asks the other device to play the source's song in a chosen app, and once it plays,
// pauses the source (a user pause: arbitration never resumes it) and moves the target to
// the source's live position.
class Coordinator {
public:
    struct Env {
        std::function<const media::MediaSessionInfo*(Device, const std::string& key)> session;
        // Apps that can take a song on that device, in preference order.
        std::function<std::vector<App>(Device)> apps;
        std::function<void(Device to, App app, const Song& song, const media::CatalogIds& ids,
                           std::function<void(const PlayResult&)> done)>
            play;
        std::function<void(Device, const std::string& key)> pause;
        std::function<void(Device, const std::string& key, int64_t positionMs)> seek;
        // Delay until a command reaches that device (the phone: half the link RTT).
        std::function<int64_t(Device)> commandLatencyMs;
    };
    struct Status {
        bool active = false;
        Device to = Device::Pc;
        std::optional<App> app;
        std::string title;
        bool waitingForTap = false; // the phone asks the user to tap a notification
    };

    Coordinator(IScheduler& sched, Env env, std::function<void()> changed = {});
    ~Coordinator();

    // The app a handoff of `source` to `to` would use: `wanted` if available there, else the
    // source's own app, else the first one; nullopt when `to` has none.
    std::optional<App> chooseApp(Device to, const media::MediaSessionInfo& source, std::optional<App> wanted) const;
    // Starts moving session `key` of `from` to the other device; `done` is called exactly once.
    void start(Device from, const std::string& key, std::optional<App> app, std::function<void(const Outcome&)> done);
    // The phone showed a "tap to continue" notification (handoff.progress).
    void onWaitingForTap();
    // The link to the phone dropped: a handoff that involves the phone fails.
    void onLinkLost();
    const Status& status() const { return status_; }

    static constexpr int64_t kTimeoutMs = 120'000;
    // Shorter than this into the song: start from the beginning, no seek.
    static constexpr int64_t kMinSeekMs = 3'000;

private:
    void finish(Outcome o);

    IScheduler& sched_;
    Env env_;
    std::function<void()> changed_;
    Status status_;
    struct Run {
        uint64_t id = 0;
        Device from = Device::Pc;
        std::string key;
        Song song;
        std::function<void(const Outcome&)> done;
    };
    std::optional<Run> run_;
    uint64_t seq_ = 0;
    ScopedTimer timeout_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::handoff
