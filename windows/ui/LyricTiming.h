// Lyric timing glue for the in-app views (plain C++, no WinRT: unit-tested headless, see tests/).
//
// The timing itself is NOT duplicated here: the clock (md::lyrics::LyricsClock), the karaoke text
// and fill rules (buildKaraoke / fillAt / lineSweepMs) and the line lookup (lineAt) are the host's
// reference implementation from windows/core (docs/ipc.md "Lyrics timing"), compiled into the UI,
// so the app, the desktop lyrics and the taskbar cannot diverge. This header only adds what the
// views need on top:
// * conversions from the UI model, the session report for the clock (atTick -> QPC time base),
// * the display rows of the main lyrics view: one row per lyric line (so the current row is
//   always lineAt(), like the overlay) plus an intro row with interlude dots when the first line
//   starts >= 4 s in; blank lines of >= 4 s are interlude rows, shorter blank lines are empty
//   (collapsed) rows - while one is current nothing is highlighted, exactly like the overlay,
// * grapheme-cluster boundaries for segmentation (LyricSegments.h), the line opacity falloff and
//   the interlude dot animation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "md/lyrics/Karaoke.h"
#include "md/lyrics/LyricsClock.h"
#include "md/lyrics/Lrc.h"

namespace mdui::lyrics
{
    using md::lyrics::FillSpan;
    using md::lyrics::KaraokeLine;

    constexpr int64_t kInterludeMs = 4000;

    // Same whitespace set as the host (Karaoke.cpp isSpace).
    inline bool IsSpace(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0x3000 || c == 0x200B || c == 0xFEFF;
    }

    inline bool IsBlank(std::wstring_view s)
    {
        for (wchar_t c : s)
            if (!IsSpace(c)) return false;
        return true;
    }

    // ---- Clock --------------------------------------------------------------------------------

    // The UI's report for the shared clock. positionMs was valid at the host's GetTickCount64()
    // value atTick; tickNow / qpcNow are read together on receipt, so atMs is atTick expressed on
    // the QPC time base the clock runs on (the host overlay converts its own report the same way).
    inline md::lyrics::LyricsClock::Report MakeReport(std::optional<int64_t> positionMs, uint64_t atTick, uint64_t tickNow,
        double qpcNow, double rate, bool playing, std::string const& trackKey)
    {
        md::lyrics::LyricsClock::Report r;
        r.positionMs = positionMs;
        r.atMs = qpcNow - static_cast<double>(static_cast<int64_t>(tickNow - atTick));
        r.rate = rate;
        r.playing = playing;
        r.trackId = trackKey;
        return r;
    }

    // Lyric position: smoothed clock minus the session's lyricsDelayMs (applied exactly once).
    inline double LyricPosition(md::lyrics::LyricsClock const& clock, double nowMs, int64_t delayMs)
    {
        auto p = clock.position(nowMs);
        return p ? *p - static_cast<double>(delayMs) : -1.0;
    }

    // ---- Display rows -----------------------------------------------------------------------------

    enum class RowKind
    {
        Text,        // a lyric line
        Interlude,   // breathing dots (blank line >= 4 s, or the intro)
        Empty,       // short blank line: collapsed, nothing highlighted while current
    };

    struct DisplayRow
    {
        int line{ -1 };   // lyric line index, -1 = intro
        int64_t t{ 0 };
        int64_t d{ 0 };   // interludes: length of the gap
        RowKind kind{ RowKind::Text };
    };

    struct DisplayRows
    {
        std::vector<DisplayRow> rows;
        bool intro{ false };   // rows[0] is the intro interlude
    };

