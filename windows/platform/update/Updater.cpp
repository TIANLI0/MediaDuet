#include "Updater.h"

#include "md/crypto/Crypto.h"
#include "md/util/Bytes.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <cwctype>
#include <fstream>

namespace md::platform {

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Web::Http;
using json = nlohmann::json;

const char* toString(Updater::Phase p) {
    switch (p) {
    case Updater::Phase::Idle: return "idle";
    case Updater::Phase::Checking: return "checking";
    case Updater::Phase::Current: return "current";
    case Updater::Phase::Available: return "available";
    case Updater::Phase::Downloading: return "downloading";
    case Updater::Phase::Installing: return "installing";
    case Updater::Phase::Error: return "error";
    }
    return "idle";
}

namespace {

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// Installed by setup.exe (installer.nsi writes this key) into the folder we run from.
bool installedHere(const fs::path& dir) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MediaDuet", L"InstallLocation",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return lower(buf) == lower(dir.wstring());
}

HttpClient client() {
    Filters::HttpBaseProtocolFilter filter;
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    HttpClient c(filter);
    c.DefaultRequestHeaders().UserAgent().TryParseAdd(L"MediaDuet/" MD_VERSION_W L" (update check)");
    return c;
}

template <class Op>
auto withTimeout(Op op, int seconds) {
    auto timer = Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [op](auto&&) {
            try {
                op.Cancel();
            } catch (...) {
            }
        },
        std::chrono::seconds(seconds));
    auto r = op.get();
    timer.Cancel();
    return r;
}

std::optional<std::string> getText(HttpClient& c, const std::string& url) {
    HttpRequestMessage req(HttpMethod::Get(), Windows::Foundation::Uri(text::toWide(url)));
    req.Headers().TryAppendWithoutValidation(L"Accept", L"application/vnd.github+json");
    auto resp = withTimeout(c.SendRequestAsync(req), 20);
    if (!resp.IsSuccessStatusCode()) {
        log::info("update", "GET {} -> HTTP {}", url, static_cast<int>(resp.StatusCode()));
        return std::nullopt;
    }
    return text::toUtf8(resp.Content().ReadAsStringAsync().get());
}

// The script that replaces a portable copy once MediaDuet has exited.
constexpr char kApplyScript[] = R"(param([int]$HostPid, [string]$Zip, [string]$Dest)
$ErrorActionPreference = 'Stop'
$log = Join-Path (Split-Path $Zip) 'apply.log'
function Log($m) { Add-Content -Path $log -Value ((Get-Date -Format s) + ' ' + $m) }
try {
    try { Wait-Process -Id $HostPid -Timeout 30 -ErrorAction SilentlyContinue } catch {}
    # The UI runs from the same folder: give it a moment to close, then stop it.
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and (Get-Process -Name MediaDuetUI -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 300 }
    Get-Process -Name MediaDuetUI, MediaDuet -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Dest, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force
    $stage = Join-Path (Split-Path $Zip) 'staged'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    Expand-Archive -Path $Zip -DestinationPath $stage -Force
    if (-not (Test-Path (Join-Path $stage 'MediaDuet.exe'))) { throw 'MediaDuet.exe missing in the update' }
    Copy-Item -Path (Join-Path $stage '*') -Destination $Dest -Recurse -Force
    Remove-Item $stage -Recurse -Force
    Log "updated $Dest"
} catch {
    Log ("failed: " + $_)
}
Start-Process -FilePath (Join-Path $Dest 'MediaDuet.exe') -WorkingDirectory $Dest
)";

} // namespace

Updater::Updater(IScheduler& sched, fs::path workDir, std::string currentVersion, Callbacks cb)
    : sched_(sched), dir_(std::move(workDir)), current_(std::move(currentVersion)), cb_(std::move(cb)) {
    timer_.bind(&sched_);
    appDir_ = exeDir();
    portable_ = !installedHere(appDir_);
    // Leftovers of an earlier update (the new version is running now).
    std::error_code ec;
    fs::remove_all(dir_, ec);
    log::info("update", "MediaDuet {} ({} copy in {})", current_, portable_ ? "portable" : "installed", text::toUtf8(appDir_.wstring()));
}

Updater::~Updater() { *alive_ = false; }

