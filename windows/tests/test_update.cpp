#include "md/update/Update.h"

#include <doctest/doctest.h>

using namespace md::update;

namespace {
json sample() {
    return json::parse(R"({
      "tag_name": "v1.5.0", "name": "MediaDuet 1.5.0", "draft": false, "prerelease": false,
      "html_url": "https://github.com/TIANLI0/MediaDuet/releases/tag/v1.5.0",
      "body": "**Full Changelog**: https://github.com/TIANLI0/MediaDuet/compare/v1.4.1...v1.5.0",
      "assets": [
        {"name": "MediaDuet-1.5.0-android.apk", "size": 2860798,
         "browser_download_url": "https://github.com/TIANLI0/MediaDuet/releases/download/v1.5.0/MediaDuet-1.5.0-android.apk"},
        {"name": "MediaDuet-1.5.0-x64-setup.exe", "size": 24621470,
         "browser_download_url": "https://github.com/TIANLI0/MediaDuet/releases/download/v1.5.0/MediaDuet-1.5.0-x64-setup.exe"},
        {"name": "evil.exe", "browser_download_url": "http://example.com/evil.exe"},
        {"name": "SHA256SUMS.txt", "size": 284,
         "browser_download_url": "https://github.com/TIANLI0/MediaDuet/releases/download/v1.5.0/SHA256SUMS.txt"}
      ]})");
}
} // namespace

TEST_CASE("update: the latest release from the GitHub API") {
    auto r = parseRelease(sample());
    REQUIRE(r);
    CHECK(r->version == "1.5.0");
    CHECK(r->tag == "v1.5.0");
    CHECK(r->page == "https://github.com/TIANLI0/MediaDuet/releases/tag/v1.5.0");
    CHECK(r->assets.size() == 3); // the http link is dropped
    auto* setup = r->find(setupAsset(r->version));
    REQUIRE(setup);
    CHECK(setup->size == 24621470);
    CHECK(r->find(portableAsset(r->version)) == nullptr);
    CHECK(r->find(androidAsset("1.5.0")));
    CHECK(r->find(kChecksumsAsset));

    auto draft = sample();
    draft["draft"] = true;
    CHECK_FALSE(parseRelease(draft));
    auto pre = sample();
    pre["prerelease"] = true;
    CHECK_FALSE(parseRelease(pre));
    auto bad = sample();
    bad["tag_name"] = "nightly";
    CHECK_FALSE(parseRelease(bad));
    CHECK_FALSE(parseRelease(json::array()));
}

TEST_CASE("update: version order and file names") {
    CHECK(isNewer("1.5.0", "1.4.1"));
    CHECK(isNewer("v1.4.2", "1.4.1"));
    CHECK(isNewer("2.0.0", "1.99.99"));
    CHECK_FALSE(isNewer("1.4.1", "1.4.1"));
    CHECK_FALSE(isNewer("1.4.0", "1.4.1"));
    CHECK_FALSE(isNewer("garbage", "1.4.1"));
    CHECK_FALSE(isNewer("1.5.0", ""));
    CHECK(setupAsset("1.5.0") == "MediaDuet-1.5.0-x64-setup.exe");
    CHECK(portableAsset("1.5.0") == "MediaDuet-1.5.0-win-x64.zip");
    CHECK(androidAsset("1.5.0") == "MediaDuet-1.5.0-android.apk");
}

TEST_CASE("update: checksums from SHA256SUMS.txt") {
    std::string sums =
        "8e4f1a63b7c1d0f2a9e5b6c7d8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f9a0b  MediaDuet-1.5.0-android.apk\n"
        "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *MediaDuet-1.5.0-x64-setup.exe\r\n"
        "not a line\n"
        "1234  short.txt\n";
    CHECK(checksumFor(sums, "MediaDuet-1.5.0-android.apk") == "8e4f1a63b7c1d0f2a9e5b6c7d8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f9a0b");
    CHECK(checksumFor(sums, "MediaDuet-1.5.0-x64-setup.exe") == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    CHECK_FALSE(checksumFor(sums, "MediaDuet-1.5.0-win-x64.zip"));
    CHECK_FALSE(checksumFor(sums, "short.txt"));
    CHECK_FALSE(checksumFor("", "x"));
}
