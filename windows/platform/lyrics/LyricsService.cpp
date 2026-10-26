#include "LyricsService.h"

#include "Catalog.h"

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

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace md::platform {

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Web::Http;

namespace {

constexpr auto kNegativeTtl = std::chrono::hours(72);
// Like the phone (LyricsRepository): at most this many cache files (.lrc / .yrc / .tlrc / .none),
// least recently used first out; a cache hit refreshes the file time.
constexpr size_t kMaxCacheFiles = 600;

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

// Blocking (directory scan): call off the scheduler thread.
void trimCache(const fs::path& dir) {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) files.push_back({e.last_write_time(ec), e.path()});
    if (files.size() <= kMaxCacheFiles) return;
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = kMaxCacheFiles; i < files.size(); ++i) fs::remove(files[i].second, ec);
    log::info("lyrics", "cache trimmed to {} files ({} removed)", kMaxCacheFiles, files.size() - kMaxCacheFiles);
}

fire_and_forget trimCacheAsync(fs::path dir) {
    co_await resume_background();
    trimCache(dir);
}

// A cached copy remembers its provider in a first line "[md-src:netease]": an LRC tag, which the
// LRC, yrc and plain-text parsers all skip. Removed again when the file is read.
constexpr std::string_view kSourceTag = "[md-src:";

