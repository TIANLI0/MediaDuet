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

// Bars are 1032..1080 (bottom); items use the same vertical band.
static Rect bx(long l, long r, long top = 1032, long bottom = 1080) { return {l, top, r, bottom}; }

static bool overlaps(const Rect& a, const Rect& b) { return a.left < b.right && b.left < a.right; }

// Windows 11, centred icons, 100 %.
static TaskbarGeometry win11() {
    TaskbarGeometry g;
    g.tray = bx(0, 1920);
    g.xaml = true;
    g.widgets = bx(0, 160);
    g.start = bx(800, 848);
    g.group = {bx(848, 896), bx(896, 944), bx(944, 1120)}; // search, task view, apps
    g.notify = bx(1700, 1920);
    return g;
}

TEST_CASE("taskbar placement: Windows 11 centred, left and right of the icons") {
    auto g = win11();
    auto l = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(l.ok);
    CHECK_FALSE(l.fellBack);
    CHECK(l.rect == Rect{432, 1032, 792, 1080}); // hugs the Start button (gap 8)
    CHECK(placeTaskbarWidget(g, "left", 100, 360).rect.left == 332);
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK_FALSE(r.fellBack);
    CHECK(r.rect == Rect{1128, 1032, 1488, 1080}); // hugs the last app button
    CHECK(placeTaskbarWidget(g, "right", 50, 360).rect.left == 1178);
}

TEST_CASE("taskbar placement: offset is clamped to the free gap") {
    auto g = win11();
    // left gap 160..800: inner 168..792, 360 wide → offset at most 264
    CHECK(placeTaskbarWidget(g, "left", 100000, 360).rect == Rect{168, 1032, 528, 1080});
    CHECK(placeTaskbarWidget(g, "left", -50, 360).rect.left == 432);
    // right gap 1120..1700: inner 1128..1692
    CHECK(placeTaskbarWidget(g, "right", 100000, 360).rect == Rect{1332, 1032, 1692, 1080});
}

TEST_CASE("taskbar placement: width shrinks to the gap, never below the minimum") {
    auto g = win11();
    g.notify = bx(1400, 1920);
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{1128, 1032, 1392, 1080}); // 264 px instead of 360
    g.notify = bx(1120 + 16 + 140, 1920);           // exactly the minimum
    r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect.width() == 140);
    g.notify = bx(1120 + 16 + 139, 1920); // one pixel short → the other side
    r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.fellBack);
    CHECK(r.rect.right == 792);
    // A configured width below the minimum only needs its own width.
    g.notify = bx(1120 + 16 + 100, 1920);
    r = placeTaskbarWidget(g, "right", 0, 100);
    REQUIRE(r.ok);
    CHECK_FALSE(r.fellBack);
    CHECK(r.rect == Rect{1128, 1032, 1228, 1080});
}

TEST_CASE("taskbar placement: Windows 11 full right side falls back to the left, nothing free → no_space") {
    auto g = win11();
    g.group.push_back(bx(1120, 1600));
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.fellBack);
    CHECK(r.rect == Rect{432, 1032, 792, 1080});
    // Icons from the widgets button to the tray.
    g.start = bx(200, 248);
    g.group = {bx(248, 1660)};
    auto n = placeTaskbarWidget(g, "left", 0, 360);
    CHECK_FALSE(n.ok);
    CHECK(n.reason == "no_space");
    CHECK(placeTaskbarWidget(g, "right", 0, 360).reason == "no_space");
}

TEST_CASE("taskbar placement: Windows 11 left aligned, widgets next to the tray") {
    TaskbarGeometry g;
    g.tray = bx(0, 1920);
    g.xaml = true;
    g.start = bx(12, 60);
    g.group = {bx(60, 108), bx(108, 156), bx(156, 600)};
    g.widgets = bx(1500, 1580); // newer builds: weather moves right in left-aligned mode
    g.notify = bx(1700, 1920);
    auto l = placeTaskbarWidget(g, "left", 0, 360); // default setting
    REQUIRE(l.ok);
    CHECK(l.fellBack);
    CHECK(l.rect == Rect{608, 1032, 968, 1080});
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == l.rect);
    // Many apps: the gap ends at the widgets button, not at the tray.
    g.group.push_back(bx(600, 1300));
    r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{1308, 1032, 1492, 1080});
    CHECK_FALSE(overlaps(r.rect, *g.widgets));
    CHECK(placeTaskbarWidget(g, "right", 1000, 360).rect.right == 1492);
    // Even fuller: the 120 px between widgets and tray is too small.
    g.group.push_back(bx(1300, 1400));
    CHECK(placeTaskbarWidget(g, "left", 0, 360).reason == "no_space");
    // Widgets still on the left (older builds): only the right gap up to the tray.
    g.group = {bx(60, 600)};
    g.widgets = bx(0, 12);
    r = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{608, 1032, 968, 1080});
}

