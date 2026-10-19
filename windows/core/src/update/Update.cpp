#include "md/update/Update.h"

#include "md/util/AppVersion.h"
#include "md/util/Text.h"

#include <cctype>
#include <tuple>

namespace md::update {

const Asset* Release::find(std::string_view name) const {
    for (auto& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

std::optional<Release> parseRelease(const json& j) {
    if (!j.is_object() || j.value("draft", false) || j.value("prerelease", false)) return std::nullopt;
    Release r;
    r.tag = text::truncate(j.value("tag_name", std::string()), 64);
    auto v = version::parse(r.tag);
    if (!v) return std::nullopt;
    r.version = std::to_string(v->major) + "." + std::to_string(v->minor) + "." + std::to_string(v->patch);
    std::string page = j.value("html_url", std::string());
    r.page = page.rfind("https://", 0) == 0 ? page : kReleasesPage;
    if (auto b = j.find("body"); b != j.end() && b->is_string()) r.notes = text::truncate(b->get<std::string>(), 4000);
    if (auto a = j.find("assets"); a != j.end() && a->is_array()) {
        for (auto& x : *a) {
            if (!x.is_object()) continue;
            Asset as;
            as.name = x.value("name", std::string());
            as.url = x.value("browser_download_url", std::string());
            if (auto s = x.find("size"); s != x.end() && s->is_number_integer()) as.size = s->get<int64_t>();
            if (as.name.empty() || as.url.rfind("https://", 0) != 0) continue;
            r.assets.push_back(std::move(as));
        }
    }
    return r;
}

bool isNewer(std::string_view latest, std::string_view current) {
    auto a = version::parse(latest), b = version::parse(current);
    if (!a || !b) return false;
    return std::tie(a->major, a->minor, a->patch) > std::tie(b->major, b->minor, b->patch);
}

std::string setupAsset(std::string_view v) { return "MediaDuet-" + std::string(v) + "-x64-setup.exe"; }
std::string portableAsset(std::string_view v) { return "MediaDuet-" + std::string(v) + "-win-x64.zip"; }
std::string androidAsset(std::string_view v) { return "MediaDuet-" + std::string(v) + "-android.apk"; }

std::optional<std::string> checksumFor(std::string_view sums, std::string_view file) {
    size_t pos = 0;
    while (pos < sums.size()) {
        size_t end = sums.find('\n', pos);
        std::string line = text::trim(sums.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        pos = end == std::string_view::npos ? sums.size() : end + 1;
        if (line.size() < 66) continue;
        std::string hex = text::toLowerAscii(line.substr(0, 64));
        bool ok = true;
        for (char c : hex) ok &= std::isxdigit(static_cast<unsigned char>(c)) != 0;
        if (!ok || (line[64] != ' ' && line[64] != '\t')) continue;
        std::string name = text::trim(line.substr(65));
        if (!name.empty() && name.front() == '*') name.erase(0, 1);
        if (name == file) return hex;
    }
    return std::nullopt;
}

} // namespace md::update
