// Segment layout of a karaoke line (plain C++, no WinRT: unit-tested headless, see tests/).
//
// The in-app karaoke line is drawn as a row of small text segments (words, single CJK characters
// / emoji clusters, whitespace runs) in a wrapping panel instead of one TextBlock whose fill was
// derived from caret hit-testing (caret rects at spaces and soft wraps report other rows, which
// produced two fill edges). Every segment has its own brush; the fill is computed in *line x-space*
// - the segments' widths concatenated on one row - exactly like the host overlay computes it on
// its single row:  x = x(from) + (x(to) - x(from)) x frac  for the FillAt() span (md::lyrics).
// That x maps to per-segment states: fully sung before the edge, a feathered gradient on the
// segments under the edge, unsung after it. Wrapped rows simply continue segment by segment, so
// there is always exactly one fill edge.
//
// * Segmentize: splits the shown text at whitespace runs, around every CJK / emoji cluster and at
//   the given cuts (word-span boundaries, so every x(from) / x(to) is a segment boundary). Never
//   splits a grapheme cluster. breakBefore marks where a row may wrap (after whitespace, around CJK,
//   honouring basic CJK punctuation rules); segments cut out of one Latin word stay glued.
// * WrapSegments: places measured segments on rows (whitespace may hang past the right edge).
// * LineX / EdgeX / SegmentFills: the line x-space mapping above.
#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "LyricTiming.h"

namespace mdui::lyrics
{
    struct Segment
    {
        int start{ 0 };   // UTF-16 offset in the line text
        int len{ 0 };
        bool space{ false };
        bool breakBefore{ false };   // a row may start with this segment
    };

    // Characters that are laid out one by one and allow a break on either side (CJK, kana, hangul,
    // fullwidth forms, emoji).
    inline bool IsBreakableCluster(std::wstring_view text, int at)
    {
        wchar_t c = text[at];
        if (c >= 0x1100 && c <= 0x11FF) return true;
        if (c >= 0x2600 && c <= 0x27BF) return true;     // symbols / dingbats (emoji presentation)
        if (c >= 0x2E80 && c <= 0x9FFF) return true;     // CJK radicals, punctuation, kana, unified
        if (c >= 0xA960 && c <= 0xA97F) return true;
        if (c >= 0xAC00 && c <= 0xD7AF) return true;     // hangul syllables
        if (c >= 0xF900 && c <= 0xFAFF) return true;
        if (c >= 0xFE30 && c <= 0xFE4F) return true;
        if (c >= 0xFF00 && c <= 0xFFEF) return true;     // fullwidth forms
        if (c >= 0xD840 && c <= 0xD87F) return true;     // CJK extension B+ (high surrogates)
        if (c >= 0xD83C && c <= 0xD83E) return true;     // emoji (high surrogates)
        return false;
    }

    // No row may start with these (closing punctuation) ...
    inline bool NoBreakBefore(wchar_t c)
    {
        static constexpr std::wstring_view k = L"\x3001\x3002\xFF0C\xFF0E\xFF1A\xFF1B\xFF01\xFF1F\xFF09\x300D\x300F\x3011\x3015\x3009\x300B\x2019\x201D\x2026\x2014\xFF5E\x00B7\x30FC,.!?:;)]}%";
        return k.find(c) != std::wstring_view::npos;
    }
    // ... and no row may end with these (opening punctuation).
    inline bool NoBreakAfter(wchar_t c)
    {
        static constexpr std::wstring_view k = L"\xFF08\x300C\x300E\x3010\x3014\x3008\x300A\x2018\x201C([{";
        return k.find(c) != std::wstring_view::npos;
    }

    inline std::vector<Segment> Segmentize(std::wstring_view text, std::vector<int> cuts = {})
    {
        std::vector<Segment> out;
        int n = static_cast<int>(text.size());
        if (n == 0) return out;
        std::sort(cuts.begin(), cuts.end());
        auto clusters = CaretStops(text);   // 0, cluster starts..., n
        enum class Cls { Space, Word, Break };
        auto classify = [&](int at) {
            if (IsSpace(text[at])) return Cls::Space;
            return IsBreakableCluster(text, at) ? Cls::Break : Cls::Word;
        };
        auto isCut = [&](int at) { return std::binary_search(cuts.begin(), cuts.end(), at); };
        Cls prev = Cls::Word;
        for (size_t ci = 0; ci + 1 < clusters.size(); ++ci)
        {
            int a = clusters[ci], b = clusters[ci + 1];
            Cls cls = classify(a);
            bool start = out.empty() || cls != prev || cls == Cls::Break || isCut(a);
            if (start)
            {
                Segment s;
                s.start = a;
                s.len = b - a;
                s.space = cls == Cls::Space;
                if (!out.empty())
                {
                    wchar_t before = text[a - 1];
                    bool afterSpace = prev == Cls::Space;
                    bool cjkSide = cls == Cls::Break || prev == Cls::Break;
                    s.breakBefore = cls != Cls::Space && (afterSpace || cjkSide) && !NoBreakBefore(text[a]) && !NoBreakAfter(before);
                }
                out.push_back(s);
            }
            else
            {
                out.back().len += b - a;
            }
            prev = cls;
        }
        return out;
    }

