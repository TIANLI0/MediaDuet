#include "pch.h"
#include "Model.h"

#include "LyricTiming.h"

#include <algorithm>
#include <cmath>

namespace mdui
{
    std::string JStr(json const& j, char const* key, std::string const& def)
    {
        if (!j.is_object()) return def;
        auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return def;
        return it->get<std::string>();
    }

    int64_t JInt(json const& j, char const* key, int64_t def)
    {
        if (!j.is_object()) return def;
        auto it = j.find(key);
        if (it == j.end()) return def;
        if (it->is_number_integer()) return it->get<int64_t>();
        if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
        if (it->is_number_float())
        {
            double d = it->get<double>();
            return std::isfinite(d) ? static_cast<int64_t>(std::llround(d)) : def;
        }
        return def;
    }

    double JDbl(json const& j, char const* key, double def)
    {
        if (!j.is_object()) return def;
        auto it = j.find(key);
        if (it == j.end() || !it->is_number()) return def;
        double d = it->get<double>();
        return std::isfinite(d) ? d : def;
    }

    bool JBool(json const& j, char const* key, bool def)
    {
        if (!j.is_object()) return def;
        auto it = j.find(key);
        if (it == j.end() || !it->is_boolean()) return def;
        return it->get<bool>();
    }

    json const& JObj(json const& j, char const* key)
    {
        static json const empty = json::object();
        if (!j.is_object()) return empty;
        auto it = j.find(key);
        if (it == j.end() || !it->is_object()) return empty;
        return *it;
    }

    json const& JArr(json const& j, char const* key)
    {
        static json const empty = json::array();
        if (!j.is_object()) return empty;
        auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return empty;
        return *it;
    }

    json const* JPath(json const& j, std::string_view path)
    {
        json const* cur = &j;
        while (!path.empty())
        {
            auto dot = path.find('.');
            auto part = path.substr(0, dot);
            if (!cur->is_object()) return nullptr;
            auto it = cur->find(std::string(part));
            if (it == cur->end()) return nullptr;
            cur = &*it;
            if (dot == std::string_view::npos) break;
            path.remove_prefix(dot + 1);
        }
        return cur;
    }

