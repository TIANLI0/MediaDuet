#pragma once

#include "MainWindow.g.h"

#include "Ambient.h"
#include "Artwork.h"
#include "Karaoke.h"
#include "LyricsView.h"
#include "Model.h"
#include "PlaybackClock.h"
#include "WindowUtil.h"

#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Windows.System.h>

namespace winrt::MediaDuetUI::implementation
{
    struct SettingsPage;

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;
        void InitializeComponent();

        // Page stack: the player and the settings sub-page (Windows 11 Settings style).
        void ShowSettings(std::wstring_view page, bool animate = true);
        void ShowPlayer(bool animate = true);
        bool IsSettingsShown() const { return m_settingsShown; }
        std::wstring SettingsPageTag() const;

    private:
        struct VolumeUi
        {
            mdui::Side side{ mdui::Side::Local };
            std::string key;   // local session key for per-app volume (empty = device volume)
            Microsoft::UI::Xaml::Controls::Slider slider{ nullptr };
            Microsoft::UI::Xaml::Controls::FontIcon icon{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock text{ nullptr };
            Microsoft::UI::Dispatching::DispatcherQueueTimer timer{ nullptr };
            std::optional<double> pending;
            double lastSent{ 0 };
            double ignoreUntil{ 0 };   // echoes of our own changes are ignored until then
            bool dragging{ false };
            bool applying{ false };
        };

        void AnimatePageIn(Microsoft::UI::Xaml::UIElement const& page, float fromX);
        void SetupVolume(VolumeUi& v, mdui::Side side, Microsoft::UI::Xaml::Controls::Slider const& slider,
            Microsoft::UI::Xaml::Controls::Button const& mute, Microsoft::UI::Xaml::Controls::FontIcon const& icon,
            Microsoft::UI::Xaml::Controls::TextBlock const& text);
        void ShowVolume(VolumeUi& v, double level, bool muted);
        void ApplyVolume(VolumeUi& v, mdui::Volume const& vol);
        void UpdateVolume();
        void UpdateA2dpQuick();
        void UpdateExtras();
        void FillQueue();
        std::string m_extrasSignature;   // custom-action buttons are rebuilt only when this changes
        void RefreshAll();
        void UpdateLink();
        void UpdateConnection();
        void UpdateCard();
        void UpdatePlayState();
        void UpdatePolicy();
        void UpdateLyrics();
        void UpdateWelcome();
        void UpdateVersionBar();
        void UpdateUpdateBar();
        void UpdateLyricsOffset();
        void UpdateProgress(uint64_t now);
        void UpdateLyricsPosition(double nowQpcMs, bool force);
        double LyricPosition(double nowQpcMs) const;
        void OnFrame();
        void UpdateFrameLoop();
        void UpdateTimer();
        void ApplyAppearance();
        void UpdateLayout();
        bool TransitionsEnabled() const;
        winrt::Windows::Foundation::Point CardOrigin();
        void SetLyricsLayout(bool on, bool animate);
        void BuildSourceMenu();
        // Handoff: the menu of the other device's music apps, the progress / result bar.
        bool CanHandoff(mdui::Session const* s) const;
        void BuildHandoffMenu();
        void UpdateHandoffBar();
        void ShowHandoffOutcome();
        bool m_handoffResultShown{ false };   // HandoffBar shows a result (not progress)
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_handoffBarTimer{ nullptr };
        void PlaceInitially();
        void SavePosition();
        void OnClosed();
        void ShowError(std::string const& error);
        // Transport with optimistic UI (rolled back if the host does not confirm).
        void TogglePlay();
        void SeekTo(int64_t positionMs);
        void SeekBy(int64_t deltaMs);
        void AdjustLyricsOffset(int64_t deltaMs);
        bool OnKey(winrt::Windows::System::VirtualKey key, Microsoft::UI::Xaml::DependencyObject const& source);

        mdui::WindowHost m_host;
        std::vector<uint32_t> m_tokens;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        mdui::ArtworkSlot m_art;
        mdui::AmbientBackground m_ambient;
        std::optional<mdui::Session> m_session;
        bool m_dirty{ false };
        bool m_closed{ false };
        bool m_a2dpRequested{ false };   // the quick toggle asked for it: report a failure once
        bool m_settingsShown{ false };
        bool m_cardCompact{ false };   // short window: optional card rows collapsed
        SettingsPage* m_settings{ nullptr };   // implementation of m_settingsView (owned by the XAML tree)
        MediaDuetUI::SettingsPage m_settingsView{ nullptr };
        VolumeUi m_inlineVolume;
        VolumeUi m_localVolume;
        VolumeUi m_remoteVolume;
        VolumeUi m_appVolume;
        bool m_dragging{ false };
        bool m_updatingSlider{ false };
        int64_t m_lastShownSecond{ -2 };

        // Timing: smoothed lyric clock, optimistic seek / play-pause, keyboard seek coalescing.
        md::lyrics::LyricsClock m_clock;
        std::string m_clockTrack;
        std::string m_versionPair;   // "pc|phone" of the version warning shown (dismissal key)   // clock track key: per song (SameSong), not per trackId
        mdui::SeekHold m_seekHold;
        mdui::OptimisticFlag m_playHold;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_playHoldTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_keySeekTimer{ nullptr };
        bool m_keySeekPending{ false };
        int64_t m_keySeekTarget{ 0 };

        // Lyrics view state
        mdui::LyricsView m_lyricsView;
        bool m_lyricsShown{ false };
        bool m_lyricsSynced{ false };
        bool m_translationOn{ true };   // settings lyrics.translation
        void ApplyTranslationSetting(bool on);
        std::string m_lyricsIdentity;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_holdTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_layoutTimer{ nullptr };
        bool m_holding{ false };       // old lyrics faded, waiting (<= 1.5 s) for the next track's
        bool m_holdExpired{ false };
        // Per-frame loop
        bool m_rendering{ false };
        double m_lastFrameMs{ 0 };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_frameTimer{ nullptr };
    };
}

namespace winrt::MediaDuetUI::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