    struct Placement
    {
        double x{ 0 };
        double y{ 0 };
        int row{ 0 };
    };

    struct WrapResult
    {
        std::vector<Placement> place;
        double width{ 0 };    // widest row (without hanging whitespace)
        double height{ 0 };
        int rows{ 0 };
    };

    // Places segments with the given widths on rows of rowHeight within availWidth (wrap=false: one
    // row). Glued segments (breakBefore=false) move to the next row together; whitespace at a row's
    // end may hang past availWidth. A glued unit wider than a whole row breaks between segments.
    inline WrapResult WrapSegments(std::vector<Segment> const& segs, std::vector<double> const& widths, double rowHeight,
        double availWidth, bool wrap)
    {
        WrapResult r;
        size_t n = segs.size();
        r.place.resize(n);
        if (n == 0) return r;
        double x = 0;
        int row = 0;
        double rowContent = 0;   // row width without trailing whitespace
        auto place = [&](size_t i) {
            r.place[i] = { x, row * rowHeight, row };
            x += widths[i];
            if (!segs[i].space) rowContent = x;
            r.width = std::max(r.width, rowContent);
        };
        auto newRow = [&] {
            ++row;
            x = 0;
            rowContent = 0;
        };
        size_t i = 0;
        while (i < n)
        {
            // Unit: this segment plus the following glued ones (trailing whitespace included).
            size_t j = i + 1;
            while (j < n && !segs[j].breakBefore) ++j;
            double unitInk = 0, w = 0;
            for (size_t k = i; k < j; ++k)
            {
                w += widths[k];
                if (!segs[k].space) unitInk = w;
            }
            if (wrap && x > 0 && x + unitInk > availWidth + 0.5) newRow();
            if (wrap && x == 0 && unitInk > availWidth + 0.5)
            {
                // Longer than a whole row: break between its segments.
                for (size_t k = i; k < j; ++k)
                {
                    if (x > 0 && !segs[k].space && x + widths[k] > availWidth + 0.5) newRow();
                    place(k);
                }
            }
            else
            {
                for (size_t k = i; k < j; ++k) place(k);
            }
            i = j;
        }
        r.rows = row + 1;
        r.height = r.rows * rowHeight;
        return r;
    }

    // Start of every segment in line x-space (widths concatenated on one row) plus the total width.
    inline std::vector<double> LineX(std::vector<double> const& widths)
    {
        std::vector<double> x(widths.size() + 1, 0.0);
        for (size_t i = 0; i < widths.size(); ++i) x[i + 1] = x[i] + widths[i];
        return x;
    }

    // x of a UTF-16 offset in line x-space: segment starts are exact; an offset inside a segment
    // (only if a span does not start on a segment boundary) is interpolated by UTF-16 units.
    inline double OffsetX(int offset, std::vector<Segment> const& segs, std::vector<double> const& lineX)
    {
        if (segs.empty()) return 0;
        for (size_t i = 0; i < segs.size(); ++i)
        {
            auto const& s = segs[i];
            if (offset <= s.start) return lineX[i];
            if (offset < s.start + s.len)
                return lineX[i] + (lineX[i + 1] - lineX[i]) * (static_cast<double>(offset - s.start) / std::max(1, s.len));
        }
        return lineX.back();
    }

    // The fill edge for a FillAt() span, in line x-space (like the host's fillX over its carets).
    inline double EdgeX(FillSpan const& span, std::vector<Segment> const& segs, std::vector<double> const& lineX)
    {
        double x0 = OffsetX(static_cast<int>(span.from), segs, lineX), x1 = OffsetX(static_cast<int>(span.to), segs, lineX);
        return x0 + (x1 - x0) * std::clamp(span.frac, 0.0, 1.0);
    }

    enum class FillMode { Unsung, Sung, Edge };

    struct SegmentFill
    {
        FillMode mode{ FillMode::Unsung };
        double gradientStart{ 0 };   // Edge: gradient in the segment's local x (sung -> unsung)
        double gradientEnd{ 0 };
    };

    // Per-segment state for an edge at edgeX with the given feather (centred on the edge).
    inline void SegmentFills(double edgeX, double feather, std::vector<double> const& lineX, std::vector<SegmentFill>& out)
    {
        size_t n = lineX.empty() ? 0 : lineX.size() - 1;
        out.resize(n);
        double a = edgeX - feather * 0.5, b = edgeX + feather * 0.5;
        for (size_t i = 0; i < n; ++i)
        {
            double s = lineX[i], e = lineX[i + 1];
            auto& f = out[i];
            if (e <= a) f = { FillMode::Sung, 0, 0 };
            else if (s >= b) f = { FillMode::Unsung, 0, 0 };
            else f = { FillMode::Edge, a - s, b - s };
        }
    }

    // Number of segments whose state shows (part of) the edge: 0 (nothing / everything sung) or
    // the contiguous run under the feather - used by tests to assert a single edge.
    inline int EdgeRuns(std::vector<SegmentFill> const& fills)
    {
        int runs = 0;
        bool in = false;
        for (auto const& f : fills)
        {
            bool edge = f.mode == FillMode::Edge;
            if (edge && !in) ++runs;
            in = edge;
        }
        return runs;
    }
}
