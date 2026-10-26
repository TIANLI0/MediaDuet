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

// Desktop lyrics (NetEase / QQ Music style): a per-pixel-alpha layered window
// rendered with Direct2D/DirectWrite (UpdateLayeredWindow).
//
// * Text is drawn from its glyph outlines: soft shadow, outline stroke, then the
//   fill, so it reads on any background. The karaoke fill is continuous with a
//   feathered edge (shared timing rules: md/lyrics/Karaoke.h).
// * Layouts: "alternate" (classic two-line KTV, rows alternate left/right and
//   only the row that changes cross-fades) and "center"; translations, when
//   shown, always use a centred line + its translation.
// * Lines wider than the window shrink a little, then scroll so the fill edge
//   stays visible.
// * Stays on top (re-asserted on foreground changes and every 2 s while shown),
//   never activates; hides only for exclusive full screen / presentation mode
//   unless hideInFullscreen is set.
// * Unlocked: draggable, hover toolbar. Locked (optionally click-through): a
//   small unlock button appears when the cursor rests over the lyrics.
// * Renders only while something changes (~60 fps only while a fill animates).
// Rendering is separate from presentation: renderOffscreen() draws a frame into
// memory without creating any window (tools/overlay_demo, tests).
class LyricsOverlay {
public:
    struct Callbacks {
        std::function<void(int x, int y)> onMoved; // physical pixels
        std::function<void(bool locked)> onLockChanged;
        std::function<void()> onHideRequested;
        std::function<void()> onOpenSettings;
        std::function<void(media::CommandAction)> onCommand;  // toolbar transport buttons
        std::function<void(int deltaDip)> onFontSize;         // toolbar A- / A+, menu sizes
        std::function<void(int deltaMs, bool reset)> onOffset; // lyrics earlier / later / reset (per app)
    };
    struct Strings {
        std::wstring lock, unlock, hide, settings;
        std::wstring earlier, later;   // "Lyrics earlier 0.1 s"
        std::wstring resetOffset;      // contains "%s" for the current offset, e.g. "+0.3 s"
        std::wstring fontSize;         // text size submenu, contains "%s" for the current size
        std::wstring previous, playPause, next, smaller, larger, close;
        std::wstring unlockTip;        // tooltip of the unlock button
    };
    struct TestOptions {
        bool offscreen = false;
        std::function<double()> now; // steady ms; default lyrics::steadyNowMs
        UINT dpi = 96;
    };

    LyricsOverlay(IScheduler& sched, Callbacks cb, TestOptions test = {});
    ~LyricsOverlay();

    void configure(const nlohmann::json& overlay, bool animations, bool lowPower, const Strings& strings);
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    void setLyrics(std::shared_ptr<const lyrics::Lyrics> lyrics, const std::string& trackId);
    // Shared smoothed clock; delayMs = total lyrics delay (display = clock − delay);
    // manualOffsetMs = the per-app part (shown in the menu); canOffset = there is an
    // app to attach an offset to.
    void setTiming(std::shared_ptr<const lyrics::LyricsClock> clock, int64_t delayMs, int64_t manualOffsetMs,
                   bool canOffset);
    bool windowVisible() const;
    // Writes the last rendered frame (premultiplied BGRA) to a PNG file; for
    // diagnostics. Returns false if nothing was rendered.
    bool snapshot(const std::wstring& pngPath) const;
    // Renders the current state at the (test) time into premultiplied BGRA
    // without any window; size receives the frame size in pixels.
    bool renderOffscreen(bool hover, std::vector<uint8_t>& bgra, SIZE& size);
    // The unlock button as it appears over locked lyrics (offscreen).
    bool renderUnlockButtonOffscreen(std::vector<uint8_t>& bgra, SIZE& size);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool enabled_ = false;
};

} // namespace md::platform
