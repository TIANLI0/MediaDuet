#pragma once

// NetEase Cloud Music for Windows keeps its audio player in static objects of
// cloudmusic.dll. They are found by code signatures (instructions that address
// them RIP-relative), which survive rebuilds of the same major version, and are
// read with PROCESS_VM_READ only (platform/media/NeteaseMemory). This file holds
// the layout and the pure logic so it can be unit tested.

#include "md/media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace md::media::netease {

// "48 8D 0D ? ? ? ?": hex bytes, `?` matches any byte.
class BytePattern {
public:
    explicit BytePattern(std::string_view text);
    size_t size() const { return bytes_.size(); }
    // Offset of the first match in [data, data + size), if any.
    std::optional<size_t> find(const uint8_t* data, size_t size) const;

private:
    std::vector<int> bytes_; // -1 = wildcard
};

struct Signature {
    const char* pattern;
    size_t dispOffset; // offset of the RIP-relative disp32 inside the match
};
// `lea rcx, [AudioPlayer]` in the player's static initialiser.
extern const Signature kPlayerSignature;
// `ucomisd xmm1, [schedule]`: the current position in seconds (double).
extern const Signature kScheduleSignature;

// Address a RIP-relative operand refers to: the disp32 is relative to the end of
// the instruction, which ends right after it in both signatures.
inline uint64_t ripTarget(uint64_t dispAddress, int32_t disp) { return dispAddress + 4 + static_cast<int64_t>(disp); }

// AudioPlayer layout (offsets from the object).
inline constexpr uint64_t kPlayInfoOffset = 0x50; // AudioPlayInfo*
inline constexpr uint64_t kStatusOffset = 0x60;   // int32, PlayerStatus
inline constexpr uint64_t kDurationOffset = 0xA8; // double, seconds
// AudioPlayInfo: MSVC std::string "<song id>_<suffix>" at +0x10 (16-byte buffer, size, capacity).
inline constexpr uint64_t kPlayInfoIdOffset = 0x10;
inline constexpr uint64_t kMaxIdLength = 64;

enum class PlayerStatus { Idle = 0, Playing = 1, Paused = 2 };

struct PlayerReading {
    int status = 0;
    double positionSec = 0;
    double durationSec = 0;
    int64_t songId = 0; // 0 for local files and when unknown
};

// "3340112782_YKR00X" -> 3340112782; 0 when it does not start with a number.
int64_t parseSongId(std::string_view raw);
// A layout that changed with an update reads garbage: reject what cannot be a player state.
bool plausible(const PlayerReading& r);

// NetEase's play queue as it saves it (%LOCALAPPDATA%/NetEase/CloudMusic/webdata/file/playingList).
struct PlayingList {
    struct Entry {
        int64_t id = 0;              // song id (0: a local file, cannot be played by id)
        std::string title, subtitle; // name, artists
        std::string source;          // where it was added from (a playlist, "liked songs" …)
    };
    std::vector<Entry> entries; // in NetEase's display order
    std::string rev;            // changes whenever the list does
};
std::optional<PlayingList> parsePlayingList(std::string_view text);
// media.queue of session `key`: the whole list, or `max` entries around `activeId` (spec §8.3).
MediaQueue queueWindow(const PlayingList& list, const std::string& key, std::optional<int64_t> activeId, size_t max = 300);

} // namespace md::media::netease
