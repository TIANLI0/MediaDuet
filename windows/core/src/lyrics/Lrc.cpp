#include "md/lyrics/Lrc.h"

#include "md/util/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace md::lyrics {

namespace {

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx", "mm:ss:xx" (also h:mm:ss.xx). Returns ms.
std::optional<int64_t> parseStamp(std::string_view s) {
    int64_t parts[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    int64_t frac = 0;
    int fracDigits = 0;
    bool any = false;
    while (i < s.size()) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (n >= 3) return std::nullopt;
            parts[n] = parts[n] * 10 + (c - '0');
            if (parts[n] > 100000) return std::nullopt;
            any = true;
            ++i;
        } else if (c == ':' && n < 2 && any) {
            ++n;
            any = false;
            ++i;
            // "mm:ss:xx" form: a third group after two colons is a fraction.
        } else if (c == '.' && any) {
            ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                if (fracDigits < 3) {
                    frac = frac * 10 + (s[i] - '0');
                    ++fracDigits;
                }
                ++i;
            }
            break;
        } else {
            return std::nullopt;
        }
    }
    if (i != s.size() || n == 0) return std::nullopt;
    int64_t minutes, seconds;
    if (n == 1) {
        minutes = parts[0];
        seconds = parts[1];
    } else {
        // Either h:mm:ss or mm:ss:cc. LRC uses the latter; treat the third group as centiseconds
        // when a fraction was not given and the value is < 100.
        if (fracDigits == 0 && parts[2] < 100 && parts[1] < 60) {
            minutes = parts[0];
            seconds = parts[1];
            frac = parts[2];
            fracDigits = 2;
        } else {
            minutes = parts[0] * 60 + parts[1];
            seconds = parts[2];
        }
    }
    if (seconds >= 60) return std::nullopt;
    while (fracDigits < 3) {
        frac *= 10;
        ++fracDigits;
    }
    return (minutes * 60 + seconds) * 1000 + frac;
}

std::string_view stripBom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.remove_prefix(3);
    return s;
}

std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            auto line = s.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            out.push_back(line);
            start = i + 1;
        }
    }
    return out;
}

// Splits enhanced word timing "<00:01.00>Hel<00:01.20>lo <00:01.50>" into words.
std::vector<Word> parseWords(std::string_view body, int64_t lineStart, std::string& plain) {
    std::vector<Word> words;
    size_t i = 0;
    std::optional<int64_t> cur;
    std::string text;
    auto flush = [&](std::optional<int64_t> nextT) {
        if (!text.empty()) {
            Word w;
            w.t = cur.value_or(lineStart);
            w.text = text;
            if (nextT) w.d = std::max<int64_t>(0, *nextT - w.t);
            words.push_back(std::move(w));
            plain += text;
            text.clear();
        }
    };
    bool sawStamp = false;
    while (i < body.size()) {
        if (body[i] == '<') {
            size_t close = body.find('>', i);
            if (close != std::string_view::npos) {
                if (auto t = parseStamp(body.substr(i + 1, close - i - 1))) {
                    flush(*t);
                    cur = *t;
                    sawStamp = true;
                    i = close + 1;
                    continue;
                }
            }
        }
        text += body[i];
        ++i;
    }
    flush(std::nullopt);
    if (!sawStamp) words.clear();
    return words;
}

} // namespace

