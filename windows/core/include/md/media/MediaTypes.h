#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace md::media {

using json = nlohmann::json;

enum class PlaybackStatus { Unknown, Playing, Paused, Stopped, Buffering };
enum class MediaKind { Music, Video, Other };
// Custom (the player's own button, MediaCommand::customAction) and PlayQueueItem
// (MediaCommand::queueId) are never part of a session's `actions` bit set.
enum class CommandAction { Play, Pause, Toggle, Next, Previous, Seek, Stop, Custom, PlayQueueItem };
enum class Origin { User, Policy };

namespace action {
inline constexpr uint32_t Play = 1u << 0;
inline constexpr uint32_t Pause = 1u << 1;
inline constexpr uint32_t Toggle = 1u << 2;
inline constexpr uint32_t Next = 1u << 3;
inline constexpr uint32_t Previous = 1u << 4;
inline constexpr uint32_t Seek = 1u << 5;
inline constexpr uint32_t Stop = 1u << 6;
} // namespace action

const char* toString(PlaybackStatus s);
std::optional<PlaybackStatus> statusFromString(std::string_view s);
const char* toString(MediaKind k);
const char* toString(CommandAction a);
std::optional<CommandAction> actionFromString(std::string_view s);
uint32_t actionBit(CommandAction a);
const char* toString(Origin o);

struct ArtworkRef {
    std::string hash; // sha256 hex of the encoded image
    std::string mime;
    uint32_t size = 0;
    bool operator==(const ArtworkRef&) const = default;
};

// A player's own button (Android custom action: like, favourite, ...); spec §8.3.
struct CustomAction {
    std::string id;
    std::string title;
    std::optional<ArtworkRef> icon; // small PNG with alpha, fetched like artwork
    bool operator==(const CustomAction&) const = default;
};
inline constexpr size_t kMaxCustomActions = 8;

// The session exposes a play queue: fetch it (media.queue.get) when rev changes.
struct QueueHint {
    int count = 0;
    std::string rev;
    std::optional<int64_t> activeId;
    bool operator==(const QueueHint&) const = default;
};

// The current track's ids in music catalogues, when the player exposes them (spec §8.3
// `ids`): NetEase song id (decimal), QQ Music song mid, Spotify track id. Used by handoff.
struct CatalogIds {
    std::string netease;
    std::string qqmusic;
    std::string spotify;
    bool empty() const { return netease.empty() && qqmusic.empty() && spotify.empty(); }
    bool operator==(const CatalogIds&) const = default;
};
json toJson(const CatalogIds& ids);
// Lenient: invalid entries (not 1..64 ASCII letters / digits) are dropped.
CatalogIds catalogIdsFromJson(const json& j);

struct QueueEntry {
    int64_t id = 0;
    std::string title;
    std::string subtitle;
};

// media.queue: a window of up to 300 entries (offset = index of the first one).
struct MediaQueue {
    std::string key;
    std::string rev;
    std::string title;
    std::optional<int64_t> activeId;
    int total = 0;
    int offset = 0;
    std::vector<QueueEntry> items;
};
std::optional<MediaQueue> queueFromWire(const json& j, std::string* err = nullptr);
json toJson(const MediaQueue& q);

struct MediaSessionInfo {
    std::string key;
    std::string appId;
    std::string appName;
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string trackId;
    std::optional<int64_t> durationMs;
    std::optional<int64_t> positionMs; // valid at positionAtMs
    double rate = 1.0;
    PlaybackStatus status = PlaybackStatus::Unknown;
    uint32_t actions = 0;
    std::optional<ArtworkRef> artwork;
    MediaKind kind = MediaKind::Music;
    // The app's own volume (Windows per-app mixer), when controllable.
    std::optional<float> appVolume;
    bool appMuted = false;
    // Estimated delay (ms, 0..2000) between the reported position and the sound
    // being audible on the playing device (e.g. a Bluetooth headset); spec §8.3.
    std::optional<int> outputLatencyMs;
    std::vector<CustomAction> customActions;
    std::optional<QueueHint> queue;
    CatalogIds ids;

    // Local monotonic time (ms) at which positionMs was valid. Not serialised.
    int64_t positionAtMs = 0;

    bool supports(CommandAction a) const { return (actions & actionBit(a)) != 0; }
    bool isPlaying() const { return status == PlaybackStatus::Playing; }
    // Extrapolated position at the given monotonic time, clamped to duration.
    std::optional<int64_t> positionAt(int64_t nowMs) const;
    // True if everything except the naturally advancing position is equal and the
    // positions are consistent (|drift| below toleranceMs).
    bool equivalent(const MediaSessionInfo& o, int64_t toleranceMs = 1500) const;
    // Fills trackId from title/artist/album/duration.
    void updateTrackId();
};

struct MediaSnapshot {
    uint64_t gen = 0;
    std::optional<std::string> active;
    std::vector<MediaSessionInfo> sessions;

    const MediaSessionInfo* find(const std::string& key) const;
    const MediaSessionInfo* activeSession() const;
    bool equivalent(const MediaSnapshot& o, int64_t toleranceMs = 1500) const;
};

inline constexpr size_t kMaxSessions = 16;

// Spec §8.3: first 16 hex chars of SHA-256(title 0x1F artist 0x1F album 0x1F dur).
std::string computeTrackId(const std::string& title, const std::string& artist, const std::string& album,
                           std::optional<int64_t> durationMs);

// Truncates over-long strings to the protocol limits (spec §6.1).
void sanitize(MediaSessionInfo& s);

// Wire format (spec §8.3). positionMs is computed at nowMs (sent as the snapshot's `at`).
json toWire(const MediaSessionInfo& s, int64_t nowMs);
json toWire(const MediaSnapshot& s, int64_t nowMs);
// Parses and validates; anchors positions at receivedAtMs (+ latencyMs).
std::optional<MediaSessionInfo> sessionFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                                std::string* err = nullptr);
// The sender's `at` (its monotonic ms for the positions), when present and valid.
std::optional<int64_t> snapshotAt(const json& j);
std::optional<MediaSnapshot> snapshotFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                              std::string* err = nullptr);

struct MediaCommand {
    std::optional<std::string> key;
    CommandAction action = CommandAction::Toggle;
    std::optional<int64_t> positionMs;
    Origin origin = Origin::User;
    std::string reason;
    std::string customAction;       // CommandAction::Custom
    std::optional<int64_t> queueId; // CommandAction::PlayQueueItem
};
json toWire(const MediaCommand& c);
std::optional<MediaCommand> commandFromWire(const json& j, std::string* err = nullptr);

} // namespace md::media
