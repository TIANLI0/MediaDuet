// Headless tests for the UI's plain-C++ logic (no XAML, no window):
//   LyricSegments.h + LyricTiming.h (segment fill in line x-space, wrapping, display rows) checked
//     against the host's reference code (windows/core md::lyrics: buildKaraoke, fillAt, lineAt,
//     LyricsClock), including an end-to-end host-overlay vs UI timing comparison,
//   PlaybackClock.h (seek hold, optimistic play/pause, offset formatting),
//   AmbientRender.h (flowing artwork background: dithered, no banding).
//
// Build and run from a VS developer prompt at the repository root:
//   cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 /wd5285 /wd4100 /DNOMINMAX /I windows\ui /I windows\core\include ^
//      /I windows\third_party windows\ui\tests\ui_logic_tests.cpp windows\core\src\lyrics\Karaoke.cpp ^
//      windows\core\src\lyrics\LyricsClock.cpp windows\core\src\lyrics\Lrc.cpp windows\core\src\util\Text.cpp ^
//      /Fe:ui_logic_tests.exe
//   ui_logic_tests.exe
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "AmbientRender.h"
#include "LyricSegments.h"
#include "LyricTiming.h"
#include "PlaybackClock.h"

#include <cstdint>
#include <random>

using namespace mdui;
using namespace mdui::lyrics;

// ---- Optimistic transport ----------------------------------------------------------------------

TEST_CASE("seek hold keeps the target until the host confirms or it times out")
{
    SeekHold h;
    h.Begin(60000, 0);
    CHECK(h.Hold(10000, 100, true, 1.0));            // stale report: keep the target
    CHECK(h.Expected(100, true, 1.0) == doctest::Approx(60100));
    CHECK_FALSE(h.Hold(60250, 300, true, 1.0));      // confirmed
    CHECK_FALSE(h.Active());
    h.Begin(60000, 0);
    CHECK_FALSE(h.Hold(10000, 3500, true, 1.0));     // timed out: accept the host
}

TEST_CASE("optimistic play/pause confirms or rolls back")
{
    OptimisticFlag f;
    f.Begin(false, 0);                                // user pressed pause
    CHECK_FALSE(f.Resolve(true, 100));               // host still says playing: show paused
    CHECK(f.Active());
    CHECK_FALSE(f.Resolve(false, 200));              // confirmed
    CHECK_FALSE(f.Active());
    f.Begin(false, 0);
    CHECK(f.Resolve(true, 3000));                    // no confirmation: roll back
}

TEST_CASE("song identity: same song across a trackId change, in either message order")
{
    SongRef lyrics{ "aaaa", 0, "QQMusic.exe", "晴天", "周杰伦", 269000 };
    SongRef session = lyrics;
    CHECK(SameSong(lyrics, session));
    session.trackId = "bbbb";                 // the player re-reported the duration: new trackId
    session.durationMs = 269600;
    CHECK(SameSong(lyrics, session));
    CHECK(SameSong(session, lyrics));
    session.durationMs = -1;                  // duration not known (yet)
    CHECK(SameSong(lyrics, session));
    session.durationMs = 280000;              // another recording
    CHECK_FALSE(SameSong(lyrics, session));
    session = lyrics;
    session.trackId = "cccc";
    session.title = "七里香";                  // the next song
    CHECK_FALSE(SameSong(lyrics, session));
    session = lyrics;
    session.trackId = "dddd";
    session.key = "cloudmusic.exe";           // same song in another player: not this session's lyrics
    CHECK_FALSE(SameSong(lyrics, session));
    session = lyrics;
    session.side = 1;
    CHECK_FALSE(SameSong(lyrics, session));   // other side, even with the same trackId
    SongRef noMeta{ "aaaa", 0, "QQMusic.exe", "", "", -1 };   // older host: lyrics without the song
    CHECK(SameSong(noMeta, lyrics));          // still matched by trackId
    lyrics.trackId = "eeee";
    CHECK_FALSE(SameSong(noMeta, lyrics));
}