    json MakePatch(std::string_view path, json value)
    {
        std::vector<std::string> parts;
        while (true)
        {
            auto dot = path.find('.');
            parts.emplace_back(path.substr(0, dot));
            if (dot == std::string_view::npos) break;
            path.remove_prefix(dot + 1);
        }
        json result = std::move(value);
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            json wrapper = json::object();
            wrapper[*it] = std::move(result);
            result = std::move(wrapper);
        }
        return result;
    }

    void DeepMerge(json& target, json const& patch)
    {
        if (!patch.is_object() || !target.is_object())
        {
            target = patch;
            return;
        }
        for (auto it = patch.begin(); it != patch.end(); ++it)
        {
            if (it->is_null())
            {
                // RFC 7396 semantics: null removes the key (the host then falls back to its default).
                target.erase(it.key());
                continue;
            }
            auto existing = target.find(it.key());
            if (existing != target.end() && existing->is_object() && it->is_object())
                DeepMerge(*existing, *it);
            else
                target[it.key()] = *it;
        }
    }

    std::wstring Widen(std::string_view utf8)
    {
        if (utf8.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
        return out;
    }

    std::string Narrow(std::wstring_view wide)
    {
        if (wide.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    namespace
    {
        bool g_frozen = false;
        double g_virtualQpc = 0;
        double g_qpcBase = 0;
        uint64_t g_tickBase = 0;

        double RealQpcMs()
        {
            static const double freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return static_cast<double>(c.QuadPart) * 1000.0 / freq;
        }

        // The host's clock (platform/Scheduler.cpp): interrupt time, GetTickCount64's time base at
        // sub-millisecond precision.
        uint64_t RealTick()
        {
            using Fn = VOID(WINAPI*)(PULONGLONG);
            static const Fn precise = [] {
                HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
                return kb ? reinterpret_cast<Fn>(GetProcAddress(kb, "QueryInterruptTimePrecise")) : nullptr;
            }();
            if (!precise) return GetTickCount64();
            ULONGLONG t = 0;
            precise(&t);
            return t / 10'000;
        }
    }

    namespace testclock
    {
        void Freeze()
        {
            g_qpcBase = g_virtualQpc = RealQpcMs();
            g_tickBase = RealTick();
            g_frozen = true;
        }
        void Advance(double ms) { g_virtualQpc += ms; }
        bool Frozen() { return g_frozen; }
    }

    uint64_t TickNow()
    {
        if (!g_frozen) return RealTick();
        return g_tickBase + static_cast<uint64_t>(std::llround(std::max(0.0, g_virtualQpc - g_qpcBase)));
    }

    double QpcNowMs()
    {
        if (g_frozen) return g_virtualQpc;
        return RealQpcMs();
    }

    // ---- Session -----------------------------------------------------------------------------

    bool Session::Has(std::string_view action) const
    {
        return std::find(actions.begin(), actions.end(), action) != actions.end();
    }

    int64_t Session::PositionAt(uint64_t nowTick) const
    {
        if (positionMs < 0) return -1;
        double pos = static_cast<double>(positionMs);
        if (status == "playing" && nowTick > atTick)
            pos += static_cast<double>(nowTick - atTick) * rate;
        if (pos < 0) pos = 0;
        if (durationMs > 0 && pos > static_cast<double>(durationMs)) pos = static_cast<double>(durationMs);
        return static_cast<int64_t>(pos);
    }

    double Session::PositionPrecise(double qpcNowMs) const
    {
        if (positionMs < 0) return -1;
        double pos = static_cast<double>(positionMs);
        if (status == "playing" && qpcNowMs > anchorQpcMs) pos += (qpcNowMs - anchorQpcMs) * rate;
        if (pos < 0) pos = 0;
        if (durationMs > 0 && pos > static_cast<double>(durationMs)) pos = static_cast<double>(durationMs);
        return pos;
    }

    void Session::Rebase(int64_t newPositionMs)
    {
        positionMs = newPositionMs;
        atTick = TickNow();
        anchorQpcMs = QpcNowMs();
    }

    static Session ParseSession(json const& j, Side side, uint64_t receivedTick)
    {
        Session s;
        s.side = side;
        s.key = JStr(j, "key");
        auto const& app = JObj(j, "app");
        s.appId = JStr(app, "id");
        s.appName = JStr(app, "name");
        s.title = JStr(j, "title");
        s.artist = JStr(j, "artist");
        s.album = JStr(j, "album");
        s.trackId = JStr(j, "trackId");
        s.durationMs = JInt(j, "durationMs", -1);
        s.positionMs = JInt(j, "positionMs", -1);
        int64_t at = JInt(j, "atTick", 0);
        s.atTick = at > 0 ? static_cast<uint64_t>(at) : receivedTick;
        // anchor = qpcNow - (GetTickCount64() - atTick): maps the host's tick to our precise clock.
        s.anchorQpcMs = QpcNowMs() - static_cast<double>(static_cast<int64_t>(TickNow() - s.atTick));
        s.rate = JDbl(j, "rate", 1.0);
        if (s.rate <= 0 || s.rate > 16) s.rate = 1.0;
        s.status = JStr(j, "status", "unknown");
        for (auto const& a : JArr(j, "actions"))
            if (a.is_string()) s.actions.push_back(a.get<std::string>());
        s.artworkPath = JStr(j, "artworkPath");
        s.kind = JStr(j, "kind");
        s.policyState = JStr(j, "policyState");
        s.source = JStr(j, "source");
        s.positionEstimated = JBool(j, "positionEstimated", false);
        auto const& av = JObj(j, "appVolume");
        s.appVolume.available = JBool(av, "available", false);
        s.appVolume.level = std::clamp(JDbl(av, "level", 0.0), 0.0, 1.0);
        s.appVolume.muted = JBool(av, "muted", false);
        s.appVolume.error = JStr(av, "error");
        auto delay = j.find("lyricsDelayMs");
        s.hasLyricsDelay = delay != j.end() && delay->is_number();
        s.lyricsDelayMs = std::clamp<int64_t>(JInt(j, "lyricsDelayMs", 0), -10000, 10000);
        s.outputLatencyMs = std::clamp<int64_t>(JInt(j, "outputLatencyMs", 0), 0, 10000);
        for (auto const& a : JArr(j, "customActions"))
        {
            if (!a.is_object()) continue;
            CustomAction ca{ JStr(a, "id"), JStr(a, "title"), JStr(a, "iconPath") };
            if (!ca.id.empty()) s.customActions.push_back(std::move(ca));
        }
        s.queueCount = static_cast<int>(std::clamp<int64_t>(JInt(JObj(j, "queue"), "count", 0), 0, 100000));
        return s;
    }

    Queue ParseQueue(json const& j)
    {
        Queue q;
        q.key = JStr(j, "key");
        q.title = JStr(j, "title");
        auto a = j.find("activeId");
        q.hasActive = a != j.end() && a->is_number_integer();
        q.activeId = q.hasActive ? a->get<int64_t>() : -1;
        q.total = static_cast<int>(JInt(j, "total", 0));
        q.offset = static_cast<int>(JInt(j, "offset", 0));
        for (auto const& i : JArr(j, "items"))
            if (i.is_object()) q.items.push_back({ JInt(i, "id", 0), JStr(i, "title"), JStr(i, "subtitle") });
        return q;
    }

    static SideState ParseSide(json const& j, Side side, uint64_t receivedTick)
    {
        SideState st;
        st.gen = JInt(j, "gen", 0);
        st.active = JStr(j, "active");
        for (auto const& s : JArr(j, "sessions"))
            if (s.is_object()) st.sessions.push_back(ParseSession(s, side, receivedTick));
        return st;
    }

    static AudioEndpoint ParseAudio(json const& j)
    {
        AudioEndpoint a;
        a.available = JBool(j, "available", false);
        a.state = JStr(j, "state", "off");
        a.device = JStr(j, "device");
        a.error = JStr(j, "error");
        a.errorCode = JStr(j, "errorCode");
        return a;
    }

    State ParseState(json const& j, uint64_t receivedTick)
    {
        State s;
        s.valid = true;
        s.tick = static_cast<uint64_t>(JInt(j, "tick", 0));
        auto const& self = JObj(j, "self");
        s.self.id = JStr(self, "id");
        s.self.name = JStr(self, "name");
        s.self.app = JStr(self, "app");
        s.standalone = JBool(j, "standalone", false);

        auto const& c = JObj(j, "connection");
        s.connection.status = JStr(c, "status", "no_peer");
        s.connection.transport = JStr(c, "transport");
        s.connection.secondary = JStr(c, "secondary");
        auto const& peer = JObj(c, "peer");
        s.connection.peer.id = JStr(peer, "id");
        s.connection.peer.name = JStr(peer, "name");
        s.connection.peer.platform = JStr(peer, "platform");
        s.connection.peer.app = JStr(peer, "app");
        s.connection.peer.versionMatch = JStr(peer, "versionMatch", "unknown");
        s.connection.sinceTick = static_cast<uint64_t>(JInt(c, "sinceTick", 0));
        s.connection.rttMs = JInt(c, "rttMs", -1);
        s.connection.mode = JStr(c, "mode");
        s.connection.lastError = JStr(c, "lastError");
        s.connection.bytesIn = JInt(c, "bytesIn", 0);
        s.connection.bytesOut = JInt(c, "bytesOut", 0);
        if (auto bt = c.find("bluetooth"); bt != c.end() && bt->is_object())
        {
            s.connection.btKnown = true;
            auto adapter = bt->find("adapter");
            if (adapter != bt->end())
            {
                if (adapter->is_boolean()) s.connection.btAdapter = adapter->get<bool>();
                else if (adapter->is_string())
                {
                    s.connection.btAdapterName = adapter->get<std::string>();
                    s.connection.btAdapter = !s.connection.btAdapterName.empty();
                }
                else if (adapter->is_object())
                {
                    s.connection.btAdapter = true;
                    s.connection.btAdapterName = JStr(*adapter, "name");
                }
            }
            s.connection.btOn = JBool(*bt, "on", false);
        }

        for (auto const& d : JArr(j, "devices"))
        {
            if (!d.is_object()) continue;
            Device dev;
            dev.id = JStr(d, "id");
            dev.name = JStr(d, "name");
            dev.platform = JStr(d, "platform");
            dev.btAddress = JStr(d, "btAddress");
            dev.lanHost = JStr(d, "lanHost");
            dev.connected = JBool(d, "connected", false);
            dev.transport = JStr(d, "transport");
            dev.lastSeen = JInt(d, "lastSeen", 0);
            dev.app = JStr(d, "app");
            dev.versionMatch = JStr(d, "versionMatch", "unknown");
            s.devices.push_back(std::move(dev));
        }

        s.local = ParseSide(JObj(j, "local"), Side::Local, receivedTick);
        s.remote = ParseSide(JObj(j, "remote"), Side::Remote, receivedTick);

        auto const& f = JObj(j, "focus");
        auto side = JStr(f, "side");
        s.focus.hasSide = side == "local" || side == "remote";
        s.focus.side = side == "remote" ? Side::Remote : Side::Local;
        s.focus.key = JStr(f, "key");
        s.focus.automatic = JBool(f, "auto", true);

        auto const& p = JObj(j, "policy");
        s.policy.mode = JStr(p, "mode");
        s.policy.enabled = JBool(p, "enabled", false);
        s.policy.remote = JStr(p, "remote");
        s.policy.local = JStr(p, "local");
        s.policy.windowsAudioActive = JBool(p, "windowsAudioActive", false);
        s.policy.suspended = JBool(p, "suspended", false);
        s.policy.gated = JBool(p, "gated", false);
        for (auto const& a : JArr(p, "activeApps"))
            if (a.is_string()) s.policy.activeApps.push_back(a.get<std::string>());
        auto const& la = JObj(p, "lastAction");
        if (!la.empty())
        {
            PolicyAction action;
            action.what = JStr(la, "what");
            action.reason = JStr(la, "reason");
            action.app = JStr(la, "app");
            action.atTick = static_cast<uint64_t>(JInt(la, "atTick", 0));
            if (!action.what.empty()) s.policy.lastAction = std::move(action);
        }

        auto const& audio = JObj(j, "audio");
        s.a2dpSink = ParseAudio(JObj(audio, "a2dpSink"));
        s.lanSend = ParseAudio(JObj(audio, "lanSend"));

        auto parseVolume = [](json const& v) {
            Volume out;
            out.available = JBool(v, "available", false);
            out.level = std::clamp(JDbl(v, "level", 0.0), 0.0, 1.0);
            out.muted = JBool(v, "muted", false);
            out.steps = static_cast<int>(JInt(v, "steps", 0));
            out.error = JStr(v, "error");
            return out;
        };
        auto const& vol = JObj(j, "volume");
        s.localVolume = parseVolume(JObj(vol, "local"));
        s.remoteVolume = parseVolume(JObj(vol, "remote"));
        auto const& tb = JObj(j, "taskbar");
        s.taskbar.known = !tb.empty();
        s.taskbar.supported = JBool(tb, "supported", false);
        s.taskbar.shown = JBool(tb, "shown", false);
        s.taskbar.reason = JStr(tb, "reason");

        if (auto const& ol = JObj(j, "audioOutputLatency"); !ol.empty())
        {
            s.outputLatency.known = true;
            s.outputLatency.bluetooth = JBool(ol, "bluetooth", false);
            s.outputLatency.ms = std::max<int64_t>(0, JInt(ol, "ms", 0));
        }

        for (auto const& o : JArr(j, "audioOutputs"))
        {
            if (!o.is_object()) continue;
            AudioOutput out;
            out.id = JStr(o, "id");
            out.name = JStr(o, "name");
            out.isDefault = JBool(o, "default", false);
            if (!out.id.empty()) s.audioOutputs.push_back(std::move(out));
        }

        auto const& pr = JObj(j, "pairing");
        s.pairing.state = JStr(pr, "state", "idle");
        s.pairing.sas = JStr(pr, "sas");
        s.pairing.peerName = JStr(pr, "peerName");
        s.pairing.error = JStr(pr, "error");
        auto const& qr = JObj(pr, "qr");
        s.pairing.qrStatus = JStr(qr, "status");
        s.pairing.qrPayload = JStr(qr, "payload");
        s.pairing.qrPeerName = JStr(qr, "peerName");

        for (auto const& d : JArr(j, "discovered"))
        {
            if (!d.is_object()) continue;
            Discovered item;
            item.kind = JStr(d, "kind");
            item.id = JStr(d, "id");
            item.name = JStr(d, "name");
            item.detail = JStr(d, "detail");
            item.paired = JBool(d, "paired", false);
            item.ready = JBool(d, "ready", true);
            s.discovered.push_back(std::move(item));
        }

        s.settings = JObj(j, "settings");
        // Live overlay state; falls back to the persisted lyrics.overlay.visible setting.
        bool persisted = false;
        if (auto v = JPath(s.settings, "lyrics.overlay.visible"); v && v->is_boolean()) persisted = v->get<bool>();
        s.overlayVisible = JBool(JObj(j, "lyricsOverlay"), "visible", persisted);
        s.diagnosticsEnabled = JBool(JObj(j, "diagnostics"), "enabled", false);

        auto const& h = JObj(j, "handoff");
        auto const& apps = JObj(h, "apps");
        for (auto const& a : JArr(apps, "local"))
            if (a.is_string()) s.handoff.localApps.push_back(a.get<std::string>());
        for (auto const& a : JArr(apps, "remote"))
            if (a.is_string()) s.handoff.remoteApps.push_back(a.get<std::string>());
        auto const& active = JObj(h, "active");
        if (!active.empty())
        {
            s.handoff.active = true;
            s.handoff.to = JStr(active, "to");
            s.handoff.app = JStr(active, "app");
            s.handoff.title = JStr(active, "title");
            s.handoff.waitingForTap = JBool(active, "waitingForTap", false);
        }
        if (auto u = j.find("update"); u != j.end()) s.update = ParseUpdate(*u);
        return s;
    }

    Update ParseUpdate(json const& u)
    {
        Update up;
        if (!u.is_object()) return up;
        up.valid = true;
        up.phase = JStr(u, "phase", "idle");
        up.current = JStr(u, "current");
        up.latest = JStr(u, "latest");
        up.page = JStr(u, "page");
        up.error = JStr(u, "error");
        up.progress = JDbl(u, "progress", 0);
        up.autoCheck = JBool(u, "auto", true);
        up.portable = JBool(u, "portable", false);
        return up;
    }

    HandoffOutcome ParseHandoffOutcome(json const& j)
    {
        HandoffOutcome o;
        o.ok = JBool(j, "ok", false);
        o.state = JStr(j, "state");
        o.error = JStr(JObj(j, "error"), "code");
        o.to = JStr(j, "to");
        o.app = JStr(j, "app");
        o.title = JStr(j, "title");
        return o;
    }

    Session const* State::Find(Side side, std::string_view key) const
    {
        auto const& list = side == Side::Local ? local.sessions : remote.sessions;
        for (auto const& s : list)
            if (s.key == key) return &s;
        return nullptr;
    }

    Session const* State::FocusedSession() const
    {
        if (focus.hasSide)
        {
            auto const& st = focus.side == Side::Local ? local : remote;
            if (!focus.key.empty())
                if (auto s = Find(focus.side, focus.key)) return s;
            if (!st.active.empty())
                if (auto s = Find(focus.side, st.active)) return s;
            if (!st.sessions.empty() && !focus.automatic) return &st.sessions.front();
        }
        for (auto const* st : { &local, &remote })
            for (auto const& s : st->sessions)
                if (s.IsPlaying()) return &s;
        if (!local.active.empty())
            if (auto s = Find(Side::Local, local.active)) return s;
        if (!remote.active.empty())
            if (auto s = Find(Side::Remote, remote.active)) return s;
        if (!local.sessions.empty()) return &local.sessions.front();
        if (!remote.sessions.empty()) return &remote.sessions.front();
        return nullptr;
    }

    AudioOutput const* State::DefaultOutput() const
    {
        for (auto const& o : audioOutputs)
            if (o.isDefault) return &o;
        return nullptr;
    }

    std::string State::PeerDisplayName() const
    {
        if (!connection.peer.name.empty()) return connection.peer.name;
        for (auto const& d : devices)
            if (d.connected && !d.name.empty()) return d.name;
        if (devices.size() == 1) return devices.front().name;
        return {};
    }

    std::string State::Setting(std::string_view path, std::string const& def) const
    {
        auto v = JPath(settings, path);
        return v && v->is_string() ? v->get<std::string>() : def;
    }

    bool State::Setting(std::string_view path, bool def) const
    {
        auto v = JPath(settings, path);
        return v && v->is_boolean() ? v->get<bool>() : def;
    }

    double State::SettingNumber(std::string_view path, double def) const
    {
        auto v = JPath(settings, path);
        return v && v->is_number() ? v->get<double>() : def;
    }

    int64_t State::LyricsOffset(std::string const& appId) const
    {
        auto offsets = JPath(settings, "lyrics.offsets");
        if (!offsets || !offsets->is_object() || appId.empty()) return 0;
        return JInt(*offsets, appId.c_str(), 0);
    }

    // ---- Lyrics ------------------------------------------------------------------------------

    Lyrics ParseLyrics(json const& j)
    {
        Lyrics l;
        l.valid = true;
        l.trackId = JStr(j, "trackId");
        l.side = JStr(j, "side") == "remote" ? Side::Remote : Side::Local;
        l.key = JStr(j, "key");
        l.status = JStr(j, "status");
        l.source = JStr(j, "source");
        l.synced = JBool(j, "synced", false);
        l.title = JStr(j, "title");
        l.artist = JStr(j, "artist");
        l.durationMs = JInt(j, "durationMs", -1);
        for (auto const& lj : JArr(j, "lines"))
        {
            if (!lj.is_object()) continue;
            LyricLine line;
            line.t = JInt(lj, "t", 0);
            line.d = JInt(lj, "d", 0);
            line.text = JStr(lj, "text");
            line.tr = JStr(lj, "tr");
            for (auto const& wj : JArr(lj, "words"))
            {
                if (!wj.is_object()) continue;
                LyricWord w;
                w.t = JInt(wj, "t", 0);
                w.d = JInt(wj, "d", 0);
                w.text = JStr(wj, "text");
                line.words.push_back(std::move(w));
            }
            l.lines.push_back(std::move(line));
        }
        // "hasTranslation" is informative; what matters is whether any line actually carries one
        // (a flag without any "tr" would show a toggle that does nothing).
        l.hasTranslation = std::any_of(l.lines.begin(), l.lines.end(), [](auto const& x) { return !x.tr.empty(); });
        if (l.synced)
            std::stable_sort(l.lines.begin(), l.lines.end(), [](auto const& a, auto const& b) { return a.t < b.t; });
        return l;
    }

    bool Lyrics::HasWords() const
    {
        return std::any_of(lines.begin(), lines.end(), [](auto const& l) { return !l.words.empty(); });
    }

    namespace
    {
        lyrics::SongRef SongOf(Session const& s)
        {
            return { s.trackId, s.side == Side::Remote ? 1 : 0, s.key, s.title, s.artist, s.durationMs };
        }
    }

    bool SameSong(Session const& a, Session const& b)
    {
        return lyrics::SameSong(SongOf(a), SongOf(b));
    }

    bool Lyrics::BelongsTo(Session const* s) const
    {
        if (!valid || !s) return false;
        if (trackId.empty() || s->trackId.empty() || key.empty())
        {
            if (!trackId.empty() && !s->trackId.empty()) return trackId == s->trackId;
            return side == s->side && key == s->key;
        }
        return lyrics::SameSong({ trackId, side == Side::Remote ? 1 : 0, key, title, artist, durationMs }, SongOf(*s)) ||
            trackId == s->trackId;
    }

    bool Lyrics::MatchesAndShowable(Session const* s) const
    {
        return status == "found" && !lines.empty() && BelongsTo(s);
    }

    std::string Lyrics::ContentKey() const
    {
        // FNV-1a over everything a view shows.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::string_view v) {
            for (unsigned char c : v) h = (h ^ c) * 1099511628211ull;
            h = (h ^ 0xFF) * 1099511628211ull;
        };
        for (auto const& l : lines)
        {
            mix(std::to_string(l.t));
            mix(l.text);
            mix(l.tr);
            for (auto const& w : l.words) mix(std::to_string(w.t) + "/" + std::to_string(w.d));
        }
        return key + "|" + source + "|" + std::to_string(lines.size()) + (synced ? "s" : "u") + (hasTranslation ? "|tr|" : "|") +
            std::to_string(h);
    }

    int Lyrics::LineAt(int64_t positionMs) const
    {
        if (!synced || lines.empty() || positionMs < 0) return -1;
        auto it = std::upper_bound(lines.begin(), lines.end(), positionMs,
            [](int64_t pos, LyricLine const& l) { return pos < l.t; });
        return static_cast<int>(it - lines.begin()) - 1;
    }

    std::wstring FormatTime(int64_t ms)
    {
        if (ms < 0) return L"--:--";
        int64_t total = ms / 1000;
        int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
        wchar_t buf[32];
        if (h > 0) swprintf_s(buf, L"%lld:%02lld:%02lld", h, m, s);
        else swprintf_s(buf, L"%lld:%02lld", m, s);
        return buf;
    }

    std::wstring FormatBytes(int64_t bytes)
    {
        wchar_t buf[32];
        if (bytes < 1024) swprintf_s(buf, L"%lld B", bytes);
        else if (bytes < 1024 * 1024) swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
        else if (bytes < 1024ll * 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024.0));
        else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }
}
