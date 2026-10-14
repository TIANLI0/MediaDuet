// Progressive (karaoke) lyric line for XAML.
//
// The text is laid out as segments (words, single CJK characters / emoji clusters, whitespace runs;
// LyricSegments.h) in a LyricWrapPanel; every segment is a TextBlock with its own foreground. The
// fill follows docs/ipc.md "Lyrics timing" through the host's own code (md::lyrics buildKaraoke /
// fillAt): the edge is placed in *line x-space* - the measured segment widths concatenated on one
// row - exactly like the desktop overlay computes it on its single row, then mapped to segments:
// fully sung before the edge, a feathered gradient (max(6, 0.5 x font size), like the overlay) on
// the segment(s) under it, unsung after it. Wrapped rows continue segment by segment, so there is
// always exactly one fill edge (no caret hit-testing, no clips).
// An optional translation ("tr") is a second, static row under the original (smaller, lower
// opacity; the fill never applies to it). It is part of Root(), so line heights, scaling and the
// follow centring of the lyrics view include it.
// An interlude line (long instrumental gap) shows three dots instead of text: while current they
// light up one after another over the gap and breathe, then shrink away just before the next line.
#pragma once

#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>

#include "LyricSegments.h"
#include "LyricTiming.h"
#include "Model.h"

namespace winrt::MediaDuetUI::implementation
{
    struct LyricWrapPanel;
}

namespace mdui
{
    // The host's representation of a UI lyric line (md::lyrics is the reference implementation).
    md::lyrics::Line ToCoreLine(LyricLine const& line);
    md::lyrics::Lyrics ToCoreLyrics(Lyrics const& lyrics);
    // True when the line has nothing to show (instrumental gap), using the host's rules.
    bool LineIsBlank(LyricLine const& line);
    // The session's position report for the lyrics clock: positionMs at anchorQpcMs (atTick on the
    // QPC time base, converted on receipt). trackKey: the clock's track key - the views pass a key
    // per song (so a same-song trackId change does not snap the clock, like the host overlay);
    // empty = "key|trackId".
    md::lyrics::LyricsClock::Report SessionReport(Session const& s, std::string const& trackKey = {});

    // Lyrics typeface: HarmonyOS Sans SC, bundled unmodified in Assets\Fonts (one file per weight;
    // licence in Assets\Fonts\HarmonyOS_Sans_LICENSE.txt, notice in Settings > About). Each weight
    // points at its own file and requests exactly that file's weight, so DirectWrite never
    // synthesises bold. Two weights are shipped (the files are ~8 MB each and must not be subset):
    // Bold for the main window's lines, Medium for the compact window's line and every translation
    // row (smaller and dimmer, so Medium reads as the lighter voice). When the files are missing the
    // default UI font is used as before: SemiBold for lines, Normal for translations.
    enum class LyricWeight
    {
        Medium,    // 500: the compact window's line, translation rows
        Bold,      // 700: the main window's lyric lines
    };
    // Applies family + weight to a TextBlock (all lyric text goes through this). translation: only
    // changes the fallback weight when the bundled font is missing (Normal instead of SemiBold).
    void ApplyLyricFont(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& tb, LyricWeight weight, bool translation = false);
    // True when the bundled lyrics font files were found next to the executable.
    bool LyricFontAvailable();

    class KaraokeLine
    {
    public:
        struct Style
        {
            double fontSize{ 22 };
            LyricWeight weight{ LyricWeight::Bold };   // the original row (the translation is Medium)
            double lineHeight{ 0 };   // row height; 0 = fontSize x 1.36
            bool wrap{ true };
            bool scrollLongLine{ false };   // single row: scroll like the overlay when wider than the view
            double translationScale{ 0.8 };   // translation font size relative to fontSize
        };

        explicit KaraokeLine(Style const& style);
        ~KaraokeLine();
        KaraokeLine(KaraokeLine const&) = delete;
        KaraokeLine& operator=(KaraokeLine const&) = delete;

        winrt::Microsoft::UI::Xaml::Controls::Grid const& Root() const { return m_root; }
        void SetLine(LyricLine const& line);
        // Shows the line as an interlude (three dots) covering [t, t + d).
        void SetInterlude(int64_t t, int64_t d);
        bool IsInterlude() const { return m_interlude; }
        void SetColors(winrt::Windows::UI::Color sung, winrt::Windows::UI::Color unsung);
        // Colour of the translation row (default: the sung colour).
        void SetTranslationColor(winrt::Windows::UI::Color color);
        // Shows the translation row when enabled and the line has one.
        void SetTranslationEnabled(bool enabled);
        bool TranslationShown() const { return m_trShown; }
        // Active: segments show the fill from Update(); inactive: plain sung colour.
        void SetActive(bool active);
        bool IsActive() const { return m_active; }
        // Per frame. fillEnabled=false shows the whole line as sung (no fill animation).
        void Update(double positionMs, bool fillEnabled);
        std::wstring const& Text() const { return m_text; }

    private:
        void BuildSegments();
        void ApplyFill(double edgeX, bool force);
        void ApplyTranslation();
        void UpdateInterlude(double positionMs, bool animate);
        void SetAllSegments(winrt::Microsoft::UI::Xaml::Media::Brush const& brush);

        winrt::Microsoft::UI::Xaml::Controls::Grid m_root;
        winrt::Microsoft::UI::Xaml::Controls::Panel m_panel{ nullptr };
        winrt::MediaDuetUI::implementation::LyricWrapPanel* m_panelImpl{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_tr;
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_dots;
        winrt::Microsoft::UI::Xaml::Shapes::Ellipse m_dot[3]{ nullptr, nullptr, nullptr };
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_sungBrush;
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_unsungBrush;
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_trBrush;
        bool m_trCustomColor{ false };
        bool m_trEnabled{ true };
        bool m_trShown{ false };

        struct Seg
        {
            winrt::Microsoft::UI::Xaml::Controls::TextBlock text{ nullptr };
            winrt::Microsoft::UI::Xaml::Media::LinearGradientBrush gradient{ nullptr };
            winrt::Microsoft::UI::Xaml::Media::GradientStop on{ nullptr };
            winrt::Microsoft::UI::Xaml::Media::GradientStop off{ nullptr };
            lyrics::FillMode mode{ lyrics::FillMode::Unsung };
            bool applied{ false };
            double gStart{ 0 }, gEnd{ 0 };
        };

        Style m_style;
        LyricLine m_line;
        bool m_interlude{ false };
        int64_t m_interludeT{ 0 };
        int64_t m_interludeD{ 0 };
        double m_lastDotScale{ -1 };
        std::wstring m_text;
        md::lyrics::KaraokeLine m_karaoke;   // display text + word spans (host rules)
        std::vector<lyrics::Segment> m_segments;
        std::vector<Seg> m_segs;
        std::vector<lyrics::SegmentFill> m_fills;
        winrt::Windows::UI::Color m_sung{};
        winrt::Windows::UI::Color m_unsung{};
        double m_lastEdge{ -1e9 };
        double m_lastScroll{ 0 };
        bool m_active{ false };
        bool m_wholeSung{ false };
    };
}
