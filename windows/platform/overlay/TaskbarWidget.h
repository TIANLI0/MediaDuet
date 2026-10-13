#pragma once

#include "md/lyrics/Lrc.h"
#include "md/lyrics/LyricsClock.h"
#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace md::platform {

// Lyrics / mini player shown inside the Windows taskbar (Windows 11 and 10).
//
// A per-pixel-alpha layered popup that is *not* owned by any Explorer window
// (a cross-process owner attaches the two threads' input queues, so a busy host
// thread could hang the taskbar and make Explorer restart it, destroying an owned
// widget with it). It stays above the taskbar by re-asserting HWND_TOPMOST when
// the taskbar comes to the front. Free space is measured with UI Automation on a
// dedicated MTA worker thread with UIA timeouts; the host thread never blocks on
// Explorer. Geometry is cached and re-queried only when the taskbar changes.
// Recovers by itself from Explorer restarts (TaskbarCreated), a destroyed window
// or failing probes (watchdog). Renders only while something changes.
class TaskbarWidget {
public:
    struct Callbacks {
        std::function<void(media::CommandAction)> onCommand;
        std::function<void()> onOpen;
        std::function<void(bool shown, const std::string& reason)> onState;
    };
    // Offscreen mode (tools/tests): no window, hook, timer-driven probes or UIA;
    // frames are rendered with renderOffscreen() at the injected time.
    struct TestOptions {
        bool offscreen = false;
        std::function<double()> now; // steady ms; default lyrics::steadyNowMs
        UINT dpi = 96;
        bool light = false;
    };
    TaskbarWidget(IScheduler& sched, Callbacks cb, TestOptions test = {});
    ~TaskbarWidget();

    void configure(const nlohmann::json& taskbar, bool animations, bool lowPower);
    void setTrack(const media::MediaSessionInfo* session);
    void setLyrics(std::shared_ptr<const lyrics::Lyrics> lyrics, const std::string& trackId);
    // Shared smoothed playback clock + total lyrics delay (display = clock − delay).
    void setTiming(std::shared_ptr<const lyrics::LyricsClock> clock, int64_t delayMs);
    // Explorer (re)created the taskbar: drop cached geometry, rebuild and re-measure.
    void onTaskbarCreated();
    bool shown() const;
    const std::string& reason() const;
    // Writes the last frame to PNG (diagnostics / tests).
    bool snapshot(const std::wstring& pngPath) const;
    // Renders one frame of widthPx x heightPx into premultiplied BGRA (no window).
    bool renderOffscreen(int widthPx, int heightPx, bool hover, std::vector<uint8_t>& bgra);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace md::platform
