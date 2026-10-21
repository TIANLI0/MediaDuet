#include "md/media/NeteasePlayer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace md::media::netease {

const Signature kPlayerSignature{
    "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 90 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 05 ? ? ? ? "
    "48 8D A5 ? ? ? ? 5F 5D C3 CC CC CC CC CC 48 89 4C 24 ? 55 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 48 8D 7C 24",
    3};
const Signature kScheduleSignature{"66 0F 2E 0D ? ? ? ? 7A ? 75 ? 66 0F 2E 15", 4};

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

BytePattern::BytePattern(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        if (text[i] == '?') {
            bytes_.push_back(-1);
            while (i < text.size() && text[i] == '?') ++i;
            continue;
        }
        int hi = hexDigit(text[i]), lo = i + 1 < text.size() ? hexDigit(text[i + 1]) : -1;
        if (hi < 0 || lo < 0) {
            bytes_.clear(); // malformed: matches nothing
            return;
        }
        bytes_.push_back(hi * 16 + lo);
        i += 2;
    }
}

std::optional<size_t> BytePattern::find(const uint8_t* data, size_t size) const {
    if (bytes_.empty() || bytes_[0] < 0 || size < bytes_.size()) return std::nullopt;
    const uint8_t first = static_cast<uint8_t>(bytes_[0]);
    const size_t last = size - bytes_.size();
    for (size_t i = 0; i <= last;) {
        auto* hit = static_cast<const uint8_t*>(std::memchr(data + i, first, last - i + 1));
        if (!hit) break;
        i = static_cast<size_t>(hit - data);
        size_t j = 1;
        while (j < bytes_.size() && (bytes_[j] < 0 || data[i + j] == bytes_[j])) ++j;
        if (j == bytes_.size()) return i;
        ++i;
    }
    return std::nullopt;
}

int64_t parseSongId(std::string_view raw) {
    int64_t id = 0;
    size_t i = 0;
    for (; i < raw.size() && raw[i] >= '0' && raw[i] <= '9'; ++i) {
        if (id > (INT64_MAX - 9) / 10) return 0;
        id = id * 10 + (raw[i] - '0');
    }
    if (i == 0 || (i < raw.size() && raw[i] != '_')) return 0;
    return id;
}

bool plausible(const PlayerReading& r) {
    constexpr double kMaxSec = 24 * 3600.0;
    if (r.status < 0 || r.status > 4) return false;
    if (!std::isfinite(r.positionSec) || r.positionSec < 0 || r.positionSec > kMaxSec) return false;
    if (!std::isfinite(r.durationSec) || r.durationSec < 0 || r.durationSec > kMaxSec) return false;
    return r.songId >= 0;
}

std::optional<PlayingList> parsePlayingList(std::string_view text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("list") || !j["list"].is_array()) return std::nullopt;
    struct Ordered {
        double order;
        PlayingList::Entry entry;
    };
    std::vector<Ordered> all;
    for (auto& e : j["list"]) {
        if (!e.is_object() || !e.contains("track") || !e["track"].is_object()) continue;
        const auto& t = e["track"];
        PlayingList::Entry x;
        const auto id = t.contains("id") ? t["id"] : e.value("id", nlohmann::json());
        if (id.is_string()) x.id = parseSongId(id.get<std::string>());
        else if (id.is_number_integer()) x.id = std::max<int64_t>(id.get<int64_t>(), 0);
        if (t.contains("name") && t["name"].is_string()) x.title = t["name"].get<std::string>();
        if (t.contains("artists") && t["artists"].is_array())
            for (auto& a : t["artists"])
                if (a.is_object() && a.contains("name") && a["name"].is_string())
                    x.subtitle += (x.subtitle.empty() ? "" : " / ") + a["name"].get<std::string>();
        if (e.contains("text") && e["text"].is_string()) x.source = e["text"].get<std::string>();
        double order = e.contains("displayOrder") && e["displayOrder"].is_number() ? e["displayOrder"].get<double>()
                                                                                    : static_cast<double>(all.size());
        all.push_back({order, std::move(x)});
    }
    std::stable_sort(all.begin(), all.end(), [](const Ordered& a, const Ordered& b) { return a.order < b.order; });
    PlayingList list;
    uint32_t h = 2166136261u; // FNV-1a over what receivers show
    auto mix = [&h](std::string_view s) {
        for (unsigned char c : s) h = (h ^ c) * 16777619u;
        h = (h ^ 0x1Fu) * 16777619u;
    };
    for (auto& o : all) {
        mix(std::to_string(o.entry.id));
        mix(o.entry.title);
        mix(o.entry.subtitle);
        mix(o.entry.source);
        list.entries.push_back(std::move(o.entry));
    }
    char rev[9];
    std::snprintf(rev, sizeof rev, "%08x", h);
    list.rev = rev;
    return list;
}

MediaQueue queueWindow(const PlayingList& list, const std::string& key, std::optional<int64_t> activeId, size_t max) {
    MediaQueue q;
    q.key = key;
    q.rev = list.rev;
    q.total = static_cast<int>(list.entries.size());
    size_t active = list.entries.size();
    if (activeId && *activeId > 0)
        for (size_t i = 0; i < list.entries.size(); ++i)
            if (list.entries[i].id == *activeId) {
                active = i;
                q.activeId = activeId;
                break;
            }
    size_t start = 0, n = list.entries.size();
    if (max > 0 && n > max) {
        // Mostly what comes next, with a few played entries for context.
        size_t before = std::min<size_t>(max / 10, 30);
        start = active < n && active > before ? std::min(active - before, n - max) : 0;
        n = max;
    }
    q.offset = static_cast<int>(start);
    for (size_t i = start; i < start + n; ++i) {
        const auto& e = list.entries[i];
        q.items.push_back({e.id, e.title, e.subtitle});
    }
    if (active < list.entries.size()) q.title = list.entries[active].source;
    else if (!list.entries.empty()) q.title = list.entries.front().source;
    return q;
}

} // namespace md::media::netease
