#include "md/lyrics/Karaoke.h"

#include "md/util/Text.h"

#include <algorithm>
#include <cmath>

namespace md::lyrics {

namespace {

bool isSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0x3000 || c == 0x200B ||
           c == 0xFEFF;
}

uint32_t countCodePoints(const std::wstring& s) {
    uint32_t n = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] >= 0xDC00 && s[i] <= 0xDFFF && i > 0 && s[i - 1] >= 0xD800 && s[i - 1] <= 0xDBFF)) ++n;
    return n;
}

} // namespace

KaraokeLine buildKaraoke(const Line& line) {
    KaraokeLine k;
    k.t = line.t;
    k.d = line.d;
    if (!line.words.empty()) {
        std::wstring all;
        std::vector<std::pair<uint32_t, uint32_t>> spans; // [start, end) in `all`
        for (auto& w : line.words) {
            uint32_t s = static_cast<uint32_t>(all.size());
            all += text::toWide(w.text);
            spans.emplace_back(s, static_cast<uint32_t>(all.size()));
        }
        size_t lead = 0;
        while (lead < all.size() && isSpace(all[lead])) ++lead;
        size_t end = all.size();
        while (end > lead && isSpace(all[end - 1])) --end;
        if (end > lead) {
            k.text = all.substr(lead, end - lead);
            uint32_t n = static_cast<uint32_t>(k.text.size());
            for (size_t i = 0; i < line.words.size(); ++i) {
                auto clampPos = [&](uint32_t p) {
                    int64_t q = static_cast<int64_t>(p) - static_cast<int64_t>(lead);
                    return static_cast<uint32_t>(std::clamp<int64_t>(q, 0, n));
                };
                uint32_t s = clampPos(spans[i].first), e = clampPos(spans[i].second);
                k.words.push_back({s, e - s, line.words[i].t, line.words[i].d});
            }
            k.codePoints = countCodePoints(k.text);
            return k;
        }
    }
    std::wstring w = text::toWide(line.text);
    size_t lead = 0;
    while (lead < w.size() && isSpace(w[lead])) ++lead;
    size_t end = w.size();
    while (end > lead && isSpace(w[end - 1])) --end;
    k.text = w.substr(lead, end - lead);
    k.codePoints = countCodePoints(k.text);
    return k;
}

double lineSweepMs(const KaraokeLine& k) {
    double d = static_cast<double>(k.d > 0 ? k.d : 4000);
    return std::min(d * 0.9, std::max(1200.0, k.codePoints * 450.0));
}

FillSpan fillAt(const KaraokeLine& k, double p) {
    uint32_t total = static_cast<uint32_t>(k.text.size());
    if (total == 0) return {};
    if (!k.words.empty()) {
        if (p < static_cast<double>(k.words.front().t)) return {0, 0, 0};
        for (auto& w : k.words) {
            double start = static_cast<double>(w.t);
            double dur = static_cast<double>(std::max<int64_t>(1, w.d));
            if (p < start) return {w.start, w.start, 0}; // gap between words: hold
            if (p < start + dur) return {w.start, w.start + w.len, w.len ? (p - start) / dur : 0.0};
        }
        return {total, total, 0};
    }
    double frac = std::clamp((p - static_cast<double>(k.t)) / lineSweepMs(k), 0.0, 1.0);
    return {0, total, frac};
}

bool fillMoving(const KaraokeLine& k, double p, double* nextMs) {
    if (nextMs) *nextMs = -1;
    if (k.text.empty()) return false;
    if (!k.words.empty()) {
        for (auto& w : k.words) {
            double start = static_cast<double>(w.t);
            double dur = static_cast<double>(std::max<int64_t>(1, w.d));
            if (w.len == 0) continue;
            if (p < start) {
                if (nextMs) *nextMs = start;
                return false;
            }
            if (p < start + dur) return true;
        }
        return false;
    }
    double start = static_cast<double>(k.t);
    if (p < start) {
        if (nextMs) *nextMs = start;
        return false;
    }
    return p < start + lineSweepMs(k);
}