void Updater::setAutoCheck(bool on) {
    if (on == auto_ && (timer_.active() || !on)) return;
    auto_ = on;
    if (!on) {
        timer_.cancel();
        return;
    }
    scheduleAuto(checkedAt_ ? kIntervalMs : kFirstCheckMs);
}

void Updater::scheduleAuto(int64_t delayMs) {
    if (!auto_) return;
    timer_.start(delayMs, [this] {
        check(false);
        scheduleAuto(kIntervalMs);
    });
}

void Updater::setPhase(Phase p, std::string error) {
    phase_ = p;
    error_ = std::move(error);
    if (cb_.changed) cb_.changed();
}

nlohmann::json Updater::status() const {
    json j = {{"phase", toString(phase_)}, {"current", current_}, {"auto", auto_}, {"portable", portable_},
              {"progress", progress_}, {"checkedAt", checkedAt_}};
    j["latest"] = latest_ ? json(latest_->version) : json(nullptr);
    j["page"] = latest_ ? latest_->page : std::string(update::kReleasesPage);
    j["notes"] = latest_ ? latest_->notes : std::string();
    j["error"] = error_.empty() ? json(nullptr) : json(error_);
    return j;
}

void Updater::check(bool manual) {
    if (phase_ == Phase::Checking || phase_ == Phase::Downloading || phase_ == Phase::Installing) return;
    Phase before = phase_;
    setPhase(Phase::Checking);
    uint64_t gen = ++gen_;
    std::weak_ptr<bool> alive = alive_;
    [](Updater* self, std::weak_ptr<bool> alive, uint64_t gen, std::string current, bool manual, Phase before) -> fire_and_forget {
        co_await resume_background();
        std::optional<update::Release> rel;
        bool answered = false;
        try {
            auto c = client();
            if (auto body = getText(c, update::kLatestReleaseUrl)) {
                answered = true;
                rel = update::parseRelease(json::parse(*body, nullptr, false));
            }
        } catch (hresult_error const& e) {
            log::info("update", "update check failed: {}", text::toUtf8(e.message()));
        } catch (...) {
        }
        self->sched_.post([self, alive, gen, rel, answered, current, manual, before] {
            if (alive.expired() || gen != self->gen_) return;
            if (!answered || !rel) {
                // A failed automatic check stays quiet; the user only sees it when they asked.
                if (manual) self->setPhase(Phase::Error, answered ? "no_release" : "network");
                else self->setPhase(before == Phase::Checking ? Phase::Idle : before);
                return;
            }
            self->checkedAt_ = self->sched_.wallMs();
            if (update::isNewer(rel->version, current)) {
                bool fresh = !self->latest_ || self->latest_->version != rel->version;
                self->latest_ = rel;
                if (fresh) log::info("update", "MediaDuet {} is available (this is {})", rel->version, current);
                self->progress_ = 0;
                self->setPhase(Phase::Available);
            } else {
                self->latest_ = rel;
                self->setPhase(Phase::Current);
            }
        });
    }(this, alive, gen, current_, manual, before);
}

