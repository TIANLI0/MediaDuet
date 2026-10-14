#include "md/overlay/SurfacePolicy.h"

#include <algorithm>
#include <cmath>

namespace md::overlay {

bool overlayBlockedByFullscreen(NotifyState s, bool hideInFullscreen) {
    if (s == NotifyState::D3DFullScreen || s == NotifyState::PresentationMode) return true;
    return hideInFullscreen && s == NotifyState::Busy;
}

bool taskbarBlockedByFullscreen(NotifyState s) {
    return s == NotifyState::D3DFullScreen || s == NotifyState::PresentationMode || s == NotifyState::Busy;
}

namespace {
struct Span {
    long l = 0, r = 0;
};
} // namespace

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
        // Left/right docked (Windows 10): a bar a few dozen pixels wide has no room
        // for a line of text.
        p.reason = "vertical_taskbar";
        return p;
    }
    if (g.xaml && !g.start) {
        // The XAML taskbar is still starting (login, Explorer restart): retry,
        // never fall back to a guessed geometry.
        p.reason = "taskbar_not_ready";
        return p;
    }

    // Items as x-intervals clipped to the bar; empty ones and ones outside the
    // bar's vertical band (stale / other monitor) are ignored.
    auto clip = [&](const Rect& r) -> std::optional<Span> {
        if (r.right <= r.left) return std::nullopt;
        if (r.bottom > r.top && (r.bottom <= tb.top || r.top >= tb.bottom)) return std::nullopt;
        long l = std::max(r.left, tb.left), rr = std::min(r.right, tb.right);
        if (rr <= l) return std::nullopt;
        return Span{l, rr};
    };
    std::optional<Span> anchor; // Start + the task group
    auto addAnchor = [&](const Rect& r) {
        if (auto sp = clip(r)) anchor = anchor ? Span{std::min(anchor->l, sp->l), std::max(anchor->r, sp->r)} : *sp;
    };
    if (g.start) addAnchor(*g.start);
    for (const auto& r : g.group) addAnchor(r);
    std::vector<Span> items;
    if (g.widgets)
        if (auto sp = clip(*g.widgets)) items.push_back(*sp);
    for (const auto& r : g.occupied)
        if (auto sp = clip(r)) items.push_back(*sp);
    if (g.notify)
        if (auto sp = clip(*g.notify)) items.push_back({sp->l, tb.right}); // tray to the end of the bar
    if (!anchor) anchor = Span{tb.left, tb.left};
    // Whatever overlaps or touches the group belongs to it (e.g. an overflow button).
    for (bool grown = true; grown;) {
        grown = false;
        for (const auto& it : items)
            if (it.l <= anchor->r && it.r >= anchor->l && (it.l < anchor->l || it.r > anchor->r)) {
                anchor = Span{std::min(anchor->l, it.l), std::max(anchor->r, it.r)};
                grown = true;
            }
    }
    std::sort(items.begin(), items.end(), [](const Span& a, const Span& b) { return a.l < b.l; });

    // Free gaps on each side of the anchor, nearest first.
    std::vector<Span> leftGaps, rightGaps;
    long cur = tb.left;
    for (const auto& it : items) {
        if (it.r > anchor->l) continue;
        if (it.l > cur) leftGaps.push_back({cur, it.l});
        cur = std::max(cur, it.r);
    }
    if (anchor->l > cur) leftGaps.push_back({cur, anchor->l});
    std::reverse(leftGaps.begin(), leftGaps.end());
    cur = anchor->r;
    for (const auto& it : items) {
        if (it.l < anchor->r) continue;
        if (it.l > cur) rightGaps.push_back({cur, it.l});
        cur = std::max(cur, it.r);
    }
    if (tb.right > cur) rightGaps.push_back({cur, tb.right});

    float s = g.scale > 0 ? g.scale : 1.f;
    long pad = std::lround(kTaskbarWidgetPadDip * s);
    long minW = std::lround(kTaskbarWidgetMinDip * s);
    long want = std::lround(widthDip * s);
    if (want <= 0) want = minW;
    long need = std::min(want, minW);
    bool preferRight = position == "right";
    struct Candidate {
        Span gap;
        bool right;
    };
    std::vector<Candidate> order;
    auto& pref = preferRight ? rightGaps : leftGaps;
    auto& other = preferRight ? leftGaps : rightGaps;
    if (!pref.empty()) order.push_back({pref[0], preferRight});
    if (!other.empty()) order.push_back({other[0], !preferRight});
    for (size_t i = 1; i < pref.size(); ++i) order.push_back({pref[i], preferRight});
    for (size_t i = 1; i < other.size(); ++i) order.push_back({other[i], !preferRight});
    for (const auto& c : order) {
        long avail = c.gap.r - c.gap.l - 2 * pad;
        if (avail < need) continue;
        long w = std::min(want, avail);
        long off = std::clamp<long>(offset, 0, avail - w);
        long x = c.right ? c.gap.l + pad + off : c.gap.r - pad - w - off; // both hug the icons
        p.ok = true;
        p.fellBack = c.right != preferRight;
        p.rect = {x, tb.top, x + w, tb.bottom};
        return p;
    }
    p.reason = "no_space";
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