TEST_CASE("offset formatting")
{
    CHECK(FormatOffsetSeconds(0) == L"0");
    CHECK(FormatOffsetSeconds(300) == L"+0.3");
    CHECK(FormatOffsetSeconds(-1200) == L"\x2212" L"1.2");
    CHECK(FormatOffsetSeconds(250) == L"+0.25");
    CHECK(FormatOffsetSeconds(-5000) == L"\x2212" L"5.0");
    CHECK(FormatOffsetSeconds(996) == L"+1.00");
}


// ---- Segments: line x-space fill (one edge), wrapping ------------------------------------------

namespace
{
    md::lyrics::Line WordLine(std::vector<std::pair<int64_t, std::string>> ws, int64_t wordDur = 400)
    {
        md::lyrics::Line l;
        for (auto& [t, s] : ws) l.words.push_back({ t, wordDur, s });
        l.t = l.words.front().t;
        l.d = 6000;
        for (auto& w : l.words) l.text += w.text;
        return l;
    }

    // Advance width of one UTF-16 unit (the host's DWrite carets are cumulative advances).
    double UnitWidth(std::wstring const& text, size_t i)
    {
        wchar_t c = text[i];
        if (IsLowSurrogate(c)) return 0;               // second half of a pair: same caret
        if (IsHighSurrogate(c)) return 22;             // emoji / extension B
        if (IsSpace(c)) return 6;
        if (c >= 0x2E80) return 24;                    // CJK
        return 11;
    }

    std::vector<float> HostCarets(std::wstring const& text)
    {
        std::vector<float> c(text.size() + 1, 0.f);
        for (size_t i = 0; i < text.size(); ++i) c[i + 1] = c[i] + static_cast<float>(UnitWidth(text, i));
        return c;
    }

    // The host overlay's fillX (windows/platform/overlay/OverlayCommon.cpp).
    float HostFillX(std::vector<float> const& c, md::lyrics::FillSpan const& f)
    {
        if (c.empty()) return 0.f;
        size_t n = c.size() - 1;
        float x0 = c[std::min<size_t>(f.from, n)], x1 = c[std::min<size_t>(f.to, n)];
        return x0 + (x1 - x0) * static_cast<float>(std::clamp(f.frac, 0.0, 1.0));
    }

    struct UiLine
    {
        md::lyrics::KaraokeLine k;
        std::vector<Segment> segs;
        std::vector<double> widths;
        std::vector<double> lineX;
    };

    std::vector<int> SpanCuts(md::lyrics::KaraokeLine const& k)
    {
        std::vector<int> cuts;
        for (auto const& w : k.words)
        {
            cuts.push_back(static_cast<int>(w.start));
            cuts.push_back(static_cast<int>(w.start + w.len));
        }
        return cuts;
    }

    UiLine MakeUiLine(md::lyrics::Line const& line)
    {
        UiLine u;
        u.k = md::lyrics::buildKaraoke(line);
        u.segs = Segmentize(u.k.text, SpanCuts(u.k));
        for (auto const& s : u.segs)
        {
            double w = 0;
            for (int i = s.start; i < s.start + s.len; ++i) w += UnitWidth(u.k.text, static_cast<size_t>(i));
            u.widths.push_back(w);
        }
        u.lineX = LineX(u.widths);
        return u;
    }

    // Edge x equals the host's for every position, and exactly one contiguous edge run exists,
    // with everything before it sung and everything after it unsung.
    void CheckLineAgainstHost(md::lyrics::Line const& line, double from, double to)
    {
        auto u = MakeUiLine(line);
        auto carets = HostCarets(u.k.text);
        std::vector<SegmentFill> fills;
        for (double p = from; p <= to; p += 7)
        {
            auto span = md::lyrics::fillAt(u.k, p);
            double ui = EdgeX(span, u.segs, u.lineX);
            double host = HostFillX(carets, span);
            REQUIRE(ui == doctest::Approx(host).epsilon(1e-6));
            SegmentFills(ui, 12.0, u.lineX, fills);
            CHECK(EdgeRuns(fills) <= 1);
            int state = 0;   // 0 sung, 1 edge, 2 unsung: never goes back
            for (auto const& f : fills)
            {
                int s = f.mode == FillMode::Sung ? 0 : f.mode == FillMode::Edge ? 1 : 2;
                CHECK(s >= state);
                state = s;
            }
        }
    }
}

