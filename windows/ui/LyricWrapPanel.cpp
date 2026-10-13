#include "pch.h"
#include "LyricWrapPanel.h"
#if __has_include("LyricWrapPanel.g.cpp")
#include "LyricWrapPanel.g.cpp"
#endif

#include <cmath>
#include <limits>

using winrt::Windows::Foundation::Rect;
using winrt::Windows::Foundation::Size;

namespace winrt::MediaDuetUI::implementation
{
    void LyricWrapPanel::SetSegments(std::vector<mdui::lyrics::Segment> segments, double rowHeight, bool wrap)
    {
        m_segments = std::move(segments);
        m_rowHeight = rowHeight;
        m_wrapEnabled = wrap;
        m_widths.clear();
        m_lineX.clear();
        InvalidateMeasure();
    }

    Size LyricWrapPanel::MeasureOverride(Size const& available)
    {
        auto children = Children();
        uint32_t n = children.Size();
        std::vector<double> widths(n, 0.0);
        Size infinite{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
        for (uint32_t i = 0; i < n; ++i)
        {
            auto child = children.GetAt(i);
            child.Measure(infinite);
            widths[i] = child.DesiredSize().Width;
        }
        std::vector<mdui::lyrics::Segment> segs = m_segments;
        segs.resize(n);   // children and segments are kept in sync by the owner
        double avail = std::isfinite(available.Width) ? available.Width : 1e9;
        m_wrap = mdui::lyrics::WrapSegments(segs, widths, m_rowHeight, avail, m_wrapEnabled);
        bool changed = widths != m_widths;
        m_widths = std::move(widths);
        m_lineX = mdui::lyrics::LineX(m_widths);
        if (changed && layoutChanged) layoutChanged();
        double width = m_wrapEnabled && std::isfinite(available.Width) ? std::min<double>(m_wrap.width, available.Width) : m_wrap.width;
        return Size{ static_cast<float>(std::ceil(width)), static_cast<float>(n == 0 ? m_rowHeight : m_wrap.height) };
    }

    Size LyricWrapPanel::ArrangeOverride(Size const& finalSize)
    {
        auto children = Children();
        uint32_t n = std::min<uint32_t>(children.Size(), static_cast<uint32_t>(m_wrap.place.size()));
        for (uint32_t i = 0; i < n; ++i)
        {
            auto const& p = m_wrap.place[i];
            children.GetAt(i).Arrange(Rect{ static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(m_widths[i]),
                static_cast<float>(m_rowHeight) });
        }
        return finalSize;
    }
}
