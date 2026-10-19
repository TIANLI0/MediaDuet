#pragma once

#include "md/lyrics/Lrc.h"

#include <winrt/Windows.Web.Http.h>

#include <cstdint>
#include <optional>
#include <string>

namespace md::platform {

// Anonymous HTTP helpers for the public web APIs of the music catalogues. Blocking:
// background threads only.
namespace web {
// No shared WinINet cookies or cache, a browser User-Agent and the given Referer.
winrt::Windows::Web::Http::HttpClient browserClient(const wchar_t* referer);
// GET with a 10 s timeout: the body, "" for 404, nullopt on other failures.
std::optional<std::string> get(winrt::Windows::Web::Http::HttpClient& client, const std::wstring& url);
std::optional<std::string> postJson(winrt::Windows::Web::Http::HttpClient& client, const std::wstring& url,
                                    const std::string& body);
std::wstring esc(const std::string& s);
} // namespace web

// Song lookups in NetEase Cloud Music and QQ Music by title / artist / album / duration (only
// those are sent), matched with lyrics::matchScore (>= 0.6). Used by the lyrics providers and
// by handoff. Blocking: background threads only.
namespace catalog {

template <class T>
struct Found {
    bool answered = false;  // false: network / server failure (try again later)
    std::optional<T> hit;   // answered without a match: nullopt
};

struct NeteaseSong {
    int64_t id = 0;
};
Found<NeteaseSong> findNetease(const lyrics::Candidate& want);

struct QqSong {
    std::string mid;   // the mobile app's id (songmid)
    int64_t id = 0;    // numeric id (QQ Music for Windows), 0 if unknown
};
// An empty result list counts as not answered: the desktop search is rate limited per IP.
// `mid`: prefer this song when it is among the hits (a known songmid, e.g. from the phone).
Found<QqSong> findQq(const lyrics::Candidate& want, const std::string& mid = {});

} // namespace catalog
} // namespace md::platform