Lyrics parseLrc(std::string_view input) {
    Lyrics out;
    int64_t offset = 0;
    bool untimedText = false;
    struct Raw {
        int64_t t;
        std::string body;
        size_t order;
    };
    std::vector<Raw> raws;
    size_t order = 0;
    for (auto line : splitLines(stripBom(input))) {
        std::vector<int64_t> stamps;
        size_t i = 0;
        bool isTag = false;
        while (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string_view::npos) break;
            auto inner = line.substr(i + 1, close - i - 1);
            if (auto t = parseStamp(inner)) {
                stamps.push_back(*t);
            } else {
                auto colon = inner.find(':');
                if (colon != std::string_view::npos && stamps.empty()) {
                    std::string key = text::toLowerAscii(text::trim(inner.substr(0, colon)));
                    std::string val = text::trim(inner.substr(colon + 1));
                    if (key == "offset") {
                        try {
                            offset = std::stoll(val);
                        } catch (...) {
                        }
                    } else if (!key.empty() && key.size() <= 16) {
                        out.tags[key] = val;
                    }
                    isTag = true;
                }
                break;
            }
            i = close + 1;
        }
        if (stamps.empty()) {
            if (!isTag && !text::trim(line).empty()) untimedText = true;
            continue;
        }
        std::string body(line.substr(i));
        for (auto t : stamps) raws.push_back({t, body, order++});
    }
    if (raws.empty()) {
        if (untimedText) return fromPlainText(input);
        return out;
    }
    std::stable_sort(raws.begin(), raws.end(), [](const Raw& a, const Raw& b) { return a.t < b.t; });
    for (auto& r : raws) {
        Line l;
        l.t = std::max<int64_t>(0, r.t - offset);
        std::string plain;
        l.words = parseWords(r.body, r.t, plain);
        if (!l.words.empty()) {
            for (auto& w : l.words) w.t = std::max<int64_t>(0, w.t - offset);
            l.text = text::trim(plain);
        } else {
            l.text = text::trim(r.body);
        }
        out.lines.push_back(std::move(l));
    }
    for (size_t k = 0; k < out.lines.size(); ++k) {
        auto& l = out.lines[k];
        int64_t next = k + 1 < out.lines.size() ? out.lines[k + 1].t : -1;
        if (!l.words.empty()) {
            auto& last = l.words.back();
            if (last.d == 0) last.d = std::max<int64_t>(0, (next > 0 ? next : last.t + 1000) - last.t);
            int64_t end = last.t + last.d;
            l.d = std::max<int64_t>(0, (next > 0 ? next : end) - l.t);
        } else {
            l.d = next > 0 ? std::max<int64_t>(0, next - l.t) : 5000;
        }
    }
    out.synced = true;
    return out;
}

Lyrics fromPlainText(std::string_view input) {
    Lyrics out;
    for (auto line : splitLines(stripBom(input))) {
        Line l;
        l.text = text::trim(line);
        out.lines.push_back(std::move(l));
    }
    while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
    out.synced = false;
    return out;
}

Lyrics parseYrc(std::string_view input) {
    Lyrics out;
    for (auto raw : splitLines(stripBom(input))) {
        std::string_view line = raw;
        if (line.empty() || line.front() != '[') continue;
        size_t close = line.find(']');
        if (close == std::string_view::npos) continue;
        auto head = line.substr(1, close - 1);
        auto comma = head.find(',');
        if (comma == std::string_view::npos) continue;
        Line l;
        try {
            l.t = std::stoll(std::string(head.substr(0, comma)));
            l.d = std::stoll(std::string(head.substr(comma + 1)));
        } catch (...) {
            continue;
        }
        size_t i = close + 1;
        while (i < line.size()) {
            if (line[i] != '(') {
                ++i;
                continue;
            }
            size_t pc = line.find(')', i);
            if (pc == std::string_view::npos) break;
            auto spec = line.substr(i + 1, pc - i - 1);
            size_t next = line.find('(', pc + 1);
            // A '(' inside the lyric text is only a timing group if it parses as numbers.
            while (next != std::string_view::npos) {
                size_t nc = line.find(')', next);
                auto cand = nc == std::string_view::npos ? std::string_view() : line.substr(next + 1, nc - next - 1);
                bool timing = !cand.empty() && cand.find_first_not_of("0123456789,") == std::string_view::npos &&
                              std::count(cand.begin(), cand.end(), ',') == 2;
                if (timing) break;
                next = line.find('(', next + 1);
            }
            std::string word(line.substr(pc + 1, (next == std::string_view::npos ? line.size() : next) - pc - 1));
            Word w;
            auto c1 = spec.find(','), c2 = spec.find(',', c1 + 1);
            if (c1 == std::string_view::npos || c2 == std::string_view::npos) break;
            try {
                w.t = std::stoll(std::string(spec.substr(0, c1)));
                w.d = std::stoll(std::string(spec.substr(c1 + 1, c2 - c1 - 1)));
            } catch (...) {
                break;
            }
            w.text = word;
            l.text += word;
            l.words.push_back(std::move(w));
            i = next == std::string_view::npos ? line.size() : next;
        }
        l.text = text::trim(l.text);
        if (!l.words.empty()) out.lines.push_back(std::move(l));
    }
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const Line& a, const Line& b) { return a.t < b.t; });
    out.synced = !out.lines.empty();
    return out;
}

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        auto ent = s.substr(i + 1, semi - i - 1);
        std::string rep;
        if (ent == "apos") rep = "'";
        else if (ent == "quot") rep = "\"";
        else if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "nbsp") rep = " ";
        else if (!ent.empty() && ent[0] == '#') {
            try {
                unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                       ? std::stoul(std::string(ent.substr(2)), nullptr, 16)
                                       : std::stoul(std::string(ent.substr(1)));
                std::wstring w;
                if (cp < 0x10000) w.push_back(static_cast<wchar_t>(cp));
                rep = text::toUtf8(w);
            } catch (...) {
            }
        }
        if (rep.empty()) {
            out += s[i];
            continue;
        }
        out += rep;
        i = semi;
    }
    return out;
}

