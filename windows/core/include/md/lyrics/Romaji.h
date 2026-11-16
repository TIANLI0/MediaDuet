#pragma once

#include "md/lyrics/Lrc.h"

#include <string>
#include <string_view>

namespace md::lyrics {

// Hepburn romanisation of a Japanese line written in kana (hiragana and katakana, small kana,
// sokuon, the long-vowel mark). Returns "" when there is nothing to romanise: no kana at all
// (English or Chinese lines) or kanji, which need a dictionary (those lines get their romaji
// from the provider, e.g. NetEase "romalrc", see mergeRomaji).
// Particles are not guessed: は stays "ha", へ "he", を "o".
std::string kanaToRomaji(std::string_view utf8);

// Fills Line::romaji of every line that has none with kanaToRomaji(text). Returns how many.
size_t fillLocalRomaji(Lyrics& l);

} // namespace md::lyrics
