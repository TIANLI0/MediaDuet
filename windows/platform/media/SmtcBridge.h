#pragma once

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <windows.h>

#include <winrt/Windows.Media.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace md::platform {

// Presents the phone's active media session as a Windows SystemMediaTransportControls
// session (media flyout, keyboard media keys, lock screen) without playing audio.
// Verified feasible on Windows 11 in Phase 0 (session appears in GSMTC).
// The bridged session is excluded from GsmtcSource, so it is never mirrored back.
class SmtcBridge {
public:
    using CommandFn = std::function<void(media::CommandAction, std::optional<int64_t> positionMs)>;
    SmtcBridge(IScheduler& sched, CommandFn onCommand);
    ~SmtcBridge();

    // hwnd: a top-level window owned by this process (may be hidden).
    bool init(HWND hwnd);
    void setEnabled(bool enabled);
    // remote == nullptr hides the session.
    void update(const media::MediaSessionInfo* remote, const std::filesystem::path& artworkPath,
                const std::string& deviceName);
    bool active() const { return shown_; }

private:
    void hide();
    void setThumbnail(const std::filesystem::path& p);

    IScheduler& sched_;
    CommandFn onCommand_;
    winrt::Windows::Media::SystemMediaTransportControls smtc_{nullptr};
    winrt::event_token buttonTok_{}, seekTok_{};
    bool enabled_ = true;
    bool shown_ = false;
    std::string lastTrack_;
    std::string lastArt_;
    media::PlaybackStatus lastStatus_ = media::PlaybackStatus::Unknown;
    uint32_t lastActions_ = 0;
    std::optional<int64_t> lastPos_;
    int64_t lastPosAt_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