bool isInstrumentalNotice(const Lyrics& l) {
    if (l.lines.empty() || l.lines.size() > 3) return false;
    for (auto& x : l.lines)
        if (x.text.find("\xE7\xBA\xAF\xE9\x9F\xB3\xE4\xB9\x90") != std::string::npos || // 纯音乐
            text::toLowerAscii(x.text).find("instrumental") != std::string::npos)
            return true;
    return false;
}

namespace {
// Text identity for aligning a line across two versions of one lyric: no whitespace, ASCII lower case.
std::string lineKey(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    }
    return out;
}

Line* nearest(Lyrics& l, int64_t t, int64_t tol) {
    Line* best = nullptr;
    int64_t bestDiff = tol + 1;
    for (auto& x : l.lines) {
        int64_t d = std::llabs(x.t - t);
        if (d < bestDiff) {
            bestDiff = d;
            best = &x;
        }
    }
    return best;
}
} // namespace

size_t mergeTranslation(Lyrics& base, const Lyrics& tr, int64_t tol, const Lyrics* timedLike) {
    if (!base.synced || !tr.synced) return 0;
    size_t n = 0;
    for (auto& t : tr.lines) {
        std::string text = text::trim(t.text);
        if (text.empty() || text == "//") continue;
        Line* best = nullptr;
        if (timedLike && timedLike->synced) {
            // The translation's own line (same timestamp) in the LRC, then that text in `base`.
            const Line* via = nullptr;
            int64_t viaDiff = tol + 1;
            for (auto& v : timedLike->lines) {
                int64_t d = std::llabs(v.t - t.t);
                if (d < viaDiff) {
                    viaDiff = d;
                    via = &v;
                }
            }
            if (via && !lineKey(via->text).empty()) {
                std::string key = lineKey(via->text);
                int64_t bestDiff = 15'000 + 1; // the same line, not a repeat of it far away
                for (auto& l : base.lines) {
                    int64_t d = std::llabs(l.t - via->t);
                    if (d < bestDiff && lineKey(l.text) == key) {
                        bestDiff = d;
                        best = &l;
                    }
                }
                if (!best) best = nearest(base, via->t, std::max<int64_t>(tol, 2000));
            }
        }
        if (!best) best = nearest(base, t.t, tol);
        if (!best || best->text.empty() || text == best->text) continue;
        if (best->translation.empty()) ++n;
        best->translation = text;
    }
    return n;
}

bool hasTranslation(const Lyrics& l) {
    for (auto& x : l.lines)
        if (!x.translation.empty()) return true;
    return false;
}

static std::string stamp(int64_t ms);

std::string translationLrc(const Lyrics& l) {
    std::string out;
    for (auto& line : l.lines)
        if (!line.translation.empty()) out += "[" + stamp(line.t) + "]" + line.translation + "\n";
    return out.empty() ? out : std::string(kTranslationMark) + "\n" + out;
}

static std::string stamp(int64_t ms) {
    char buf[16];
    if (ms < 0) ms = 0;
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld.%02lld", static_cast<long long>(ms / 60000),
                  static_cast<long long>((ms / 1000) % 60), static_cast<long long>((ms % 1000) / 10));
    return buf;
}

std::string toLrc(const Lyrics& l) {
    std::string out;
    for (auto& line : l.lines) {
        out += "[" + stamp(line.t) + "]";
        if (line.words.empty()) {
            out += line.text;
        } else {
            for (auto& w : line.words) out += "<" + stamp(w.t) + ">" + w.text;
            auto& last = line.words.back();
            out += "<" + stamp(last.t + last.d) + ">";
        }
        out += "\n";
    }
    return out;
}

int lineAt(const Lyrics& l, int64_t pos) {
    if (!l.synced || l.lines.empty() || pos < l.lines.front().t) return -1;
    auto it = std::upper_bound(l.lines.begin(), l.lines.end(), pos,
                               [](int64_t p, const Line& x) { return p < x.t; });
    return static_cast<int>(std::distance(l.lines.begin(), it)) - 1;
}

