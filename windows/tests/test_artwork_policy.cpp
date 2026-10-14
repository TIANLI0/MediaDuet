#include "md/media/ArtworkPolicy.h"

#include <doctest/doctest.h>

using namespace md::media;
using namespace md::media::artwork;

TEST_CASE("artwork upgrade threshold") {
    CHECK(shouldUpgrade(std::nullopt));           // no thumbnail at all
    CHECK(shouldUpgrade(ImageSize{0, 0}));
    CHECK(shouldUpgrade(ImageSize{100, 100}));    // QQ Music / NetEase desktop
    CHECK(shouldUpgrade(ImageSize{300, 300}));
    CHECK(shouldUpgrade(ImageSize{240, 250}));    // nearly square
    CHECK_FALSE(shouldUpgrade(ImageSize{512, 512}));
    CHECK_FALSE(shouldUpgrade(ImageSize{640, 640})); // Spotify
    CHECK_FALSE(shouldUpgrade(ImageSize{1000, 1000}));
    CHECK_FALSE(shouldUpgrade(ImageSize{320, 180})); // video frame: never replaced
    CHECK_FALSE(shouldUpgrade(ImageSize{480, 480})); // a 512 px cover would not be clearly better
    // A larger online size makes more thumbnails worth replacing.
    CHECK(shouldUpgrade(ImageSize{480, 480}, 1000));
    CHECK_FALSE(shouldUpgrade(ImageSize{500, 500}, 1000));
}

TEST_CASE("candidate must be a real improvement") {
    CHECK(isBetter(std::nullopt, {800, 800}));
    CHECK(isBetter(ImageSize{100, 100}, {800, 800}));
    CHECK(isBetter(ImageSize{300, 300}, {512, 512}));
    CHECK_FALSE(isBetter(ImageSize{300, 300}, {300, 300}));  // same size
    CHECK_FALSE(isBetter(ImageSize{300, 300}, {340, 340}));  // marginal
    CHECK_FALSE(isBetter(std::nullopt, {200, 200}));         // too small to be worth it
    CHECK_FALSE(isBetter(std::nullopt, {1000, 500}));        // not a cover
    CHECK_FALSE(isBetter(ImageSize{512, 512}, {1000, 1000})); // both normalise to 512
    CHECK(isBetter(ImageSize{512, 512}, {1000, 1000}, 1000));
}

TEST_CASE("cover URL builders") {
    CHECK(neteaseCoverUrl("http://p1.music.126.net/eBF7bHnJYBUfOFrJ_7SUfw==/109951163351825356.jpg") ==
          "https://p1.music.126.net/eBF7bHnJYBUfOFrJ_7SUfw==/109951163351825356.jpg?param=1000y1000");
    CHECK(neteaseCoverUrl("https://p2.music.126.net/x/1.jpg?param=130y130", 800) ==
          "https://p2.music.126.net/x/1.jpg?param=800y800");
    CHECK(neteaseCoverUrl("").empty());
    CHECK(neteaseCoverUrl("https://evil.example/1.jpg").empty());
    CHECK(neteaseCoverUrl("https://music.126.net.evil.example/1.jpg").empty());
    CHECK(neteaseCoverUrl("file:///C:/x.jpg").empty());

    CHECK(qqAlbumCoverUrl("0017zqT34WuQwa") == "https://y.gtimg.cn/music/photo_new/T002R800x800M0000017zqT34WuQwa.jpg");
    CHECK(qqAlbumCoverUrl("0017zqT34WuQwa", 500) == "https://y.gtimg.cn/music/photo_new/T002R500x500M0000017zqT34WuQwa.jpg");
    CHECK(qqAlbumCoverUrl("").empty());
    CHECK(qqAlbumCoverUrl("../etc").empty());

    CHECK(itunesCoverUrl("https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/f5/93/8c/x/190295978075.jpg/100x100bb.jpg") ==
          "https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/f5/93/8c/x/190295978075.jpg/1000x1000bb.jpg");
    CHECK(itunesCoverUrl("http://is3.mzstatic.com/image/thumb/a/b.png/60x60bb.png", 600) ==
          "https://is3.mzstatic.com/image/thumb/a/b.png/600x600bb.png");
    CHECK(itunesCoverUrl("https://is1-ssl.mzstatic.com/image/thumb/a/cover.jpg").empty()); // no size segment
    CHECK(itunesCoverUrl("https://example.com/100x100bb.jpg").empty());

    CHECK(urlHost("https://P1.Music.126.net:443/a") == "p1.music.126.net");
    CHECK(urlHost("https://user@evil.example/").empty());
    CHECK(urlHost("ftp://x/").empty());
}

TEST_CASE("image sniffing") {
    const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const uint8_t html[] = {'<', 'h', 't', 'm', 'l', '>'};
    CHECK(sniffImageMime(jpg, sizeof jpg) == "image/jpeg");
    CHECK(sniffImageMime(png, sizeof png) == "image/png");
    CHECK(sniffImageMime(webp, sizeof webp) == "image/webp");
    CHECK_FALSE(sniffImageMime(html, sizeof html));
    CHECK_FALSE(sniffImageMime(jpg, 2));
    CHECK_FALSE(sniffImageMime(nullptr, 0));
}

