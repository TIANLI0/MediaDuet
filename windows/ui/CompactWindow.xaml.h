#pragma once

#include "CompactWindow.g.h"

#include "Ambient.h"
#include "Artwork.h"
#include "Model.h"
#include "PlaybackClock.h"
#include "WindowUtil.h"

#include "Karaoke.h"

namespace winrt::MediaDuetUI::implementation
{
    struct CompactWindow : CompactWindowT<CompactWindow>
    {
        CompactWindow() = default;
        void InitializeComponent();

    private:
        void RefreshAll();
        void UpdateLyrics();
        void Tick(uint64_t now);
        void UpdateLyricLine(double positionMs, double nowQpcMs);
        void EnsureLyricSlots();
        void ApplyLyricColors();
        void OnFrame();
        void UpdateFrameLoop();
        void UpdateTimer();
        void ApplyAppearance();
        void UpdateControlsVisibility();
        bool ControlsHaveKeyboardFocus();
        void CheckHover();
        void TogglePlay();
        void UpdatePlayState();
        double LyricPosition(double nowQpcMs) const;
        Microsoft::UI::Xaml::Media::Brush PillBrush();
        void PlaceInitially();
        void EnsureOnScreen();
        void SavePosition();
        void OnClosed();
        std::optional<LRESULT> OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

        mdui::WindowHost m_host;
        std::vector<uint32_t> m_tokens;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_hoverTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_pillHideTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_leaveTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_playHoldTimer{ nullptr };
        Microsoft::UI::Xaml::Media::Brush m_pillBrush{ nullptr };
        bool m_pillBrushDark{ false };
        mdui::ArtworkSlot m_art;
        mdui::AmbientBackground m_ambient;
        std::optional<mdui::Session> m_session;
        // Lyric clock (smoothed, minus the session's lyricsDelayMs) and optimistic play/pause.
        md::lyrics::LyricsClock m_clock;
        std::string m_clockTrack;   // clock track key: per song (SameSong), not per trackId
        mdui::OptimisticFlag m_playHold;
        bool m_dirty{ false };
        bool m_closed{ false };
        bool m_hover{ false };
        bool m_controlsShown{ false };

        // Current lyric line (synced lyrics of the focused track only).
        bool m_lyricsShown{ false };
        bool m_lyricsDark{ false };
        // Translation (settings lyrics.translation): shown as the second lyric row; the artist line
        // gives way to it so everything still fits into the 96 px window.
        bool m_translationOn{ true };
        bool m_hasArtistLine{ false };
        bool m_lyricLineVisible{ false };
        void UpdateSubLabel();
        std::string m_lyricsIdentity;
        std::vector<mdui::LyricLine> m_lines;
        md::lyrics::LyricRows m_rows;   // of m_lines (shared row model: gaps, end of lyrics)
        int m_activeLine{ -2 };
        std::unique_ptr<mdui::KaraokeLine> m_slots[2];
        int m_front{ 0 };
        double m_transitionStart{ -1 };
        bool m_rendering{ false };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_frameTimer{ nullptr };
    };
}

namespace winrt::MediaDuetUI::factory_implementation
{
    struct CompactWindow : CompactWindowT<CompactWindow, implementation::CompactWindow>
    {
    };
}
