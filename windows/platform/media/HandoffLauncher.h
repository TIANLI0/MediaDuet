#pragma once

#include "md/handoff/Handoff.h"
#include "md/util/Scheduler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace md::platform {

// Opens a song in a music app on this PC (handoff, spec §8.7):
//  * NetEase Cloud Music: `orpheus://` web command {"cmd":"play","type":"song","id"} (its
//    `cloudmusic.exe --webcmd` URL handler, forwarded to the running instance);
//  * QQ Music: `QQMusic.exe /tencentprotocol 'tencent://QQMusic/?…cmd_0==playsong&&id_0==…'`;
//  * Spotify: `spotify:track:<id>` when the track id is known, else a search (not played).
// Song ids come from the session (`ids`) or a catalogue search (title / artist / album /
// duration only). Whether the song then plays is checked by the caller.
class HandoffLauncher {
public:
    explicit HandoffLauncher(IScheduler& sched);
    ~HandoffLauncher();

    // Apps installed on this PC, in preference order (cached; refresh() reads the registry).
    const std::vector<handoff::App>& apps() const { return apps_; }
    // Returns true when the list changed.
    bool refresh();

    struct Launched {
        bool ok = false;
        bool exact = false; // the song itself was opened (false: only a search)
        std::string error;  // handoff::error codes, or "network"
    };
    // Resolves the song (background thread) and opens it; `done` runs on the scheduler thread.
    void launch(handoff::App app, const handoff::Song& song, const media::CatalogIds& ids,
                std::function<void(const Launched&)> done);

private:
    IScheduler& sched_;
    std::vector<handoff::App> apps_;
    std::filesystem::path qqExe_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