TEST_CASE("search query and provider order") {
    CHECK(searchQuery("Yellow (Remastered)", "Coldplay") == "Yellow Coldplay");
    CHECK(searchQuery("\xE5\x90\x8E\xE6\x9D\xA5", "A/B") == "\xE5\x90\x8E\xE6\x9D\xA5 A"); // 后来
    CHECK(searchQuery("Song", "") == "Song");
    CHECK(providerOrder("QQMusic.exe").front() == "qqmusic");
    CHECK(providerOrder("cloudmusic.exe").front() == "netease");
    CHECK(providerOrder("Spotify.exe").back() == "itunes");
}

TEST_CASE("cover match score: strict, prefers the same album") {
    md::lyrics::Candidate want{"Yellow", "Coldplay", "Parachutes", 269000};
    double sameAlbum = coverMatchScore(want, {"Yellow", "Coldplay", "Parachutes", 269208});
    double otherAlbum = coverMatchScore(want, {"Yellow", "Coldplay", "Greatest Hits", 269500});
    CHECK(sameAlbum > otherAlbum);
    CHECK(otherAlbum >= kMinMatchScore);
    CHECK(coverMatchScore(want, {"Yellow (Live)", "Coldplay", "Live 2012", 411973}) == 0); // duration
    CHECK(coverMatchScore(want, {"Yellow", "Someone Else", "Parachutes", 269000}) == 0);   // artist
    CHECK(coverMatchScore(want, {"Fix You", "Coldplay", "X&Y", 269000}) == 0);
}

TEST_CASE("upgrade stays for the same song only") {
    SongIdentity a{"aaaa", "Yellow", "Coldplay", "Parachutes", 269000};
    CHECK(sameRecording(a, a));
    // trackId churn: duration jitter, album filled in later, re-sent properties.
    CHECK(sameRecording(a, {"bbbb", "Yellow", "Coldplay", "Parachutes", 270400}));
    CHECK(sameRecording({"cccc", "Yellow", "Coldplay", "", std::nullopt}, a));
    CHECK(sameRecording({"dddd", "Yellow ", "coldplay", "Parachutes", 269000}, a));
    // Same trackId is always the same song.
    CHECK(sameRecording({"aaaa", "", "", "", std::nullopt}, a));
    // Real track changes.
    CHECK_FALSE(sameRecording(a, {"eeee", "Fix You", "Coldplay", "X&Y", 295000}));
    CHECK_FALSE(sameRecording(a, {"ffff", "Yellow", "Coldplay", "Live 2012", 269000})); // other album, other cover
    CHECK_FALSE(sameRecording(a, {"gggg", "Yellow", "Coldplay", "Parachutes", 411000}));
    CHECK_FALSE(sameRecording(a, {"hhhh", "Yellow", "Other", "Parachutes", 269000}));
    CHECK_FALSE(sameRecording(a, {"", "", "", "", std::nullopt})); // properties went blank
}

TEST_CASE("cover cache keys, negative TTL and index records") {
    CHECK(isValidCacheKey("0123456789abcdef"));
    CHECK_FALSE(isValidCacheKey(""));
    CHECK_FALSE(isValidCacheKey("..\\x"));
    CHECK_FALSE(isValidCacheKey(std::string(65, 'a')));

    CHECK(negativeFresh(1000, 1000 + kNegativeTtlSec - 1));
    CHECK_FALSE(negativeFresh(1000, 1000 + kNegativeTtlSec));
    CHECK_FALSE(negativeFresh(1000 + 2 * kNegativeTtlSec, 1000)); // clock went backwards a lot

    IndexRecord hit{"0123456789abcdef", std::string(64, 'a'), 123456, 1790000000, "netease"};
    auto line = formatIndexRecord(hit);
    CHECK(line == "0123456789abcdef\t" + std::string(64, 'a') + "\t123456\t1790000000\tnetease");
    CHECK(parseIndexRecord(line) == hit);
    CHECK(parseIndexRecord(line + "\r") == hit);

    IndexRecord miss{"fedcba9876543210", std::nullopt, 0, 1790000001, ""};
    CHECK(formatIndexRecord(miss) == "fedcba9876543210\t-\t0\t1790000001\t");
    CHECK(parseIndexRecord(formatIndexRecord(miss)) == miss);

    CHECK_FALSE(parseIndexRecord(""));
    CHECK_FALSE(parseIndexRecord("0123456789abcdef\t" + std::string(64, 'a') + "\t123")); // truncated write
    CHECK_FALSE(parseIndexRecord("0123456789abcdef\tnothex\t1\t2\tx"));
    CHECK_FALSE(parseIndexRecord("0123456789abcdef\t" + std::string(64, 'a') + "\t0\t2\tx"));       // empty file
    CHECK_FALSE(parseIndexRecord("0123456789abcdef\t" + std::string(64, 'a') + "\t2000000\t2\tx")); // > 1 MiB
    CHECK_FALSE(parseIndexRecord("../../x\t-\t0\t2\t"));
}
