#include "md/overlay/SurfacePolicy.h"

#include <algorithm>

namespace md::overlay {

bool overlayBlockedByFullscreen(NotifyState s, bool hideInFullscreen) {
    if (s == NotifyState::D3DFullScreen || s == NotifyState::PresentationMode) return true;
    return hideInFullscreen && s == NotifyState::Busy;
}

bool taskbarBlockedByFullscreen(NotifyState s) {
    return s == NotifyState::D3DFullScreen || s == NotifyState::PresentationMode || s == NotifyState::Busy;
}

TaskbarPlacement placeTaskbarWidget(const TaskbarGeometry& g, const std::string& position, int offset,
                                    float widthDip) {
    TaskbarPlacement p;
    const Rect& tb = g.tray;
    if (tb.empty()) {
        p.reason = "no_taskbar";
        return p;
    }
    if (g.autoHide) {
        p.reason = "autohide";
        return p;
    }
    if (tb.height() > tb.width()) {
        p.reason = "vertical_taskbar";
        return p;
    }
    float s = g.scale > 0 ? g.scale : 1.f;
    long gap = static_cast<long>(8 * s);
    long leftStart = tb.left, leftEnd = tb.right, rightStart = tb.left;
    long rightEnd = g.notify.left > tb.left ? g.notify.left : tb.right;
    long want = static_cast<long>(widthDip * s);
    if (g.xaml) {
        if (!g.start) {
            // The XAML taskbar is still starting (login, Explorer restart): retry,
            // never fall back to the Windows 10 geometry.
            p.reason = "taskbar_not_ready";
            return p;
        }
        leftEnd = g.start->left;
        if (g.widgets) leftStart = g.widgets->right;
        rightStart = std::max(g.start->right, g.tasksRight);
    } else {
        // Windows 10 (left-aligned task list): only the end of the task band is usable.
        leftStart = leftEnd = tb.left;
        rightStart = g.rebar.right > tb.left ? g.rebar.right - want : tb.left;
    }
    long from, to;
    if (position == "right") {
        from = rightStart + gap;
        to = rightEnd - gap;
    } else {
        from = leftStart + gap;
        to = leftEnd - gap;
    }
    long avail = to - from;
    if (avail < static_cast<long>(140 * s)) {
        p.reason = "no_space";
        return p;
    }
    long w = std::min(want, avail);
    long off = std::clamp<long>(offset, 0, avail - w);
    long x = position == "right" ? from + off : to - w - off; // "left" hugs the icons
    p.ok = true;
    p.rect = {x, tb.top, x + w, tb.bottom};
    return p;
}

TaskbarDecision decideTaskbar(const TaskbarInputs& in) {
    if (!in.enabled) return {false, "disabled"};
    if (!in.hasContent) return {false, "nothing_playing"};
    if (in.fullscreen) return {false, "fullscreen"};
    if (!in.havePlacement) return {false, in.placementError.empty() ? "unavailable" : in.placementError};
    return {true, ""};
}

bool isExpectedHideReason(const std::string& r) {
    return r == "disabled" || r == "nothing_playing" || r == "fullscreen" || r == "autohide" ||
           r == "vertical_taskbar" || r == "no_space";
}

bool ShowWatchdog::tick(int64_t now, bool wantShown, bool actuallyShown) {
    if (!wantShown || actuallyShown) {
        badSince_ = -1;
        return false;
    }
    if (badSince_ < 0) badSince_ = now;
    if (now - badSince_ < kGraceMs) return false;
    if (lastRebuild_ >= 0 && now - lastRebuild_ < kMinRebuildIntervalMs) return false;
    lastRebuild_ = now;
    badSince_ = now;
    ++rebuilds_;
    return true;
}

int64_t probeBackoffMs(int failures, int64_t baseMs, int64_t capMs) {
    if (failures <= 0) return baseMs;
    int64_t v = baseMs;
    for (int i = 0; i < failures && v < capMs; ++i) v *= 2;
    return std::min(v, capMs);
}

} // namespace md::overlay