TEST_CASE("segments: single and multiple spaces, words carrying their spaces")
{
    auto line = WordLine({ { 1000, "Two " }, { 1400, "devices  " }, { 1800, "keep" } });
    auto u = MakeUiLine(line);
    CHECK(u.k.text == L"Two devices  keep");
    REQUIRE(u.segs.size() == 5);   // Two | ' ' | devices | '  ' | keep
    CHECK(u.segs[1].space);
    CHECK(u.segs[3].space);
    CHECK(u.segs[3].len == 2);
    CHECK_FALSE(u.segs[1].breakBefore);   // never start a row with whitespace
    CHECK(u.segs[2].breakBefore);         // a row may start after whitespace
    CheckLineAgainstHost(line, 0, 3000);
}

TEST_CASE("segments: leading / trailing blanks and a word trimmed away")
{
    // The first word is whitespace only (trimmed away -> empty span), the last carries spaces.
    auto line = WordLine({ { 500, "  " }, { 1000, "  Hello " }, { 1500, "world  " } });
    auto u = MakeUiLine(line);
    CHECK(u.k.text == L"Hello world");
    CHECK(u.k.words[0].len == 0);
    REQUIRE(u.segs.size() == 3);
    CheckLineAgainstHost(line, 0, 3000);
}

TEST_CASE("segments: CJK + Latin + emoji, syllables of one word stay glued")
{
    // "beau" "ti" "ful" are one word (no break between them), CJK characters break anywhere,
    // the emoji (surrogate pair) is one segment, the closing full-width punctuation sticks to its
    // preceding character.
    auto line = WordLine({ { 1000, "\xE6\xB5\x8B\xE8\xAF\x95" }, { 1400, "beau" }, { 1600, "ti" }, { 1800, "ful " },
        { 2200, "\xF0\x9F\x8E\xB5" }, { 2600, "\xE6\x99\xB4\xEF\xBC\x8C" } });
    auto u = MakeUiLine(line);
    std::wstring t = u.k.text;
    std::vector<std::wstring> parts;
    for (auto const& s : u.segs) parts.push_back(t.substr(s.start, s.len));
    std::vector<std::wstring> want = { L"\x6D4B", L"\x8BD5", L"beau", L"ti", L"ful", L" ", L"\xD83C\xDFB5", L"\x6674", L"\xFF0C" };
    CHECK(parts == want);
    CHECK(u.segs[1].breakBefore);         // between two CJK characters
    CHECK(u.segs[2].breakBefore);         // CJK -> Latin
    CHECK_FALSE(u.segs[3].breakBefore);   // "ti" glued to "beau"
    CHECK_FALSE(u.segs[4].breakBefore);
    CHECK(u.segs[6].breakBefore);         // emoji after a space
    CHECK(u.segs[7].breakBefore);
    CHECK_FALSE(u.segs[8].breakBefore);   // no row starts with the full-width comma
    CheckLineAgainstHost(line, 0, 4000);
}

TEST_CASE("segments: line-timed sweep is x-linear over the whole line like the host")
{
    md::lyrics::Line line;
    line.t = 10000;
    line.d = 5000;
    line.text = "  Bridges,  pipes and JSON lines \xE2\x80\x94 \xE6\xB5\x8B\xE8\xAF\x95  ";
    CheckLineAgainstHost(line, 9000, 16000);
    auto u = MakeUiLine(line);
    auto mid = md::lyrics::fillAt(u.k, 10000 + md::lyrics::lineSweepMs(u.k) / 2);
    CHECK(EdgeX(mid, u.segs, u.lineX) == doctest::Approx(u.lineX.back() / 2));
}

