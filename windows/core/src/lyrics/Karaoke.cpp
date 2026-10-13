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

float scrollOffset(float textWidth, float viewWidth, float fillX, float anchor) {
    if (textWidth <= viewWidth || viewWidth <= 0) return 0.f;
    float maxOff = textWidth - viewWidth;
    return std::clamp(fillX - viewWidth * anchor, 0.f, maxOff);
}

float fitScale(float textWidth, float viewWidth, float minScale) {
    if (textWidth <= viewWidth || textWidth <= 0) return 1.f;
    return std::max(minScale, viewWidth / textWidth);
}

DesktopRows desktopRows(const Lyrics& l, int cur, RowLayout layout, bool twoLines, bool showTranslation) {
    DesktopRows r;
    int n = static_cast<int>(l.lines.size());
    if (n == 0) return r;
    auto lineRow = [&](int i, bool active) {
        return i >= 0 && i < n ? RowContent{RowKind::Line, i, active} : RowContent{};
    };
    int main = cur >= 0 ? std::min(cur, n - 1) : 0; // before the first line: preview it
    bool active = cur >= 0;
    // A translated song stays centred for its whole length (no flipping between
    // layouts on the odd untranslated line; that line simply has no second row).
    bool trSong = showTranslation && hasTranslation(l);
    if (trSong || layout == RowLayout::Center || !twoLines) {
        r.centered = true;
        r.row[0] = lineRow(main, active);
        if (trSong && !l.lines[main].translation.empty()) r.row[1] = {RowKind::Translation, main, false};
        else if (!trSong && twoLines) r.row[1] = lineRow(main + 1, false);
        return r;
    }
    // Alternate: line i in row i % 2; the other row previews the upcoming line.
    int a = main % 2;
    r.row[a] = lineRow(main, active);
    r.row[1 - a] = lineRow(main + 1, false);
    r.alignRight[1] = true;
    return r;
}

} // namespace md::lyrics
