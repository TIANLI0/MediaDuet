#include "md/settings/Settings.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace md::settings;

TEST_CASE("defaults and typed lookup") {
    Settings s;
    CHECK(s.get<std::string>("policy.mode", "") == "windows_first");
    CHECK(s.get<int>("connection.switchBackDelaySec", 0) == 10);
    CHECK(s.get<bool>("lyrics.online", false));
    CHECK(s.get<int>("missing.path", 7) == 7);
    CHECK(s.get<int>("lyrics.overlay.x", -1) == -1); // null -> fallback
}

TEST_CASE("patches merge deeply and are validated") {
    Settings s;
    s.apply({{"policy", {{"mode", "latest"}, {"sensitivity", 50}}}, {"custom", {{"keep", true}}}});
    CHECK(s.get<std::string>("policy.mode", "") == "latest");
    CHECK(s.get<double>("policy.sensitivity", 0) == doctest::Approx(1.0));
    CHECK(s.get<bool>("policy.autoResume", false)); // untouched sibling preserved
    CHECK(s.data()["custom"]["keep"] == true);       // unknown keys kept
    s.apply({{"policy", {{"mode", "chaos"}}}, {"theme", 5}});
    CHECK(s.get<std::string>("policy.mode", "") == "windows_first");
    CHECK(s.get<std::string>("theme", "") == "system");
    s.apply({{"policy", {{"priorities", {{"chrome.exe", 5}, {"bad", "x"}}}}}});
    CHECK(s.data()["policy"]["priorities"].size() == 1);
    s.apply({{"lyrics", {{"overlay", {{"x", 120}, {"y", -40}}}}}});
    CHECK(s.get<int>("lyrics.overlay.x", 0) == 120);
    CHECK(s.get<int>("lyrics.overlay.y", 0) == -40);
}

TEST_CASE("save and load round trip; corrupt file falls back to defaults") {
    auto dir = std::filesystem::temp_directory_path() / "mediaduet_test_settings";
    std::filesystem::create_directories(dir);
    auto file = dir / "settings.json";
    Settings s;
    s.apply({{"language", "zh-CN"}});
    REQUIRE(s.save(file));
    auto l = Settings::load(file);
    CHECK(l.get<std::string>("language", "") == "zh-CN");
    {
        std::ofstream out(file, std::ios::trunc);
        out << "{not json";
    }
    CHECK(Settings::load(file).get<std::string>("language", "") == "auto");
    std::filesystem::remove_all(dir);
}

TEST_CASE("lyrics latency, offsets and overlay layout settings") {
    Settings s;
    CHECK(s.get<bool>("lyrics.latency.auto", false));
    CHECK(s.get<int>("lyrics.latency.bluetoothMs", 0) == 180);
    CHECK(s.data()["lyrics"]["offsets"].is_object());
    CHECK(s.data()["lyrics"]["offsets"].empty());
    CHECK(s.get<bool>("lyrics.overlay.topmost", false));
    CHECK_FALSE(s.get<bool>("lyrics.overlay.hideInFullscreen", true));
    CHECK(s.get<std::string>("lyrics.overlay.layout", "") == "alternate");

    s.apply({{"lyrics", {{"latency", {{"bluetoothMs", 5000}}}, {"overlay", {{"layout", "center"}}}}}});
    CHECK(s.get<int>("lyrics.latency.bluetoothMs", 0) == 1000);
    CHECK(s.get<std::string>("lyrics.overlay.layout", "") == "center");
    s.apply({{"lyrics", {{"latency", {{"bluetoothMs", -3}, {"auto", "yes"}}}, {"overlay", {{"layout", "zigzag"}}}}}});
    CHECK(s.get<int>("lyrics.latency.bluetoothMs", -1) == 0);
    CHECK(s.get<bool>("lyrics.latency.auto", false)); // wrong type -> default
    CHECK(s.get<std::string>("lyrics.overlay.layout", "") == "alternate");
    s.apply({{"lyrics", {{"latency", {{"bluetoothMs", 212.6}}}}}});
    CHECK(s.data()["lyrics"]["latency"]["bluetoothMs"].is_number_integer());
    CHECK(s.get<int>("lyrics.latency.bluetoothMs", 0) == 213);

    // Offsets merge per app id, are clamped, and null / 0 / junk removes an entry.
    s.apply({{"lyrics", {{"offsets", {{"cloudmusic.exe", 300}, {"com.netease.cloudmusic", -9000}, {"bad", "x"}}}}}});
    CHECK(s.data()["lyrics"]["offsets"]["cloudmusic.exe"] == 300);
    CHECK(s.data()["lyrics"]["offsets"]["com.netease.cloudmusic"] == -5000);
    CHECK_FALSE(s.data()["lyrics"]["offsets"].contains("bad"));
    s.apply({{"lyrics", {{"offsets", {{"spotify.exe", 100}}}}}});
    CHECK(s.data()["lyrics"]["offsets"].size() == 3);
    s.apply({{"lyrics", {{"offsets", {{"cloudmusic.exe", nullptr}, {"spotify.exe", 0}}}}}});
    CHECK(s.data()["lyrics"]["offsets"].size() == 1);
    s.apply({{"lyrics", {{"offsets", "garbage"}}}});
    CHECK(s.data()["lyrics"]["offsets"].is_object());
    CHECK(s.data()["lyrics"]["offsets"].empty());
}

TEST_CASE("work mode and online artwork settings") {
    Settings s;
    CHECK(s.get<std::string>("mode", "") == "linked");
    CHECK_FALSE(s.get<bool>("modeChosen", true));
    CHECK(s.get<bool>("artwork.online", false));
    s.apply({{"mode", "standalone"}, {"modeChosen", true}, {"artwork", {{"online", false}}}});
    CHECK(s.get<std::string>("mode", "") == "standalone");
    CHECK(s.get<bool>("modeChosen", false));
    CHECK_FALSE(s.get<bool>("artwork.online", true));
    s.apply({{"mode", "offline"}});                 // unknown value -> default
    CHECK(s.get<std::string>("mode", "") == "linked");
    s.apply({{"mode", 3}, {"modeChosen", "yes"}});  // wrong types -> defaults
    CHECK(s.get<std::string>("mode", "") == "linked");
    CHECK_FALSE(s.get<bool>("modeChosen", true));
}
