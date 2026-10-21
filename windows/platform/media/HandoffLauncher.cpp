#include "HandoffLauncher.h"

#include "lyrics/Catalog.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.h>

#include <cwctype>

namespace md::platform {

using handoff::App;
using namespace winrt;

namespace {

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags = 0) {
    DWORD size = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | flags, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t))
        return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | flags, nullptr, s.data(), &size) != ERROR_SUCCESS)
        return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

// The executable of a "command line" or "icon" registry value: `"C:\x\a.exe" --arg`, `C:\x\a.exe,0`.
std::filesystem::path exeOf(std::wstring v) {
    if (v.empty()) return {};
    if (v.front() == L'"') {
        auto end = v.find(L'"', 1);
        v = end == std::wstring::npos ? v.substr(1) : v.substr(1, end - 1);
    } else {
        std::wstring lower = v;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        if (auto exe = lower.find(L".exe"); exe != std::wstring::npos) v.resize(exe + 4);
    }
    return std::filesystem::path(v);
}

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

std::string fileName(const std::filesystem::path& p) { return text::toLowerAscii(text::toUtf8(p.filename().wstring())); }

std::filesystem::path protocolHandler(const wchar_t* scheme) {
    std::wstring key = std::wstring(scheme) + L"\\shell\\open\\command";
    auto exe = exeOf(regString(HKEY_CLASSES_ROOT, key.c_str(), nullptr));
    return isFile(exe) ? exe : std::filesystem::path();
}

bool protocolHandledBy(const wchar_t* scheme, const char* exeName) {
    auto exe = protocolHandler(scheme);
    return !exe.empty() && fileName(exe) == exeName;
}

bool protocolRegistered(const wchar_t* scheme) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, scheme, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    bool url = RegQueryValueExW(h, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(h);
    return url;
}

std::filesystem::path findQqMusic() {
    std::vector<std::filesystem::path> candidates;
    const wchar_t* uninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QQMusic";
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (DWORD view : {DWORD(RRF_SUBKEY_WOW6432KEY), DWORD(RRF_SUBKEY_WOW6464KEY)}) {
            candidates.push_back(exeOf(regString(root, uninstall, L"DisplayIcon", view)));
            if (auto dir = regString(root, uninstall, L"InstallLocation", view); !dir.empty())
                candidates.push_back(std::filesystem::path(dir) / L"QQMusic.exe");
            if (auto un = exeOf(regString(root, uninstall, L"UninstallString", view)); !un.empty())
                candidates.push_back(un.parent_path() / L"QQMusic.exe");
            if (auto dir = regString(root, L"Software\\Tencent\\QQMusic", L"Install", view); !dir.empty())
                candidates.push_back(std::filesystem::path(dir) / L"QQMusic.exe");
        }
    candidates.push_back(
        exeOf(regString(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\QQMusic.exe", nullptr)));
    for (auto& c : candidates)
        if (isFile(c) && fileName(c) == "qqmusic.exe") return c;
    return {};
}

bool shellOpen(const std::wstring& target, const std::wstring& params) {
    auto h = ShellExecuteW(nullptr, L"open", target.c_str(), params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
}

} // namespace

HandoffLauncher::HandoffLauncher(IScheduler& sched) : sched_(sched) { refresh(); }

HandoffLauncher::~HandoffLauncher() { *alive_ = false; }

bool HandoffLauncher::refresh() {
    std::vector<App> apps;
    neteaseExe_ = protocolHandledBy(L"orpheus", "cloudmusic.exe") ? protocolHandler(L"orpheus") : std::filesystem::path();
    if (!neteaseExe_.empty()) apps.push_back(App::Netease);
    qqExe_ = findQqMusic();
    if (!qqExe_.empty()) apps.push_back(App::QqMusic);
    if (protocolRegistered(L"spotify")) apps.push_back(App::Spotify);
    spotifyExe_ = protocolHandledBy(L"spotify", "spotify.exe") ? protocolHandler(L"spotify") : std::filesystem::path();
    if (apps == apps_) return false;
    apps_ = std::move(apps);
    std::string names;
    for (auto a : apps_) names += std::string(names.empty() ? "" : ", ") + handoff::toString(a);
    log::info("handoff", "music apps on this PC: {}", names.empty() ? "none" : names);
    return true;
}

std::filesystem::path HandoffLauncher::exePath(App app) const {
    switch (app) {
    case App::Netease: return neteaseExe_;
    case App::QqMusic: return qqExe_;
    case App::Spotify: return spotifyExe_;
    }
    return {};
}

void HandoffLauncher::launch(App app, const handoff::Song& song, const media::CatalogIds& ids,
                             std::function<void(const Launched&)> done) {
    std::weak_ptr<bool> alive = alive_;
    [](HandoffLauncher* self, std::weak_ptr<bool> alive, App app, handoff::Song song, media::CatalogIds ids,
       std::filesystem::path qqExe, std::function<void(const Launched&)> done) -> fire_and_forget {
        co_await resume_background();
        // Background: the catalogue lookup. The launch itself runs on the host thread.
        Launched r;
        std::wstring target, params;
        lyrics::Candidate want{song.title, song.artist, song.album, song.durationMs};
        try {
            switch (app) {
            case App::Netease: {
                std::string id = ids.netease;
                if (id.empty()) {
                    auto f = catalog::findNetease(want);
                    if (!f.answered) r.error = "network";
                    else if (!f.hit) r.error = handoff::error::NotFound;
                    else id = std::to_string(f.hit->id);
                }
                if (!id.empty()) {
                    r.exact = true;
                    target = text::toWide(handoff::neteaseDesktopPlay(id));
                    log::info("handoff", "NetEase: play song {}", id);
                }
                break;
            }
            case App::QqMusic: {
                // The desktop client takes the numeric id; the phone knows the songmid.
                auto f = catalog::findQq(want, ids.qqmusic);
                if (!f.answered) r.error = "network";
                else if (!f.hit || f.hit->id <= 0) r.error = handoff::error::NotFound;
                else if (qqExe.empty()) r.error = handoff::error::AppUnavailable;
                else {
                    r.exact = true;
                    target = qqExe.wstring();
                    params = L"/tencentprotocol '" + text::toWide(handoff::qqDesktopProtocol(f.hit->id)) + L"'";
                    log::info("handoff", "QQ Music: play song {}", f.hit->id);
                }
                break;
            }
            case App::Spotify: {
                // No anonymous Spotify catalogue: exact only with the track id from the phone.
                r.exact = !ids.spotify.empty();
                target = text::toWide(r.exact ? handoff::spotifyTrackUri(ids.spotify) : handoff::spotifySearchUri(song));
                log::info("handoff", "Spotify: {}", r.exact ? "open the track" : "search");
                break;
            }
            }
        } catch (hresult_error const& e) {
            log::info("handoff", "catalogue lookup failed: {}", text::toUtf8(e.message()));
            r.error = "network";
            target.clear();
        } catch (...) {
            r.error = "network";
            target.clear();
        }
        self->sched_.post([alive, r, target, params, done]() mutable {
            if (alive.expired()) return;
            if (!target.empty()) {
                r.ok = shellOpen(target, params);
                if (!r.ok) {
                    r.error = handoff::error::LaunchFailed;
                    log::warn("handoff", "could not open the music app");
                }
            }
            if (done) done(r);
        });
    }(this, alive, app, song, ids, qqExe_, std::move(done));
}

} // namespace md::platform
