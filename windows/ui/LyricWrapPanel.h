// Wrapping panel for the segments of a karaoke line (LyricSegments.h). Children are the segment
// TextBlocks in text order; Measure gives each its natural width, WrapSegments places them on rows
// of a fixed height. The measured widths (= the line x-space the fill uses) are exposed to the
// owning KaraokeLine; layoutChanged fires when they change.
#pragma once

#include "LyricWrapPanel.g.h"

#include <functional>
#include <vector>

#include "LyricSegments.h"

namespace winrt::MediaDuetUI::implementation
{
    struct LyricWrapPanel : LyricWrapPanelT<LyricWrapPanel>
    {
        LyricWrapPanel() = default;

        winrt::Windows::Foundation::Size MeasureOverride(winrt::Windows::Foundation::Size const& available);
        winrt::Windows::Foundation::Size ArrangeOverride(winrt::Windows::Foundation::Size const& finalSize);

        void SetSegments(std::vector<mdui::lyrics::Segment> segments, double rowHeight, bool wrap);
        std::vector<double> const& Widths() const { return m_widths; }
        std::vector<double> const& LineX() const { return m_lineX; }
        double TotalWidth() const { return m_lineX.empty() ? 0.0 : m_lineX.back(); }

        std::function<void()> layoutChanged;

    private:
        std::vector<mdui::lyrics::Segment> m_segments;
        std::vector<double> m_widths;
        std::vector<double> m_lineX;
        mdui::lyrics::WrapResult m_wrap;
        double m_rowHeight{ 30 };
        bool m_wrapEnabled{ true };
    };
}

namespace winrt::MediaDuetUI::factory_implementation
{
    struct LyricWrapPanel : LyricWrapPanelT<LyricWrapPanel, implementation::LyricWrapPanel>
    {
    };
}