nlohmann::json toJson(const Lyrics& l) {
    nlohmann::json lines = nlohmann::json::array();
    for (auto& x : l.lines) {
        nlohmann::json j = {{"t", x.t}, {"d", x.d}, {"text", x.text}};
        if (!x.translation.empty()) j["tr"] = x.translation;
        if (!x.words.empty()) {
            auto& w = j["words"] = nlohmann::json::array();
            for (auto& y : x.words) w.push_back({{"t", y.t}, {"d", y.d}, {"text", y.text}});
        }
        lines.push_back(std::move(j));
    }
    return {{"synced", l.synced}, {"lines", std::move(lines)}};
}

std::string normalize(std::string_view s) {
    std::string in = text::toLowerAscii(s);
    // Remove bracketed parts: (), [], （）, 【】
    std::string out;
    int depth = 0;
    for (size_t i = 0; i < in.size();) {
        auto starts = [&](std::string_view tok) { return in.compare(i, tok.size(), tok) == 0; };
        if (in[i] == '(' || in[i] == '[') {
            ++depth;
            ++i;
            continue;
        }
        if (starts("\xEF\xBC\x88") || starts("\xE3\x80\x90")) { // （ 【
            ++depth;
            i += 3;
            continue;
        }
        if (in[i] == ')' || in[i] == ']') {
            if (depth) --depth;
            ++i;
            continue;
        }
        if (starts("\xEF\xBC\x89") || starts("\xE3\x80\x91")) { // ） 】
            if (depth) --depth;
            i += 3;
            continue;
        }
        if (depth == 0) out += in[i];
        ++i;
    }
    // Cut " - live", " feat. x", " ft. x"
    for (std::string_view cut : {" feat.", " feat ", " ft.", " - "}) {
        auto p = out.find(cut);
        if (p != std::string::npos && p > 0) out.erase(p);
    }
    std::string res;
    for (unsigned char c : out) {
        if (c < 0x80) {
            if (std::isalnum(c)) res += static_cast<char>(c);
        } else {
            res += static_cast<char>(c);
        }
    }
    return res;
}

static std::vector<std::string> splitArtists(const std::string& a) {
    std::vector<std::string> out;
    std::string cur;
    std::string s = text::toLowerAscii(a);
    for (size_t i = 0; i < s.size(); ++i) {
        bool sep = s[i] == ',' || s[i] == '/' || s[i] == '&' || s[i] == ';';
        if (!sep && s.compare(i, 3, "\xE3\x80\x81") == 0) { // 、
            sep = true;
            i += 2;
        }
        if (sep) {
            if (auto n = normalize(cur); !n.empty()) out.push_back(n);
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    if (auto n = normalize(cur); !n.empty()) out.push_back(n);
    return out;
}

double matchScore(const Candidate& want, const Candidate& have) {
    if (want.durationMs && have.durationMs) {
        int64_t diff = std::llabs(*want.durationMs - *have.durationMs);
        if (diff > 5000) return 0;
    }
    std::string wt = normalize(want.title), ht = normalize(have.title);
    if (wt.empty() || ht.empty()) return 0;
    double score = 0;
    if (wt == ht) score += 0.6;
    else if (wt.find(ht) != std::string::npos || ht.find(wt) != std::string::npos) score += 0.35;
    else return 0;
    auto wa = splitArtists(want.artist), ha = splitArtists(have.artist);
    if (wa.empty() || ha.empty()) {
        score += 0.1;
    } else {
        bool overlap = false;
        for (auto& x : wa)
            for (auto& y : ha)
                if (x == y || x.find(y) != std::string::npos || y.find(x) != std::string::npos) overlap = true;
        if (overlap) score += 0.3;
        else score -= 0.2;
    }
    if (want.durationMs && have.durationMs) {
        int64_t diff = std::llabs(*want.durationMs - *have.durationMs);
        score += diff <= 2000 ? 0.1 : 0.0;
    }
    return std::clamp(score, 0.0, 1.0);
}

bool sameSong(const Candidate& a, const Candidate& b, int64_t durationToleranceMs) {
    if (a.durationMs && b.durationMs && *a.durationMs > 0 && *b.durationMs > 0 &&
        std::llabs(*a.durationMs - *b.durationMs) > durationToleranceMs)
        return false;
    auto same = [](const std::string& x, const std::string& y) {
        std::string tx = text::trim(x), ty = text::trim(y);
        if (tx == ty) return true;
        std::string nx = normalize(tx), ny = normalize(ty);
        return !nx.empty() && nx == ny;
    };
    if (text::trim(a.title).empty() || text::trim(b.title).empty()) return false;
    return same(a.title, b.title) && same(a.artist, b.artist);
}

} // namespace md::lyrics