LyricRows buildRows(const Lyrics& l) {
    LyricRows out;
    if (!l.synced) return out;
    const auto& lines = l.lines;
    size_t n = lines.size();
    std::vector<bool> blank(n);
    for (size_t i = 0; i < n; ++i) blank[i] = buildKaraoke(lines[i]).empty();
    size_t first = 0;
    while (first < n && blank[first]) ++first;
    if (first == n) return out;
    auto& rows = out.rows;
    if (lines[first].t >= kMinGapMs) rows.push_back({LyricRow::Kind::Gap, -1, 0, lines[first].t});
    // Timing of the last sung line, for endMs.
    int64_t lastStart = 0, lastDuration = 0, lastVocalEnd = 0;
    for (size_t i = first; i < n; ++i) {
        const auto& line = lines[i];
        bool hasNext = i + 1 < n;
        int64_t next = hasNext ? lines[i + 1].t : 0;
        if (blank[i]) {
            // An empty timestamp line marks an interlude; short ones are dropped.
            if (hasNext && next - line.t >= kMinGapMs) rows.push_back({LyricRow::Kind::Gap, -1, line.t, next});
            continue;
        }
        // Same estimates as Android's LyricTiming.prepare: a word lasts until the next word starts,
        // the last one until the next line (or 1 s); singing ends at most 1.5 s into the last word.
        int64_t start = line.t, vocalEnd = 0, duration = 0, sungEnd = 0;
        if (!line.words.empty()) {
            const auto& last = line.words.back();
            int64_t lastDur = std::max<int64_t>(1, (hasNext ? next : last.t + 1000) - last.t);
            start = std::min(line.t, line.words.front().t);
            vocalEnd = sungEnd = last.t + std::min<int64_t>(lastDur, 1500);
            duration = std::max<int64_t>(0, (hasNext ? next : last.t + lastDur) - line.t);
        } else {
            KaraokeLine k = buildKaraoke(line);
            duration = hasNext ? std::max<int64_t>(0, next - line.t) : 5000;
            k.d = duration;
            int64_t sweep = std::max<int64_t>(1, static_cast<int64_t>(lineSweepMs(k)));
            sungEnd = line.t + sweep;
            // A generous estimate, so long held notes are not mistaken for interludes.
            vocalEnd = std::max(sungEnd, line.t + static_cast<int64_t>(k.codePoints) * 450 + 2000);
            if (hasNext) vocalEnd = std::min(next, vocalEnd);
        }
        // Row starts stay sorted (binary search) even for odd word timestamps.
        if (!rows.empty()) start = std::max(start, rows.back().start);
        rows.push_back({LyricRow::Kind::Line, static_cast<int>(i), start, vocalEnd});
        lastStart = start;
        lastDuration = duration;
        lastVocalEnd = sungEnd;
        if (!hasNext || blank[i + 1]) continue;
        if (next - vocalEnd >= kMinGapMs && vocalEnd > start) rows.push_back({LyricRow::Kind::Gap, -1, vocalEnd, next});
    }
    const auto& tail = rows.back();
    out.endMs = tail.kind == LyricRow::Kind::Gap
                    ? tail.end
                    : std::max(lastStart + std::min<int64_t>(lastDuration, 5000), lastVocalEnd) + kEndHoldMs;
    return out;
}

int LyricRows::activeAt(int64_t posMs) const {
    if (rows.empty()) return -1;
    if (posMs >= endMs) return kEnded;
    auto it = std::upper_bound(rows.begin(), rows.end(), posMs, [](int64_t p, const LyricRow& r) { return p < r.start; });
    return static_cast<int>(it - rows.begin()) - 1;
}

int LyricRows::lineAt(int64_t posMs) const {
    int i = activeAt(posMs);
    return i >= 0 && rows[i].kind == LyricRow::Kind::Line ? rows[i].line : -1;
}

