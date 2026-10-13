// Developer tool: measures the lyrics hit rate of the real LyricsService against a
// list of tracks (title|artist|album|durationMs per line, UTF-8). Not shipped.
//
//   MediaDuetLyricsTest.exe tracks.txt [providers e.g. netease,qqmusic,lrclib]

#include "Scheduler.h"
#include "lyrics/LyricsService.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>

#include <winrt/base.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: MediaDuetLyricsTest tracks.txt [providers]\n");
        return 2;
    }
    md::log::init((std::filesystem::temp_directory_path() / L"mediaduet_lyrics_log").wstring(), md::log::Level::Debug);
    std::vector<std::string> providers{"netease", "qqmusic", "lrclib"};
    if (argc > 2) {
        providers.clear();
        std::stringstream ss(md::text::toUtf8(argv[2]));
        std::string p;
        while (std::getline(ss, p, ',')) providers.push_back(p);
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<md::media::MediaSessionInfo> tracks;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string x;
        while (std::getline(ls, x, '|')) f.push_back(x);
        if (f.size() < 2) continue;
        md::media::MediaSessionInfo t;
        t.title = f[0];
        t.artist = f[1];
        t.album = f.size() > 2 ? f[2] : "";
        if (f.size() > 3 && !f[3].empty()) t.durationMs = std::stoll(f[3]);
        t.updateTrackId();
        tracks.push_back(t);
    }
    // Fresh cache so every lookup really goes online.
    auto cache = std::filesystem::temp_directory_path() / L"mediaduet_lyrics_test";
    std::filesystem::remove_all(cache);
    md::platform::Win32Scheduler sched;
    size_t idx = 0, found = 0, synced = 0, words = 0, translated = 0;
    md::platform::LyricsService* svc = nullptr;
    auto next = [&] {
        if (idx >= tracks.size()) {
            std::printf("\nfound %zu/%zu (synced %zu, word-timed %zu, translated %zu) with providers:", found, tracks.size(),
                        synced, words, translated);
            for (auto& p : providers) std::printf(" %s", p.c_str());
            std::printf("\n");
            PostQuitMessage(0);
            return;
        }
        svc->request(tracks[idx]);
    };
    md::platform::LyricsService service(sched, cache, [&](const md::platform::LyricsService::Result& r) {
        if (r.status == "loading") return;
        const auto& t = tracks[idx];
        bool hasWords = false;
        for (auto& l : r.lyrics.lines) hasWords |= !l.words.empty();
        bool hasTr = md::lyrics::hasTranslation(r.lyrics);
        std::printf("%-8s %-8s %3zu lines%s%s  %s - %s\n", r.status.c_str(), r.source.c_str(), r.lyrics.lines.size(),
                    hasWords ? " (words)" : "", hasTr ? " (translated)" : "", t.title.c_str(), t.artist.c_str());
        if (r.status == "found") {
            ++found;
            synced += r.lyrics.synced ? 1 : 0;
            words += hasWords ? 1 : 0;
            translated += hasTr ? 1 : 0;
        }
        ++idx;
        sched.post(next);
    });
    svc = &service;
    service.configure(true, {}, providers);
    sched.post(next);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    std::filesystem::remove_all(cache);
    return 0;
}
