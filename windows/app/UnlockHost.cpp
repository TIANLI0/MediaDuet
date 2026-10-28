#include "UnlockHost.h"
#include "SecureStore.h"
#include "Stores.h"
#include "md/crypto/Crypto.h"
#include "md/unlock/Component.h"
#include "md/unlock/Unlock.h"
#include "md/util/Text.h"
#include "BootStore.h"
#include <algorithm>

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include <wtsapi32.h>

namespace md::app {
using nlohmann::json;
namespace {
void wipe(std::string& s) { SecureZeroMemory(s.data(), s.size()); s.clear(); }
bool systemClient(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE token = nullptr;
    bool ok = false;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        DWORD n = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        Bytes b(n);
        ok = GetTokenInformation(token, TokenUser, b.data(), n, &n) &&
             IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, WinLocalSystemSid);
        CloseHandle(token);
    }
    RevertToSelf();
    return ok;
}
bool verifyPassword(std::string& password, const std::wstring& sidText) {
    PSID sid = nullptr;
    bool ok = false;
    std::wstring pw = text::toWide(password);
    wipe(password);
    if (ConvertStringSidToSidW(sidText.c_str(), &sid)) {
        wchar_t user[256], domain[256];
        DWORD nu = 256, nd = 256;
        SID_NAME_USE use;
        HANDLE token = nullptr;
        if (LookupAccountSidW(nullptr, sid, user, &nu, domain, &nd, &use) &&
            LogonUserW(user, domain, pw.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &token)) {
            DWORD n = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &n);
            Bytes b(n);
            ok = GetTokenInformation(token, TokenUser, b.data(), n, &n) &&
                 EqualSid(sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
            CloseHandle(token);
        }
        LocalFree(sid);
    }
    SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
    return ok;
}
}

UnlockHost::UnlockHost(IScheduler& sched, Callbacks cb)
    : sched_(sched), cb_(std::move(cb)), userSid_(currentUserSid()), setupTimer_(&sched), requestTimer_(&sched), lockTimer_(&sched), bootTimer_(&sched) {
    load();
    if (enrollment_ && unlock::bootEnrollmentRevoked(userSid_)) { enrollment_.reset(); save(); }
    component_ = unlockComponentState(userSid_);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    auto alive = alive_;
    pipe_ = std::make_unique<IpcServer>(sched_, IpcServer::Callbacks{
        {}, [this, alive] { if (*alive) onPipeClosed(); },
        [this, alive](const std::string& line) { if (*alive) { try { onPipeLine(line); } catch (...) {} } }},
        IpcServer::Options{std::wstring(unlock::component::kPipePrefix) + std::to_wstring(session),
                           L"D:P(A;;GA;;;SY)", systemClient});
    pipe_->start();
    refreshBoot();
}

UnlockHost::~UnlockHost() {
    *alive_ = false;
    cancelRequest();
    if (setup_) {
        if (setup_->msgId) cb_.send(proto::types::UnlockCancel, json::object(), setup_->msgId);
        secureZero(setup_->key);
    }
    pipe_->stop();
    for (auto& worker : workers_) if (worker.valid()) worker.wait();
    // UAC can finish while the app is closing. Retain the already-approved
    // enrollment so its boot registration remains manageable on the next launch.
    if (setup_ && setup_->phoneConfirmed) {
        auto status = unlock::bootControl(userSid_, {{"type", "status"}});
        if (status && status->value("enabled", false) && status->value("enrollment", std::string()) == setup_->pending.id) {
            enrollment_ = setup_->pending; save();
        }
    }
}

