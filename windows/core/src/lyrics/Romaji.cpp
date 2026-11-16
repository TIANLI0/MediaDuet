#include "md/lyrics/Romaji.h"

#include "md/util/Text.h"

#include <cstring>

namespace md::lyrics {

namespace {

// Hiragana U+3041 .. U+3093.
constexpr const char* kRomaji[] = {
    "a",  "a",  "i",   "i",  "u",   "u",   "e",  "e",  "o",  "o",                                  // ぁ..ぉ
    "ka", "ga", "ki",  "gi", "ku",  "gu",  "ke", "ge", "ko", "go",                                 // か..ご
    "sa", "za", "shi", "ji", "su",  "zu",  "se", "ze", "so", "zo",                                 // さ..ぞ
    "ta", "da", "chi", "ji", "",    "tsu", "zu", "te", "de", "to", "do",                           // た..ど (っ is handled apart)
    "na", "ni", "nu",  "ne", "no",                                                                 // な..の
    "ha", "ba", "pa",  "hi", "bi",  "pi",  "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",   // は..ぽ
    "ma", "mi", "mu",  "me", "mo",                                                                 // ま..も
    "ya", "ya", "yu",  "yu", "yo",  "yo",                                                          // ゃ..よ
    "ra", "ri", "ru",  "re", "ro",                                                                 // ら..ろ
    "wa", "wa", "i",   "e",  "o",   "n"};                                                          // ゎ..ん

constexpr char32_t kFirst = 0x3041, kLast = 0x3093;
static_assert(sizeof(kRomaji) / sizeof(kRomaji[0]) == kLast - kFirst + 1, "kana table out of step");

bool isVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// Katakana -> hiragana; everything else unchanged.
char32_t fold(char32_t c) { return c >= 0x30A1 && c <= 0x30F3 ? c - 0x60 : c; }

bool isHan(char32_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || c == 0x3005; }

const char* punctuation(char32_t c) {
    switch (c) {
    case 0x3001: return ", ";
    case 0x3002: return ". ";
    case 0x30FB: return " ";
    case 0x3000: return " ";
    case 0xFF01: return "!";
    case 0xFF1F: return "?";
    case 0x300C: case 0x300D: case 0x300E: case 0x300F: return "\"";
    case 0x301C: return "~";
    default: return nullptr;
    }
}

bool startsVowelKana(char32_t c) {
    switch (c) {
    case 0x3042: case 0x3044: case 0x3046: case 0x3048: case 0x304A: // あ い う え お
    case 0x3084: case 0x3086: case 0x3088:                          // や ゆ よ
        return true;
    default: return false;
    }
}

} // namespace

std::string kanaToRomaji(std::string_view utf8) {
    std::wstring w = text::toWide(utf8);
    std::string out;
    bool hasKana = false, sokuon = false;
    auto emit = [&](std::string s) {
        if (sokuon && !s.empty() && !isVowel(s[0]) && s[0] != 'n')
            s = (s.compare(0, 2, "ch") == 0 ? std::string("t") : std::string(1, s[0])) + s;
        sokuon = false;
        out += s;
    };
    for (size_t i = 0; i < w.size(); ++i) {
        char32_t raw = w[i];
        char32_t c = fold(raw);
        if (isHan(raw)) return {}; // needs a dictionary: leave it to the provider
        if (c == 0x3063) {         // っ
            sokuon = hasKana = true;
            continue;
        }
        if (raw == 0x30FC) { // ー lengthens the vowel before it
            if (!out.empty() && isVowel(out.back())) out += out.back();
            continue;
        }
        std::string r;
        if (raw == 0x30F4) r = "vu";       // ヴ
        else if (raw == 0x30F5) r = "ka";  // ヵ
        else if (raw == 0x30F6) r = "ke";  // ヶ
        else if (c >= kFirst && c <= kLast) r = kRomaji[c - kFirst];
        if (r.empty()) {
            sokuon = false;
            if (const char* p = punctuation(raw)) out += p;
            else out += text::toUtf8(std::wstring(1, static_cast<wchar_t>(raw)));
            continue;
        }
        hasKana = true;
        if (c == 0x3093) { // ん: n' before a vowel or y so "kan'i" differs from "kani"
            bool apostrophe = i + 1 < w.size() && startsVowelKana(fold(w[i + 1]));
            emit(apostrophe ? "n'" : "n");
            continue;
        }
        // A small ゃゅょ or ぁぃぅぇぉ right after combines with this kana.
        bool small = c == 0x3083 || c == 0x3085 || c == 0x3087 || (c <= 0x3049 && (c & 1) == 1); // ゃゅょ ぁぃぅぇぉ
        if (i + 1 < w.size() && !small) {
            char32_t n = fold(w[i + 1]);
            bool smallY = n == 0x3083 || n == 0x3085 || n == 0x3087;
            bool smallV = n == 0x3041 || n == 0x3043 || n == 0x3045 || n == 0x3047 || n == 0x3049;
            if (smallY || smallV) {
                char v = kRomaji[n - kFirst][std::strlen(kRomaji[n - kFirst]) - 1]; // the vowel: "ya" -> a
                std::string stem = r;
                if (smallY) {
                    bool soft = r == "shi" || r == "chi" || r == "ji";
                    stem.pop_back(); // the i (or the u of "fu")
                    if (!soft) stem += 'y';
                } else if (r == "u") {
                    stem = "w";
                } else if (r == "vu") {
                    stem = "v";
                } else {
                    stem.pop_back();
                }
                emit(stem + v);
                ++i;
                continue;
            }
        }
        emit(r);
    }
    return hasKana ? out : std::string();
}

size_t fillLocalRomaji(Lyrics& l) {
    size_t n = 0;
    for (auto& line : l.lines) {
        if (!line.romaji.empty() || line.text.empty()) continue;
        line.romaji = kanaToRomaji(line.text);
        if (!line.romaji.empty()) ++n;
    }
    return n;
}

} // namespace md::lyrics
