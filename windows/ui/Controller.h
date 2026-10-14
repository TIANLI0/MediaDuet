// Process-wide UI controller: owns the host connection, the latest host state, window lifetimes,
// theme/language, and exposes typed commands. Lives on the UI thread (except QueueActivation).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Model.h"

namespace winrt::MediaDuetUI::implementation
{
    struct MainWindow;
}

namespace mdui
{
    class PipeClient;

    enum class WindowKind { Main, Compact };
    enum class HostLink { Connecting, Connected, NotRunning };
    enum class Event { State, Lyrics, Diagnostics, Link, Appearance };

    struct LaunchOptions
    {
        std::vector<std::wstring> show;   // main | compact | settings | pair
        std::wstring pipe;
        std::wstring language;            // explicit --language (used by the UI's own restart)
        bool languageGiven{ false };
        std::wstring renderTest;          // --render-test <outdir>: invisible render test (RenderTest.h)
        std::wstring renderOnly;          // --render-only karaoke|sizes|light|transitions|compact
    };

    LaunchOptions ParseCommandLine(std::wstring_view commandLine);
    // %LOCALAPPDATA%\MediaDuet\settings.json (written by the host; empty object when unavailable).
    json ReadPersistedSettings();
    // "language" from %LOCALAPPDATA%\MediaDuet\settings.json ("auto" when unavailable).
    std::wstring ReadPersistedLanguage();

    class Controller
    {
    public:
        static Controller& Get();

        void Start(LaunchOptions const& options);
        void Shutdown();
        // Thread-safe: called by AppInstance redirection with the second instance's command line.
        void QueueActivation(std::wstring commandLine);

        // Windows ----------------------------------------------------------------------------
        void Show(std::wstring_view what);
        void ShowSettings(std::wstring_view page);
        void WindowClosed(WindowKind kind, winrt::Microsoft::UI::Xaml::Window const& window);
        void SwitchToCompact();
        void SwitchToMain();
        // Render test only: creates a window off-screen, never activated (RenderTest.h).
        winrt::Microsoft::UI::Xaml::Window OpenOffscreen(WindowKind kind, int widthDip, int heightDip);

        // State ------------------------------------------------------------------------------
        State const& Model() const { return m_state; }
        Lyrics const& CurrentLyrics() const { return m_lyrics; }
        json const& Diagnostics() const { return m_diagnostics; }
        HostLink Link() const { return m_link; }
        std::string const& HostVersion() const { return m_hostVersion; }
        winrt::Microsoft::UI::Xaml::ElementTheme Theme() const;
        bool AnimationsEnabled() const;
        bool LowPower() const;

        // Events (UI thread). Subscribe returns a token for Unsubscribe.
        uint32_t Subscribe(Event event, std::function<void()> handler);
        uint32_t SubscribeResults(std::function<void(bool ok, std::string const& error, std::string const& type)> handler);
        void Unsubscribe(uint32_t token);

