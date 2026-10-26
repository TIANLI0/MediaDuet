#include "Catalog.h"

#include "md/handoff/Handoff.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <nlohmann/json.hpp>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Web::Http;

namespace web {

namespace {
constexpr wchar_t kBrowserUa[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)";

template <class Op>
auto withTimeout(Op op) {
    auto timer = Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [op](auto&&) {
            try {
                op.Cancel();
            } catch (...) {
            }
        },
        std::chrono::seconds(10));
    auto resp = op.get();
    timer.Cancel();
    return resp;
}
} // namespace

std::wstring esc(const std::string& s) { return std::wstring(Windows::Foundation::Uri::EscapeComponent(text::toWide(s))); }

HttpClient browserClient(const wchar_t* referer) {
    // No shared WinINet cookies/cache: requests must be anonymous and fresh.
    Filters::HttpBaseProtocolFilter filter;
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    HttpClient c(filter);
    c.DefaultRequestHeaders().UserAgent().TryParseAdd(kBrowserUa);
    c.DefaultRequestHeaders().Referer(Windows::Foundation::Uri(referer));
    return c;
}

std::optional<std::string> get(HttpClient& client, const std::wstring& url) {
    auto resp = withTimeout(client.GetAsync(Windows::Foundation::Uri(url)));
    if (resp.StatusCode() == HttpStatusCode::NotFound) return std::string();
    if (!resp.IsSuccessStatusCode()) {
        log::debug("catalog", "GET {} -> HTTP {}", text::toUtf8(url), static_cast<int>(resp.StatusCode()));
        return std::nullopt;
    }
    return text::toUtf8(resp.Content().ReadAsStringAsync().get());
}

std::optional<std::string> postJson(HttpClient& client, const std::wstring& url, const std::string& body) {
    HttpStringContent content(text::toWide(body), Windows::Storage::Streams::UnicodeEncoding::Utf8, L"application/json");
    auto resp = withTimeout(client.PostAsync(Windows::Foundation::Uri(url), content));
    if (!resp.IsSuccessStatusCode()) {
        log::debug("catalog", "POST {} -> HTTP {}", text::toUtf8(url), static_cast<int>(resp.StatusCode()));
        return std::nullopt;
    }
    return text::toUtf8(resp.Content().ReadAsStringAsync().get());
}

} // namespace web

namespace catalog {

namespace {
std::string query(const lyrics::Candidate& c) { return handoff::searchQuery({c.title, c.artist, c.album, c.durationMs}); }

std::string artistsOf(const nlohmann::json& arr) {
    std::string out;
    if (arr.is_array())
        for (auto& a : arr) out += (out.empty() ? "" : "/") + a.value("name", std::string());
    return out;
}
} // namespace

// NetEase Cloud Music public web API: /api/cloudsearch/pc.
Found<NeteaseSong> findNetease(const lyrics::Candidate& want) {
    Found<NeteaseSong> f;
    auto client = web::browserClient(L"https://music.163.com/");
    auto body = web::get(client, L"https://music.163.com/api/cloudsearch/pc?type=1&offset=0&limit=10&s=" + web::esc(query(want)));
    if (!body) return f;
    f.answered = true;
    auto j = nlohmann::json::parse(*body, nullptr, false);
    if (!j.is_object() || !j.contains("result") || !j["result"].contains("songs")) return f;
    int64_t bestId = 0;
    double best = 0;
    for (auto& s : j["result"]["songs"]) {
        if (!s.is_object()) continue;
        lyrics::Candidate have{s.value("name", std::string()), artistsOf(s.value("ar", nlohmann::json())),
                               s.contains("al") && s["al"].is_object() ? s["al"].value("name", std::string()) : std::string(),
                               s.contains("dt") && s["dt"].is_number() ? std::optional<int64_t>(s["dt"].get<int64_t>()) : std::nullopt};
        double score = lyrics::matchScore(want, have);
        if (score > best) {
            best = score;
            bestId = s.value("id", int64_t(0));
        }
    }
    if (best >= 0.6 && bestId) f.hit = NeteaseSong{bestId};
    else log::debug("catalog", "NetEase: no match for \"{}\" among {} results (best {:.2f})", query(want), j["result"]["songs"].size(), best);
    return f;
}

// QQ Music public web API: musicu.fcg search, falling back to the legacy client_search_cp
// search when throttled.
Found<QqSong> findQq(const lyrics::Candidate& want, const std::string& wantMid) {
    Found<QqSong> f;
    auto client = web::browserClient(L"https://y.qq.com/");
    struct Hit {
        QqSong song;
        lyrics::Candidate have;
    };
    std::vector<Hit> hits;
    auto interval = [](const nlohmann::json& s) {
        return s.contains("interval") && s["interval"].is_number() ? std::optional<int64_t>(s["interval"].get<int64_t>() * 1000)
                                                                    : std::nullopt;
    };
    nlohmann::json req = {{"comm", {{"ct", "19"}, {"cv", "1859"}, {"uin", "0"}}},
                          {"req_1",
                           {{"method", "DoSearchForQQMusicDesktop"},
                            {"module", "music.search.SearchCgiService"},
                            {"param", {{"num_per_page", 10}, {"page_num", 1}, {"query", query(want)}, {"search_type", 0}}}}}};
    bool transportOk = false;
    if (auto body = web::postJson(client, L"https://u.y.qq.com/cgi-bin/musicu.fcg", req.dump())) {
        transportOk = true;
        auto j = nlohmann::json::parse(*body, nullptr, false);
        try {
            for (auto& s : j.at("req_1").at("data").at("body").at("song").at("list")) {
                Hit h{{s.value("mid", std::string()), s.value("id", int64_t(0))},
                      {s.value("name", std::string()), artistsOf(s.value("singer", nlohmann::json())),
                       s.contains("album") && s["album"].is_object() ? s["album"].value("name", std::string()) : std::string(),
                       interval(s)}};
                if (!h.song.mid.empty()) hits.push_back(std::move(h));
            }
        } catch (...) {
        }
    }
    if (hits.empty()) {
        auto body = web::get(client, L"https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&p=1&n=10&cr=1&w=" +
                                         web::esc(query(want)));
        if (body) {
            transportOk = true;
            auto j = nlohmann::json::parse(*body, nullptr, false);
            try {
                for (auto& s : j.at("data").at("song").at("list")) {
                    Hit h{{s.value("songmid", std::string()), s.value("songid", int64_t(0))},
                          {s.value("songname", std::string()), artistsOf(s.value("singer", nlohmann::json())),
                           s.value("albumname", std::string()), interval(s)}};
                    if (!h.song.mid.empty()) hits.push_back(std::move(h));
                }
            } catch (...) {
            }
        }
    }
    if (!transportOk) return f;
    f.answered = !hits.empty();
    if (!wantMid.empty())
        for (auto& h : hits)
            if (h.song.mid == wantMid) {
                f.hit = h.song;
                return f;
            }
    const Hit* best = nullptr;
    double bestScore = 0;
    for (auto& h : hits) {
        double score = lyrics::matchScore(want, h.have);
        if (score > bestScore) {
            bestScore = score;
            best = &h;
        }
    }
    if (best && bestScore >= 0.6) f.hit = best->song;
    return f;
}

} // namespace catalog
} // namespace md::platform
