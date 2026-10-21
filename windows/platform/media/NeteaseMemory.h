#pragma once

#include "md/media/NeteasePlayer.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>

namespace md::platform {

// Read-only view of NetEase Cloud Music's audio player (md/media/NeteasePlayer.h):
// the process is opened with PROCESS_VM_READ only; nothing is written or injected.
class NeteaseMemory {
public:
    // Locates the player in cloudmusic.dll of process `pid` (blocking, ~50 ms: worker thread only).
    // nullptr with the reason in *why when this NetEase build is not recognised.
    static std::unique_ptr<NeteaseMemory> open(DWORD pid, std::string* why);
    ~NeteaseMemory();
    NeteaseMemory(const NeteaseMemory&) = delete;
    NeteaseMemory& operator=(const NeteaseMemory&) = delete;

    // Current state; nullopt when the process is gone or the values are not plausible.
    std::optional<media::netease::PlayerReading> read() const;

private:
    NeteaseMemory(HANDLE process, uint64_t player, uint64_t schedule)
        : process_(process), player_(player), schedule_(schedule) {}
    HANDLE process_;
    uint64_t player_, schedule_;
};

} // namespace md::platform