TEST_CASE("segments: a wrapped line continues the fill row by row with one edge")
{
    auto line = WordLine({ { 0, "Every " }, { 400, "word " }, { 800, "lights " }, { 1200, "up " }, { 1600, "when " },
        { 2000, "it " }, { 2400, "is " }, { 2800, "sung" } });
    auto u = MakeUiLine(line);
    auto wrap = WrapSegments(u.segs, u.widths, 33, 120, true);
    CHECK(wrap.rows >= 3);
    CHECK(wrap.height == doctest::Approx(33.0 * wrap.rows));
    // Rows never start with whitespace; ink never exceeds the available width.
    for (size_t i = 0; i < u.segs.size(); ++i)
    {
        if (wrap.place[i].x == 0 && i > 0) CHECK_FALSE(u.segs[i].space);
        if (!u.segs[i].space) CHECK(wrap.place[i].x + u.widths[i] <= 120.5);
        if (i > 0) CHECK((wrap.place[i].row > wrap.place[i - 1].row || wrap.place[i].x >= wrap.place[i - 1].x));
    }
    CheckLineAgainstHost(line, -200, 3600);
    // Unwrapped (compact window): a single row.
    CHECK(WrapSegments(u.segs, u.widths, 33, 120, false).rows == 1);
    // A glued unit wider than a row breaks between its segments instead of overflowing.
    std::vector<Segment> glued = { { 0, 4, false, false }, { 4, 4, false, false }, { 8, 4, false, false } };
    auto w2 = WrapSegments(glued, { 50, 50, 50 }, 30, 120, true);
    CHECK(w2.rows == 2);
}

// ---- Display rows: the shared row model (md::lyrics::buildRows), as the overlay uses it ------------

TEST_CASE("display rows: intro, long and short blank lines, vocal gaps, end of lyrics")
{
    md::lyrics::Lyrics l;
    l.synced = true;
    l.lines = { { 8000, 3000, "one", {}, "" }, { 11000, 2000, "", {}, "" }, { 13000, 3000, "two", {}, "" },
        { 16000, 6000, "", {}, "" }, { 22000, 2000, "three", {}, "" } };
    auto rows = BuildDisplayRows(l);
    REQUIRE(rows.rows.size() == 5);
    CHECK(rows.rows[0].kind == RowKind::Interlude);   // intro
    CHECK(rows.rows[0].d == 8000);
    CHECK(rows.rows[1].line == 0);                    // the 2 s blank line after it is dropped
    CHECK(rows.rows[2].line == 2);
    CHECK(rows.rows[3].kind == RowKind::Interlude);   // 6 s blank
    CHECK(rows.rows[3].d == 6000);
    CHECK(rows.rows[4].line == 4);
    for (double p = -500; p < 40000; p += 50)
    {
        int row = CurrentRow(rows, p);
        int line = p < 0 ? -1 : rows.model.lineAt(static_cast<int64_t>(p));
        if (row >= 0 && rows.rows[static_cast<size_t>(row)].kind == RowKind::Text) CHECK(rows.rows[static_cast<size_t>(row)].line == line);
        else CHECK(line == -1);
    }
    CHECK(CurrentRow(rows, 12000) == 1);   // the short blank line keeps "one" current
    CHECK(CurrentRow(rows, rows.model.endMs) == kEndedRow);
    // Without an intro (first line < 4 s) nothing is current before the first line.
    l.lines.front().t = 2000;
    auto r2 = BuildDisplayRows(l);
    CHECK(r2.rows[0].kind == RowKind::Text);
    CHECK(CurrentRow(r2, 1000) == -1);
    CHECK(CurrentRow(r2, 2000) == 0);
}

// ---- End-to-end timing: host overlay pipeline vs UI pipeline --------------------------------------
//
// The same timeline (a player reporting positions with jitter, a pause, a seek; lyrics delay 0 /
// 180 / -300 ms) goes through
//   host: snapshot -> LyricsClock (atMs from the host's GetTickCount64) -> position - delay ->
//         lineAt / fillAt -> caret x (overlay)
//   UI:   the host's state message (positionMs re-extrapolated to the send tick, atTick) ->
//         IPC delay -> MakeReport (atTick -> QPC on receipt) -> LyricsClock -> LyricPosition ->
//         CurrentRow / fillAt -> segment line x-space
// and both are sampled at 60 Hz. GetTickCount64's 15.6 ms granularity is simulated on both sides.

