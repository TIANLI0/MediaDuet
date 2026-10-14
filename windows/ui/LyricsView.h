// Apple-Music / AMLL-style synced lyrics view of the main window.
//
// * One row per lyric line, so the current row is always the host overlay's lineAt() (plus an intro
//   row with breathing dots when the first line starts >= 4 s in; blank lines >= 4 s are interlude
//   rows, shorter ones are empty rows). See LyricTiming.h BuildDisplayRows / CurrentRow.
// * The current line is fully emphasised: scale 1.0 (others 0.93), sung part bright, unsung part
//   ~50 % - always brighter than any other line, which fall off with distance. The current line's
//   emphasis animates quickly (~200 ms) so it never lags behind the fill.
// * The lines live in a Canvas inside a clipped viewport (no ScrollViewer): every line moves with
//   its own Translation and implicit Vector3Transition, so the composition thread animates the
//   follow scroll at the display refresh rate; lines below the current one arrive slightly later
//   (wave). The current line sits at 38 % height.
// * Font size and spacing scale with the viewport height (26 -> 34 epx).
// * New lyrics fade in line by line (short stagger); FadeOut() dims the old lines while the next
//   track's lyrics are loading.
// * Mouse wheel / touch (or pen) dragging pauses following for 4 s; a "back to the current line"
//   button appears only while the current line is scrolled out of its place. Mouse clicks never
//   scroll (the mouse uses the wheel); clicking a line seeks there (when the session can seek).
// * Lines fade out toward the top / bottom edge (no half-cut line at full strength).
// * Plain (unsynced) lyrics are a static, scrollable list.
// Everything here runs on the UI thread from the window's frame clock; nothing runs while paused.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Karaoke.h"
#include "LyricTiming.h"
#include "Model.h"

namespace mdui
{
    class LyricsView
    {
    public:
        struct Callbacks
        {
            std::function<bool()> canSeek;
            std::function<void(int64_t positionMs)> seek;
        };

        void Attach(winrt::Microsoft::UI::Xaml::Controls::Grid const& viewport, winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas,
            winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, winrt::Microsoft::UI::Xaml::Controls::Button const& followButton,
            Callbacks callbacks);
        void Detach();

        void Build(Lyrics const& lyrics, bool translationOn);
        void Clear();
        bool Empty() const { return m_lines.empty(); }
        bool Synced() const { return m_synced; }
        int ActiveLine() const { return m_active; }
        void SetTranslation(bool on);
        // Per frame (positionMs is the lyric position: smoothed clock minus the lyrics delay).
        // force re-applies the line states (appearance changes, new lyrics).
        void Update(double positionMs, bool force);
        // Ends a manual-scroll pause and eases back to the current line.
        void ResumeFollow();
        // Dims all lines (the next track's lyrics are loading); Build() replaces them.
        void FadeOut();

    private:
        enum class Mode { Follow, Wheel, Direct };
        void ApplyColors();
        void ApplyLineStates();
        double TargetFor(int index) const;
        double ClampOffset(double offset) const;
        void ApplyOffset(double offset, Mode mode);
        void Follow(Mode mode);
        void PauseFollow();
        void UpdateFollowButton();
        double EdgeFactor(int index) const;
        void OnViewportSize(double width, double height);
        void Place();
        void RevealStep();
        double FontSizeFor(double height) const;
        void Rebuild();

        winrt::Microsoft::UI::Xaml::Controls::Grid m_viewport{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Canvas m_canvas{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_panel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_followButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::RectangleGeometry m_clip{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_resumeTimer{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_revealTimer{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_fontTimer{ nullptr };
        Callbacks m_callbacks;
        std::shared_ptr<bool> m_alive;
        winrt::event_token m_wheelToken{}, m_manipStartToken{}, m_manipDeltaToken{}, m_manipDoneToken{};
        winrt::event_token m_viewportSizeToken{}, m_panelSizeToken{}, m_followClickToken{}, m_themeToken{};

        Lyrics m_source;                 // what was built (kept for font-size rebuilds)
        md::lyrics::Lyrics m_core;       // host representation for lineAt()
        lyrics::DisplayRows m_rows;
        std::vector<std::unique_ptr<KaraokeLine>> m_lines;
        std::vector<int> m_bucket;       // per line: current translation transition (-2 none set)
        std::vector<bool> m_revealed;    // staggered fade-in after Build
        bool m_synced{ false };
        bool m_dark{ false };
        bool m_colorsSet{ false };
        bool m_translationOn{ true };
        bool m_animations{ true };
        int m_active{ -2 };
        int m_pinned{ -1 };
        double m_pinUntil{ 0 };
        double m_offset{ 0 };
        double m_width{ 0 };
        double m_height{ 0 };
        double m_fontSize{ 26 };
        bool m_placed{ false };          // initial position applied (needs layout)
        bool m_followPaused{ false };
        bool m_dragging{ false };        // touch / pen manipulation in progress
        double m_dragTravel{ 0 };        // its travel before following pauses
    };
}
