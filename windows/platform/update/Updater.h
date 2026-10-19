#pragma once

#include "md/update/Update.h"
#include "md/util/Scheduler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace md::platform {

// Software updates (Settings > About, docs/ipc.md state.update): asks GitHub for the latest
// release (60 s after start, then once a day while enabled, or on request), downloads the
// installer — or the zip for a portable copy — checks it against the release's SHA256SUMS.txt
// and installs it:
//  * installed copy (HKCU uninstall entry pointing at this folder): `setup.exe /S /UPDATE`, which
//    stops MediaDuet, installs in place and starts it again;
//  * portable copy: a hidden PowerShell script waits for MediaDuet to exit, copies the new files
//    over this folder and starts it again.
// Only the version check and the download leave the PC (no account, no identifiers).
class Updater {
public:
    enum class Phase { Idle, Checking, Current, Available, Downloading, Installing, Error };
    struct Callbacks {
        std::function<void()> changed;
        // Quit the host so the installer / script can replace its files.
        std::function<void()> quit;
        // Tests / tools only: called with the verified download instead of installing it.
        std::function<void(const std::filesystem::path&)> apply;
    };

    Updater(IScheduler& sched, std::filesystem::path workDir, std::string currentVersion, Callbacks cb);
    ~Updater();

    void setAutoCheck(bool on);
    // manual: the user asked (shows "up to date" afterwards).
    void check(bool manual);
    // Downloads (when needed), verifies and installs the available update.
    void install();
    nlohmann::json status() const;
    Phase phase() const { return phase_; }
    const std::string& latest() const { return latest_ ? latest_->version : empty_; }
    bool portable() const { return portable_; }

    static constexpr int64_t kFirstCheckMs = 60'000;
    static constexpr int64_t kIntervalMs = 24LL * 3600 * 1000;

private:
    void setPhase(Phase p, std::string error = {});
    void scheduleAuto(int64_t delayMs);
    void apply(const std::filesystem::path& file);

    IScheduler& sched_;
    std::filesystem::path dir_;
    std::filesystem::path appDir_;
    std::string current_;
    Callbacks cb_;
    bool auto_ = true;
    bool portable_ = false;
    Phase phase_ = Phase::Idle;
    std::string error_;
    double progress_ = 0;
    int64_t checkedAt_ = 0; // wall ms of the last answer
    std::optional<update::Release> latest_;
    std::string empty_;
    uint64_t gen_ = 0;
    ScopedTimer timer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

const char* toString(Updater::Phase p);

} // namespace md::platform
