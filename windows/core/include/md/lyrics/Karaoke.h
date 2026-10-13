#pragma once

// Karaoke timing shared by every desktop surface (desktop lyrics overlay, taskbar
// widget; the WinUI app mirrors the same rules, see docs/ipc.md "Lyrics timing").
// Pure logic: text positions are UTF-16 code units of the display text, so the
// renderers map them straight to DirectWrite caret positions.

#include "md/lyrics/Lrc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace md::lyrics {

struct KaraokeWord {
    uint32_t start = 0; // UTF-16 offset into KaraokeLine::text
    uint32_t len = 0;   // UTF-16 units (0 when the word was trimmed away)
    int64_t t = 0;      // ms
    int64_t d = 0;
};

struct KaraokeLine {
    std::wstring text;              // what is drawn (trimmed)
    std::vector<KaraokeWord> words; // empty = line-timed
    int64_t t = 0, d = 0;
    uint32_t codePoints = 0;        // of text (line-timed sweep pace)
    bool empty() const { return text.empty(); }
};

// Display text and word spans for a line. Word-timed lines are drawn as the
// concatenation of their words (line.text may have lost characters to trimming);
// leading/trailing whitespace of the whole line is trimmed and the word spans are
// shifted/clamped accordingly. Inner whitespace (also runs of spaces) belongs to
// the word that carries it. Lines whose words carry no visible text fall back to
// line.text, line-timed.
KaraokeLine buildKaraoke(const Line& line);

// Progress of the fill: the edge lies between the carets at `from` and `to`
// (UTF-16 offsets, always on code-point boundaries), `frac` of the way.
struct FillSpan {
    uint32_t from = 0, to = 0;
    double frac = 0;
    bool operator==(const FillSpan&) const = default;
};

// Line-timed sweep duration: min(0.9 x d, max(1200 ms, codePoints x 450 ms)),
// d = 4000 ms when unknown.
double lineSweepMs(const KaraokeLine& k);

// Where the fill edge is at posMs. Word-timed: inside a word it moves linearly
// over that word, in gaps it holds at the next word's start, after the last word
// it is complete. Line-timed: an even sweep over lineSweepMs().
FillSpan fillAt(const KaraokeLine& k, double posMs);

// True while the edge moves (frames are needed); otherwise *nextMs (if given)
// receives the line-relative absolute time at which it starts moving again
// (-1 = never within this line).
bool fillMoving(const KaraokeLine& k, double posMs, double* nextMs = nullptr);

// ---------------------------------------------------------------- layout helpers

// Horizontal scroll (QQ Music style) of a line wider than the view, in the same
// units as the arguments: start aligned until the fill edge passes `anchor`
// (0.6) of the view, then the edge stays there, finally end aligned. 0 when the
// text fits.
float scrollOffset(float textWidth, float viewWidth, float fillX, float anchor = 0.6f);

// Font scale (minScale..1) that fits textWidth into viewWidth; long lines shrink
// at most to minScale and scroll beyond that.
float fitScale(float textWidth, float viewWidth, float minScale = 0.85f);

// Which line each of the two desktop-lyrics rows shows.
enum class RowLayout { Alternate, Center };
enum class RowKind { None, Line, Translation };
struct RowContent {
    RowKind kind = RowKind::None;
    int line = -1;
    bool active = false; // the line being sung (karaoke fill)
    bool operator==(const RowContent&) const = default;
};
struct DesktopRows {
    RowContent row[2];
    bool alignRight[2] = {false, false}; // Alternate: row 2 is right aligned
    bool centered = false;
};
// cur = lineAt() result (-1 before the first line). Alternate (classic two-line
// KTV): line i lives in row i % 2 (row 1 left, row 2 right aligned) and the other
// row shows the upcoming line, so a line change only replaces one row. Center:
// row 1 = current line, row 2 = the next line (twoLines). When translations are
// shown and the lyrics have any, Center is used for the whole song and row 2 is
// the current line's translation (empty for untranslated lines). twoLines=false
// keeps a single row.
DesktopRows desktopRows(const Lyrics& l, int cur, RowLayout layout, bool twoLines, bool showTranslation);

} // namespace md::lyrics
