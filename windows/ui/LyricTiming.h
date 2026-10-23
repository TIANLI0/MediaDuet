// Lyric timing glue for the in-app views (plain C++, no WinRT: unit-tested headless, see tests/).
//
// The timing itself is NOT duplicated here: the clock (md::lyrics::LyricsClock), the karaoke text
// and fill rules (buildKaraoke / fillAt / lineSweepMs) and the line lookup (lineAt) are the host's
// reference implementation from windows/core (docs/ipc.md "Lyrics timing"), compiled into the UI,
// so the app, the desktop lyrics and the taskbar cannot diverge. This header only adds what the
// views need on top:
// * conversions from the UI model, the session report for the clock (atTick -> QPC time base),
// * the display rows of the main lyrics view, from the shared row model (md::lyrics::buildRows:
//   sung lines plus interludes for the intro, long blank lines and the silence after a line),
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

    // ---- Song identity --------------------------------------------------------------------------

    // One player session's song as the UI sees it (a session of the state, or the song a lyrics
    // message names). durationMs < 0 = unknown.
    struct SongRef
    {
        std::string trackId;
        int side{ 0 };
        std::string key;
        std::string title;
        std::string artist;
        int64_t durationMs{ -1 };
    };

    // Same song of the same player session: equal trackIds, or (players such as QQ Music re-report
    // the duration / album, which changes the trackId of the song that is playing) the same title and
    // artist with durations within 3 s (md::lyrics::sameSong, the host's rule). The state and the
    // lyrics message that carry a new trackId arrive in either order, so the views use this instead
    // of comparing trackIds to keep lyrics (and the clock, holds, scroll) across such a change.
    inline bool SameSong(SongRef const& a, SongRef const& b)
    {
        if (a.side != b.side || a.key != b.key) return false;
        if (!a.trackId.empty() && a.trackId == b.trackId) return true;
        auto dur = [](int64_t d) { return d > 0 ? std::optional<int64_t>(d) : std::nullopt; };
        return md::lyrics::sameSong({ a.title, a.artist, {}, dur(a.durationMs) }, { b.title, b.artist, {}, dur(b.durationMs) });
    }

    // ---- Display rows -----------------------------------------------------------------------------

    enum class RowKind
    {
        Text,        // a lyric line
        Interlude,   // breathing dots: an instrumental gap (intro, long blank line, silence after a line)
    };

    struct DisplayRow
    {
        int line{ -1 };   // lyric line index, -1 = interlude
        int64_t t{ 0 };
        int64_t d{ 0 };   // interludes: length of the gap
        RowKind kind{ RowKind::Text };
    };

    struct DisplayRows
    {
        std::vector<DisplayRow> rows;
        md::lyrics::LyricRows model;   // the shared row model the rows come from (synced lyrics)
    };

    // No row is current any more: the lyrics are over (md::lyrics::kEndHoldMs after the last line).
    constexpr int kEndedRow = -3;

    // The rows of synced lyrics: exactly the host's md::lyrics::buildRows (also Android's
    // LyricRows), so the app, the desktop lyrics and the taskbar agree on gaps and the end.
    inline DisplayRows BuildDisplayRows(md::lyrics::Lyrics const& l)
    {
        DisplayRows out;
        out.model = md::lyrics::buildRows(l);
        for (auto const& r : out.model.rows)
        {
            if (r.kind == md::lyrics::LyricRow::Kind::Gap) out.rows.push_back({ -1, r.start, r.end - r.start, RowKind::Interlude });
            else out.rows.push_back({ r.line, r.start, l.lines[static_cast<size_t>(r.line)].d, RowKind::Text });
        }
        return out;
    }

    // Current row: -1 before the first row (or without a position), kEndedRow once the lyrics are over.
    inline int CurrentRow(DisplayRows const& rows, double positionMs)
    {
        if (positionMs < 0) return -1;
        int i = rows.model.activeAt(static_cast<int64_t>(positionMs));
        return i == md::lyrics::LyricRows::kEnded ? kEndedRow : i;
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

    // Same motion as Android's gap row: breathing 1 + 0.12 sin(2 pi t / 2400 ms); over the last
    // 450 ms the dots shrink to 40 % (InterludeExit fades them); they grow in over the first 300 ms.
    inline double InterludeExit(double elapsedMs, double durationMs)
    {
        return std::clamp((durationMs - elapsedMs) / 450.0, 0.0, 1.0);
    }

    inline double InterludeScale(double elapsedMs, double durationMs)
    {
        constexpr double kPi = 3.14159265358979323846;
        double breathe = 1.0 + 0.12 * std::sin(elapsedMs / 2400.0 * 2.0 * kPi);
        double head = std::clamp(elapsedMs / 300.0, 0.0, 1.0);
        auto ease = [](double x) { return x * x * (3 - 2 * x); };
        return breathe * (0.4 + 0.6 * InterludeExit(elapsedMs, durationMs)) * (0.6 + 0.4 * ease(head));
    }

    // Opacity of one interlude dot while its row is current (an inactive row hides its dots).
    inline double InterludeDotOpacity(double elapsedMs, double durationMs, double fill)
    {
        double in = std::clamp(elapsedMs / 400.0, 0.0, 1.0);
        return (0.25 + 0.75 * fill) * std::max(InterludeExit(elapsedMs, durationMs), 0.2) * in;
    }
}
