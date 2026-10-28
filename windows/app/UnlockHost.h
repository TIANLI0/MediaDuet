#pragma once

#include "IpcServer.h"
#include "UnlockSetup.h"

#include "md/protocol/Envelope.h"
#include "md/util/Bytes.h"
#include "md/util/Scheduler.h"
#include "md/unlock/BootRecord.h"
#include <future>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

namespace md::app {

// Unlocking this PC with the phone (docs/unlock.md): the enrollment (the sealed Windows
// password), setup from the UI, the credential provider's pipe and the unlock.* messages.
class UnlockHost {
public:
    struct Callbacks {
        // The connected phone (empty id: none) and whether it announced `unlock`.
        std::function<std::string()> peerId;
        std::function<std::string()> peerName;
        std::function<bool()> peerCanUnlock;
        std::function<uint64_t(const char* type, nlohmann::json body, std::optional<uint64_t> re)> send;
        std::function<std::string()> pcName;
        std::function<std::string()> language; // "zh-CN" | "en-US"
        std::function<void()> changed;
        std::function<std::optional<unlock::BootRecord>(const std::string& peerId)> bootRecord;
    };
    using Done = std::function<void(bool ok, const std::string& error)>;

    UnlockHost(IScheduler& sched, Callbacks cb);
    ~UnlockHost();

    void onLinkUp();
    void onLinkDown();
    // unlock.* from the phone; false if `e` is not one.
    bool onEnvelope(const proto::Envelope& e);
    // The phone was unpaired: its enrollment is useless.
    void onPeerRemoved(const std::string& peerId);

    // From the UI (docs/unlock.md "Windows UI ↔ host IPC").
    void enable(std::string password, Done done);
    void disable(Done done);
    void repair(Done done);
    nlohmann::json stateJson() const;

private:
    struct Enrollment {
        std::string id;
        std::string peerId, peerName;
        Bytes sealed; // unlock::seal output, DPAPI-protected at rest
        bool stale = false;
        int64_t lastUnlockAt = 0;
    };
    void load();
    bool save() const;
    bool phoneConnected() const;

    // Setup steps of enable().
    struct Setup {
        std::string step; // verifying | installing | waiting_phone
        Done done;
        Enrollment pending;
        Bytes key;
        uint64_t msgId = 0;
        bool phoneConfirmed = false;
    };
    void setupStep(const std::string& step);
    void enrollWithPhone();
    void commitSetup();
    void refreshBoot();
    void removeBootAccess();
    void launch(std::function<void()> worker);
    void finishSetup(bool ok, const std::string& error);
    // Runs `MediaDuet.exe --unlock-component <action> <sid>` elevated; done(exitCode or -1 cancelled / -2 failed).
    void runElevated(const std::wstring& action, std::function<void(int)> done);

    // Credential provider pipe.
    void onPipeLine(const std::string& line);
    void onPipeClosed();
    void requestUnlock();
    void sendRequest();
    void cancelRequest();
    void watchLock();
    void onUnlockResult(const proto::Envelope& e);
    void sendStatus();
    std::string pipeState() const;
    static bool sessionLocked();

    IScheduler& sched_;
    Callbacks cb_;
    std::wstring userSid_;
    std::optional<Enrollment> enrollment_;
    ComponentState component_ = ComponentState::Missing;
    std::optional<Setup> setup_;
    ScopedTimer setupTimer_, requestTimer_, lockTimer_, bootTimer_;

    std::unique_ptr<IpcServer> pipe_;
    bool cpHello_ = false;   // the credential provider said hello for this user
    bool wantUnlock_ = false; // it asked; sent when the phone connects
    bool maintenance_ = false;
    uint64_t requestId_ = 0;  // unlock.request waiting for the phone
    std::string outcome_;     // denied | failed | unlocking: until the next request
    std::string outcomeCode_;
    std::string lastStatus_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    std::vector<std::future<void>> workers_;
    bool bootEnabled_ = false, bootQuery_ = false;
};

} // namespace md::app