namespace
{
    uint64_t Tick(double qpcMs) { return static_cast<uint64_t>(std::floor(qpcMs / 15.625) * 15.625) + 5000000; }

    struct Player
    {
        double base{ 0 };       // position at baseAt
        double baseAt{ 0 };
        bool playing{ true };
        double Pos(double t) const { return playing ? base + (t - baseAt) : base; }
    };

    void RunTimeline(int64_t delayMs, uint32_t seed)
    {
        md::lyrics::Lyrics l;
        l.synced = true;
        l.lines.push_back(WordLine({ { 6000, "Two " }, { 6400, "devices " }, { 6900, "keep " }, { 7300, "the " }, { 7600, "same " }, { 8000, "old " }, { 8400, "song" } }));
        l.lines.push_back(WordLine({ { 10000, "\xE6\xB5\x8B\xE8\xAF\x95" }, { 10500, "\xE6\xAD\x8C\xE8\xAF\x8D" }, { 11000, "\xF0\x9F\x8E\xB5 " }, { 11500, "beau" }, { 11700, "ti" }, { 11900, "ful" } }));
        md::lyrics::Line plain;
        plain.t = 14000;
        plain.d = 4000;
        plain.text = "Pause the phone,  the PC plays on";
        l.lines.push_back(plain);
        md::lyrics::Line gap;
        gap.t = 18000;
        gap.d = 5000;
        l.lines.push_back(gap);
        l.lines.push_back(WordLine({ { 23000, "Last " }, { 23500, "line" } }));
        std::vector<UiLine> ui;
        for (auto const& line : l.lines) ui.push_back(MakeUiLine(line));
        auto rows = BuildDisplayRows(l);

        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> jitter(-25, 25), ipc(1, 9), gapMs(150, 1400);
        md::lyrics::LyricsClock host, uiClock;
        Player player;
        player.base = 4000;
        player.baseAt = 1000;
        const std::string key = "app|track";

        double nextReport = 1000, nextExtraState = 1300;
        // Reports are processed in order of arrival; the UI receives each one after an IPC delay.
        struct Pending { double at; int64_t pos; uint64_t atTick; bool playing; };
        std::vector<Pending> inflight;
        int mismatchedLines = 0, samples = 0, fillOutliers = 0;
        double maxPosDiff = 0;
        bool paused = false, sought = false;
        bool haveSnap = false, lastPlaying = false;
        int64_t lastPos = 0;
        uint64_t lastTick = 0;
        for (double now = 1000; now < 26000; now += 1000.0 / 60.0)
        {
            // Events on the player.
            if (!paused && now >= 12500)
            {
                player.base = player.Pos(now);
                player.baseAt = now;
                player.playing = false;
                paused = true;
                nextReport = now;
            }
            if (paused && player.playing == false && now >= 13300)
            {
                player.baseAt = now;
                player.playing = true;
                nextReport = now;
            }
            if (!sought && now >= 16000)
            {
                player.base = 20500;   // seek forward (skips into the blank line / interlude)
                player.baseAt = now;
                sought = true;
                nextReport = now;
            }
            // A snapshot from the player (GSMTC / phone): sampled at a tick, with report jitter.
            if (now >= nextReport)
            {
                uint64_t sampleTick = Tick(now);
                int64_t pos = static_cast<int64_t>(std::llround(player.Pos(now) + (player.playing ? jitter(rng) : 0)));
                // Host overlay: positionAtMs = sampleTick; atMs = now - (tickNow - positionAtMs).
                md::lyrics::LyricsClock::Report hr;
                hr.positionMs = pos;
                hr.atMs = now - static_cast<double>(Tick(now) - sampleTick);
                hr.rate = 1.0;
                hr.playing = player.playing;
                hr.trackId = key;
                host.update(hr, now);
                // State to the UI: positionMs extrapolated to the send tick (= now), atTick = send tick.
                inflight.push_back({ now + ipc(rng), pos, sampleTick, player.playing });
                haveSnap = true;
                lastPos = pos;
                lastTick = sampleTick;
                lastPlaying = player.playing;
                nextReport = now + gapMs(rng);
            }
            // Other state changes (volume, connection...) re-send the same session: the host
            // re-extrapolates its last snapshot to the send tick (sessionJson / positionAt).
            if (now >= nextExtraState && haveSnap)
            {
                uint64_t sendTick = Tick(now);
                int64_t pos = lastPos + (lastPlaying ? static_cast<int64_t>(sendTick - lastTick) : 0);
                inflight.push_back({ now + ipc(rng), pos, sendTick, lastPlaying });
                nextExtraState = now + gapMs(rng) / 2;
            }
            for (auto it = inflight.begin(); it != inflight.end();)
            {
                if (it->at > now) { ++it; continue; }
                auto r = MakeReport(it->pos, it->atTick, Tick(it->at), it->at, 1.0, it->playing, key);
                uiClock.update(r, it->at);
                it = inflight.erase(it);
            }
            // Sample both pipelines.
            double hp = host.position(now) ? *host.position(now) - static_cast<double>(delayMs) : -1;
            double up = LyricPosition(uiClock, now, delayMs);
            if (hp < 0 || up < -1e8) continue;
            ++samples;
            // Right after a pause / seek the UI learns about it one IPC hop later (<= 9 ms here).
            bool justChanged = std::abs(now - 12500) < 30 || std::abs(now - 13300) < 30 || std::abs(now - 16000) < 30;
            if (now > 1500 && !justChanged) maxPosDiff = std::max(maxPosDiff, std::abs(hp - up));
            int hostLine = rows.model.lineAt(static_cast<int64_t>(hp));   // what the overlay shows
            int row = CurrentRow(rows, up);
            int uiLine = row < 0 ? -1 : rows.rows[static_cast<size_t>(row)].line;
            if (hostLine != uiLine)
            {
                // Only allowed right at a line boundary that lies between the two positions.
                bool boundary = false;
                for (auto const& r : rows.model.rows)
                    if ((hp - r.start) * (up - r.start) <= 0) boundary = true;
                if ((hp - rows.model.endMs) * (up - rows.model.endMs) <= 0) boundary = true;
                CHECK(boundary);
                ++mismatchedLines;
                continue;
            }
            if (hostLine < 0) continue;
            auto const& u = ui[static_cast<size_t>(hostLine)];
            if (u.k.text.empty()) continue;
            double total = u.lineX.back();
            double hx = HostFillX(HostCarets(u.k.text), md::lyrics::fillAt(u.k, hp)) / total;
            double ux = EdgeX(md::lyrics::fillAt(u.k, up), u.segs, u.lineX) / total;
            if (std::abs(hx - ux) > 0.01) ++fillOutliers;
        }
        INFO("delay " << delayMs << ", max |host - ui| = " << maxPosDiff << " ms");
        CHECK(samples > 1000);
        CHECK(maxPosDiff < 40.0);                    // both pipelines show the same moment
        CHECK(mismatchedLines <= samples / 200);     // only boundary frames
        CHECK(fillOutliers <= samples / 100);        // fill x within 1 % of the line width
    }
}