TEST_CASE("taskbar placement: XAML taskbar without Start button is not ready, not Windows 10") {
    auto g = win11();
    g.start.reset();
    auto p = placeTaskbarWidget(g, "left", 0, 360);
    CHECK_FALSE(p.ok);
    CHECK(p.reason == "taskbar_not_ready");
    g.autoHide = true;
    CHECK(placeTaskbarWidget(g, "left", 0, 360).reason == "autohide");
    CHECK(placeTaskbarWidget(TaskbarGeometry{}, "left", 0, 360).reason == "no_taskbar");
}

// Windows 10, bottom, 100 %: Start, search box, task view, measured task buttons.
static TaskbarGeometry win10(long top = 1040, long bottom = 1080) {
    TaskbarGeometry g;
    g.tray = {0, top, 1920, bottom};
    g.start = Rect{0, top, 48, bottom};
    g.group = {Rect{48, top, 392, bottom}, Rect{392, top, 440, bottom}, Rect{440, top, 1200, bottom}};
    g.notify = Rect{1650, top, 1920, bottom};
    return g;
}

TEST_CASE("taskbar placement: Windows 10 bottom, default 'left' falls back to after the buttons") {
    auto g = win10();
    auto l = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(l.ok);
    CHECK(l.fellBack);
    CHECK(l.rect == Rect{1208, 1040, 1568, 1080});
    for (const auto& r : g.group) CHECK_FALSE(overlaps(l.rect, r));
    CHECK_FALSE(overlaps(l.rect, *g.start));
    CHECK_FALSE(overlaps(l.rect, *g.notify));
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK_FALSE(r.fellBack);
    CHECK(r.rect == l.rect);
    // Many buttons: shrinks, ends 8 px before the tray.
    g.group.back() = Rect{440, 1040, 1450, 1080};
    r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{1458, 1040, 1642, 1080});
    // Task band full.
    g.group.back() = Rect{440, 1040, 1600, 1080};
    CHECK(placeTaskbarWidget(g, "left", 0, 360).reason == "no_space");
}

TEST_CASE("taskbar placement: Windows 10 extra items and an unmeasured task band") {
    auto g = win10();
    g.group.back() = Rect{440, 1040, 800, 1080};
    // A toolbar at the end of the band and a button left of the tray (People / news).
    g.occupied = {Rect{1400, 1040, 1500, 1080}, Rect{1600, 1040, 1650, 1080}};
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{808, 1040, 1168, 1080});
    r = placeTaskbarWidget(g, "right", 10000, 360);
    CHECK(r.rect.right == 1392); // never into the toolbar
    // Task buttons could not be measured: the whole band counts as occupied.
    auto u = win10();
    u.group.pop_back();
    u.occupied = {Rect{440, 1040, 1600, 1080}};
    CHECK(placeTaskbarWidget(u, "left", 0, 360).reason == "no_space");
    u.occupied = {Rect{440, 1040, 1400, 1080}};
    auto p = placeTaskbarWidget(u, "left", 0, 360);
    REQUIRE(p.ok);
    CHECK(p.rect == Rect{1408, 1040, 1642, 1080});
}

TEST_CASE("taskbar placement: Windows 10 top-docked uses the bar's own top and bottom") {
    auto g = win10(0, 40);
    auto p = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(p.ok);
    CHECK(p.rect == Rect{1208, 0, 1568, 40});
    // Items reported for another band (stale, other monitor) are ignored.
    g.occupied = {Rect{1200, 1040, 1700, 1080}};
    CHECK(placeTaskbarWidget(g, "left", 0, 360).rect == Rect{1208, 0, 1568, 40});
}

TEST_CASE("taskbar placement: vertical taskbars are not supported") {
    TaskbarGeometry v;
    v.tray = {0, 0, 62, 1040};
    v.start = Rect{0, 0, 62, 40};
    CHECK(placeTaskbarWidget(v, "left", 0, 360).reason == "vertical_taskbar");
    v.tray = {1858, 0, 1920, 1040};
    CHECK(placeTaskbarWidget(v, "right", 0, 360).reason == "vertical_taskbar");
}

TEST_CASE("taskbar placement: 150 % (measured on a Windows 11 26340 machine)") {
    TaskbarGeometry g;
    g.tray = {0, 1528, 2560, 1600};
    g.scale = 1.5f;
    g.xaml = true;
    g.widgets = Rect{15, 1528, 81, 1600};
    g.start = Rect{916, 1528, 984, 1600};
    g.group = {Rect{984, 1528, 1050, 1600}, Rect{1050, 1528, 1116, 1600}, Rect{1116, 1528, 1644, 1600}};
    g.notify = Rect{2048, 1528, 2096, 1600}; // leftmost XAML tray button
    auto l = placeTaskbarWidget(g, "left", 0, 360);
    REQUIRE(l.ok);
    CHECK(l.rect == Rect{364, 1528, 904, 1600}); // 540 px wide, 12 px from Start
    auto r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.rect == Rect{1656, 1528, 2036, 1600}); // shrunk to 380 px
    g.group.push_back(Rect{1644, 1528, 1830, 1600}); // 218 - 24 = 194 < 210 px minimum
    r = placeTaskbarWidget(g, "right", 0, 360);
    REQUIRE(r.ok);
    CHECK(r.fellBack);
    CHECK(r.rect == l.rect);
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
