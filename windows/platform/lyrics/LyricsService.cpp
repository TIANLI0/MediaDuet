#include "LyricsService.h"

#include "md/util/Bytes.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace md::platform {

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Web::Http;

namespace {

constexpr auto kNegativeTtl = std::chrono::hours(72);

std::optional<std::string> readText(const fs::path& p, size_t maxBytes = 512 * 1024) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) > maxBytes) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (!text::isValidUtf8(s)) {
        // Many legacy .lrc files are in the ANSI code page (GBK on Chinese systems).
        int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        s = text::toUtf8(w);
    }
    return s;
}

void writeText(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

std::wstring safeFileName(const std::string& s) {
    std::wstring w = text::toWide(s);
    for (auto& c : w)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return w;
}

std::optional<std::string> searchLocal(const fs::path& dir, const media::MediaSessionInfo& t) {
    if (dir.empty()) return std::nullopt;
    std::vector<std::wstring> names;
    auto title = safeFileName(t.title), artist = safeFileName(t.artist);
    if (!artist.empty()) {
        names.push_back(artist + L" - " + title + L".lrc");
        names.push_back(title + L" - " + artist + L".lrc");
    }
    names.push_back(title + L".lrc");
    for (auto& n : names)
        if (auto s = readText(dir / n)) return s;
    return std::nullopt;
}

struct OnlineResult {
    std::string lrc;     // raw text as returned
    bool synced = false;
    bool yrc = false;    // NetEase word-timed format
    std::string provider;
    std::string translation; // LRC with the same timestamps
};

constexpr wchar_t kBrowserUa[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)";

std::wstring esc(const std::string& s) { return std::wstring(Windows::Foundation::Uri::EscapeComponent(text::toWide(s))); }

// Search keyword: title without bracketed suffixes plus the first artist.
std::string searchQuery(const media::MediaSessionInfo& t) {
    std::string title = t.title;
    for (const char* open : {" (", "\xEF\xBC\x88", " [", "\xE3\x80\x90"}) { // ( （ [ 【
        auto p = title.find(open);
        if (p != std::string::npos && p > 0) title.resize(p);
    }
    std::string artist = t.artist;
    for (const char* sep : {"/", ",", "&", "\xE3\x80\x81", ";"}) { // 、
        auto p = artist.find(sep);
        if (p != std::string::npos && p > 0) artist.resize(p);
    }
    return text::trim(title + " " + artist);
}

// GET with a timeout; returns body or nullopt (404 -> empty string).
std::optional<std::string> httpGet(HttpClient& client, const std::wstring& url) {
    auto op = client.GetAsync(Windows::Foundation::Uri(url));
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
    if (resp.StatusCode() == HttpStatusCode::NotFound) return std::string();
    if (!resp.IsSuccessStatusCode()) return std::nullopt;
    return text::toUtf8(resp.Content().ReadAsStringAsync().get());
}

std::optional<std::string> httpPostJson(HttpClient& client, const std::wstring& url, const std::string& body) {
    HttpStringContent content(text::toWide(body), Windows::Storage::Streams::UnicodeEncoding::Utf8, L"application/json");
    auto op = client.PostAsync(Windows::Foundation::Uri(url), content);
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
    if (!resp.IsSuccessStatusCode()) {
        log::debug("lyrics", "POST {} -> HTTP {}", text::toUtf8(url), static_cast<int>(resp.StatusCode()));
        return std::nullopt;
    }
    return text::toUtf8(resp.Content().ReadAsStringAsync().get());
}

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

std::optional<OnlineResult> neteaseLyricById(HttpClient& client, int64_t id) {
    auto ly = httpGet(client, L"https://music.163.com/api/song/lyric/v1?lv=1&yv=1&tv=-1&id=" + std::to_wstring(id));
    if (!ly || ly->empty()) return std::nullopt;
    auto lj = nlohmann::json::parse(*ly, nullptr, false);
    if (!lj.is_object()) return std::nullopt;
    if (lj.value("pureMusic", false)) return OnlineResult{};
    std::string trans;
    if (lj.contains("tlyric") && lj["tlyric"].is_object()) trans = lj["tlyric"].value("lyric", std::string());
    if (lj.contains("yrc") && lj["yrc"].is_object()) {
        std::string y = lj["yrc"].value("lyric", std::string());
        if (!y.empty() && !lyrics::parseYrc(y).empty()) return OnlineResult{y, true, true, "netease", trans};
    }
    if (lj.contains("lrc") && lj["lrc"].is_object()) {
        std::string l = lj["lrc"].value("lyric", std::string());
        auto parsed = lyrics::parseLrc(l);
        if (!parsed.empty() && !lyrics::isInstrumentalNotice(parsed))
            return OnlineResult{l, parsed.synced, false, "netease", trans};
    }
    return OnlineResult{};
}

// NetEase Cloud Music public web API: /api/cloudsearch/pc + /api/song/lyric/v1.
std::optional<OnlineResult> queryNetease(const media::MediaSessionInfo& t, int64_t knownId = 0) {
    auto client = browserClient(L"https://music.163.com/");
    if (knownId > 0) return neteaseLyricById(client, knownId);
    auto body = httpGet(client, L"https://music.163.com/api/cloudsearch/pc?type=1&offset=0&limit=10&s=" + esc(searchQuery(t)));
    if (!body) return std::nullopt;
    auto j = nlohmann::json::parse(*body, nullptr, false);
    if (!j.is_object() || !j.contains("result") || !j["result"].contains("songs")) return OnlineResult{};
    lyrics::Candidate want{t.title, t.artist, t.album, t.durationMs};
    int64_t bestId = 0;
    double best = 0;
    for (auto& s : j["result"]["songs"]) {
        if (!s.is_object()) continue;
        std::string artists;
        if (s.contains("ar") && s["ar"].is_array())
            for (auto& a : s["ar"]) artists += (artists.empty() ? "" : "/") + a.value("name", std::string());
        lyrics::Candidate have{s.value("name", std::string()), artists,
                               s.contains("al") && s["al"].is_object() ? s["al"].value("name", std::string()) : std::string(),
                               s.contains("dt") && s["dt"].is_number() ? std::optional<int64_t>(s["dt"].get<int64_t>()) : std::nullopt};
        double score = lyrics::matchScore(want, have);
        if (score > best) {
            best = score;
            bestId = s.value("id", int64_t(0));
        }
    }
    if (best < 0.6 || !bestId) return OnlineResult{};
    return neteaseLyricById(client, bestId);
}

// QQ Music public web API: musicu.fcg search (falls back to the legacy
// client_search_cp search when throttled) + PlayLyricInfo (lyrics and translation,
// base64 UTF-8), falling back to fcg_query_lyric_new (no translation).
std::optional<OnlineResult> queryQq(const media::MediaSessionInfo& t) {
    auto client = browserClient(L"https://y.qq.com/");
    struct Hit {
        std::string mid, name, artists, album;
        std::optional<int64_t> durationMs;
    };
    std::vector<Hit> hits;
    auto artistsOf = [](const nlohmann::json& arr) {
        std::string out;
        if (arr.is_array())
            for (auto& a : arr) out += (out.empty() ? "" : "/") + a.value("name", std::string());
        return out;
    };
    nlohmann::json req = {{"comm", {{"ct", "19"}, {"cv", "1859"}, {"uin", "0"}}},
                          {"req_1",
                           {{"method", "DoSearchForQQMusicDesktop"},
                            {"module", "music.search.SearchCgiService"},
                            {"param", {{"num_per_page", 10}, {"page_num", 1}, {"query", searchQuery(t)}, {"search_type", 0}}}}}};
    bool transportOk = false;
    if (auto body = httpPostJson(client, L"https://u.y.qq.com/cgi-bin/musicu.fcg", req.dump())) {
        transportOk = true;
        auto j = nlohmann::json::parse(*body, nullptr, false);
        try {
            for (auto& s : j.at("req_1").at("data").at("body").at("song").at("list")) {
                Hit h{s.value("mid", std::string()), s.value("name", std::string()), artistsOf(s.value("singer", nlohmann::json())),
                      s.contains("album") && s["album"].is_object() ? s["album"].value("name", std::string()) : std::string(),
                      s.contains("interval") && s["interval"].is_number() ? std::optional<int64_t>(s["interval"].get<int64_t>() * 1000)
                                                                           : std::nullopt};
                if (!h.mid.empty()) hits.push_back(std::move(h));
            }
        } catch (...) {
        }
    }
    if (hits.empty()) {
        // The desktop search is rate limited per IP; the legacy endpoint usually still answers.
        auto body = httpGet(client, L"https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&p=1&n=10&cr=1&w=" +
                                        esc(searchQuery(t)));
        if (body) {
            transportOk = true;
            auto j = nlohmann::json::parse(*body, nullptr, false);
            try {
                for (auto& s : j.at("data").at("song").at("list")) {
                    Hit h{s.value("songmid", std::string()), s.value("songname", std::string()),
                          artistsOf(s.value("singer", nlohmann::json())), s.value("albumname", std::string()),
                          s.contains("interval") && s["interval"].is_number()
                              ? std::optional<int64_t>(s["interval"].get<int64_t>() * 1000)
                              : std::nullopt};
                    if (!h.mid.empty()) hits.push_back(std::move(h));
                }
            } catch (...) {
            }
        }
    }
    if (!transportOk) return std::nullopt;
    lyrics::Candidate want{t.title, t.artist, t.album, t.durationMs};
    std::string bestMid;
    double best = 0;
    for (auto& h : hits) {
        double score = lyrics::matchScore(want, {h.name, h.artists, h.album, h.durationMs});
        if (score > best) {
            best = score;
            bestMid = h.mid;
        }
    }
    if (best < 0.6 || bestMid.empty()) return hits.empty() ? std::nullopt : std::optional<OnlineResult>(OnlineResult{});
    std::string l, trans;
    nlohmann::json lreq = {{"comm", {{"ct", "19"}, {"cv", "1859"}, {"uin", "0"}}},
                           {"req_1",
                            {{"module", "music.musichallSong.PlayLyricInfo"},
                             {"method", "GetPlayLyricInfo"},
                             {"param", {{"songMID", bestMid}, {"songID", 0}, {"trans", 1}, {"roma", 0}, {"crypt", 0}, {"type", -1}}}}}};
    if (auto body = httpPostJson(client, L"https://u.y.qq.com/cgi-bin/musicu.fcg", lreq.dump())) {
        auto j = nlohmann::json::parse(*body, nullptr, false);
        auto field = [&](const char* k) {
            try {
                if (auto b = base64Decode(j.at("req_1").at("data").at(k).get<std::string>()))
                    return lyrics::decodeEntities(std::string(b->begin(), b->end()));
            } catch (...) {
            }
            return std::string();
        };
        l = field("lyric");
        if (!l.empty()) trans = field("trans");
    }
    if (l.empty()) {
        auto ly = httpGet(client, L"https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&g_tk=5381&songmid=" +
                                      esc(bestMid));
        if (!ly || ly->empty()) return std::nullopt;
        auto lj = nlohmann::json::parse(*ly, nullptr, false);
        if (!lj.is_object() || lj.value("retcode", -1) != 0) return std::nullopt;
        l = lyrics::decodeEntities(lj.value("lyric", std::string()));
        trans = lyrics::decodeEntities(lj.value("trans", std::string()));
    }
    auto parsed = lyrics::parseLrc(l);
    if (parsed.empty() || lyrics::isInstrumentalNotice(parsed)) return OnlineResult{};
    return OnlineResult{l, parsed.synced, false, "qqmusic", trans};
}

std::optional<OnlineResult> queryLrclib(const media::MediaSessionInfo& t) {
    HttpClient client;
    client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"MediaDuet/" MD_VERSION_W L" (https://github.com/mediaduet)");
    auto esc = [](const std::string& s) { return std::wstring(Windows::Foundation::Uri::EscapeComponent(text::toWide(s))); };
    lyrics::Candidate want{t.title, t.artist, t.album, t.durationMs};
    auto pick = [&](const nlohmann::json& j) -> std::optional<OnlineResult> {
        if (!j.is_object() || j.value("instrumental", false)) return std::nullopt;
        lyrics::Candidate have{j.value("trackName", std::string()), j.value("artistName", std::string()),
                               j.value("albumName", std::string()), std::nullopt};
        if (j.contains("duration") && j["duration"].is_number())
            have.durationMs = static_cast<int64_t>(j["duration"].get<double>() * 1000);
        if (lyrics::matchScore(want, have) < 0.6) return std::nullopt;
        auto synced = j.value("syncedLyrics", std::string());
        if (!synced.empty()) return OnlineResult{synced, true, false, "lrclib"};
        auto plain = j.value("plainLyrics", std::string());
        if (!plain.empty()) return OnlineResult{plain, false, false, "lrclib"};
        return std::nullopt;
    };

    std::wstring url = L"https://lrclib.net/api/get?track_name=" + esc(t.title) + L"&artist_name=" + esc(t.artist);
    if (!t.album.empty()) url += L"&album_name=" + esc(t.album);
    if (t.durationMs) url += L"&duration=" + std::to_wstring((*t.durationMs + 500) / 1000);
    auto body = httpGet(client, url);
    if (!body) return std::nullopt;
    if (!body->empty()) {
        if (auto r = pick(nlohmann::json::parse(*body, nullptr, false))) return r;
    }
    url = L"https://lrclib.net/api/search?track_name=" + esc(t.title);
    if (!t.artist.empty()) url += L"&artist_name=" + esc(t.artist);
    body = httpGet(client, url);
    if (!body || body->empty()) return OnlineResult{};
    auto arr = nlohmann::json::parse(*body, nullptr, false);
    if (!arr.is_array()) return OnlineResult{};
    std::optional<OnlineResult> best;
    double bestScore = 0;
    for (auto& j : arr) {
        auto r = pick(j);
        if (!r) continue;
        lyrics::Candidate have{j.value("trackName", std::string()), j.value("artistName", std::string()), "",
                               j.contains("duration") && j["duration"].is_number()
                                   ? std::optional<int64_t>(static_cast<int64_t>(j["duration"].get<double>() * 1000))
                                   : std::nullopt};
        double score = lyrics::matchScore(want, have) + (r->synced ? 0.2 : 0.0);
        if (score > bestScore) {
            bestScore = score;
            best = r;
        }
    }
    return best ? *best : OnlineResult{};
}

} // namespace