TEST_CASE("timing: the UI pipeline matches the host overlay (delay 0 / 180 / -300 ms)")
{
    RunTimeline(0, 1);
    RunTimeline(180, 2);
    RunTimeline(-300, 3);
}

TEST_CASE("timing: the delay is applied exactly once")
{
    md::lyrics::LyricsClock c;
    auto r = MakeReport(10000, 5000000, 5000000, 1000.0, 1.0, false, "k");
    c.update(r, 1000.0);
    CHECK(LyricPosition(c, 1000.0, 0) == doctest::Approx(10000));
    CHECK(LyricPosition(c, 1000.0, 180) == doctest::Approx(9820));
    CHECK(LyricPosition(c, 1000.0, -300) == doctest::Approx(10300));
    // atTick conversion: a report that is 40 ms old on receipt is extrapolated by 40 ms.
    md::lyrics::LyricsClock p;
    p.update(MakeReport(10000, 5000000, 5000040, 2000.0, 1.0, true, "k"), 2000.0);
    CHECK(*p.position(2000.0) == doctest::Approx(10040));
}

// ---- Looks ------------------------------------------------------------------------------------

TEST_CASE("interlude dots fill in order and shrink away at the end")
{
    CHECK(InterludeDotFill(0.0, 0) == 0);
    CHECK(InterludeDotFill(0.5, 0) == 1);
    CHECK(InterludeDotFill(0.5, 1) == doctest::Approx(0.5));
    CHECK(InterludeDotFill(0.5, 2) == 0);
    CHECK(InterludeScale(4800, 4800) == doctest::Approx(0.4));   // shrunk at the end (breathing at rest)
    CHECK(InterludeDotOpacity(5000, 5000, 1.0) == doctest::Approx(0.2));
    CHECK(InterludeDotOpacity(0, 5000, 1.0) == doctest::Approx(0.0)); // fades in
    CHECK(InterludeScale(2000, 5000) > 0.8);
}

