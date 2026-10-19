// Developer tool: runs the real update check and download against GitHub without installing
// anything. Not shipped.
//
//   MediaDuetUpdateCheck [current-version] [work-dir]
//
// With an older current version (default 0.0.0) it downloads the release's file for a portable
// copy, verifies it against SHA256SUMS.txt and prints the result; the file stays in work-dir.

#include "Scheduler.h"
#include "update/Updater.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>

#include <winrt/base.h>

#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::string current = argc > 1 ? md::text::toUtf8(argv[1]) : "0.0.0";
    std::filesystem::path dir = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::temp_directory_path() / L"MediaDuetUpdateCheck";
    md::platform::Win32Scheduler sched;
    int rc = 1;
    md::platform::Updater* self = nullptr;
    std::string last;
    md::platform::Updater::Callbacks cb;
    cb.changed = [&] {
        auto s = self->status();
        std::string line = s.dump();
        if (line == last) return;
        last = line;
        std::printf("%s\n", line.c_str());
        auto phase = s.value("phase", std::string());
        if (phase == "available") self->install();
        if (phase == "current") {
            rc = 0;
            PostQuitMessage(0);
        }
        if (phase == "error") PostQuitMessage(0);
    };
    cb.quit = [] { PostQuitMessage(0); };
    cb.apply = [&](const std::filesystem::path& file) {
        std::error_code ec;
        std::printf("verified %s (%lld bytes); not installed\n", md::text::toUtf8(file.wstring()).c_str(),
                    static_cast<long long>(std::filesystem::file_size(file, ec)));
        rc = 0;
        PostQuitMessage(0);
    };
    md::platform::Updater u(sched, dir, current, cb);
    self = &u;
    u.check(true);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return rc;
}
