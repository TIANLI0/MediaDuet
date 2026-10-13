#include "md/overlay/SurfacePolicy.h"

#include <doctest/doctest.h>

using namespace md::overlay;

TEST_CASE("full-screen rules: overlay stays over borderless full screen by default") {
    CHECK(overlayBlockedByFullscreen(NotifyState::D3DFullScreen, false));
    CHECK(overlayBlockedByFullscreen(NotifyState::PresentationMode, false));
    CHECK_FALSE(overlayBlockedByFullscreen(NotifyState::Busy, false));
    CHECK(overlayBlockedByFullscreen(NotifyState::Busy, true));
    CHECK_FALSE(overlayBlockedByFullscreen(NotifyState::AcceptsNotifications, true));
    CHECK(taskbarBlockedByFullscreen(NotifyState::Busy));
    CHECK_FALSE(taskbarBlockedByFullscreen(NotifyState::QuietTime));
}

static TaskbarGeometry win11() {
    TaskbarGeometry g;
    g.tray = {0, 1032, 1920, 1080};
    g.notify = {1700, 1032, 1920, 1080};
    g.xaml = true;
    g.start = Rect{800, 1032, 848, 1080};
    g.widgets = Rect{0, 1032, 160, 1080};
    g.tasksRight = 1120;
    return g;
}

TEST_CASE("taskbar placement: Windows 11 left and right areas") {
    auto g = win11();
    auto l = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(l.ok);
    CHECK(l.rect == Rect{432, 1032, 792, 1080}); // hugs the Start button (gap 8)
    auto off = placeTaskbarWidget(g, "left", 100, 360);
    CHECK(off.rect.left == 332);
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{1128, 1032, 1488, 1080});
    g.tasksRight = 1600;
    CHECK(placeTaskbarWidget(g, "right", 0, 360).reason == "no_space");
}

TEST_CASE("taskbar placement: XAML taskbar without Start button is not ready, not Windows 10") {
    auto g = win11();
    g.start.reset();
    auto p = placeTaskbarWidget(g, "left", 0, 360);
    CHECK_FALSE(p.ok);
    CHECK(p.reason == "taskbar_not_ready");
    g.autoHide = true;
    CHECK(placeTaskbarWidget(g, "left", 0, 360).reason == "autohide");
    TaskbarGeometry v;
    v.tray = {0, 0, 48, 1080};
    CHECK(placeTaskbarWidget(v, "left", 0, 360).reason == "vertical_taskbar");
    CHECK(placeTaskbarWidget(TaskbarGeometry{}, "left", 0, 360).reason == "no_taskbar");
}

TEST_CASE("taskbar placement: Windows 10 uses the end of the task band") {
    TaskbarGeometry g;
    g.tray = {0, 1040, 1920, 1080};
    g.notify = {1650, 1040, 1920, 1080};
    g.rebar = {100, 1040, 1600, 1080};
    auto p = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(p.ok);
    CHECK(p.rect.left == 1248);
    CHECK(placeTaskbarWidget(g, "left", 0, 360).reason == "no_space");
}

TEST_CASE("taskbar decision and expected reasons") {
    TaskbarInputs in;
    CHECK(decideTaskbar(in) == TaskbarDecision{false, "disabled"});
    in.enabled = true;
    CHECK(decideTaskbar(in).reason == "nothing_playing");
    in.hasContent = true;
    CHECK(decideTaskbar(in).reason == "unavailable");
    in.placementError = "taskbar_not_ready";
    CHECK(decideTaskbar(in).reason == "taskbar_not_ready");
    in.havePlacement = true;
    CHECK(decideTaskbar(in) == TaskbarDecision{true, ""});
    in.fullscreen = true;
    CHECK(decideTaskbar(in).reason == "fullscreen");
    CHECK(isExpectedHideReason("fullscreen"));
    CHECK(isExpectedHideReason("autohide"));
    CHECK_FALSE(isExpectedHideReason("taskbar_not_ready"));
    CHECK_FALSE(isExpectedHideReason("no_taskbar"));
    CHECK_FALSE(isExpectedHideReason("unavailable"));
}

TEST_CASE("show watchdog rebuilds after 5 s, rate limited") {
    ShowWatchdog w;
    CHECK_FALSE(w.tick(0, true, true));
    CHECK_FALSE(w.tick(1000, true, false)); // window lost at 1 s
    CHECK_FALSE(w.tick(5000, true, false));
    CHECK(w.tick(6000, true, false));
    CHECK(w.rebuilds() == 1);
    CHECK_FALSE(w.tick(12000, true, false)); // < 10 s since the last rebuild
    CHECK(w.tick(16000, true, false));
    CHECK_FALSE(w.tick(17000, true, true)); // recovered
    CHECK_FALSE(w.tick(30000, false, false)); // legitimately hidden
    CHECK(w.rebuilds() == 2);
}

TEST_CASE("probe back-off") {
    CHECK(probeBackoffMs(0) == 2000);
    CHECK(probeBackoffMs(1) == 4000);
    CHECK(probeBackoffMs(3) == 16000);
    CHECK(probeBackoffMs(20) == 60000);
}
