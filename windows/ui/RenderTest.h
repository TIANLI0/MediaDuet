// Invisible render test (MediaDuetUI.exe --render-test <outdir> --pipe <private test pipe>).
//
// Renders the real windows against tools/fake_host.py and saves PNGs (RenderTargetBitmap), without
// ever showing anything on a monitor or touching focus:
// * windows are tool windows (no taskbar button, not in Alt+Tab, WS_EX_NOACTIVATE), placed
//   entirely outside the union of all monitors and shown with SW_SHOWNOACTIVATE - never
//   Activate(), never SetForegroundWindow; the test aborts at once if a window rectangle would
//   intersect any monitor or if the foreground window changes;
// * requires a non-default --pipe (it can never talk to a running MediaDuet) and skips the
//   single-instance redirection, persisted settings and every window placement / settings write;
// * time is frozen and stepped (testclock) so lyric frames are deterministic.
#pragma once

#include <string>

namespace mdui::render
{
    // True in --render-test mode (set before any window exists).
    bool Active();
    void SetActive(std::wstring outDir, std::wstring only = {});
    // Turns a freshly created (never shown) window into an off-screen, non-activating tool window of
    // the given size in DIPs and shows it without activation. Aborts the process when unsafe.
    void PrepareOffscreen(HWND hwnd, int widthDip, int heightDip);
    // Runs the scenario (UI thread; fire-and-forget). Exits the process when done.
    void Run();
    // Slow motion for capturing composition animations mid-way (render test only): factor >= 1.
    void SetSlowMotion(double factor);
    double SlowMotion();
    // Appends a line to <outdir>/render-test.log.
    void LogLine(std::wstring const& line);
}