        // Commands (docs/ipc.md "UI -> Host") ------------------------------------------------
        int64_t Send(json message);
        void Media(Side side, std::string const& key, std::string const& action, std::optional<int64_t> positionMs = {});
        // The player's own button / a queue entry of a phone session.
        void PressCustomAction(Side side, std::string const& key, std::string const& id);
        void PlayQueueItem(Side side, std::string const& key, int64_t queueId);
        // Play queue of a session (remote sessions only), null when it has none.
        Queue const* QueueFor(Side side, std::string const& key) const;
        void SetFocus(std::optional<Side> side, std::string const& key, bool automatic);
        void SetSetting(std::string_view path, json value);
        void SetSettings(json patch);
        void PairStart(std::string const& kind, std::string const& target);
        void PairRespond(bool accept);
        void PairCancel();
        void DeviceForget(std::string const& id);
        void DeviceConnect(std::string const& id);
        void DeviceDisconnect(std::string const& id);
        void ConnectManual(std::string const& host, int port);
        void AcquireDiscovery();
        void ReleaseDiscovery();
        void SetA2dpSink(bool enable);
        void SetLanSend(bool enable);
        void SetLyricsOverlay(bool visible);
        // volume.set: level 0..1 and/or muted.
        void SetVolume(Side side, std::optional<double> level, std::optional<bool> muted, std::string const& key = {});
        void SetDiagnostics(bool enable);
        // lyrics.offset: adjust (deltaMs) or reset the manual lyrics offset of an app. Applied
        // optimistically to settings.lyrics.offsets and the sessions' lyricsDelayMs.
        void AdjustLyricsOffset(std::string const& appId, int64_t deltaMs);
        void ResetLyricsOffset(std::string const& appId);
        // The phone refused a volume change because MediaDuet runs in the background there
        // (Android "background_restricted"); cleared by the next successful phone volume change.
        bool RemoteVolumeRestricted() const { return m_remoteVolumeRestricted; }
        void ExitHost();

        bool HostExecutableExists() const;

        // Pairing dialog policy: active states always need the user; a result (succeeded/failed) is
        // shown at most once and only after this UI saw the pairing in progress, so a host that keeps
        // reporting the last result never pops a dialog when settings are opened.
        bool PairingDialogWanted() const;
        void AcknowledgePairingResult() { m_pairingResultAcknowledged = true; }
        bool LaunchHost();

    private:
        Controller() = default;
        void OnLine(std::string&& line);
        void Dispatch(std::shared_ptr<json> message);
        void SetLink(HostLink link);
        void Raise(Event event);
        void ApplyState(json const& message);
        void ProcessActivation(std::wstring const& commandLine);
        void ProcessPendingActivations();
        bool EvaluateLanguage();
        void SwitchLanguage(std::string const& language);
        bool AnyWindowOpen() const;
        winrt::Microsoft::UI::Xaml::Window Open(WindowKind kind);
        static winrt::MediaDuetUI::implementation::MainWindow* MainImpl(winrt::Microsoft::UI::Xaml::Window const& window);
        bool SettingsVisible() const;

        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_queue{ nullptr };
        std::unique_ptr<PipeClient> m_pipe;
        std::wstring m_pipeName;
        bool m_customPipe{ false };
        HostLink m_link{ HostLink::Connecting };
        uint64_t m_hostLaunchDeadline{ 0 };
        State m_state;
        std::map<std::string, Queue> m_queues;   // remote session key -> queue
        Lyrics m_lyrics;
        json m_diagnostics = json::object();
        std::string m_hostVersion;
        std::string m_lastTheme;
        bool m_lastAnimations{ true };
        bool m_restarting{ false };
        bool m_recreating{ false };
        std::string m_hostLanguage;
        bool m_shuttingDown{ false };
        std::string m_lastPairingState{ "idle" };
        bool m_pairingSeenActive{ false };
        bool m_pairingResultAcknowledged{ false };
        int m_discoveryRefs{ 0 };
        int64_t m_nextRid{ 1 };
        struct PendingRequest
        {
            std::string type;
            std::string side;   // volume.set
        };
        std::map<int64_t, PendingRequest> m_pending;
        Volume m_hostRemoteVolume;   // last phone volume reported by the host (rollback target)
        bool m_remoteVolumeRestricted{ false };

        winrt::Microsoft::UI::Xaml::Window m_main{ nullptr };
        winrt::Microsoft::UI::Xaml::Window m_compact{ nullptr };

        uint32_t m_nextToken{ 1 };
        std::vector<std::tuple<uint32_t, Event, std::function<void()>>> m_handlers;
        std::vector<std::pair<uint32_t, std::function<void(bool, std::string const&, std::string const&)>>> m_resultHandlers;

        std::mutex m_activationMutex;
        std::vector<std::wstring> m_pendingActivations;
        bool m_started{ false };
    };
}
