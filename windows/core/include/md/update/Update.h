#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Software updates from the project's GitHub releases (Settings > About): the pure parts —
// reading the "latest release" answer, comparing versions, the release's file names and
// checksums. Mirrors android/app/src/main/kotlin/app/mediaduet/update/UpdateInfo.kt.
namespace md::update {

using json = nlohmann::json;

inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/TIANLI0/MediaDuet/releases/latest";
inline constexpr const char* kReleasesPage = "https://github.com/TIANLI0/MediaDuet/releases/latest";
inline constexpr const char* kChecksumsAsset = "SHA256SUMS.txt";

struct Asset {
    std::string name;
    std::string url; // browser_download_url (https)
    int64_t size = 0;
};

struct Release {
    std::string tag;     // "v1.5.0"
    std::string version; // "1.5.0"
    std::string page;    // html_url
    std::string notes;   // body (Markdown), may be empty
    std::vector<Asset> assets;
    const Asset* find(std::string_view name) const;
};

// The GitHub API's release object; nullopt for drafts, prereleases, unreadable versions and
// download links that are not https.
std::optional<Release> parseRelease(const json& j);

// True when `latest` is a higher major.minor.patch than `current` (suffixes are ignored).
bool isNewer(std::string_view latest, std::string_view current);

// File names the release workflow publishes (.github/workflows/release.yml).
std::string setupAsset(std::string_view version);    // MediaDuet-<v>-x64-setup.exe
std::string portableAsset(std::string_view version); // MediaDuet-<v>-win-x64.zip
std::string androidAsset(std::string_view version);  // MediaDuet-<v>-android.apk

// The lowercase SHA-256 of `file` in a `sha256sum` listing ("<hex>  <name>" or "<hex> *<name>").
std::optional<std::string> checksumFor(std::string_view sums, std::string_view file);

} // namespace md::update