bool namesProvider(const std::string& source) {
    if (source.empty() || source.size() > 32 || source == "cache" || source == "remote") return false;
    return std::all_of(source.begin(), source.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

std::string tagSource(const std::string& source, const std::string& text) {
    return namesProvider(source) ? std::string(kSourceTag) + source + "]\n" + text : text;
}

// Strips the tag from `text`; returns the provider it names ("" without one).
std::string untagSource(std::string& text) {
    if (text.rfind(kSourceTag, 0) != 0) return {};
    size_t close = text.find(']'), eol = text.find('\n');
    if (close == std::string::npos || eol == std::string::npos || close > eol) return {};
    std::string source = text.substr(kSourceTag.size(), close - kSourceTag.size());
    text.erase(0, eol + 1);
    return namesProvider(source) ? source : std::string();
}

void touch(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
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
    std::string translationTiming; // the LRC `translation` is timed by when `lrc` is not (NetEase yrc)
};

using web::browserClient;
using web::esc;
std::optional<std::string> httpGet(HttpClient& client, const std::wstring& url) { return web::get(client, url); }
std::optional<std::string> httpPostJson(HttpClient& client, const std::wstring& url, const std::string& body) {
    return web::postJson(client, url, body);
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
        if (!y.empty() && !lyrics::parseYrc(y).empty()) {
            // tlyric follows the plain LRC's timestamps, which often differ from the yrc's.
            std::string plain;
            if (lj.contains("lrc") && lj["lrc"].is_object()) plain = lj["lrc"].value("lyric", std::string());
            return OnlineResult{y, true, true, "netease", trans, plain};
        }
    }
    if (lj.contains("lrc") && lj["lrc"].is_object()) {
        std::string l = lj["lrc"].value("lyric", std::string());
        auto parsed = lyrics::parseLrc(l);
        if (!parsed.empty() && !lyrics::isInstrumentalNotice(parsed))
            return OnlineResult{l, parsed.synced, false, "netease", trans};
    }
    return OnlineResult{};
}

// NetEase Cloud Music public web API: /api/cloudsearch/pc (catalog) + /api/song/lyric/v1.
std::optional<OnlineResult> queryNetease(const media::MediaSessionInfo& t, int64_t knownId = 0) {
    auto client = browserClient(L"https://music.163.com/");
    if (knownId > 0) return neteaseLyricById(client, knownId);
    auto found = catalog::findNetease({t.title, t.artist, t.album, t.durationMs});
    if (!found.answered) return std::nullopt;
    if (!found.hit) return OnlineResult{};
    return neteaseLyricById(client, found.hit->id);
}

// QQ Music public web API: musicu.fcg search (falls back to the legacy
// client_search_cp search when throttled) + PlayLyricInfo (lyrics and translation,
// base64 UTF-8), falling back to fcg_query_lyric_new (no translation).
std::optional<OnlineResult> queryQq(const media::MediaSessionInfo& t) {
    auto client = browserClient(L"https://y.qq.com/");
    auto found = catalog::findQq({t.title, t.artist, t.album, t.durationMs});
    if (!found.hit) return found.answered ? std::optional<OnlineResult>(OnlineResult{}) : std::nullopt;
    const std::string bestMid = found.hit->mid;
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
    trimCacheAsync(cacheDir_); // caches from before 1.5.1 were never trimmed
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

bool LyricsService::rekey(const std::string& trackId) {
    if (cur_.trackId.empty() || trackId.empty()) return false;
    if (cur_.status != "found" && cur_.status != "loading") return false;
    if (trackId == cur_.trackId) return true;
    cur_.trackId = trackId;
    cacheCopy(cur_);
    return true;
}

void LyricsService::cacheCopy(const Result& r) {
    // Found lyrics, cached under the (new) trackId too unless that entry exists already.
    if (r.status != "found" || r.lrc.empty() || r.trackId.size() > 64) return;
    for (char c : r.trackId)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return; // ids are file names
    auto stem = cacheDir_ / text::toWide(r.trackId);
    std::error_code ec;
    auto lrc = fs::path(stem).replace_extension(L".lrc");
    if (!fs::exists(lrc, ec) && !fs::exists(fs::path(stem).replace_extension(L".yrc"), ec)) writeText(lrc, tagSource(r.source, r.lrc));
    auto tr = fs::path(stem).replace_extension(L".tlrc");
    if (!r.translation.empty() && !fs::exists(tr, ec)) writeText(tr, r.translation);
    trimCacheAsync(cacheDir_);
}

void LyricsService::offerRemote(const std::string& trackId, const std::string& lrc, const std::string& source,
                                const std::string& translation) {
    if (trackId.size() > 64 || lrc.size() > 256 * 1024 || translation.size() > 256 * 1024) return;
    auto parsed = lyrics::parseLrc(lrc);
    if (parsed.empty()) return;
    if (!translation.empty()) lyrics::mergeTranslation(parsed, lyrics::parseLrc(translation));
    writeText(cacheDir_ / (text::toWide(trackId) + L".lrc"), tagSource(source, lrc));
    if (!translation.empty()) writeText(cacheDir_ / (text::toWide(trackId) + L".tlrc"), translation);
    std::error_code ec;
    fs::remove(cacheDir_ / (text::toWide(trackId) + L".none"), ec); // our own "not found" is outdated
    trimCacheAsync(cacheDir_);
    if (trackId != cur_.trackId || cur_.status == "found") return;
    log::info("lyrics", "lyrics for {} from the phone ({}, {} lines)", trackId, source.empty() ? "remote" : source,
              parsed.lines.size());
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
        // Translations of word-timed lyrics cached before they were aligned through the plain
        // LRC (no kTranslationMark) may miss lines: look the song up again, once. The old
        // cache stays in use when that lookup finds nothing.
        bool staleTr = false;
        if (online && !providers.empty() && !negative && fs::exists(hitYrc, ec))
            if (auto tr = readText(hitTr); tr && tr->rfind(lyrics::kTranslationMark, 0) != 0) staleTr = true;
        if (staleTr) log::info("lyrics", "refreshing the cached translation of {}", t.trackId);
        bool wroteYrc = false, wroteTr = false;
        // Word-timed yrc only ever comes from NetEase; an LRC cached before 1.9.0 names no provider.
        auto cachedYrc = [&](std::string y) {
            std::string source = untagSource(y);
            auto parsed = lyrics::parseYrc(y);
            std::string asLrc = lyrics::toLrc(parsed);
            return Result{t.trackId, "found", source.empty() ? "netease" : source, std::move(parsed), std::move(asLrc), {}, true};
        };
        if (auto y = staleTr ? std::nullopt : readText(hitYrc)) {
            r = cachedYrc(std::move(*y));
            touch(hitYrc); // LRU
            touch(hitTr);
        } else if (auto s = staleTr ? std::nullopt : readText(hitLrc)) {
            std::string source = untagSource(*s);
            r = {t.trackId, "found", source.empty() ? "cache" : source, lyrics::parseLrc(*s), *s, {}, true};
            touch(hitLrc);
            touch(hitTr);
        } else if (auto l = searchLocal(local, t)) {
            r = {t.trackId, "found", "local", lyrics::parseLrc(*l), *l};
            writeText(hitLrc, tagSource("local", *l));
        } else if (!online || providers.empty()) {
            r.status = "disabled";
        } else if (!negative) {
            bool definitive = true; // every provider answered (even if "no lyrics")
            for (auto& p : providers) {
                std::optional<OnlineResult> res;
                auto started = std::chrono::steady_clock::now();
                try {
                    if (p == "netease") res = queryNetease(t, neteaseId);
                    else if (p == "qqmusic") res = queryQq(t);
                    else if (p == "lrclib") res = queryLrclib(t);
                    else continue;
                } catch (hresult_error const& e) {
                    log::info("lyrics", "{} lookup failed: {}", p, text::toUtf8(e.message()));
                } catch (...) {
                }
                log::debug("lyrics", "{} for {}: {} after {} ms", p, t.trackId,
                           !res ? "no answer" : res->lrc.empty() ? "none" : "found",
                           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
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
                writeText(res->yrc ? hitYrc : hitLrc, tagSource(res->provider, res->lrc));
                wroteYrc = res->yrc;
                // `lrc` is what the phone receives: always LRC (enhanced if word-timed).
                std::string asLrc = res->yrc ? lyrics::toLrc(parsed) : res->lrc;
                std::string tr;
                auto timing = res->translationTiming.empty() ? std::optional<lyrics::Lyrics>()
                                                              : std::optional<lyrics::Lyrics>(lyrics::parseLrc(res->translationTiming));
                if (!res->translation.empty() &&
                    lyrics::mergeTranslation(parsed, lyrics::parseLrc(res->translation), 600, timing ? &*timing : nullptr) > 0) {
                    tr = lyrics::translationLrc(parsed);
                    writeText(hitTr, tr);
                    wroteTr = true;
                }
                r = {t.trackId, "found", res->provider, std::move(parsed), std::move(asLrc), std::move(tr)};
                break;
            }
            if (r.status == "found" && !r.lyrics.synced) writeText(hitLrc, tagSource(r.source, r.lrc));
            if (r.status != "found" && definitive) writeText(miss, ""); // all providers said no
        }
        if (staleTr && r.status == "found" && r.lyrics.synced) {
            // Replaced: never treat the old files as stale again.
            if (!wroteYrc) fs::remove(hitYrc, ec);
            if (!wroteTr) fs::remove(hitTr, ec);
        } else if (staleTr) {
            // Nothing better online: keep the cached lyrics. When the providers did answer, mark
            // the old translation as checked so the song is not looked up on every play.
            bool answered = r.status == "found" || fs::exists(miss, ec);
            if (auto y = readText(hitYrc)) r = cachedYrc(std::move(*y));
            if (auto old = readText(hitTr); old && answered)
                writeText(hitTr, std::string(lyrics::kTranslationMark) + "\n" + *old);
        }
        if (r.status == "found" && r.cached && !lyrics::hasTranslation(r.lyrics)) {
            if (auto tr = readText(hitTr); tr && lyrics::mergeTranslation(r.lyrics, lyrics::parseLrc(*tr)) > 0)
                r.translation = *tr;
        }
        if (!r.cached) trimCache(cache); // something new may have been written
        if (r.status == "found" && r.lyrics.empty()) r.status = "notfound";
        if (r.status == "notfound")
            log::info("lyrics", "no lyrics found for {} ({} - {}){}", t.trackId, t.title, t.artist,
                      negative ? " (remembered miss)" : "");
        self->sched_.post([self, alive, gen, r = std::move(r)]() mutable {
            if (alive.expired() || gen != self->gen_) return;
            // rekey() relabelled the track while this lookup ran (same song, new trackId).
            if (r.trackId != self->cur_.trackId) {
                r.trackId = self->cur_.trackId;
                self->cacheCopy(r);
            }
            self->deliver(std::move(r));
        });
    }(this, alive, t, gen, online_, local_, cacheDir_, providers_, neteaseId);
}

} // namespace md::platform
