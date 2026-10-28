#include "BootStore.h"
#include "ServiceTransport.h"
#include "IpcServer.h"
#include "Scheduler.h"
#include "md/unlock/Component.h"
#include "md/unlock/Unlock.h"
#include "md/util/Text.h"
#include <wtsapi32.h>
#include <sddl.h>
#include <winsock2.h>
#include <map>

namespace md::unlock {
using json = nlohmann::json;
namespace {
bool systemClient(HANDLE pipe, std::atomic<DWORD>& session) {
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE token = nullptr; bool ok = false;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        DWORD n = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &n); Bytes b(n);
        ok = GetTokenInformation(token, TokenUser, b.data(), n, &n) &&
             IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, WinLocalSystemSid);
        CloseHandle(token);
    }
    RevertToSelf(); ULONG actual = 0;
    if (ok && GetNamedPipeClientSessionId(pipe, &actual)) { session = actual; return true; }
    return false;
}
struct ConsoleState { bool known = false, hasUser = false, locked = false, sameUser = false; };
ConsoleState consoleState(DWORD session, const std::string& targetSid) {
    ConsoleState state;
    if (session == 0 || session != WTSGetActiveConsoleSessionId()) return state;
    LPWSTR name = nullptr; DWORD n = 0;
    state.known = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &name, &n) != FALSE;
    state.hasUser = state.known && name && *name;
    if (name) WTSFreeMemory(name);
    if (!state.hasUser) return state;
    HANDLE token = nullptr;
    if (WTSQueryUserToken(session, &token)) {
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size); Bytes bytes(size);
        LPWSTR sid = nullptr;
        if (GetTokenInformation(token, TokenUser, bytes.data(), size, &size) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) {
            state.sameUser = text::toUtf8(sid) == targetSid; LocalFree(sid);
        }
        CloseHandle(token);
    }
    LPWSTR data = nullptr;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &data, &n)) {
        auto info = reinterpret_cast<WTSINFOEXW*>(data);
        if (n >= sizeof(WTSINFOEXW) && info->Level == 1)
            state.locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        else state.known = false;
        WTSFreeMemory(data);
    } else state.known = false;
    return state;
}
bool serviceSessionAllowed(DWORD session, const std::string& sid) {
    auto state = consoleState(session, sid);
    bool host = false;
    if (state.hasUser) {
        auto pipe = std::wstring(component::kPipePrefix) + std::to_wstring(session);
        host = WaitNamedPipeW(pipe.c_str(), 1) != FALSE;
        if (!host) host = GetLastError() != ERROR_FILE_NOT_FOUND;
    }
    return bootSessionAllowed(session, WTSGetActiveConsoleSessionId(), state.known, state.hasUser, state.locked, state.sameUser, host);
}
bool credentialSucceeded(DWORD session, const std::string& sid) {
    auto state = consoleState(session, sid);
    return state.known && state.hasUser && state.sameUser && !state.locked;
}
class User final : public link::ConnectionManager::Listener {
    struct Bridge final : link::ConnectionManager::Listener {
        User& owner; bool active = true;
        explicit Bridge(User& user) : owner(user) {}
        void onLinkUp(const std::string& id, bool resumed) override { if (active) owner.onLinkUp(id, resumed); }
        void onLinkDown(const std::string& id) override { if (active) owner.onLinkDown(id); }
        void onEnvelope(const std::string& id, const proto::Envelope& e, uint64_t conn) override { if (active) owner.onEnvelope(id, e, conn); }
        void onBlob(const std::string& id, const proto::BlobChunk& blob) override { if (active) owner.onBlob(id, blob); }
        void onPeerRevoked(const std::string& id) override { if (active) owner.onPeerRevoked(id); }
        void onStatusChanged() override { if (active) owner.onStatusChanged(); }
    };
public:
    struct Environment {
        std::function<bool(DWORD, const std::string&)> allowSession = serviceSessionAllowed;
        std::function<bool(DWORD, const std::string&)> signedIn = credentialSucceeded;
        std::function<bool(const BootRecord&)> save = writeBootRecord;
        std::function<bool(const std::string&)> erase = eraseBootRecord;
        std::function<void(const json&)> output;
        bool pipes = true;
    };
    User(IScheduler& sched, link::ITransportFactory& factory, BootRecord record)
        : User(sched, factory, std::move(record), Environment{}) {}
    User(IScheduler& sched, link::ITransportFactory& factory, BootRecord record, Environment environment)
        : sched_(sched), factory_(factory), record_(std::move(record)), env_(std::move(environment)), timer_(&sched), lockWatch_(&sched) {
        if (!env_.pipes) return;
        auto sid = text::toWide(record_.userSid);
        auto guard = alive_;
        boot_ = std::make_unique<app::IpcServer>(sched, app::IpcServer::Callbacks{
            [this, guard] { if (*guard) { hello_ = false; lastStatus_.clear(); } },
            [this, guard] { if (*guard) { hello_ = false; if (state_ != "unlocking") cancel(); } },
            [this, guard](const std::string& line) { if (*guard) { try { onLine(line); } catch (...) {} } }},
            app::IpcServer::Options{std::wstring(component::kBootPipePrefix) + sid, L"D:P(A;;GA;;;SY)",
                [session = session_](HANDLE pipe) { return systemClient(pipe, *session); },
                [](std::string& line, HANDLE pipe) {
                    ULONG session = 0;
                    auto j = json::parse(line, nullptr, false);
                    if (j.is_object()) {
                        j["_clientSession"] = GetNamedPipeClientSessionId(pipe, &session) ? session : 0;
                        line = j.dump();
                    }
                }});
        control_ = std::make_unique<app::IpcServer>(sched, app::IpcServer::Callbacks{{}, {},
            [this, guard](const std::string& line) { if (*guard) { try { controlLine(line); } catch (...) {} } }},
            app::IpcServer::Options{std::wstring(component::kControlPipePrefix) + sid,
                L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + sid + L")", {}});
        boot_->start(); control_->start();
    }
    ~User() override {
        *alive_ = false; cancel(); if (boot_) boot_->stop(); if (control_) control_->stop();
        secureZero(record_.peer.key);
    }
    void replace(BootRecord record) {
        if (record.enrollment != record_.enrollment) { cancel(); state_.clear(); endpoints_.clear(); }
        else {
            record.stale = record.stale || record_.stale;
            record.lastUnlockAt = std::max(record.lastUnlockAt, record_.lastUnlockAt);
        }
        enabled_ = true;
        secureZero(record_.peer.key); record_ = std::move(record); status();
    }
    void missing() { if (enabled_) { enabled_ = false; cancel(); status(); } }
    bool canRetire() const { return !enabled_ && (!boot_ || !boot_->connected()) && (!control_ || !control_->connected()); }
    void warm(DWORD console) {
        if (!hello_) *session_ = console;
        if (!allowed()) { if (cm_) cancel(); return; }
        ensureConnection(false); watch();
    }
    void endpoint(const std::string& id, const link::LanEndpoint& ep) {
        if (id != record_.peer.id) return;
        if (endpoints_.size() < 16 || endpoints_.contains(ep.str())) endpoints_[ep.str()] = ep;
        if (cm_) cm_->onLanEndpoint(id, ep, true);
    }
    void onLinkUp(const std::string&, bool) override {
        if (!allowed()) return cancel();
        auto caps = cm_->peerCaps();
        if (!caps || std::find(caps->begin(), caps->end(), proto::caps::Unlock) == caps->end()) return fail("unsupported");
        if (!want_) { status(); return; } // preconnected; no biometric prompt until requested
        request_ = cm_->send(proto::types::UnlockRequest, requestBody(record_.enrollment, record_.self.name));
        if (!request_) return fail("link_lost");
        state_ = "waiting"; status();
    }
    void onLinkDown(const std::string&) override { if (want_ && request_) fail("link_lost"); else status(); }
    void onBlob(const std::string&, const proto::BlobChunk&) override {}
    void onPeerRevoked(const std::string&) override { record_.stale = true; env_.save(record_); fail("not_enrolled"); }
    void onStatusChanged() override { status(); }
    void onPeerEndpoints(const std::string&, const std::vector<link::LanEndpoint>&) override {}
    void onEnvelope(const std::string& peer, const proto::Envelope& e, uint64_t) override {
        if (peer != record_.peer.id || e.src != peer) return;
        if (e.type == proto::types::UnlockRevoke && parseRevoke(e.body) == record_.enrollment) {
            if (env_.erase(record_.userSid)) enabled_ = false;
            cancel(); status(); return;
        }
        if (e.type != proto::types::UnlockResult || !request_ || e.re != request_) return;
        auto result = parseResult(e.body);
        request_ = 0; want_ = false;
        if (!result) return fail("bad_message");
        if (!result->ok) {
            secureZero(result->key);
            if (result->error == "key_invalidated" || result->error == "not_enrolled") { record_.stale = true; env_.save(record_); }
            return fail(result->error);
        }
        auto password = allowed() && hello_ ? open(result->key, record_.enrollment, record_.userSid, record_.sealed) : std::nullopt;
        secureZero(result->key);
        if (!password) return fail("bad_credential");
        timer_.cancel(); state_ = "unlocking"; status();
        json credential{{"type", "credential"}, {"password", *password}};
        emit(credential); SecureZeroMemory(password->data(), password->size());
        auto& copy = credential["password"].get_ref<std::string&>(); SecureZeroMemory(copy.data(), copy.size());
        timer_.start(15000, [this] { fail("timeout"); });
        watch();
    }
private:
    friend struct UserTestAccess;
    bool allowed() const { return enabled_ && !record_.stale && env_.allowSession(*session_, record_.userSid); }
    void emit(const json& j) { if (env_.output) env_.output(j); else if (boot_) boot_->send(j.dump()); }
    void ensureConnection(bool expedite) {
        if (cm_) {
            if (expedite && !cm_->isUp()) cm_->connectNow(record_.peer.id);
            return;
        }
        bridge_ = std::make_shared<Bridge>(*this);
        cm_ = std::make_unique<link::ConnectionManager>(sched_, factory_, *bridge_, record_.self,
            std::vector<std::string>{proto::caps::Unlock});
        cm_->onBluetoothRadio(true); cm_->setPeers({record_.peer});
        for (const auto& [name, ep] : endpoints_) cm_->onLanEndpoint(record_.peer.id, ep, true);
        cm_->onPeerBluetoothPresence(record_.peer.id, true);
        cm_->connectNow(record_.peer.id);
    }
    void onLine(const std::string& line) {
        auto j = json::parse(line, nullptr, false); if (!j.is_object()) return;
        if (env_.pipes) *session_ = j.at("_clientSession").get<DWORD>();
        auto type = j.value("type", std::string());
        if (type == "hello") {
            cancel(false); state_.clear();
            hello_ = j.value("v", 0) == 1 && j.value("sid", std::string()) == record_.userSid;
            if (hello_ && allowed()) { ensureConnection(false); watch(); }
            else cancel();
            status(); return;
        }
        if (!hello_) return;
        if (type == "request") {
            cancel(false); state_.clear(); code_.clear();
            if (!allowed()) { cancel(); status(); return; }
            want_ = true;
            timer_.start(kRequestWaitMs, [this] { fail("timeout"); });
            ensureConnection(true);
            if (cm_->isUp()) onLinkUp(record_.peer.id, true);
            watch(); status();
        } else if (type == "cancel") { cancel(false); state_.clear(); if (!allowed()) cancel(); else watch(); status(); }
        else if (type == "result" && state_ == "unlocking") {
            bool ok = j.value("ok", false); auto code = static_cast<uint32_t>(j.value("status", int64_t(0)));
            if (!ok && (code == 0xc000006d || code == 0xc000006a)) record_.stale = true;
            if (ok) record_.lastUnlockAt = sched_.wallMs();
            env_.save(record_); cancel(); state_.clear(); status();
        }
    }
    void watch() {
        lockWatch_.start(500, [this] {
            if (!env_.allowSession(*session_, record_.userSid)) {
                if (state_ == "unlocking" && env_.signedIn(*session_, record_.userSid)) { record_.lastUnlockAt = sched_.wallMs(); env_.save(record_); }
                cancel(); state_.clear(); status();
            } else if (want_ || state_ == "unlocking" || cm_) watch();
        });
    }
    void cancel(bool disconnect = true) {
        if (cm_ && request_) cm_->send(proto::types::UnlockCancel, json::object(), request_);
        request_ = 0; want_ = false; timer_.cancel(); lockWatch_.cancel();
        // Defer destruction: a link callback may be on ConnectionManager's stack.
        if (cm_ && disconnect) {
            bridge_->active = false;
            auto retired = std::make_shared<std::pair<std::shared_ptr<Bridge>, std::unique_ptr<link::ConnectionManager>>>(
                std::move(bridge_), std::move(cm_));
            sched_.post([retired] {});
        }
    }
    void fail(const std::string& code) { cancel(); state_ = code == "denied" ? "denied" : "failed"; code_ = code; status(); }
    void status() {
        if (!env_.output && (!boot_ || !boot_->connected())) return;
        std::string state = !hello_ ? "other_user" : !enabled_ || !env_.allowSession(*session_, record_.userSid) ? "not_enrolled" :
            record_.stale ? "stale" : !state_.empty() ? state_ : cm_ && cm_->isUp() ? "ready" : "offline";
        json j{{"type", "status"}, {"state", state}, {"phone", record_.peer.name}, {"lang", record_.language}, {"code", code_}};
        auto line = j.dump(); if (line != lastStatus_) { lastStatus_ = line; emit(j); }
    }
    void controlLine(const std::string& line) {
        auto j = json::parse(line, nullptr, false); if (!j.is_object()) return;
        auto type = j.value("type", std::string());
        bool ok = type == "status";
        if (type == "remove") {
            ok = env_.erase(record_.userSid);
            if (ok) { enabled_ = false; cancel(); status(); }
        }
        control_->send(json{{"sid", record_.userSid}, {"ok", ok}, {"enabled", enabled_},
            {"enrollment", record_.enrollment}, {"stale", record_.stale}, {"lastUnlockAt", record_.lastUnlockAt}}.dump());
    }
    IScheduler& sched_;
    link::ITransportFactory& factory_;
    BootRecord record_;
    Environment env_;
    std::unique_ptr<app::IpcServer> boot_, control_;
    std::unique_ptr<link::ConnectionManager> cm_;
    std::shared_ptr<Bridge> bridge_;
    ScopedTimer timer_, lockWatch_;
    bool hello_ = false, want_ = false, enabled_ = true;
    uint64_t request_ = 0;
    std::string state_, code_, lastStatus_;
    std::map<std::string, link::LanEndpoint> endpoints_;
    std::shared_ptr<std::atomic<DWORD>> session_ = std::make_shared<std::atomic<DWORD>>(0);
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
};
#ifndef MD_UNLOCK_SERVICE_TESTING
class Runtime {
public:
    Runtime() : factory_(sched_), refresh_(&sched_) {
        auto guard = alive_;
        discovery_ = std::make_unique<ServiceDiscovery>(sched_, [this, guard](std::string id, link::LanEndpoint ep) {
            if (*guard) for (auto& [sid, user] : users_) user->endpoint(id, ep);
        });
        refresh();
    }
    ~Runtime() {
        *alive_ = false; refresh_.cancel(); discovery_.reset(); users_.clear();
        sched_.flushPosted(); factory_.stop(); sched_.flushPosted();
    }
private:
    void refresh() {
        std::map<std::string, bool> present;
        std::error_code ec; auto dir = bootDataDir();
        if (!dir.empty()) {
            std::filesystem::directory_iterator files(dir, ec);
            for (const auto& file : files) {
                if (file.path().extension() != L".bin") continue;
                auto record = readBootRecord(file.path()); if (!record) continue;
                if (text::toUtf8(file.path().stem().wstring()) != record->userSid) continue;
                auto sid = record->userSid; present[sid] = true;
                auto it = users_.find(sid);
                if (it == users_.end() && users_.size() >= 128) continue;
                if (it == users_.end()) users_[sid] = std::make_unique<User>(sched_, factory_, std::move(*record));
                else it->second->replace(std::move(*record));
            }
        }
        for (auto& [sid, user] : users_) if (!present.contains(sid)) user->missing();
        std::erase_if(users_, [](const auto& entry) { return entry.second->canRetire(); });
        // Warm the previous Windows user's authenticated link before the tile is
        // selected. This read-only OS hint never authorizes credential release.
        wchar_t sid[256]{}; DWORD size = sizeof(sid);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\LogonUI",
            L"LastLoggedOnUserSID", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, sid, &size);
        auto preferred = users_.find(text::toUtf8(sid));
        if (preferred == users_.end() && users_.size() == 1) preferred = users_.begin();
        if (preferred != users_.end()) preferred->second->warm(WTSGetActiveConsoleSessionId());
        refresh_.start(3000, [this] { refresh(); });
    }
    platform::Win32Scheduler sched_;
    ServiceTransportFactory factory_;
    std::map<std::string, std::unique_ptr<User>> users_;
    std::unique_ptr<ServiceDiscovery> discovery_;
    ScopedTimer refresh_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
};
SERVICE_STATUS_HANDLE statusHandle;
SERVICE_STATUS serviceStatus{};
HANDLE stopEvent;
void setStatus(DWORD state, DWORD error = NO_ERROR) {
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = state;
    serviceStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    serviceStatus.dwWin32ExitCode = error;
    serviceStatus.dwWaitHint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 15000 : 0;
    SetServiceStatus(statusHandle, &serviceStatus);
}
DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) SetEvent(stopEvent);
    return NO_ERROR;
}
void WINAPI serviceMain(DWORD, LPWSTR*) {
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    statusHandle = RegisterServiceCtrlHandlerExW(component::kServiceName, control, nullptr);
    if (!statusHandle) { CloseHandle(stopEvent); return; }
    setStatus(SERVICE_START_PENDING);
    WSADATA wsa{}; DWORD error = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!error) {
        try {
            Runtime runtime;
            setStatus(SERVICE_RUNNING);
            bool running = true;
            while (running) {
                auto wait = MsgWaitForMultipleObjects(1, &stopEvent, FALSE, INFINITE, QS_ALLINPUT);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_FAILED) { error = GetLastError(); break; }
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { running = false; break; }
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
            }
            setStatus(SERVICE_STOP_PENDING);
        } catch (...) { error = ERROR_SERVICE_SPECIFIC_ERROR; }
        WSACleanup();
    }
    setStatus(SERVICE_STOPPED, error); CloseHandle(stopEvent);
}
#endif
}
}
#ifndef MD_UNLOCK_SERVICE_TESTING
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(md::unlock::component::kServiceName), md::unlock::serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
#endif
