#pragma once

// Pure decision logic of the desktop lyrics overlay and the taskbar widget
// (platform/overlay). Kept free of Win32 so it is unit-tested.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace md::overlay {

// SHQueryUserNotificationState values (QUERY_USER_NOTIFICATION_STATE).
enum class NotifyState : int {
    NotPresent = 1,
    Busy = 2,               // a full-screen (often borderless) window is in front
    D3DFullScreen = 3,      // exclusive full-screen D3D
    PresentationMode = 4,
    AcceptsNotifications = 5,
    QuietTime = 6,
    App = 7,
};

// Desktop lyrics: exclusive full screen and presentation mode always hide it
// (nothing can be drawn over exclusive mode anyway); a borderless full-screen
// window (Busy) only when hideInFullscreen is set.
bool overlayBlockedByFullscreen(NotifyState s, bool hideInFullscreen);
// Taskbar widget: hidden whenever the taskbar itself is covered (Busy included).
bool taskbarBlockedByFullscreen(NotifyState s);

struct Rect {
    long left = 0, top = 0, right = 0, bottom = 0;
    long width() const { return right - left; }
    long height() const { return bottom - top; }
    bool empty() const { return right <= left || bottom <= top; }
    bool operator==(const Rect&) const = default;
};

// What the UI Automation / window probes found out about the taskbar, in
// physical screen pixels. Every item is something the widget must never cover.
//
// Layout model (horizontal taskbars, top or bottom docked): the occupied
// x-intervals along the bar are Start, the task group (search box / task view /
// pinned and running app buttons), widgets, the system tray (from its left edge
// to the end of the bar) and anything else found. The "task group" anchor is
// Start + `group`, grown by every item overlapping or touching it. The free gaps
// between occupied intervals are the candidate areas:
//   "left"  = the gap immediately left of the anchor (x = gapEnd - pad - w - offset,
//             hugging the icons),
//   "right" = the gap immediately right of the anchor up to the next item
//             (x = gapStart + pad + offset).
// The preferred side's nearest gap is used if it holds the minimum width
// (140 DIP + 8 DIP padding on each side), else the other side's nearest gap,
// else any farther gap (preferred side first); the width shrinks to the gap
// (never below the minimum); offset is clamped to the gap. Nothing fits →
// "no_space".
//
//   Windows 11, centred:      [widgets]   gap L   [Start search tasks…]   gap R   [tray]
//   Windows 11, left aligned: [Start search tasks…]   gap R   [widgets][tray]  (L too small → R)
//   Windows 10:               [Start][search][task view][buttons… |band end]   gap R   [tray]
struct TaskbarGeometry {
    Rect tray;                   // Shell_TrayWnd
    float scale = 1.f;           // DPI / 96
    bool autoHide = false;
    bool xaml = false;           // Windows 11 XAML taskbar (no Start → still starting)
    std::optional<Rect> start;   // Start button
    std::vector<Rect> group;     // search box, task view, pinned / running app buttons (with Start)
    std::optional<Rect> widgets; // Widgets / weather (Windows 11)
    std::optional<Rect> notify;  // system tray: its left edge bounds everything (else the bar's end)
    std::vector<Rect> occupied;  // anything else: toolbars, unknown buttons, an unmeasured task band
};

struct TaskbarPlacement {
    bool ok = false;
    Rect rect;
    std::string reason; // when !ok: no_taskbar, autohide, vertical_taskbar, taskbar_not_ready, no_space
    bool fellBack = false; // placed on the other side than requested (diagnostics)
};

inline constexpr float kTaskbarWidgetMinDip = 140.f;
inline constexpr float kTaskbarWidgetPadDip = 8.f;

// position = "left" | "right" (see above); offset in physical pixels; width in DIPs.
// Vertical (left/right docked) taskbars are not supported → "vertical_taskbar".
TaskbarPlacement placeTaskbarWidget(const TaskbarGeometry& g, const std::string& position, int offset,
                                    float widthDip);

// Show/hide decision of the taskbar widget.
struct TaskbarInputs {
    bool enabled = false;
    bool hasContent = false;
    bool fullscreen = false;
    bool havePlacement = false;          // a valid rect is known
    std::string placementError;          // why there is none (from placeTaskbarWidget / probes)
};
struct TaskbarDecision {
    bool show = false;
    std::string reason; // "" when shown; disabled, nothing_playing, fullscreen, autohide, …
    bool operator==(const TaskbarDecision&) const = default;
};
TaskbarDecision decideTaskbar(const TaskbarInputs& in);
// Reasons that are legitimate user/system states rather than failures; the
// watchdog does not try to recover from them.
bool isExpectedHideReason(const std::string& reason);

// Watchdog: the widget should be visible but has not been for too long (window
// destroyed with Explorer, surface lost, probes failing) → rebuild.
class ShowWatchdog {
public:
    static constexpr int64_t kGraceMs = 5000;
    static constexpr int64_t kMinRebuildIntervalMs = 10000;
    // wantShown: decision says show, or a failure reason (not expected); actuallyShown:
    // window valid and visible. Returns true when a rebuild should happen now.
    bool tick(int64_t nowMs, bool wantShown, bool actuallyShown);
    int rebuilds() const { return rebuilds_; }

private:
    int64_t badSince_ = -1;
    int64_t lastRebuild_ = -1;
    int rebuilds_ = 0;
};

// Exponential back-off for failing probes: 2 s, 4 s, … capped.
int64_t probeBackoffMs(int failures, int64_t baseMs = 2000, int64_t capMs = 60000);

} // namespace md::overlay
