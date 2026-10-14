#include "AudioActivityMonitor.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <unknwn.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <algorithm>

namespace md::platform {

using winrt::com_ptr;

namespace {

constexpr int64_t kSampleMs = 250;
constexpr int64_t kGapResetMs = 1000;

const char* kCommApps[] = {"teams.exe",    "ms-teams.exe", "zoom.exe",       "discord.exe", "wechat.exe",
                           "weixin.exe",   "qq.exe",       "skype.exe",      "slack.exe",   "webexmta.exe",
                           "ciscowebexstart.exe", "feishu.exe", "lark.exe",  "dingtalk.exe", "wemeetapp.exe",
                           "telegram.exe", "whatsapp.exe", "signal.exe",     "facetime.exe", "voov.exe"};

std::string processName(DWORD pid) {
    if (pid == 0) return "system";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "pid" + std::to_string(pid);
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        std::wstring full(buf, n);
        auto slash = full.find_last_of(L"\\/");
        name = text::toLowerAscii(text::toUtf8(slash == std::wstring::npos ? full : full.substr(slash + 1)));
    }
    CloseHandle(h);
    return name.empty() ? "pid" + std::to_string(pid) : name;
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct AudioActivityMonitor::Impl {
    struct Session {
        com_ptr<IAudioSessionControl2> ctl;
        com_ptr<IAudioMeterInformation> meter;
        com_ptr<ISimpleAudioVolume> volume;
        com_ptr<IAudioSessionEvents> events;
        DWORD pid = 0;
        std::string proc;
        bool system = false;
        bool capture = false;
        AudioSessionState state = AudioSessionStateInactive;
        std::wstring endpoint;
        int64_t audibleSince = -1;
        int64_t lastAudible = -1;
    };
    struct Endpoint {
        com_ptr<IAudioSessionManager2> mgr;
        com_ptr<IAudioSessionNotification> notifier;
        com_ptr<IAudioEndpointVolume> volume;
        bool capture = false;
    };

    IScheduler& sched;
    Fn onChange;
    AudioActivityMonitor* owner;
    AudioActivityConfig cfg;
    com_ptr<IMMDeviceEnumerator> enumerator;
    com_ptr<IMMNotificationClient> deviceClient;
    std::map<std::wstring, Endpoint> endpoints;
    std::map<std::wstring, Session> sessions; // key: session instance id
    ScopedTimer sampleTimer;
    ScopedTimer rescanTimer;
    std::set<std::string> lastApps;
    int64_t lastStreamStart = -1;

    void noteStreamStart(const Session& s) {
        if (s.pid == GetCurrentProcessId()) return;
        if (std::find(cfg.ignorePids.begin(), cfg.ignorePids.end(), s.pid) != cfg.ignorePids.end()) return;
        lastStreamStart = sched.nowMs();
    }
    bool lastComm = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::function<void(const std::string&)> onAppVolume;

    Impl(IScheduler& s, Fn fn, AudioActivityMonitor* o) : sched(s), onChange(std::move(fn)), owner(o), sampleTimer(&s), rescanTimer(&s) {}

    // ---- COM callback objects (post everything to the scheduler) ----
    struct SessionEvents : winrt::implements<SessionEvents, IAudioSessionEvents> {
        Impl* impl;
        std::weak_ptr<bool> alive;
        std::wstring key;
        SessionEvents(Impl* i, std::wstring k) : impl(i), alive(i->alive), key(std::move(k)) {}
        HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override {
            auto i = impl;
            auto a = alive;
            auto k = key;
            i->sched.post([i, a, k] {
                if (a.expired()) return;
                auto it = i->sessions.find(k);
                if (it != i->sessions.end() && i->onAppVolume) i->onAppVolume(it->second.proc);
            });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState st) override {
            auto i = impl;
            auto a = alive;
            auto k = key;
            i->sched.post([i, a, k, st] {
                if (!a.expired()) i->onState(k, st);
            });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
            auto i = impl;
            auto a = alive;
            auto k = key;
            i->sched.post([i, a, k] {
                if (!a.expired()) i->removeSession(k);
            });
            return S_OK;
        }
    };

    struct SessionNotifier : winrt::implements<SessionNotifier, IAudioSessionNotification> {
        Impl* impl;
        std::weak_ptr<bool> alive;
        std::wstring endpoint;
        bool capture;
        SessionNotifier(Impl* i, std::wstring ep, bool cap) : impl(i), alive(i->alive), endpoint(std::move(ep)), capture(cap) {}
        HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* ctl) override {
            com_ptr<IAudioSessionControl> c;
            c.copy_from(ctl);
            auto i = impl;
            auto a = alive;
            auto ep = endpoint;
            bool cap = capture;
            i->sched.post([i, a, c, ep, cap] {
                if (!a.expired()) i->addSession(c, ep, cap);
            });
            return S_OK;
        }
    };

