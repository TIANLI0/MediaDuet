#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace md::lyrics {

struct Word {
    int64_t t = 0; // ms
    int64_t d = 0;
    std::string text;
};

struct Line {
    int64_t t = 0;
    int64_t d = 0;
    std::string text;
    std::vector<Word> words; // only for word-timed (enhanced) LRC
    std::string translation; // optional translated line
};

struct Lyrics {
    std::vector<Line> lines;
    bool synced = false;
    std::map<std::string, std::string> tags;
    bool empty() const { return lines.empty(); }
};

// Parses LRC: multiple timestamps per line, [offset:±ms], metadata tags and the
// enhanced <mm:ss.xx> word timing. Lines without timestamps make it "unsynced".
Lyrics parseLrc(std::string_view text);
Lyrics fromPlainText(std::string_view text);
// NetEase "yrc" word-timed format: `[lineStartMs,lineDurMs](wordStartMs,wordDurMs,0)word…`;
// JSON metadata lines ({"t":…}) are ignored.
Lyrics parseYrc(std::string_view text);
// Decodes the HTML entities QQ Music uses in lyrics (&apos; &quot; &amp; &lt; &gt; &#NN;).
std::string decodeEntities(std::string_view s);
// True if the lyrics only say "instrumental" (e.g. 纯音乐，请欣赏) rather than words.
bool isInstrumentalNotice(const Lyrics& l);
// Attaches translated lines (an LRC with the same timestamps, e.g. NetEase tlyric or
// QQ Music trans) to the closest original line within toleranceMs. Returns how many.
size_t mergeTranslation(Lyrics& base, const Lyrics& translation, int64_t toleranceMs = 600);
bool hasTranslation(const Lyrics& l);
// The translations as an LRC (line timestamps of the base lyrics).
std::string translationLrc(const Lyrics& l);

// Serialises synced lyrics as LRC (enhanced <mm:ss.xx> word stamps when word-timed).
std::string toLrc(const Lyrics& l);

// Index of the line active at posMs, or -1 before the first line.
int lineAt(const Lyrics& l, int64_t posMs);

nlohmann::json toJson(const Lyrics& l);

// Normalises a title/artist for comparisons: lower case, strips bracketed
// suffixes such as "(Live)", "[Remastered]", "feat." parts and punctuation.
std::string normalize(std::string_view s);

struct Candidate {
    std::string title, artist, album;
    std::optional<int64_t> durationMs;
};
// Score in [0,1]; 0 means "do not use". Duration differences > 5 s are rejected.
double matchScore(const Candidate& want, const Candidate& have);

// True if two reports describe the same song of one player: equal non-empty titles and
// equal artists after normalize() (or literally equal), and durations within
// durationToleranceMs (or either unknown). The album is ignored. Used to keep lyrics
// when a player's trackId churns for one song (a late or jittery duration, an album
// filled in later) instead of reloading them.
bool sameSong(const Candidate& a, const Candidate& b, int64_t durationToleranceMs = 3000);

} // namespace md::lyrics