void Updater::install() {
    if (phase_ != Phase::Available && !(phase_ == Phase::Error && latest_)) return;
    if (!latest_ || !update::isNewer(latest_->version, current_)) return;
    std::string file = portable_ ? update::portableAsset(latest_->version) : update::setupAsset(latest_->version);
    const update::Asset* asset = latest_->find(file);
    const update::Asset* sums = latest_->find(update::kChecksumsAsset);
    if (!asset || !sums) {
        log::warn("update", "release {} has no {} or checksums", latest_->version, file);
        return setPhase(Phase::Error, "no_asset");
    }
    progress_ = 0;
    setPhase(Phase::Downloading);
    uint64_t gen = ++gen_;
    std::weak_ptr<bool> alive = alive_;
    [](Updater* self, std::weak_ptr<bool> alive, uint64_t gen, update::Asset asset, std::string sumsUrl, fs::path dir) -> fire_and_forget {
        co_await resume_background();
        std::string error;
        fs::path target = dir / text::toWide(asset.name);
        try {
            std::error_code ec;
            fs::create_directories(dir, ec);
            auto c = client();
            auto sums = getText(c, sumsUrl);
            auto want = sums ? update::checksumFor(*sums, asset.name) : std::nullopt;
            if (!want) {
                error = sums ? "no_checksum" : "network";
            } else {
                auto resp = withTimeout(c.GetAsync(Windows::Foundation::Uri(text::toWide(asset.url)),
                                                   HttpCompletionOption::ResponseHeadersRead),
                                        30);
                if (!resp.IsSuccessStatusCode()) {
                    error = "network";
                } else {
                    int64_t total = asset.size;
                    if (auto len = resp.Content().Headers().ContentLength()) total = static_cast<int64_t>(len.Value());
                    auto in = resp.Content().ReadAsInputStreamAsync().get();
                    std::ofstream out(target, std::ios::binary | std::ios::trunc);
                    if (!out) {
                        error = "write";
                    } else {
                        Bytes data;
                        data.reserve(total > 0 ? static_cast<size_t>(total) : 0);
                        Windows::Storage::Streams::Buffer buf(256 * 1024);
                        int64_t lastPost = 0;
                        for (;;) {
                            auto got = withTimeout(in.ReadAsync(buf, buf.Capacity(), Windows::Storage::Streams::InputStreamOptions::Partial), 60);
                            if (got.Length() == 0) break;
                            out.write(reinterpret_cast<const char*>(got.data()), got.Length());
                            data.insert(data.end(), got.data(), got.data() + got.Length());
                            if (alive.expired()) co_return;
                            int64_t done = static_cast<int64_t>(data.size());
                            if (total > 0 && done - lastPost > total / 50) {
                                lastPost = done;
                                double p = std::min(1.0, static_cast<double>(done) / static_cast<double>(total));
                                self->sched_.post([self, alive, gen, p] {
                                    if (alive.expired() || gen != self->gen_) return;
                                    self->progress_ = p;
                                    if (self->cb_.changed) self->cb_.changed();
                                });
                            }
                        }
                        out.close();
                        std::string got = hexEncode(crypto::sha256(ByteView(data)));
                        if (total > 0 && static_cast<int64_t>(data.size()) != total) error = "network";
                        else if (got != *want) {
                            log::warn("update", "checksum mismatch for {}: {} != {}", asset.name, got, *want);
                            error = "checksum";
                        }
                    }
                }
            }
        } catch (hresult_error const& e) {
            log::info("update", "download failed: {}", text::toUtf8(e.message()));
            error = "network";
        } catch (...) {
            error = "network";
        }
        if (!error.empty()) {
            std::error_code ec;
            fs::remove(target, ec);
        }
        self->sched_.post([self, alive, gen, error, target] {
            if (alive.expired() || gen != self->gen_) return;
            if (!error.empty()) return self->setPhase(Phase::Error, error);
            self->progress_ = 1;
            self->apply(target);
        });
    }(this, alive, gen, *asset, sums->url, dir_);
}

void Updater::apply(const fs::path& file) {
    setPhase(Phase::Installing);
    if (cb_.apply) return cb_.apply(file);
    bool ok = false;
    if (!portable_) {
        // installer.nsi: silent, stops MediaDuet, installs in place, /UPDATE starts it again.
        auto h = ShellExecuteW(nullptr, L"open", file.c_str(), L"/S /UPDATE", file.parent_path().c_str(), SW_HIDE);
        ok = reinterpret_cast<INT_PTR>(h) > 32;
    } else {
        // The folder must be writable for the script to replace the files.
        fs::path probe = appDir_ / L".mediaduet-update-probe";
        std::error_code ec;
        {
            std::ofstream t(probe);
            ok = static_cast<bool>(t);
        }
        fs::remove(probe, ec);
        if (!ok) {
            log::warn("update", "{} is not writable: cannot update this portable copy", text::toUtf8(appDir_.wstring()));
            return setPhase(Phase::Error, "not_writable");
        }
        fs::path script = file.parent_path() / L"apply.ps1";
        std::ofstream(script, std::ios::binary) << kApplyScript;
        std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script.wstring() +
                            L"\" -HostPid " + std::to_wstring(GetCurrentProcessId()) + L" -Zip \"" + file.wstring() +
                            L"\" -Dest \"" + appDir_.wstring() + L"\"";
        auto h = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), file.parent_path().c_str(), SW_HIDE);
        ok = reinterpret_cast<INT_PTR>(h) > 32;
    }
    if (!ok) {
        log::warn("update", "could not start the update");
        return setPhase(Phase::Error, "launch");
    }
    log::info("update", "installing MediaDuet {}; exiting", latest());
    if (cb_.quit) sched_.schedule(300, [quit = cb_.quit] { quit(); });
}

} // namespace md::platform