    // blank[i]: the line shows nothing (see LineIsBlank). Lines must be sorted by t (synced).
    inline DisplayRows BuildDisplayRows(md::lyrics::Lyrics const& l, std::vector<bool> const& blank)
    {
        DisplayRows out;
        size_t n = l.lines.size();
        if (n == 0) return out;
        if (l.lines.front().t >= kInterludeMs)
        {
            out.intro = true;
            out.rows.push_back({ -1, 0, l.lines.front().t, RowKind::Interlude });
        }
        for (size_t i = 0; i < n; ++i)
        {
            auto const& line = l.lines[i];
            DisplayRow r{ static_cast<int>(i), line.t, line.d, RowKind::Text };
            if (i < blank.size() && blank[i])
            {
                int64_t until = i + 1 < n ? l.lines[i + 1].t : line.t + std::max<int64_t>(0, line.d);
                r.d = until - line.t;
                r.kind = r.d >= kInterludeMs ? RowKind::Interlude : RowKind::Empty;
            }
            out.rows.push_back(r);
        }
        return out;
    }

    // Current row: the overlay's lineAt() (integer position, like the host), shifted by the intro
    // row; the intro is current from the start of the track until the first line.
    inline int CurrentRow(md::lyrics::Lyrics const& l, DisplayRows const& rows, double positionMs)
    {
        if (positionMs < 0 && !rows.intro) return -1;
        int line = positionMs < 0 ? -1 : md::lyrics::lineAt(l, static_cast<int64_t>(positionMs));
        if (line < 0) return rows.intro ? 0 : -1;
        return line + (rows.intro ? 1 : 0);
    }

    // ---- Clusters -------------------------------------------------------------------------------

    inline bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
    inline bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
    inline bool IsJoiningMark(wchar_t c)
    {
        return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
            (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0xFE00 && c <= 0xFE0F) ||
            c == 0x200D || c == 0x3099 || c == 0x309A;
    }

    // Grapheme-ish cluster boundaries: 0, every cluster start, and text.size(). Never inside a
    // surrogate pair, before a combining mark / variation selector, around a ZWJ or before an emoji
    // skin-tone modifier.
    inline std::vector<int> CaretStops(std::wstring_view text)
    {
        std::vector<int> stops;
        int n = static_cast<int>(text.size());
        stops.push_back(0);
        for (int i = 1; i < n; ++i)
        {
            wchar_t c = text[i];
            if (IsLowSurrogate(c) && IsHighSurrogate(text[i - 1])) continue;
            if (IsJoiningMark(c)) continue;
            if (text[i - 1] == 0x200D) continue;   // the character after a ZWJ joins the cluster
            if (c == 0xD83C && i + 1 < n && text[i + 1] >= 0xDFFB && text[i + 1] <= 0xDFFF) continue;
            stops.push_back(i);
        }
        if (n > 0) stops.push_back(n);
        return stops;
    }

    // ---- Looks ----------------------------------------------------------------------------------

    // Opacity of a line at the given distance from the current one (Apple-Music-style falloff; the
    // current line's unsung part is drawn at ~50 %, so every other line must stay below that).
    inline double LineOpacity(int distance)
    {
        static constexpr double kFalloff[] = { 1.0, 0.36, 0.28, 0.22, 0.18 };
        distance = std::abs(distance);
        return kFalloff[std::min(distance, 4)];
    }

    // Interlude dots: per-dot fill (0..1) and the group scale (breathing; shrinks away at the end).
    inline double InterludeDotFill(double progress, int dot)
    {
        return std::clamp(progress * 3.0 - dot, 0.0, 1.0);
    }

    inline double InterludeScale(double elapsedMs, double durationMs)
    {
        constexpr double kPi = 3.14159265358979323846;
        double breathe = 1.0 + 0.07 * std::sin(elapsedMs / 1700.0 * 2.0 * kPi);
        double remaining = durationMs - elapsedMs;
        double tail = std::clamp(remaining / 420.0, 0.0, 1.0);
        double head = std::clamp(elapsedMs / 300.0, 0.0, 1.0);
        auto ease = [](double x) { return x * x * (3 - 2 * x); };
        return breathe * ease(tail) * (0.6 + 0.4 * ease(head));
    }
}