int64_t LyricRows::nextChange(int64_t posMs) const {
    if (rows.empty() || posMs >= endMs) return -1;
    auto it = std::upper_bound(rows.begin(), rows.end(), posMs, [](int64_t p, const LyricRow& r) { return p < r.start; });
    return it != rows.end() ? std::min(it->start, endMs) : endMs;
}

float scrollOffset(float textWidth, float viewWidth, float fillX, float anchor) {
    if (textWidth <= viewWidth || viewWidth <= 0) return 0.f;
    float maxOff = textWidth - viewWidth;
    return std::clamp(fillX - viewWidth * anchor, 0.f, maxOff);
}

float fitScale(float textWidth, float viewWidth, float minScale) {
    if (textWidth <= viewWidth || textWidth <= 0) return 1.f;
    return std::max(minScale, viewWidth / textWidth);
}

namespace {

DesktopRows layoutRows(const Lyrics& l, int main, int next, bool active, int parity, RowLayout layout, bool twoLines,
                       bool showTranslation) {
    DesktopRows r;
    int n = static_cast<int>(l.lines.size());
    auto lineRow = [&](int i, bool act) {
        return i >= 0 && i < n ? RowContent{RowKind::Line, i, act} : RowContent{};
    };
    // A translated song stays centred for its whole length (no flipping between
    // layouts on the odd untranslated line; that line simply has no second row).
    bool trSong = showTranslation && hasTranslation(l);
    if (trSong || layout == RowLayout::Center || !twoLines) {
        r.centered = true;
        r.row[0] = lineRow(main, active);
        if (trSong && !l.lines[main].translation.empty()) r.row[1] = {RowKind::Translation, main, false};
        else if (!trSong && twoLines) r.row[1] = lineRow(next, false);
        return r;
    }
    // Alternate: line i in row i % 2; the other row previews the upcoming line.
    int a = parity % 2;
    r.row[a] = lineRow(main, active);
    r.row[1 - a] = lineRow(next, false);
    r.alignRight[1] = true;
    return r;
}

} // namespace

DesktopRows desktopRows(const Lyrics& l, int cur, RowLayout layout, bool twoLines, bool showTranslation) {
    int n = static_cast<int>(l.lines.size());
    if (n == 0) return {};
    int main = cur >= 0 ? std::min(cur, n - 1) : 0; // before the first line: preview it
    return layoutRows(l, main, main + 1, cur >= 0, main, layout, twoLines, showTranslation);
}

DesktopRows desktopRows(const Lyrics& l, const LyricRows& rows, int64_t posMs, RowLayout layout, bool twoLines,
                        bool showTranslation, int* mainLine) {
    if (mainLine) *mainLine = -1;
    int nr = static_cast<int>(rows.rows.size());
    if (nr == 0) return {};
    int at = rows.activeAt(posMs);
    // The line row to show: the current one, else the next one (intro / gap), else the last one.
    int ri = -1;
    bool active = false;
    if (at >= 0 && rows.rows[at].kind == LyricRow::Kind::Line) {
        ri = at;
        active = true;
    } else {
        int from = at == LyricRows::kEnded ? nr : at + 1;
        for (int i = from; i < nr && ri < 0; ++i)
            if (rows.rows[i].kind == LyricRow::Kind::Line) ri = i;
        for (int i = std::min(from, nr) - 1; i >= 0 && ri < 0; --i)
            if (rows.rows[i].kind == LyricRow::Kind::Line) ri = i;
    }
    if (ri < 0) return {};
    // Parity counts sung lines, so the two alternating rows never collide over a blank line.
    int parity = 0, next = -1;
    for (int i = 0; i < ri; ++i)
        if (rows.rows[i].kind == LyricRow::Kind::Line) ++parity;
    for (int i = ri + 1; i < nr && next < 0; ++i)
        if (rows.rows[i].kind == LyricRow::Kind::Line) next = rows.rows[i].line;
    int main = rows.rows[ri].line;
    if (mainLine) *mainLine = main;
    return layoutRows(l, main, next, active, parity, layout, twoLines, showTranslation);
}

} // namespace md::lyrics