    struct DeviceClient : winrt::implements<DeviceClient, IMMNotificationClient> {
        Impl* impl;
        std::weak_ptr<bool> alive;
        explicit DeviceClient(Impl* i) : impl(i), alive(i->alive) {}
        void rescan() {
            auto i = impl;
            auto a = alive;
            i->sched.post([i, a] {
                if (!a.expired()) i->rescanTimer.start(500, [i] { i->scanEndpoints(); });
            });
        }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
            rescan();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
            rescan();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
            rescan();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    };

    bool start() {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())))) {
            log::error("audio", "MMDeviceEnumerator unavailable");
            return false;
        }
        deviceClient = winrt::make<DeviceClient>(this);
        enumerator->RegisterEndpointNotificationCallback(deviceClient.get());
        scanEndpoints();
        return true;
    }

    void shutdown() {
        *alive = false;
        sampleTimer.cancel();
        rescanTimer.cancel();
        if (enumerator && deviceClient) enumerator->UnregisterEndpointNotificationCallback(deviceClient.get());
        for (auto& [k, s] : sessions)
            if (s.ctl && s.events) s.ctl->UnregisterAudioSessionNotification(s.events.get());
        for (auto& [id, ep] : endpoints)
            if (ep.mgr && ep.notifier) ep.mgr->UnregisterSessionNotification(ep.notifier.get());
        sessions.clear();
        endpoints.clear();
    }

    void scanEndpoints() {
        std::set<std::wstring> seen;
        for (EDataFlow flow : {eRender, eCapture}) {
            com_ptr<IMMDeviceCollection> coll;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.put()))) continue;
            UINT n = 0;
            coll->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                com_ptr<IMMDevice> dev;
                if (FAILED(coll->Item(i, dev.put()))) continue;
                LPWSTR idp = nullptr;
                if (FAILED(dev->GetId(&idp))) continue;
                std::wstring id(idp);
                CoTaskMemFree(idp);
                seen.insert(id);
                if (endpoints.count(id)) continue;
                Endpoint ep;
                ep.capture = flow == eCapture;
                if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, ep.mgr.put_void())))
                    continue;
                dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, ep.volume.put_void());
                ep.notifier = winrt::make<SessionNotifier>(this, id, ep.capture);
                // Enumerating once is required before session notifications start flowing.
                com_ptr<IAudioSessionEnumerator> en;
                if (SUCCEEDED(ep.mgr->GetSessionEnumerator(en.put()))) {
                    int count = 0;
                    en->GetCount(&count);
                    for (int k = 0; k < count; ++k) {
                        com_ptr<IAudioSessionControl> c;
                        if (SUCCEEDED(en->GetSession(k, c.put()))) addSession(c, id, ep.capture);
                    }
                }
                ep.mgr->RegisterSessionNotification(ep.notifier.get());
                endpoints[id] = std::move(ep);
            }
        }
        for (auto it = endpoints.begin(); it != endpoints.end();) {
            if (!seen.count(it->first)) {
                if (it->second.mgr && it->second.notifier) it->second.mgr->UnregisterSessionNotification(it->second.notifier.get());
                for (auto s = sessions.begin(); s != sessions.end();) {
                    if (s->second.endpoint == it->first) {
                        if (s->second.events) s->second.ctl->UnregisterAudioSessionNotification(s->second.events.get());
                        s = sessions.erase(s);
                    } else {
                        ++s;
                    }
                }
                it = endpoints.erase(it);
            } else {
                ++it;
            }
        }
        evaluate();
    }

    void addSession(const com_ptr<IAudioSessionControl>& c, const std::wstring& endpoint, bool capture) {
        auto c2 = c.try_as<IAudioSessionControl2>();
        if (!c2) return;
        LPWSTR inst = nullptr;
        if (FAILED(c2->GetSessionInstanceIdentifier(&inst))) return;
        std::wstring key(inst);
        CoTaskMemFree(inst);
        if (sessions.count(key)) return;
        Session s;
        s.ctl = c2;
        s.capture = capture;
        s.endpoint = endpoint;
        c2->GetProcessId(&s.pid);
        s.system = c2->IsSystemSoundsSession() == S_OK;
        s.proc = processName(s.pid);
        if (!capture) {
            s.meter = c.try_as<IAudioMeterInformation>();
            s.volume = c.try_as<ISimpleAudioVolume>();
        }
        c->GetState(&s.state);
        if (s.state == AudioSessionStateActive) noteStreamStart(s);
        s.events = winrt::make<SessionEvents>(this, key);
        c->RegisterAudioSessionNotification(s.events.get());
        sessions[key] = std::move(s);
        evaluate();
    }

    void removeSession(const std::wstring& key) {
        auto it = sessions.find(key);
        if (it == sessions.end()) return;
        if (it->second.events) it->second.ctl->UnregisterAudioSessionNotification(it->second.events.get());
        sessions.erase(it);
        evaluate();
    }

    void onState(const std::wstring& key, AudioSessionState st) {
        if (st == AudioSessionStateExpired) return removeSession(key);
        auto it = sessions.find(key);
        if (it == sessions.end()) return;
        if (st == AudioSessionStateActive && it->second.state != AudioSessionStateActive) noteStreamStart(it->second);
        it->second.state = st;
        evaluate();
    }

    bool eligible(const Session& s) const {
        if (s.capture || s.system || s.pid == GetCurrentProcessId()) return false;
        if (std::find(cfg.ignorePids.begin(), cfg.ignorePids.end(), s.pid) != cfg.ignorePids.end()) return false;
        if (std::find(cfg.blacklist.begin(), cfg.blacklist.end(), s.proc) != cfg.blacklist.end()) return false;
        if (!cfg.whitelist.empty() && std::find(cfg.whitelist.begin(), cfg.whitelist.end(), s.proc) == cfg.whitelist.end())
            return false;
        return true;
    }

    bool endpointMuted(const std::wstring& ep) const {
        auto it = endpoints.find(ep);
        if (it == endpoints.end() || !it->second.volume) return false;
        BOOL mute = FALSE;
        it->second.volume->GetMute(&mute);
        float level = 1.0f;
        it->second.volume->GetMasterVolumeLevelScalar(&level);
        return mute || level < 0.01f;
    }

    // Samples peaks (only while needed) and recomputes the aggregate state.
    void evaluate() {
        int64_t now = sched.nowMs();
        bool anyActiveStream = false;
        std::set<std::string> apps;
        std::set<DWORD> capturing;
        for (auto& [k, s] : sessions)
            if (s.capture && s.state == AudioSessionStateActive) capturing.insert(s.pid);
        for (auto& [k, s] : sessions) {
            if (!eligible(s)) continue;
            bool streamActive = s.state == AudioSessionStateActive;
            anyActiveStream |= streamActive;
            float peak = 0.f;
            if (streamActive && s.meter) {
                s.meter->GetPeakValue(&peak);
                BOOL mute = FALSE;
                float vol = 1.f;
                if (s.volume) {
                    s.volume->GetMute(&mute);
                    s.volume->GetMasterVolume(&vol);
                }
                if (mute || vol < 0.01f || endpointMuted(s.endpoint)) peak = 0.f;
            }
            bool audible = peak >= cfg.sensitivity;
            if (audible) {
                if (s.audibleSince < 0 || now - s.lastAudible > kGapResetMs) s.audibleSince = now;
                s.lastAudible = now;
            }
            bool active = s.audibleSince >= 0 && s.lastAudible - s.audibleSince >= cfg.activationMs &&
                          now - s.lastAudible <= cfg.releaseMs;
            if (!active && s.lastAudible >= 0 && now - s.lastAudible > cfg.releaseMs) s.audibleSince = -1;
            if (active) apps.insert(s.proc);
        }
        bool comm = false;
        for (auto& a : apps) {
            for (auto* c : kCommApps)
                if (a == c) comm = true;
        }
        for (auto& [k, s] : sessions)
            if (!s.capture && eligible(s) && apps.count(s.proc) && capturing.count(s.pid)) comm = true;

        bool needSampling = anyActiveStream || !apps.empty();
        // Sample only while something may be audible; idle cost is zero.
        if (needSampling && !sampleTimer.active()) sampleTimer.start(kSampleMs, [this] { evaluate(); });
        if (apps != lastApps || comm != lastComm) {
            lastApps = apps;
            lastComm = comm;
            owner->active_ = !apps.empty();
            onChange(!apps.empty(), comm, std::vector<std::string>(apps.begin(), apps.end()));
        }
    }

    int activeCount() const {
        int n = 0;
        for (auto& [k, s] : sessions)
            if (!s.capture && s.state == AudioSessionStateActive) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------------------------

int64_t AudioActivityMonitor::lastStreamStartMs() const { return impl_ ? impl_->lastStreamStart : -1; }

AudioActivityMonitor::AudioActivityMonitor(IScheduler& sched, Fn onChange)
    : impl_(std::make_unique<Impl>(sched, std::move(onChange), this)) {}

AudioActivityMonitor::~AudioActivityMonitor() { impl_->shutdown(); }

bool AudioActivityMonitor::start() { return impl_->start(); }

void AudioActivityMonitor::setConfig(AudioActivityConfig cfg) {
    for (auto& s : cfg.blacklist) s = text::toLowerAscii(s);
    for (auto& s : cfg.whitelist) s = text::toLowerAscii(s);
    impl_->cfg = std::move(cfg);
    impl_->evaluate();
}

int AudioActivityMonitor::activeSessions() const { return impl_->activeCount(); }

AudioActivityMonitor::AppVolume AudioActivityMonitor::appVolume(const std::string& process) const {
    AppVolume v;
    for (auto& [k, s] : impl_->sessions) {
        if (s.capture || s.proc != process || !s.volume) continue;
        float level = 1.f;
        BOOL mute = FALSE;
        if (FAILED(s.volume->GetMasterVolume(&level)) || FAILED(s.volume->GetMute(&mute))) continue;
        // Prefer an active session's values; otherwise the first one found.
        if (!v.available || s.state == AudioSessionStateActive) {
            v = {true, level, mute != FALSE};
            if (s.state == AudioSessionStateActive) break;
        }
    }
    return v;
}

bool AudioActivityMonitor::setAppVolume(const std::string& process, std::optional<float> level, std::optional<bool> muted) {
    bool any = false;
    for (auto& [k, s] : impl_->sessions) {
        if (s.capture || s.proc != process || !s.volume) continue;
        if (level) s.volume->SetMasterVolume(std::clamp(*level, 0.f, 1.f), nullptr);
        if (muted) s.volume->SetMute(*muted ? TRUE : FALSE, nullptr);
        any = true;
    }
    return any;
}

void AudioActivityMonitor::setOnAppVolumeChanged(std::function<void(const std::string&)> fn) {
    impl_->onAppVolume = std::move(fn);
}

} // namespace md::platform