TEST_CASE("line opacity falls off and stays below the current line's unsung alpha")
{
    CHECK(LineOpacity(0) == 1.0);
    for (int d = 1; d < 8; ++d)
    {
        CHECK(LineOpacity(d) < 0.5);
        CHECK(LineOpacity(d) <= LineOpacity(d - 1));
    }
}

// ---- Ambient background ----------------------------------------------------------------------

TEST_CASE("ambient render: dithered upsampling, no banding")
{
    // A slow horizontal ramp (a few LSB over the whole width) upsampled 8x: plain rounding would
    // give wide constant bands.
    int sw = 200, sh = 2, w = 1600, h = 96;
    std::vector<ambient::Rgb> scene(static_cast<size_t>(sw) * sh);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
        {
            float v = 0.40f + 0.02f * x / (sw - 1);
            scene[static_cast<size_t>(y) * sw + x] = { v, 0.5f * v, 0.25f };
        }
    std::vector<uint8_t> px;
    ambient::Upsample(scene, sw, sh, w, h, px, 0x1234567u);
    REQUIRE(px.size() == static_cast<size_t>(w) * h * 4);
    double maxMeanErr = 0, maxPixelErr = 0, plainMaxErr = 0;
    for (int x = 0; x < w; ++x)
    {
        float s = std::clamp((x + 0.5f) * sw / w - 0.5f, 0.0f, sw - 1.0f);
        double exact = (0.40 + 0.02 * s / (sw - 1)) * 255.0;
        double sum = 0;
        for (int y = 0; y < h; ++y)
        {
            uint8_t const* q = &px[(static_cast<size_t>(y) * w + x) * 4];
            CHECK(q[3] == 255);
            sum += q[2];
            maxPixelErr = std::max(maxPixelErr, std::abs(q[2] - exact));
        }
        maxMeanErr = std::max(maxMeanErr, std::abs(sum / h - exact));
        plainMaxErr = std::max(plainMaxErr, std::abs(std::floor(exact + 0.5) - exact));
    }
    // Every column's average tracks the float field (no steps), each pixel is within the dither.
    // Plain 8-bit rounding has a sawtooth error of up to 0.5 LSB that is constant along each band.
    CHECK(maxMeanErr < 0.25);
    CHECK(plainMaxErr > 0.45);
    CHECK(maxPixelErr <= 1.6);
}