LyricsService::LyricsService(IScheduler& sched, fs::path cacheDir, Fn onResult)
    : sched_(sched), cacheDir_(std::move(cacheDir)), onResult_(std::move(onResult)) {
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);
}

LyricsService::~LyricsService() { *alive_ = false; }

void LyricsService::configure(bool online, fs::path localFolder, std::vector<std::string> providers) {
    online_ = online;
    local_ = std::move(localFolder);
    providers_ = std::move(providers);
}

void LyricsService::clear() {
    ++gen_;
    cur_ = Result{};
    deliver(cur_);
}

void LyricsService::deliver(Result r) {
    cur_ = std::move(r);
    if (onResult_) onResult_(cur_);
}

void LyricsService::offerRemote(const std::string& trackId, const std::string& lrc, const std::string& source,
                                const std::string& translation) {
    if (trackId.size() > 64 || lrc.size() > 256 * 1024 || translation.size() > 256 * 1024) return;
    auto parsed = lyrics::parseLrc(lrc);
    if (parsed.empty()) return;
    if (!translation.empty()) lyrics::mergeTranslation(parsed, lyrics::parseLrc(translation));
    writeText(cacheDir_ / (text::toWide(trackId) + L".lrc"), lrc);
    if (!translation.empty()) writeText(cacheDir_ / (text::toWide(trackId) + L".tlrc"), translation);
    if (trackId != cur_.trackId || cur_.status == "found") return;
    ++gen_;
    deliver({trackId, "found", source.empty() ? "remote" : source, std::move(parsed), lrc, translation});
}

