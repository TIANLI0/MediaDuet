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
enum class CommandAction { Play, Pause, Toggle, Next, Previous, Seek, Stop };
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

// Wire format (spec §8.3). positionMs is computed at nowMs.
json toWire(const MediaSessionInfo& s, int64_t nowMs);
json toWire(const MediaSnapshot& s, int64_t nowMs);
// Parses and validates; anchors positions at receivedAtMs (+ latencyMs).
std::optional<MediaSessionInfo> sessionFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                                std::string* err = nullptr);
std::optional<MediaSnapshot> snapshotFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                              std::string* err = nullptr);

struct MediaCommand {
    std::optional<std::string> key;
    CommandAction action = CommandAction::Toggle;
    std::optional<int64_t> positionMs;
    Origin origin = Origin::User;
    std::string reason;
};
json toWire(const MediaCommand& c);
std::optional<MediaCommand> commandFromWire(const json& j, std::string* err = nullptr);

} // namespace md::media