TEST_CASE("ambient render: flowing artwork")
{
    int side = ambient::kArtSide;
    auto solid = [&](uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> bgra(static_cast<size_t>(side) * side * 4);
        for (size_t i = 0; i < bgra.size(); i += 4) { bgra[i] = b; bgra[i + 1] = g; bgra[i + 2] = r; bgra[i + 3] = 255; }
        return bgra;
    };

    // A single-colour artwork is a flat field: the colour under the scrim, wherever the copies are.
    auto grey = solid(128, 128, 128);
    auto art = ambient::PrepareArtwork(grey.data(), side);
    CHECK(std::abs(art.ground.r - 128 / 255.0f) < 1e-4f);
    auto sizes = ambient::SizesFor(400, 300);
    CHECK(sizes.sceneW == 50);
    CHECK(sizes.sceneH == 38);
    CHECK(sizes.outW == 400);
    CHECK(sizes.outH == 300);
    std::vector<uint8_t> px;
    ambient::Render(art, ambient::kStartSeconds, true, sizes, px);
    REQUIRE(px.size() == 400u * 300u * 4u);
    double expected = 128 * 0.5;   // dark scrim: black, 50 %
    for (size_t i = 0; i < px.size(); i += 4)
    {
        CHECK(std::abs(px[i + 2] - expected) <= 1.5);
        CHECK(px[i + 3] == 255);
    }
    ambient::Render(art, ambient::kStartSeconds, false, sizes, px);
    double expectedLight = 0xF5 * 0.55 + 128 * 0.45;
    CHECK(std::abs(px[2] - expectedLight) <= 1.5);

    // Saturation pushes colours away from grey.
    auto tinted = solid(160, 120, 120);
    auto sat = ambient::PrepareArtwork(tinted.data(), side);
    CHECK(sat.rgba[0] > 160 / 255.0f);
    CHECK(sat.rgba[1] < 120 / 255.0f);

    // A two-colour artwork moves: frames at different times differ, the same time is identical.
    std::vector<uint8_t> halves(static_cast<size_t>(side) * side * 4);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
        {
            uint8_t* q = &halves[(static_cast<size_t>(y) * side + x) * 4];
            q[0] = x < side / 2 ? 30 : 220; q[1] = 60; q[2] = x < side / 2 ? 220 : 30; q[3] = 255;
        }
    auto two = ambient::PrepareArtwork(halves.data(), side);
    std::vector<uint8_t> a, b, c;
    ambient::Render(two, 12.0f, true, sizes, a);
    ambient::Render(two, 12.0f, true, sizes, b);
    ambient::Render(two, 20.0f, true, sizes, c);
    CHECK(a == b);
    double diff = 0;
    for (size_t i = 0; i < a.size(); i += 4) diff += std::abs(a[i] - c[i]) + std::abs(a[i + 2] - c[i + 2]);
    CHECK(diff / (a.size() / 4) > 2.0);

    // The motion is continuous: at 60 fps no frame jumps (copies are placed with sub-pixel
    // precision in the small scene canvas, not snapped to whole canvas pixels = ~8 screen pixels).
    {
        auto window = ambient::SizesFor(1560, 1140);
        float sigma = 0.57735f * std::min(window.sceneW, window.sceneH) * 0.16f + 0.5f;
        std::vector<ambient::Rgb> prev, next;
        std::vector<double> steps;
        for (int i = 0; i <= 300; ++i)
        {
            ambient::DrawScene(two, 12.0f + i / 60.0f, window.sceneW, window.sceneH, next);
            ambient::Blur(next, window.sceneW, window.sceneH, sigma);
            if (i > 0)
            {
                double d = 0;
                for (size_t k = 0; k < next.size(); ++k)
                    d += std::abs(next[k].r - prev[k].r) + std::abs(next[k].g - prev[k].g) + std::abs(next[k].b - prev[k].b);
                steps.push_back(d / next.size());
            }
            std::swap(prev, next);
        }
        double mean = 0, peak = 0;
        for (double s : steps)
        {
            mean += s / steps.size();
            peak = std::max(peak, s);
        }
        CHECK(mean > 0);
        CHECK(peak < 2.5 * mean);
    }

    // Large windows render at a reduced output size; empty sizes give nothing.
    auto big = ambient::SizesFor(3840, 2160);
    CHECK(static_cast<double>(big.outW) * big.outH <= ambient::kMaxOutputPixels);
    CHECK(std::abs(static_cast<double>(big.outW) / big.outH - 3840.0 / 2160.0) < 0.02);
    ambient::Render(two, 12.0f, true, ambient::Sizes{ 0, 0, 0, 0 }, px);
    CHECK(px.empty());
}
