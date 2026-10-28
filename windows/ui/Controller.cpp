#include "pch.h"
#include "Controller.h"

#include "CompactWindow.xaml.h"
#include "MainWindow.xaml.h"
#include "PipeClient.h"
#include "RenderTest.h"
#include "WindowUtil.h"

#include <shellapi.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.Globalization.h>

#include "Strings.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace mdui
{
    // ---- Command line ------------------------------------------------------------------------

    LaunchOptions ParseCommandLine(std::wstring_view commandLine)
    {
        LaunchOptions options;
        int argc = 0;
        std::wstring copy(commandLine);
        LPWSTR* argv = copy.empty() ? nullptr : CommandLineToArgvW(copy.c_str(), &argc);
        if (!argv) return options;
        for (int i = 0; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring{}; };
            if (arg == L"--show" || arg == L"/show")
            {
                auto v = next();
                if (v == L"main" || v == L"compact" || v == L"settings" || v == L"pair") options.show.push_back(v);
            }
            else if (arg.rfind(L"--show=", 0) == 0)
            {
                options.show.push_back(arg.substr(7));
            }
            else if (arg == L"--pipe")
            {
                options.pipe = next();
            }
            else if (arg == L"--render-test")
            {
                options.renderTest = next();
            }
            else if (arg == L"--render-only")
            {
                options.renderOnly = next();
            }
            else if (arg == L"--language")
            {
                options.language = next();
                options.languageGiven = true;
            }
        }
        LocalFree(argv);
        return options;
    }

    json ReadPersistedSettings()
    {
        PWSTR folder = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
            path = std::wstring(folder) + L"\\MediaDuet\\settings.json";
        CoTaskMemFree(folder);
        if (path.empty()) return json::object();
        std::ifstream in(path, std::ios::binary);
        if (!in) return json::object();
        std::stringstream ss;
        ss << in.rdbuf();
        auto j = json::parse(ss.str(), nullptr, false);
        return j.is_object() ? j : json::object();
    }

    std::wstring ReadPersistedLanguage()
    {
        auto lang = JStr(ReadPersistedSettings(), "language", "auto");
        if (lang != "zh-CN" && lang != "en-US") lang = "auto";
        return Widen(lang);
    }

    // ---- Controller --------------------------------------------------------------------------

    Controller& Controller::Get()
    {
        static Controller instance;
        return instance;
    }

    void Controller::Start(LaunchOptions const& options)
    {
        m_queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        // Until the host's first state arrives, use the persisted settings (theme, window positions).
        // The render test never reads the user's settings.
        m_state.settings = render::Active() ? json::object() : ReadPersistedSettings();
        m_lastTheme = m_state.Setting("theme", std::string("system"));
        m_lastAnimations = AnimationsEnabled();

        if (!options.pipe.empty())
        {
            m_customPipe = true;
            m_pipeName = options.pipe.rfind(L"\\\\.\\pipe\\", 0) == 0 ? options.pipe : L"\\\\.\\pipe\\" + options.pipe;
        }
        else
        {
            m_pipeName = PipeClient::DefaultPipeName();
        }

        PipeClient::Callbacks callbacks;
        callbacks.connected = [this] {
            m_queue.TryEnqueue([this] { SetLink(HostLink::Connected); });
        };
        callbacks.disconnected = [this](bool hostMissing) {
            m_queue.TryEnqueue([this, hostMissing] {
                if (m_shuttingDown) return;
                bool hadState = m_state.valid;
                auto settings = std::move(m_state.settings);
                m_state = State{};
                m_state.settings = std::move(settings);   // keep theme/positions while disconnected
                m_lyrics = Lyrics{};
                // Right after "Start MediaDuet" the pipe needs a moment to appear.
                bool launching = GetTickCount64() < m_hostLaunchDeadline;
                SetLink(hostMissing && !launching ? HostLink::NotRunning : HostLink::Connecting);
                if (hadState) Raise(Event::State);
            });
        };
        callbacks.line = [this](std::string&& line) { OnLine(std::move(line)); };
        m_pipe = std::make_unique<PipeClient>(m_pipeName, std::move(callbacks));

        if (render::Active())
        {
            // Off-screen render test: windows are created by the scenario, never shown normally.
            m_started = true;
            m_pipe->Start();
            render::Run();
            return;
        }

        auto show = options.show;
        if (show.empty()) show.push_back(L"main");
        for (auto const& s : show) Show(s);

        m_started = true;
        m_pipe->Start();
        ProcessPendingActivations();
    }

    void Controller::Shutdown()
    {
        m_shuttingDown = true;
        if (m_pipe)
        {
            if (m_discoveryRefs > 0)
            {
                m_discoveryRefs = 0;
                Send({ { "type", "discover" }, { "enable", false } });
            }
            m_pipe->Stop();
            m_pipe.reset();
        }
        m_handlers.clear();
        m_resultHandlers.clear();
    }

    void Controller::QueueActivation(std::wstring commandLine)
    {
        {
            std::lock_guard lock(m_activationMutex);
            m_pendingActivations.push_back(std::move(commandLine));
        }
        if (m_queue) m_queue.TryEnqueue([this] { ProcessPendingActivations(); });
    }

    void Controller::ProcessPendingActivations()
    {
        if (!m_started) return;
        std::vector<std::wstring> pending;
        {
            std::lock_guard lock(m_activationMutex);
            pending.swap(m_pendingActivations);
        }
        for (auto const& cmd : pending) ProcessActivation(cmd);
    }

    void Controller::ProcessActivation(std::wstring const& commandLine)
    {
        auto options = ParseCommandLine(commandLine);
        if (options.show.empty()) options.show.push_back(m_compact && !m_main ? L"compact" : L"main");
        for (auto const& s : options.show) Show(s);
    }

    // ---- Windows -----------------------------------------------------------------------------

    bool Controller::AnyWindowOpen() const
    {
        return m_main || m_compact;
    }

    Window Controller::OpenOffscreen(WindowKind kind, int widthDip, int heightDip)
    {
        Window w{ nullptr };
        if (kind == WindowKind::Main)
        {
            if (!m_main) m_main = make<MediaDuetUI::implementation::MainWindow>();
            w = m_main;
        }
        else
        {
            if (!m_compact) m_compact = make<MediaDuetUI::implementation::CompactWindow>();
            w = m_compact;
        }
        render::LogLine(L"window created");
        try
        {
            w.AppWindow().IsShownInSwitchers(false);   // WS_EX_TOOLWINDOW below also keeps it out
        }
        catch (hresult_error const& e)
        {
            render::LogLine(L"IsShownInSwitchers: " + std::wstring(e.message()));
        }
        render::PrepareOffscreen(GetHwnd(w), widthDip, heightDip);
        return w;
    }

    Window Controller::Open(WindowKind kind)
    {
        if (render::Active()) return kind == WindowKind::Main ? m_main : m_compact;   // never Activate()
        switch (kind)
        {
        case WindowKind::Main:
            if (!m_main)
            {
                m_main = make<MediaDuetUI::implementation::MainWindow>();
                m_main.Activate();
            }
            return m_main;
        case WindowKind::Compact:
            if (!m_compact)
            {
                m_compact = make<MediaDuetUI::implementation::CompactWindow>();
                m_compact.Activate();
            }
            return m_compact;
        default:
            return nullptr;
        }
    }

    void Controller::Show(std::wstring_view what)
    {
        if (m_restarting || m_shuttingDown || render::Active()) return;
        if (what == L"compact")
        {
            BringToFront(Open(WindowKind::Compact));
        }
        else if (what == L"settings")
        {
            ShowSettings(L"");
        }
        else if (what == L"pair")
        {
            ShowSettings(L"pair");
        }
        else
        {
            auto window = Open(WindowKind::Main);
            MainImpl(window)->ShowPlayer();
            BringToFront(window);
        }
    }

    MediaDuetUI::implementation::MainWindow* Controller::MainImpl(Window const& window)
    {
        return get_self<MediaDuetUI::implementation::MainWindow>(window.as<MediaDuetUI::MainWindow>());
    }

    bool Controller::SettingsVisible() const
    {
        return m_main && get_self<MediaDuetUI::implementation::MainWindow>(m_main.as<MediaDuetUI::MainWindow>())->IsSettingsShown();
    }

    void Controller::ShowSettings(std::wstring_view page)
    {
        if (m_restarting || m_shuttingDown || render::Active()) return;
        // Settings is a page of the main window (Windows 11 Settings style sub-page).
        auto window = Open(WindowKind::Main);
        MainImpl(window)->ShowSettings(page);
        BringToFront(window);
    }

    void Controller::SwitchToCompact()
    {
        if (render::Active()) return;
        BringToFront(Open(WindowKind::Compact));
        if (m_main) m_main.Close();
    }

    void Controller::SwitchToMain()
    {
        if (render::Active()) return;
        auto window = Open(WindowKind::Main);
        MainImpl(window)->ShowPlayer();
        BringToFront(window);
        if (m_compact) m_compact.Close();
    }

    void Controller::WindowClosed(WindowKind kind, Window const& window)
    {
        // Only forget the window if it is still the current one of its kind (language switches
        // replace windows).
        auto forget = [&](Window& slot) {
            if (slot && slot == window) slot = nullptr;
        };
        switch (kind)
        {
        case WindowKind::Main: forget(m_main); break;
        case WindowKind::Compact: forget(m_compact); break;
        }
        if (!AnyWindowOpen() && !m_restarting && !m_recreating)
        {
            // Last window closed: the process exits (docs/ipc.md). Flush and stop the pipe first.
            Shutdown();
            Application::Current().Exit();
        }
    }

    // ---- Appearance --------------------------------------------------------------------------

    ElementTheme Controller::Theme() const
    {
        auto theme = m_state.Setting("theme", std::string("system"));
        if (theme == "light") return ElementTheme::Light;
        if (theme == "dark") return ElementTheme::Dark;
        return ElementTheme::Default;
    }

    bool Controller::LowPower() const
    {
        return m_state.Setting("ui.lowPower", false);
    }

    bool Controller::AnimationsEnabled() const
    {
        return m_state.Setting("ui.animations", true) && !LowPower();
    }

    // ---- Events ------------------------------------------------------------------------------

    uint32_t Controller::Subscribe(Event event, std::function<void()> handler)
    {
        auto token = m_nextToken++;
        m_handlers.emplace_back(token, event, std::move(handler));
        return token;
    }

    uint32_t Controller::SubscribeResults(std::function<void(bool, std::string const&, std::string const&)> handler)
    {
        auto token = m_nextToken++;
        m_resultHandlers.emplace_back(token, std::move(handler));
        return token;
    }

    void Controller::Unsubscribe(uint32_t token)
    {
        std::erase_if(m_handlers, [token](auto const& h) { return std::get<0>(h) == token; });
        std::erase_if(m_resultHandlers, [token](auto const& h) { return h.first == token; });
    }

    void Controller::Raise(Event event)
    {
        auto copy = m_handlers;   // handlers may (un)subscribe while running
        for (auto const& [token, ev, fn] : copy)
        {
            if (ev != event) continue;
            bool stillSubscribed = std::any_of(m_handlers.begin(), m_handlers.end(),
                [t = token](auto const& h) { return std::get<0>(h) == t; });
            if (stillSubscribed && fn) fn();
        }
    }

    void Controller::SetLink(HostLink link)
    {
        if (m_link == link) return;
        m_link = link;
        if (link == HostLink::Connected && m_discoveryRefs > 0)
            Send({ { "type", "discover" }, { "enable", true } });
        Raise(Event::Link);
    }

    // ---- Incoming messages ------------------------------------------------------------------

    void Controller::OnLine(std::string&& line)
    {
        // Pipe thread: parse here so the UI thread only applies.
        auto parsed = std::make_shared<json>(json::parse(line, nullptr, false));
        if (parsed->is_discarded() || !parsed->is_object()) return;
        m_queue.TryEnqueue([this, parsed] { Dispatch(parsed); });
    }

    void Controller::Dispatch(std::shared_ptr<json> message)
    {
        if (m_shuttingDown) return;
        auto type = JStr(*message, "type");
        if (type == "state")
        {
            ApplyState(*message);
        }
        else if (type == "lyrics")
        {
            m_lyrics = ParseLyrics(*message);
            Raise(Event::Lyrics);
        }
        else if (type == "queue")
        {
            // Phone sessions, and NetEase's on this PC (its play queue file).
            auto key = JStr(*message, "side", "remote") + '\x1f' + JStr(*message, "key");
            auto q = message->find("queue");
            if (q == message->end() || !q->is_object()) m_queues.erase(key);
            else m_queues[key] = ParseQueue(*q);
            Raise(Event::State);
        }
        else if (type == "handoff")
        {
            m_handoff = ParseHandoffOutcome(JObj(*message, "outcome"));
            Raise(Event::Handoff);
        }
        else if (type == "diagnostics")
        {
            m_diagnostics = *message;
            Raise(Event::Diagnostics);
        }
        else if (type == "hello")
        {
            m_hostVersion = JStr(*message, "version");
            // The host re-sends hello (with the resolved language) after a language change.
            m_hostLanguage = JStr(*message, "language");
            EvaluateLanguage();
        }
        else if (type == "show")
        {
            Show(Widen(JStr(*message, "window", "main")));
        }
        else if (type == "result")
        {
            auto rid = JInt(*message, "rid", 0);
            PendingRequest request;
            if (auto it = m_pending.find(rid); it != m_pending.end())
            {
                request = it->second;
                m_pending.erase(it);
            }
            std::string const& requestType = request.type;
            bool ok = JBool(*message, "ok", true);
            auto error = JStr(*message, "error");
            if (requestType == "volume.set" && request.side == "remote")
            {
                if (!ok)
                {
                    // No fake success: the phone keeps its real level.
                    m_state.remoteVolume = m_hostRemoteVolume;
                    if (error == "background_restricted") m_remoteVolumeRestricted = true;
                    Raise(Event::State);
                }
                else if (m_remoteVolumeRestricted)
                {
                    m_remoteVolumeRestricted = false;
                    Raise(Event::State);
                }
            }
            auto handlers = m_resultHandlers;
            for (auto const& [token, fn] : handlers)
                if (fn) fn(ok, error, requestType);
        }
    }

    void Controller::ApplyState(json const& message)
    {
        bool hadRestriction = m_state.remoteVolume.error == "background_restricted";
        m_state = ParseState(message, TickNow());
        m_hostRemoteVolume = m_state.remoteVolume;
        if (m_state.remoteVolume.error == "background_restricted") m_remoteVolumeRestricted = true;
        else if (hadRestriction) m_remoteVolumeRestricted = false;   // the host cleared it
        if (m_link != HostLink::Connected) SetLink(HostLink::Connected);

        // Language follows settings.language; "auto" uses the language the host resolved (hello).
        EvaluateLanguage();

        auto theme = m_state.Setting("theme", std::string("system"));
        bool animations = AnimationsEnabled();
        bool appearanceChanged = theme != m_lastTheme || animations != m_lastAnimations;
        m_lastTheme = theme;
        m_lastAnimations = animations;

        auto const& ps = m_state.pairing.state;
        if (ps == "connecting" || ps == "awaiting_user" || ps == "awaiting_peer")
        {
            m_pairingSeenActive = true;
            m_pairingResultAcknowledged = false;
        }
        else if (ps == "idle")
        {
            m_pairingSeenActive = false;
            m_pairingResultAcknowledged = false;
        }

        // A pairing request that needs this user's confirmation must be visible.
        if (m_state.pairing.state == "awaiting_user" && m_lastPairingState != "awaiting_user" && !SettingsVisible())
            ShowSettings(L"pair");
        m_lastPairingState = m_state.pairing.state;

        if (appearanceChanged) Raise(Event::Appearance);
        Raise(Event::State);
    }

    bool Controller::EvaluateLanguage()
    {
        if (render::Active()) return false;   // windows are never recreated (that would Activate them)
        std::string setting = m_state.valid ? m_state.Setting("language", std::string("auto")) : std::string("auto");
        std::string target;
        if (setting == "zh-CN" || setting == "en-US") target = setting;
        else if (m_hostLanguage == "zh-CN" || m_hostLanguage == "en-US") target = m_hostLanguage;
        else target = "auto";
        auto wanted = ResolveLanguage(target == "auto" ? std::wstring{} : Widen(target));
        if (wanted == UiLanguage()) return false;
        SwitchLanguage(target);
        return true;
    }

    void Controller::SwitchLanguage(std::string const& language)
    {
        // Applied in-process: MRT Core honours a new PrimaryLanguageOverride for XAML loaded afterwards,
        // so the open windows are recreated (same kind, page and placement) to reload every x:Uid.
        std::wstring lang = language == "auto" ? std::wstring{} : Widen(language);
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(lang);
        InitStrings(lang);

        struct Reopen { WindowKind kind; winrt::Windows::Graphics::RectInt32 rect; };
        std::vector<Reopen> reopen;
        std::wstring settingsPage;
        auto capture = [&](Window const& w, WindowKind kind) {
            if (!w) return;
            auto aw = w.AppWindow();
            reopen.push_back({ kind, { aw.Position().X, aw.Position().Y, aw.Size().Width, aw.Size().Height } });
        };
        capture(m_main, WindowKind::Main);
        capture(m_compact, WindowKind::Compact);
        bool settingsShown = SettingsVisible();
        if (settingsShown) settingsPage = MainImpl(m_main)->SettingsPageTag();

        std::vector<Window> old{ m_compact, m_main };
        m_compact = m_main = nullptr;
        m_recreating = true;
        for (auto const& w : old)
            if (w) w.Close();
        m_recreating = false;

        for (auto const& r : reopen)
        {
            auto w = Open(r.kind);
            if (r.kind != WindowKind::Compact) w.AppWindow().MoveAndResize(r.rect);
            else w.AppWindow().Move({ r.rect.X, r.rect.Y });
            if (r.kind == WindowKind::Main && settingsShown)
                MainImpl(w)->ShowSettings(settingsPage.empty() ? std::wstring_view(L"general") : std::wstring_view(settingsPage), false);
        }
    }

    // ---- Commands ----------------------------------------------------------------------------

    int64_t Controller::Send(json message)
    {
        if (!m_pipe) return 0;
        int64_t rid = m_nextRid++;
        message["rid"] = rid;
        PendingRequest request{ JStr(message, "type"), JStr(message, "side") };
        bool sent = m_pipe->Send(message.dump(-1, ' ', false, json::error_handler_t::replace));
        if (message.contains("password") && message["password"].is_string()) {
            auto& password = message["password"].get_ref<std::string&>();
            SecureZeroMemory(password.data(), password.size());
        }
        if (!sent) return 0;
        m_pending[rid] = std::move(request);
        if (m_pending.size() > 256) m_pending.erase(m_pending.begin());
        return rid;
    }

    void Controller::Media(Side side, std::string const& key, std::string const& action, std::optional<int64_t> positionMs)
    {
        json msg{ { "type", "media.command" }, { "side", SideName(side) }, { "action", action } };
        msg["key"] = key.empty() ? json(nullptr) : json(key);
        if (positionMs) msg["positionMs"] = *positionMs;
        Send(std::move(msg));
    }

    void Controller::StartHandoff(Side side, std::string const& key, std::string const& app)
    {
        json msg{ { "type", "handoff.start" }, { "side", SideName(side) }, { "key", key } };
        if (!app.empty()) msg["app"] = app;
        Send(std::move(msg));
    }

    void Controller::PressCustomAction(Side side, std::string const& key, std::string const& id)
    {
        json msg{ { "type", "media.command" }, { "side", SideName(side) }, { "action", "custom" }, { "customAction", id } };
        msg["key"] = key.empty() ? json(nullptr) : json(key);
        Send(std::move(msg));
    }

    void Controller::PlayQueueItem(Side side, std::string const& key, int64_t queueId)
    {
        json msg{ { "type", "media.command" }, { "side", SideName(side) }, { "action", "playQueueItem" }, { "queueId", queueId } };
        msg["key"] = key.empty() ? json(nullptr) : json(key);
        Send(std::move(msg));
    }

    Queue const* Controller::QueueFor(Side side, std::string const& key) const
    {
        auto it = m_queues.find(std::string(SideName(side)) + '\x1f' + key);
        return it == m_queues.end() ? nullptr : &it->second;
    }

    void Controller::SetFocus(std::optional<Side> side, std::string const& key, bool automatic)
    {
        json msg{ { "type", "focus.set" } };
        msg["side"] = side ? json(SideName(*side)) : json(nullptr);
        msg["key"] = (automatic || key.empty()) ? json(nullptr) : json(key);
        if (automatic) msg["auto"] = true;
        Send(std::move(msg));
    }

    void Controller::SetSetting(std::string_view path, json value)
    {
        SetSettings(MakePatch(path, std::move(value)));
    }

    void Controller::SetSettings(json patch)
    {
        // Optimistic local merge so dependent UI reacts immediately; the next state is authoritative.
        DeepMerge(m_state.settings, patch);
        Send({ { "type", "settings.set" }, { "patch", std::move(patch) } });
    }

    void Controller::PairStart(std::string const& kind, std::string const& target)
    {
        Send({ { "type", "pair.start" }, { "kind", kind }, { "target", target } });
    }

    void Controller::PairRespond(bool accept)
    {
        Send({ { "type", "pair.respond" }, { "accept", accept } });
    }

    void Controller::PairCancel()
    {
        Send({ { "type", "pair.cancel" } });
    }

    void Controller::DeviceForget(std::string const& id)
    {
        Send({ { "type", "device.forget" }, { "id", id } });
    }

    void Controller::DeviceConnect(std::string const& id)
    {
        Send({ { "type", "device.connect" }, { "id", id } });
    }

    void Controller::DeviceDisconnect(std::string const& id)
    {
        Send({ { "type", "device.disconnect" }, { "id", id } });
    }

    void Controller::ConnectManual(std::string const& host, int port)
    {
        Send({ { "type", "connect.manual" }, { "host", host }, { "port", port } });
    }

    void Controller::AcquireDiscovery()
    {
        if (m_discoveryRefs++ == 0) Send({ { "type", "discover" }, { "enable", true } });
    }

    void Controller::ReleaseDiscovery()
    {
        if (m_discoveryRefs <= 0) return;
        if (--m_discoveryRefs == 0) Send({ { "type", "discover" }, { "enable", false } });
    }

    void Controller::SetA2dpSink(bool enable)
    {
        Send({ { "type", "audio.a2dpSink" }, { "enable", enable } });
    }

    void Controller::SetLanSend(bool enable)
    {
        Send({ { "type", "audio.lanSend" }, { "enable", enable } });
    }

    void Controller::SetLyricsOverlay(bool visible)
    {
        m_state.overlayVisible = visible;
        Send({ { "type", "lyrics.overlay" }, { "visible", visible } });
    }

    void Controller::SetVolume(Side side, std::optional<double> level, std::optional<bool> muted, std::string const& key)
    {
        json msg{ { "type", "volume.set" }, { "side", SideName(side) } };
        Volume scratch;
        Volume* target = side == Side::Local ? &m_state.localVolume : &m_state.remoteVolume;
        if (!key.empty())
        {
            // Per-app volume of a local session (Windows mixer).
            msg["key"] = key;
            target = &scratch;
            for (auto& s : m_state.local.sessions)
                if (s.key == key) target = &s.appVolume;
        }
        auto& v = *target;
        if (level)
        {
            double l = std::round(std::clamp(*level, 0.0, 1.0) * 1000.0) / 1000.0;
            msg["level"] = l;
            v.level = l;
        }
        if (muted)
        {
            msg["muted"] = *muted;
            v.muted = *muted;
        }
        Send(std::move(msg));
    }

    void Controller::SetDiagnostics(bool enable)
    {
        m_state.diagnosticsEnabled = enable;
        Send({ { "type", "diagnostics" }, { "enable", enable } });
    }

    void Controller::AdjustLyricsOffset(std::string const& appId, int64_t deltaMs)
    {
        if (appId.empty() || deltaMs == 0) return;
        int64_t before = m_state.LyricsOffset(appId);
        int64_t after = std::clamp<int64_t>(before + deltaMs, -5000, 5000);
        Send({ { "type", "lyrics.offset" }, { "appId", appId }, { "deltaMs", deltaMs } });
        // Optimistic: the next state from the host is authoritative.
        DeepMerge(m_state.settings, json{ { "lyrics", { { "offsets", { { appId, after } } } } } });
        for (auto* side : { &m_state.local, &m_state.remote })
            for (auto& s : side->sessions)
                if (s.appId == appId) s.lyricsDelayMs += after - before;
        Raise(Event::State);
    }

    void Controller::ResetLyricsOffset(std::string const& appId)
    {
        if (appId.empty()) return;
        int64_t before = m_state.LyricsOffset(appId);
        Send({ { "type", "lyrics.offset" }, { "appId", appId }, { "reset", true } });
        DeepMerge(m_state.settings, json{ { "lyrics", { { "offsets", { { appId, nullptr } } } } } });
        for (auto* side : { &m_state.local, &m_state.remote })
            for (auto& s : side->sessions)
                if (s.appId == appId) s.lyricsDelayMs -= before;
        Raise(Event::State);
    }

    void Controller::ExitHost()
    {
        Send({ { "type", "host.exit" } });
        // docs/ipc.md: host.exit quits the host *and* the UI.
        m_restarting = true;
        Shutdown();
        Application::Current().Exit();
    }

    bool Controller::PairingDialogWanted() const
    {
        if (m_link != HostLink::Connected) return false;
        auto const& ps = m_state.pairing.state;
        if (ps == "connecting" || ps == "awaiting_user" || ps == "awaiting_peer") return true;
        if (ps == "succeeded" || ps == "failed") return m_pairingSeenActive && !m_pairingResultAcknowledged;
        return false;
    }

    bool Controller::HostExecutableExists() const
    {
        auto path = ExeDirectory() + L"MediaDuet.exe";
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool Controller::LaunchHost()
    {
        if (render::Active()) return false;
        if (!HostExecutableExists()) return false;
        auto dir = ExeDirectory();
        auto path = dir + L"MediaDuet.exe";
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOASYNC;
        info.lpFile = path.c_str();
        info.lpDirectory = dir.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) return false;
        m_hostLaunchDeadline = GetTickCount64() + 15000;
        SetLink(HostLink::Connecting);
        if (m_pipe) m_pipe->RetryNow();
        return true;
    }
}