void UnlockHost::launch(std::function<void()> worker) {
    std::erase_if(workers_, [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    workers_.push_back(std::async(std::launch::async, std::move(worker)));
}
void UnlockHost::refreshBoot() {
    bootTimer_.start(15000, [this] { refreshBoot(); });
    if (enrollment_ && !setup_ && unlock::bootEnrollmentRevoked(userSid_)) {
        cancelRequest(); enrollment_.reset(); bootEnabled_ = false; save(); sendStatus(); cb_.changed();
    }
    if (bootQuery_ || !enrollment_) return;
    bootQuery_ = true; auto guard = alive_;
    auto expected = enrollment_->id;
    launch([this, guard, expected, sid = userSid_] {
        auto response = unlock::bootControl(sid, {{"type", "status"}});
        sched_.post([this, guard, expected, response] {
            if (!*guard) return;
            bootQuery_ = false;
            if (!enrollment_ || enrollment_->id != expected) return;
            bootEnabled_ = response && response->value("ok", false) && response->value("enabled", false) &&
                           response->value("enrollment", std::string()) == expected;
            if (bootEnabled_) {
                bool stale = response->value("stale", false);
                auto last = response->value("lastUnlockAt", int64_t(0));
                if ((stale && !enrollment_->stale) || last > enrollment_->lastUnlockAt) {
                    enrollment_->stale = enrollment_->stale || stale;
                    enrollment_->lastUnlockAt = std::max(enrollment_->lastUnlockAt, last); save(); sendStatus();
                }
            }
            cb_.changed();
        });
    });
}
void UnlockHost::removeBootAccess() {
    auto guard = alive_;
    launch([this, guard, sid = userSid_] {
        auto result = unlock::bootControl(sid, {{"type", "remove"}});
        bool removed = result && result->value("ok", false);
        sched_.post([this, guard, removed] {
            if (!*guard) return;
            bootEnabled_ = false;
            if (!removed && !maintenance_) runElevated(L"remove", [](int) {});
            cb_.changed();
        });
    });
}

void UnlockHost::load() {
    auto j = platform::readJson(platform::appDataDir() / L"unlock.json");
    try {
        if (!j || j->value("sid", std::string()) != text::toUtf8(userSid_)) return;
        Enrollment e;
        e.id = j->at("enrollment").get<std::string>();
        e.peerId = j->at("peerId").get<std::string>();
        e.peerName = j->value("peerName", std::string());
        auto blob = base64Decode(j->at("seal").get<std::string>());
        auto sealed = blob ? platform::unprotect(*blob) : std::nullopt;
        if (!unlock::isEnrollmentId(e.id) || !proto::isValidDeviceId(e.peerId) || !sealed || sealed->size() < 28) return;
        e.sealed = std::move(*sealed);
        e.stale = j->value("stale", false);
        e.lastUnlockAt = j->value("lastUnlockAt", int64_t(0));
        enrollment_ = std::move(e);
    } catch (...) { /* Invalid or another user's DPAPI blob: fail closed. */ }
}

bool UnlockHost::save() const {
    auto path = platform::appDataDir() / L"unlock.json";
    if (!enrollment_) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return !ec;
    }
    auto blob = platform::protect(enrollment_->sealed);
    return blob && platform::writeJson(path, {{"sid", text::toUtf8(userSid_)}, {"enrollment", enrollment_->id},
        {"peerId", enrollment_->peerId}, {"peerName", enrollment_->peerName}, {"seal", base64Encode(*blob)},
        {"stale", enrollment_->stale}, {"lastUnlockAt", enrollment_->lastUnlockAt}});
}

bool UnlockHost::phoneConnected() const {
    return enrollment_ && cb_.peerCanUnlock() && cb_.peerId() == enrollment_->peerId;
}
json UnlockHost::stateJson() const {
    return {{"available", cb_.peerCanUnlock()}, {"enabled", enrollment_.has_value()}, {"bootEnabled", bootEnabled_},
        {"phone", enrollment_ ? json{{"id", enrollment_->peerId}, {"name", enrollment_->peerName}} : json(nullptr)},
        {"phoneConnected", phoneConnected()},
        {"component", component_ == ComponentState::Ok ? "ok" : component_ == ComponentState::Missing ? "missing" : "outdated"},
        {"stale", enrollment_ && enrollment_->stale}, {"setup", setup_ ? json(setup_->step) : json(nullptr)},
        {"lastUnlockAt", enrollment_ && enrollment_->lastUnlockAt ? json(enrollment_->lastUnlockAt) : json(nullptr)}};
}
void UnlockHost::setupStep(const std::string& step) { setup_->step = step; cb_.changed(); }

void UnlockHost::enable(std::string password, Done done) {
    if (setup_ || maintenance_ || wantUnlock_) { wipe(password); return done(false, "busy"); }
    if (!cb_.peerCanUnlock()) { wipe(password); return done(false, "no_phone"); }
    if (password.empty() || password.size() > 2048 || password.find('\0') != std::string::npos || !text::isValidUtf8(password)) {
        wipe(password); return done(false, "wrong_password");
    }
    setup_.emplace();
    setup_->done = std::move(done);
    setup_->pending.id = crypto::randomHex(16);
    setup_->pending.peerId = cb_.peerId();
    setup_->pending.peerName = cb_.peerName();
    setup_->key = crypto::randomBytes(unlock::kKeyBytes);
    setup_->pending.sealed = unlock::seal(setup_->key, setup_->pending.id, text::toUtf8(userSid_), password);
    setupStep("verifying");
    auto alive = alive_;
    auto enrollmentId = setup_->pending.id;
    launch([this, alive, enrollmentId, sid = userSid_, password = std::move(password)]() mutable {
        bool ok = verifyPassword(password, sid);
        sched_.post([this, alive, enrollmentId, ok] {
            if (!*alive || !setup_ || setup_->pending.id != enrollmentId) return;
            if (!ok) return finishSetup(false, "wrong_password");
            enrollWithPhone();
        });
    });
}

void UnlockHost::runElevated(const std::wstring& action, std::function<void(int)> done) {
    std::optional<std::filesystem::path> payload;
    if (action == L"install") {
        const auto* source = setup_ ? &setup_->pending : enrollment_ ? &*enrollment_ : nullptr;
        if (source) {
            auto record = cb_.bootRecord(source->peerId);
            if (!record) return done(-2);
            record->userSid = text::toUtf8(userSid_); record->enrollment = source->id; record->sealed = source->sealed;
            record->language = cb_.language(); record->stale = source->stale; record->lastUnlockAt = source->lastUnlockAt;
            payload = unlock::stageBootRecord(*record, platform::appDataDir());
            secureZero(record->peer.key);
            if (!payload) return done(-2);
        }
    }
    maintenance_ = true;
    auto alive = alive_;
    launch([this, alive, action, payload, done = std::move(done), sid = userSid_] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::wstring args = L"--unlock-component " + action + L" " + sid;
        if (payload) args += L" \"" + payload->wstring() + L"\"";
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas"; info.lpFile = exe; info.lpParameters = args.c_str(); info.nShow = SW_HIDE;
        int rc = -2;
        if (ShellExecuteExW(&info)) {
            WaitForSingleObject(info.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
            rc = static_cast<int>(code);
        } else if (GetLastError() == ERROR_CANCELLED) rc = -1;
        if (SUCCEEDED(com)) CoUninitialize();
        if (payload) DeleteFileW(payload->c_str());
        sched_.post([this, alive, rc, done] {
            if (!*alive) return;
            maintenance_ = false;
            component_ = unlockComponentState(userSid_);
            done(rc); refreshBoot(); cb_.changed(); sendStatus();
        });
    });
}
void UnlockHost::commitSetup() {
    setupTimer_.cancel(); setupStep("installing");
    auto id = setup_->pending.id;
    runElevated(L"install", [this, id](int rc) {
        if (!setup_ || setup_->pending.id != id) { removeBootAccess(); return; }
        if (rc) return finishSetup(false, rc == -1 ? "elevation_cancelled" : "component_failed");
        auto old = std::move(enrollment_); enrollment_ = setup_->pending;
        if (!save()) {
            enrollment_ = std::move(old);
            auto sid = userSid_; launch([sid] { unlock::bootControl(sid, {{"type", "remove"}}); });
            finishSetup(false, "component_failed");
        } else {
            bootEnabled_ = true;
            if (old && old->peerId == cb_.peerId()) cb_.send(proto::types::UnlockRevoke, unlock::revokeBody(old->id), {});
            finishSetup(true, "");
        }
    });
}

void UnlockHost::enrollWithPhone() {
    if (!cb_.peerCanUnlock() || cb_.peerId() != setup_->pending.peerId) return finishSetup(false, "no_phone");
    setupStep("waiting_phone");
    setup_->msgId = cb_.send(proto::types::UnlockEnroll,
        unlock::enrollBody(setup_->pending.id, setup_->key, cb_.pcName()), {});
    secureZero(setup_->key);
    if (!setup_->msgId) return finishSetup(false, "phone_failed");
    setupTimer_.start(unlock::kEnrollWaitMs, [this] { finishSetup(false, "phone_timeout"); });
}
void UnlockHost::finishSetup(bool ok, const std::string& error) {
    if (!setup_) return;
    auto s = std::move(*setup_);
    setup_.reset(); setupTimer_.cancel(); secureZero(s.key);
    if (!ok && s.msgId && cb_.peerId() == s.pending.peerId) {
        cb_.send(proto::types::UnlockCancel, json::object(), s.msgId);
        cb_.send(proto::types::UnlockRevoke, unlock::revokeBody(s.pending.id), {});
    }
    if (!ok && enrollment_ && enrollment_->peerId == s.pending.peerId && s.msgId &&
        (s.phoneConfirmed || error == "phone_failed" || error == "phone_timeout")) {
        // The phone may have replaced its old key before the acknowledgement was lost.
        enrollment_->stale = true; save();
    }
    s.done(ok, error); cb_.changed(); sendStatus();
}
void UnlockHost::disable(Done done) {
    if (setup_ || maintenance_) return done(false, "busy");
    cancelRequest();
    maintenance_ = true;
    auto guard = alive_;
    launch([this, guard, done, sid = userSid_] {
        auto response = unlock::bootControl(sid, {{"type", "remove"}});
        bool removed = response && response->value("ok", false) && !response->value("enabled", true);
        sched_.post([this, guard, done, removed] {
            if (!*guard) return;
            maintenance_ = false;
            auto clear = [this, done](bool ok, const std::string& error) {
                if (!ok) return done(false, error);
                if (phoneConnected()) cb_.send(proto::types::UnlockRevoke, unlock::revokeBody(enrollment_->id), {});
                auto old = std::move(enrollment_); enrollment_.reset();
                if (!save()) { enrollment_ = std::move(old); return done(false, "component_failed"); }
                bootEnabled_ = false; cb_.changed(); sendStatus(); done(true, "");
            };
            if (removed) {
                clear(true, "");
                runElevated(L"remove", [](int) {}); // cancelling this cannot restore boot enrollment
            } else runElevated(L"remove", [clear](int rc) { clear(rc == 0, rc == -1 ? "elevation_cancelled" : "component_failed"); });
        });
    });
}
void UnlockHost::repair(Done done) {
    if (setup_ || maintenance_) return done(false, "busy");
    if (unlock::bootEnrollmentRevoked(userSid_)) {
        enrollment_.reset(); save(); bootEnabled_ = false; cb_.changed();
        return done(false, "phone_failed");
    }
    runElevated(L"install", [done](int rc) { done(rc == 0, rc == -1 ? "elevation_cancelled" : rc ? "component_failed" : ""); });
}
void UnlockHost::onLinkUp() { sendStatus(); if (wantUnlock_ && !requestId_) sendRequest(); cb_.changed(); }
void UnlockHost::onLinkDown() {
    if (setup_ && setup_->step == "waiting_phone") finishSetup(false, "phone_failed");
    if (requestId_) { requestTimer_.cancel(); requestId_ = 0; }
    sendStatus(); cb_.changed();
}
void UnlockHost::onPeerRemoved(const std::string& id) {
    if (setup_ && setup_->pending.peerId == id) finishSetup(false, "phone_failed");
    if (enrollment_ && enrollment_->peerId == id) { removeBootAccess(); cancelRequest(); enrollment_.reset(); save(); sendStatus(); cb_.changed(); }
}
bool UnlockHost::onEnvelope(const proto::Envelope& e) {
    if (e.type == proto::types::UnlockRevoke) {
        auto id = unlock::parseRevoke(e.body);
        if (id && enrollment_ && e.src == enrollment_->peerId && *id == enrollment_->id) {
            removeBootAccess(); cancelRequest(); enrollment_.reset(); save(); sendStatus(); cb_.changed();
        }
        return true;
    }
    if (e.type != proto::types::UnlockResult) return e.type.starts_with("unlock.");
    if (setup_ && setup_->step == "waiting_phone" && setup_->msgId && e.re == setup_->msgId && e.src == setup_->pending.peerId) {
        auto r = unlock::parseResult(e.body);
        if (!r || !r->ok) return finishSetup(false, r ? unlock::enableError(r->error) : "phone_failed"), true;
        secureZero(r->key);
        setup_->phoneConfirmed = true;
        commitSetup();
    } else onUnlockResult(e);
    return true;
}

bool UnlockHost::sessionLocked() {
    DWORD id = 0, size = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &id)) return false;
    LPWSTR data = nullptr;
    bool locked = false;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSSessionInfoEx, &data, &size)) {
        if (size >= sizeof(WTSINFOEXW)) {
            auto info = reinterpret_cast<WTSINFOEXW*>(data);
            locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        }
        WTSFreeMemory(data);
    }
    return locked;
}
void UnlockHost::onPipeLine(const std::string& line) {
    auto j = json::parse(line, nullptr, false);
    if (!j.is_object()) return;
    auto t = j.find("type");
    if (t == j.end() || !t->is_string()) return;
    if (*t == "hello") {
        cancelRequest();
        cpHello_ = j.value("v", 0) == 1 && j.value("sid", std::string()) == text::toUtf8(userSid_);
        lastStatus_.clear(); sendStatus(); return;
    }
    if (!cpHello_) return;
    if (*t == "request") requestUnlock();
    else if (*t == "cancel") { cancelRequest(); outcome_.clear(); sendStatus(); }
    else if (*t == "result" && enrollment_ && outcome_ == "unlocking") {
        requestTimer_.cancel(); lockTimer_.cancel();
        const bool ok = j.value("ok", false);
        const uint32_t status = static_cast<uint32_t>(j.value("status", int64_t(0)));
        if (!ok && (status == 0xc000006d || status == 0xc000006a)) enrollment_->stale = true;
        if (ok) enrollment_->lastUnlockAt = sched_.wallMs();
        outcome_.clear(); save(); sendStatus(); cb_.changed();
    }
}
void UnlockHost::onPipeClosed() {
    cpHello_ = false;
    // A successful LogonUI may close its pipe before WTS reports the session unlock.
    if (outcome_ != "unlocking") { cancelRequest(); outcome_.clear(); }
    lastStatus_.clear();
}
void UnlockHost::requestUnlock() {
    cancelRequest(); outcome_.clear(); outcomeCode_.clear();
    if (!enrollment_ || enrollment_->stale || setup_ || maintenance_ || !sessionLocked()) { sendStatus(); return; }
    wantUnlock_ = true;
    watchLock();
    sendRequest();
}
void UnlockHost::sendRequest() {
    if (!wantUnlock_ || !cpHello_ || !sessionLocked()) { cancelRequest(); sendStatus(); return; }
    if (phoneConnected()) requestId_ = cb_.send(proto::types::UnlockRequest,
        unlock::requestBody(enrollment_->id, cb_.pcName()), {});
    requestTimer_.start(unlock::kRequestWaitMs, [this] {
        cancelRequest(); outcome_ = "failed"; outcomeCode_ = "timeout"; sendStatus();
    });
    sendStatus();
}
void UnlockHost::cancelRequest() {
    if (requestId_ && phoneConnected()) cb_.send(proto::types::UnlockCancel, json::object(), requestId_);
    requestId_ = 0; wantUnlock_ = false; requestTimer_.cancel(); lockTimer_.cancel();
}
void UnlockHost::watchLock() {
    lockTimer_.start(500, [this] {
        if (!sessionLocked()) {
            // LogonUI can exit on success without calling ReportResult.
            if (outcome_ == "unlocking" && enrollment_) {
                enrollment_->lastUnlockAt = sched_.wallMs(); save(); cb_.changed();
            }
            cancelRequest(); outcome_.clear(); sendStatus();
        } else if (wantUnlock_ || outcome_ == "unlocking") watchLock();
    });
}
void UnlockHost::onUnlockResult(const proto::Envelope& e) {
    if (!enrollment_ || !requestId_ || e.re != requestId_ || e.src != enrollment_->peerId) return;
    requestId_ = 0; wantUnlock_ = false; requestTimer_.cancel(); lockTimer_.cancel();
    auto r = unlock::parseResult(e.body);
    outcome_ = "failed"; outcomeCode_ = r ? r->error : "bad_message";
    if (r && !r->ok && r->error == "denied") outcome_ = "denied";
    if (r && (r->error == "not_enrolled" || r->error == "key_invalidated")) { enrollment_->stale = true; save(); }
    if (r && r->ok && cpHello_ && sessionLocked()) {
        auto pw = unlock::open(r->key, enrollment_->id, text::toUtf8(userSid_), enrollment_->sealed);
        if (pw) {
            outcome_ = "unlocking"; sendStatus();
            json credential{{"type", "credential"}, {"password", *pw}};
            pipe_->send(credential.dump()); wipe(*pw);
            auto& s = credential["password"].get_ref<std::string&>(); wipe(s);
            watchLock();
            requestTimer_.start(15000, [this] {
                lockTimer_.cancel(); outcome_ = "failed"; outcomeCode_ = "timeout"; sendStatus();
            });
        } else { enrollment_->stale = true; save(); }
    }
    if (r) secureZero(r->key);
    sendStatus(); cb_.changed();
}
std::string UnlockHost::pipeState() const {
    if (!cpHello_) return "other_user";
    if (!enrollment_) return "not_enrolled";
    if (enrollment_->stale) return "stale";
    if (!outcome_.empty()) return outcome_;
    if (!phoneConnected()) return "offline";
    return requestId_ ? "waiting" : "ready";
}
void UnlockHost::sendStatus() {
    if (!pipe_ || !pipe_->connected()) return;
    json j{{"type", "status"}, {"state", pipeState()}, {"lang", cb_.language()}, {"code", outcomeCode_}};
    if (enrollment_) j["phone"] = enrollment_->peerName;
    std::string s = j.dump();
    if (s != lastStatus_) { lastStatus_ = s; pipe_->send(std::move(s)); }
}
} // namespace md::app
