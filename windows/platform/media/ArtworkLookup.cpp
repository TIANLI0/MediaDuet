#include "ArtworkLookup.h"

#include "md/lyrics/Lrc.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>

namespace md::platform {

namespace fs = std::filesystem;
namespace aw = media::artwork;
using namespace winrt;
using namespace winrt::Windows::Web::Http;
using nlohmann::json;

namespace {

constexpr wchar_t kBrowserUa[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)";
constexpr auto kTimeout = std::chrono::seconds(10);
constexpr size_t kMaxIndexRecords = 2000;
// Best-ranked results downloaded per provider before moving on.
constexpr int kTriesPerProvider = 2;

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring esc(const std::string& s) { return std::wstring(Windows::Foundation::Uri::EscapeComponent(text::toWide(s))); }

// Blocks on an async operation, cancelling it after kTimeout (get() then throws).
template <typename Op>
auto awaitWithTimeout(Op op) {
    auto timer = Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [op](auto&&) {
            try {
                op.Cancel();
            } catch (...) {
            }
        },
        kTimeout);
    struct Guard {
        Windows::System::Threading::ThreadPoolTimer t;
        ~Guard() {
            try {
                t.Cancel();
            } catch (...) {
            }
        }
    } guard{timer};
    return op.get();
}

HttpClient browserClient(const wchar_t* referer) {
    // No shared WinINet cookies/cache: requests must be anonymous and fresh.
    Filters::HttpBaseProtocolFilter filter;
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    HttpClient c(filter);
    c.DefaultRequestHeaders().UserAgent().TryParseAdd(kBrowserUa);
    if (referer) c.DefaultRequestHeaders().Referer(Windows::Foundation::Uri(referer));
    return c;
}

// Body of a successful GET; "" for 404; nullopt for other failures.
std::optional<std::string> httpGetText(HttpClient& client, const std::wstring& url) {
    auto resp = awaitWithTimeout(client.GetAsync(Windows::Foundation::Uri(url)));
    if (resp.StatusCode() == HttpStatusCode::NotFound) return std::string();
    if (!resp.IsSuccessStatusCode()) return std::nullopt;
    return text::toUtf8(awaitWithTimeout(resp.Content().ReadAsStringAsync()));
}

std::optional<std::string> httpPostJson(HttpClient& client, const std::wstring& url, const std::string& body) {
    HttpStringContent content(text::toWide(body), Windows::Storage::Streams::UnicodeEncoding::Utf8, L"application/json");
    auto resp = awaitWithTimeout(client.PostAsync(Windows::Foundation::Uri(url), content));
    if (!resp.IsSuccessStatusCode()) return std::nullopt;
    return text::toUtf8(awaitWithTimeout(resp.Content().ReadAsStringAsync()));
}

// Image bytes, or nullopt (HTTP error, not an image type, too large).
std::optional<Bytes> httpGetImage(HttpClient& client, const std::string& url) {
    auto resp = awaitWithTimeout(
        client.GetAsync(Windows::Foundation::Uri(text::toWide(url)), HttpCompletionOption::ResponseHeadersRead));
    if (!resp.IsSuccessStatusCode()) return std::nullopt;
    auto content = resp.Content();
    auto headers = content.Headers();
    if (auto len = headers.ContentLength(); len && len.Value() > aw::kMaxDownloadBytes) return std::nullopt;
    if (auto ct = headers.ContentType()) {
        std::string type = text::toLowerAscii(text::toUtf8(ct.MediaType()));
        if (!type.empty() && type.rfind("image/", 0) != 0) return std::nullopt;
    }
    auto buf = awaitWithTimeout(content.ReadAsBufferAsync());
    if (buf.Length() == 0 || buf.Length() > aw::kMaxDownloadBytes) return std::nullopt;
    return Bytes(buf.data(), buf.data() + buf.Length());
}

std::string str(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::optional<int64_t> num(const json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return std::nullopt;
    return static_cast<int64_t>(it->get<double>());
}

std::string artistNames(const json& o, const char* key) {
    std::string out;
    auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return out;
    for (auto& a : *it) {
        if (!a.is_object()) continue;
        auto n = str(a, "name");
        if (!n.empty()) out += (out.empty() ? "" : "/") + n;
    }
    return out;
}

const json* object(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_object() ? &*it : nullptr;
}

struct Cover {
    lyrics::Candidate meta;
    std::string url;
    double score = 0;
};
using Covers = std::optional<std::vector<Cover>>; // nullopt = provider unreachable

// NetEase Cloud Music: /api/cloudsearch/pc, result.songs[].al.picUrl.
Covers neteaseCovers(HttpClient& client, const std::string& query) {
    auto body = httpGetText(client, L"https://music.163.com/api/cloudsearch/pc?type=1&offset=0&limit=10&s=" + esc(query));
    if (!body) return std::nullopt;
    std::vector<Cover> out;
    auto j = json::parse(*body, nullptr, false);
    const json* result = j.is_object() ? object(j, "result") : nullptr;
    if (!result) return out;
    auto songs = result->find("songs");
    if (songs == result->end() || !songs->is_array()) return out;
    for (auto& s : *songs) {
        if (!s.is_object()) continue;
        const json* al = object(s, "al");
        if (!al) continue;
        std::string url = aw::neteaseCoverUrl(str(*al, "picUrl"));
        if (url.empty()) continue;
        out.push_back({{str(s, "name"), artistNames(s, "ar"), str(*al, "name"), num(s, "dt")}, url});
    }
    return out;
}

// QQ Music: musicu.fcg desktop search (album.mid), falling back to the legacy
// client_search_cp (albummid) when the former is throttled.
Covers qqCovers(HttpClient& client, const std::string& query) {
    std::vector<Cover> out;
    bool reachable = false;
    auto seconds = [](const json& s) -> std::optional<int64_t> {
        auto v = num(s, "interval");
        return v ? std::optional<int64_t>(*v * 1000) : std::nullopt;
    };
    json req = {{"comm", {{"ct", "19"}, {"cv", "1859"}, {"uin", "0"}}},
                {"req_1",
                 {{"method", "DoSearchForQQMusicDesktop"},
                  {"module", "music.search.SearchCgiService"},
                  {"param", {{"num_per_page", 10}, {"page_num", 1}, {"query", query}, {"search_type", 0}}}}}};
    if (auto body = httpPostJson(client, L"https://u.y.qq.com/cgi-bin/musicu.fcg", req.dump())) {
        reachable = true;
        auto j = json::parse(*body, nullptr, false);
        try {
            for (auto& s : j.at("req_1").at("data").at("body").at("song").at("list")) {
                if (!s.is_object()) continue;
                const json* album = object(s, "album");
                if (!album) continue;
                std::string url = aw::qqAlbumCoverUrl(str(*album, "mid"));
                if (url.empty()) continue;
                out.push_back({{str(s, "name"), artistNames(s, "singer"), str(*album, "name"), seconds(s)}, url});
            }
        } catch (...) {
        }
    }
    if (out.empty()) {
        auto body = httpGetText(client, L"https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&p=1&n=10&cr=1&w=" +
                                            esc(query));
        if (body) {
            reachable = true;
            auto j = json::parse(*body, nullptr, false);
            try {
                for (auto& s : j.at("data").at("song").at("list")) {
                    if (!s.is_object()) continue;
                    std::string url = aw::qqAlbumCoverUrl(str(s, "albummid"));
                    if (url.empty()) continue;
                    out.push_back({{str(s, "songname"), artistNames(s, "singer"), str(s, "albumname"), seconds(s)}, url});
                }
            } catch (...) {
            }
        }
    }
    if (!reachable) return std::nullopt;
    return out;
}

// iTunes Search API (last resort): results[].artworkUrl100 resized to kFetchPx.
Covers itunesCovers(HttpClient& client, const std::string& query) {
    bool ascii = std::all_of(query.begin(), query.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    std::wstring url = L"https://itunes.apple.com/search?entity=song&limit=10&term=" + esc(query);
    if (!ascii) url += L"&country=cn"; // CJK titles are mostly in the Chinese storefront
    auto body = httpGetText(client, url);
    if (!body) return std::nullopt;
    std::vector<Cover> out;
    auto j = json::parse(*body, nullptr, false);
    if (!j.is_object()) return out;
    auto results = j.find("results");
    if (results == j.end() || !results->is_array()) return out;
    for (auto& r : *results) {
        if (!r.is_object()) continue;
        std::string img = aw::itunesCoverUrl(str(r, "artworkUrl100"));
        if (img.empty()) continue;
        out.push_back({{str(r, "trackName"), str(r, "artistName"), str(r, "collectionName"), num(r, "trackTimeMillis")}, img});
    }
    return out;
}

} // namespace

ArtworkLookup::ArtworkLookup(ArtworkCache& art, fs::path indexFile) : art_(art), indexFile_(std::move(indexFile)) {}

void ArtworkLookup::load() {
    std::map<std::string, aw::IndexRecord> recs;
    size_t lines = 0;
    {
        std::ifstream in(indexFile_, std::ios::binary);
        std::string line;
        while (in && std::getline(in, line)) {
            if (line.empty()) continue;
            ++lines;
            if (auto r = aw::parseIndexRecord(line)) recs[r->trackId] = std::move(*r);
        }
    }
    bool dirty = lines != recs.size();
    int64_t now = unixNow();
    for (auto it = recs.begin(); it != recs.end();) {
        bool keep = it->second.hash ? art_.has(*it->second.hash) : aw::negativeFresh(it->second.atSec, now);
        if (keep) {
            ++it;
        } else {
            it = recs.erase(it);
            dirty = true;
        }
    }
    if (recs.size() > kMaxIndexRecords) {
        std::vector<std::pair<int64_t, std::string>> byAge;
        for (auto& [k, r] : recs) byAge.push_back({r.atSec, k});
        std::sort(byAge.begin(), byAge.end());
        for (size_t i = 0; i + kMaxIndexRecords < byAge.size(); ++i) recs.erase(byAge[i].second);
        dirty = true;
    }
    if (dirty) {
        std::lock_guard lock(fileM_);
        std::error_code ec;
        fs::create_directories(indexFile_.parent_path(), ec);
        auto tmp = indexFile_;
        tmp += L".tmp";
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            for (auto& [k, r] : recs) out << aw::formatIndexRecord(r) << '\n';
            ok = static_cast<bool>(out);
        }
        if (!ok || !MoveFileExW(tmp.c_str(), indexFile_.c_str(), MOVEFILE_REPLACE_EXISTING)) fs::remove(tmp, ec);
    }
    size_t hits = 0;
    for (auto& [k, r] : recs) hits += r.hash ? 1 : 0;
    std::lock_guard lock(m_);
    for (auto& [k, r] : index_) recs[k] = r; // records added while loading win
    index_ = std::move(recs);
    log::debug("art", "cover index: {} covers, {} misses", hits, index_.size() - hits);
}

std::optional<ArtworkLookup::Cached> ArtworkLookup::cached(const std::string& trackId) const {
    std::lock_guard lock(m_);
    auto it = index_.find(trackId);
    if (it == index_.end()) return std::nullopt;
    const auto& r = it->second;
    if (!r.hash) {
        if (!aw::negativeFresh(r.atSec, unixNow())) return std::nullopt;
        return Cached{};
    }
    return Cached{media::ArtworkRef{*r.hash, "image/jpeg", r.size}};
}

void ArtworkLookup::remember(const std::string& trackId, const std::optional<media::ArtworkRef>& ref,
                             const std::string& provider) {
    if (!aw::isValidCacheKey(trackId)) return;
    aw::IndexRecord r{trackId, std::nullopt, 0, unixNow(), provider};
    if (ref) {
        if (ref->hash.size() != 64 || ref->size == 0 || ref->size > aw::kMaxBytes) return;
        r.hash = ref->hash;
        r.size = ref->size;
    }
    {
        std::lock_guard lock(m_);
        index_[trackId] = r;
    }
    std::lock_guard lock(fileM_);
    std::error_code ec;
    fs::create_directories(indexFile_.parent_path(), ec);
    std::ofstream out(indexFile_, std::ios::binary | std::ios::app);
    out << aw::formatIndexRecord(r) << '\n';
}

ArtworkLookup::Result ArtworkLookup::search(const Query& q) {
    if (text::trim(q.title).empty() || text::trim(q.artist).empty()) return {Status::NotFound, std::nullopt};
    lyrics::Candidate want{q.title, q.artist, q.album, q.durationMs};
    std::string query = aw::searchQuery(q.title, q.artist);
    bool failed = false;
    for (const auto& provider : aw::providerOrder(q.appId)) {
        try {
            HttpClient client = provider == "netease" ? browserClient(L"https://music.163.com/")
                                : provider == "qqmusic" ? browserClient(L"https://y.qq.com/")
                                                        : browserClient(nullptr);
            Covers covers = provider == "netease" ? neteaseCovers(client, query)
                            : provider == "qqmusic" ? qqCovers(client, query)
                                                    : itunesCovers(client, query);
            if (!covers) {
                failed = true;
                continue;
            }
            for (auto& c : *covers) c.score = aw::coverMatchScore(want, c.meta);
            std::erase_if(*covers, [](const Cover& c) { return c.score <= 0; });
            std::stable_sort(covers->begin(), covers->end(), [](const Cover& a, const Cover& b) { return a.score > b.score; });
            int tries = 0;
            std::string lastUrl;
            for (auto& c : *covers) {
                if (c.url == lastUrl) continue; // same album as the previous result
                if (tries++ >= kTriesPerProvider) break;
                lastUrl = c.url;
                std::optional<Bytes> bytes;
                try {
                    bytes = httpGetImage(client, c.url);
                } catch (...) {
                    failed = true;
                    continue;
                }
                if (!bytes || !aw::sniffImageMime(bytes->data(), bytes->size())) continue;
                auto size = ArtworkCache::probe(*bytes);
                if (!size || !aw::isBetter(q.current, *size)) continue;
                auto ing = art_.ingestImage(*bytes, aw::kOnlineMaxSide);
                if (!ing) continue;
                log::info("art", "{} cover {}x{} for track {} (score {:.2f})", provider, size->width, size->height, q.trackId,
                          c.score);
                return {Status::Found, Hit{ing->ref, *size, provider}};
            }
        } catch (hresult_error const& e) {
            log::info("art", "{} cover lookup failed: {}", provider, text::toUtf8(e.message()));
            failed = true;
        } catch (...) {
            failed = true;
        }
    }
    log::debug("art", "no cover for track {}{}", q.trackId, failed ? " (some providers unreachable)" : "");
    return {failed ? Status::Failed : Status::NotFound, std::nullopt};
}

} // namespace md::platform