void LyricsService::request(const media::MediaSessionInfo& t, int64_t neteaseId) {
    if (t.trackId.empty() || t.title.empty()) {
        if (cur_.trackId.empty()) return;
        clear();
        return;
    }
    if (t.trackId == cur_.trackId) return;
    uint64_t gen = ++gen_;
    deliver({t.trackId, "loading", "", {}, ""});
    std::weak_ptr<bool> alive = alive_;
    [](LyricsService* self, std::weak_ptr<bool> alive, media::MediaSessionInfo t, uint64_t gen, bool online,
       fs::path local, fs::path cache, std::vector<std::string> providers, int64_t neteaseId) -> fire_and_forget {
        co_await resume_background();
        Result r{t.trackId, "notfound", "", {}, ""};
        auto stem = cache / text::toWide(t.trackId);
        auto hitLrc = fs::path(stem).replace_extension(L".lrc");
        auto hitYrc = fs::path(stem).replace_extension(L".yrc");
        auto miss = fs::path(stem).replace_extension(L".none");
        auto hitTr = fs::path(stem).replace_extension(L".tlrc");
        std::error_code ec;
        bool negative = fs::exists(miss, ec) &&
                        fs::last_write_time(miss, ec) + kNegativeTtl > fs::file_time_type::clock::now();
        if (auto y = readText(hitYrc)) {
            auto parsed = lyrics::parseYrc(*y);
            std::string asLrc = lyrics::toLrc(parsed);
            r = {t.trackId, "found", "cache", std::move(parsed), std::move(asLrc)};
        } else if (auto s = readText(hitLrc)) {
            r = {t.trackId, "found", "cache", lyrics::parseLrc(*s), *s};
        } else if (auto l = searchLocal(local, t)) {
            r = {t.trackId, "found", "local", lyrics::parseLrc(*l), *l};
            writeText(hitLrc, *l);
        } else if (!online || providers.empty()) {
            r.status = "disabled";
        } else if (!negative) {
            bool definitive = true; // every provider answered (even if "no lyrics")
            for (auto& p : providers) {
                std::optional<OnlineResult> res;
                try {
                    if (p == "netease") res = queryNetease(t, neteaseId);
                    else if (p == "qqmusic") res = queryQq(t);
                    else if (p == "lrclib") res = queryLrclib(t);
                    else continue;
                } catch (hresult_error const& e) {
                    log::info("lyrics", "{} lookup failed: {}", p, text::toUtf8(e.message()));
                } catch (...) {
                }
                if (!res) {
                    definitive = false;
                    continue;
                }
                if (res->lrc.empty()) continue;
                lyrics::Lyrics parsed = res->yrc ? lyrics::parseYrc(res->lrc)
                                        : res->synced ? lyrics::parseLrc(res->lrc)
                                                      : lyrics::fromPlainText(res->lrc);
                if (parsed.empty()) continue;
                // Prefer synced lyrics: keep looking if this one is plain text.
                if (!parsed.synced && r.status != "found") {
                    r = {t.trackId, "found", res->provider, std::move(parsed), res->lrc};
                    continue;
                }
                writeText(res->yrc ? hitYrc : hitLrc, res->lrc);
                // `lrc` is what the phone receives: always LRC (enhanced if word-timed).
                std::string asLrc = res->yrc ? lyrics::toLrc(parsed) : res->lrc;
                std::string tr;
                if (!res->translation.empty() && lyrics::mergeTranslation(parsed, lyrics::parseLrc(res->translation)) > 0) {
                    tr = lyrics::translationLrc(parsed);
                    writeText(hitTr, tr);
                }
                r = {t.trackId, "found", res->provider, std::move(parsed), std::move(asLrc), std::move(tr)};
                break;
            }
            if (r.status == "found" && !r.lyrics.synced) writeText(hitLrc, r.lrc);
            if (r.status != "found" && definitive) writeText(miss, ""); // all providers said no
        }
        if (r.status == "found" && r.source == "cache" && !lyrics::hasTranslation(r.lyrics)) {
            if (auto tr = readText(hitTr); tr && lyrics::mergeTranslation(r.lyrics, lyrics::parseLrc(*tr)) > 0)
                r.translation = *tr;
        }
        if (r.status == "found" && r.lyrics.empty()) r.status = "notfound";
        self->sched_.post([self, alive, gen, r = std::move(r)]() mutable {
            if (alive.expired() || gen != self->gen_) return;
            self->deliver(std::move(r));
        });
    }(this, alive, t, gen, online_, local_, cacheDir_, providers_, neteaseId);
}

} // namespace md::platform
