#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Controller.h"
#include "Karaoke.h"
#include "RenderTest.h"
#include "SettingsPage.xaml.h"
#include "Strings.h"

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.h>

#include <algorithm>

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace mdui;
using winrt::Windows::System::VirtualKey;
using winrt::Windows::UI::Color;

namespace
{
    constexpr int64_t kKeySeekMs = 5000;        // Left / Right
    constexpr int64_t kOffsetStepMs = 100;      // [ / ] and the offset flyout
    constexpr int64_t kOffsetLimitMs = 5000;    // lyrics.offsets range (host validates)
    constexpr auto kVkOpenBracket = static_cast<VirtualKey>(219);    // VK_OEM_4  [
    constexpr auto kVkCloseBracket = static_cast<VirtualKey>(221);   // VK_OEM_6  ]

    std::wstring TransportName(std::string const& t)
    {
        if (t == "bt") return std::wstring(Str(L"TransportBluetooth"));
        if (t == "lan") return std::wstring(Str(L"TransportLan"));
        return Widen(t);
    }

    SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush(Color{ a, r, g, b });
    }

    // How long host echoes are ignored after a volume change: the phone's result waits for the
    // phone itself (up to 3 s, docs/ipc.md volume.set), this PC answers at once.
    double EchoWindowMs(Side side)
    {
        return side == Side::Remote ? 3200.0 : 800.0;
    }

    std::wstring OffsetText(int64_t ms)
    {
        return StrF(L"OffsetSeconds", { FormatOffsetSeconds(ms) });
    }
}

namespace winrt::MediaDuetUI::implementation
{
    void MainWindow::InitializeComponent()
    {
        render::LogLine(L"main: InitializeComponent");
        MainWindowT::InitializeComponent();
        render::LogLine(L"main: XAML loaded");
        auto& c = Controller::Get();

        Title(Str(L"AppName"));
        TitleText().Text(Str(L"AppName"));
        ExtendsContentIntoTitleBar(true);
        // Only the drag region is caption; the back button left of it stays interactive.
        SetTitleBar(DragRegion());
        AppWindow().TitleBar().PreferredHeightOption(TitleBarHeightOption::Tall);
        TitleIcon().Source(BitmapImage(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/MediaDuet.png")));

        m_host.Attach(*this, Root(), SIZE{ 760, 560 });
        m_host.showingChanged = [this] {
            if (m_host.IsShowing() && m_dirty) RefreshAll();
            HostProgress().IsActive(m_host.IsShowing() && !m_settingsShown && Controller::Get().Link() == HostLink::Connecting);
            UpdateTimer();
            if (m_settings) m_settings->SetActive(m_settingsShown && m_host.IsShowing());
        };
        PlaceInitially();
        m_ambient.Attach(TintLayer(), ambient::Shape::Main);

        m_timer = DispatcherQueue().CreateTimer();
        m_timer.IsRepeating(true);
        m_timer.Tick([this](auto&&, auto&&) { UpdateProgress(TickNow()); });

        // Optimistic play/pause: if the host never confirms, the next refresh rolls the button back.
        m_playHoldTimer = DispatcherQueue().CreateTimer();
        m_playHoldTimer.IsRepeating(false);
        m_playHoldTimer.Interval(std::chrono::milliseconds(static_cast<int64_t>(OptimisticFlag::kTimeoutMs) + 50));
        m_playHoldTimer.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get(); self && !self->m_closed) self->RefreshAll();
        });
        // Keyboard seeking (slider arrows, Left/Right) is coalesced: one seek after the keys settle.
        m_keySeekTimer = DispatcherQueue().CreateTimer();
        m_keySeekTimer.IsRepeating(false);
        m_keySeekTimer.Interval(std::chrono::milliseconds(250));
        m_keySeekTimer.Tick([weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || self->m_closed || !self->m_keySeekPending) return;
            self->m_keySeekPending = false;
            self->SeekTo(self->m_keySeekTarget);
        });

        // Lyrics layout transitions: hold while the next track's lyrics load, then finish a
        // "lyrics off" transition once the pane has faded out.
        m_holdTimer = DispatcherQueue().CreateTimer();
        m_holdTimer.IsRepeating(false);
        m_holdTimer.Interval(std::chrono::milliseconds(1500));
        m_holdTimer.Tick([weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || self->m_closed) return;
            self->m_holding = false;
            self->m_holdExpired = true;
            self->UpdateLyrics();
        });
        m_layoutTimer = DispatcherQueue().CreateTimer();
        m_layoutTimer.IsRepeating(false);
        m_layoutTimer.Interval(std::chrono::milliseconds(220));
        m_layoutTimer.Tick([weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || self->m_closed || !self->m_lyricsShown) return;
            auto before = self->CardOrigin();
            self->m_lyricsShown = false;
            self->LyricsHost().Visibility(Visibility::Collapsed);
            self->m_lyricsView.Clear();
            self->UpdateLayout();
            self->PlayerGrid().UpdateLayout();
            self->UpdateLayout();
            self->PlayerGrid().UpdateLayout();
            auto after = self->CardOrigin();
            ResetVisual(self->LyricsHost());
            render::LogLine(L"layout off: card moved by " + std::to_wstring(before.X - after.X) + L", " + std::to_wstring(before.Y - after.Y));
            AnimateVisualTranslation(self->CardScroller(), { before.X - after.X, before.Y - after.Y, 0 }, { 0, 0, 0 }, 420);
            self->UpdateLyricsOffset();
            self->UpdateFrameLoop();
        });

        // Caption-button inset of the custom title bar.
        AppTitleBar().SizeChanged([this](auto&&, auto&&) {
            double scale = m_host.Scale();
            CaptionInsetColumn().Width(GridLength{ AppWindow().TitleBar().RightInset() / scale, GridUnitType::Pixel });
        });
        Root().SizeChanged([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get(); self && !self->m_closed) self->UpdateLayout();
        });
        CardScroller().SizeChanged([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get(); self && !self->m_closed) self->UpdateLayout();
        });
        Card().SizeChanged([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get(); self && !self->m_closed) self->UpdateLayout();
        });

        // Lyrics view: clicking a line seeks there.
        LyricsView::Callbacks callbacks;
        callbacks.canSeek = [this] { return !m_closed && m_session && m_session->CanSeek(); };
        callbacks.seek = [this](int64_t t) { SeekTo(t); };
        m_lyricsView.Attach(LyricsViewport(), LyricsCanvas(), LyricsPanel(), LyricsFollowButton(), std::move(callbacks));

        // Settings sub-page navigation
        BackButton().Click([this](auto&&, auto&&) { ShowPlayer(); });
        for (auto key : { VirtualKey::GoBack, VirtualKey::Left })
        {
            Microsoft::UI::Xaml::Input::KeyboardAccelerator accel;
            accel.Key(key);
            if (key == VirtualKey::Left) accel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Menu);
            accel.Invoked([this](auto&&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& e) {
                if (m_settingsShown)
                {
                    ShowPlayer();
                    e.Handled(true);
                }
            });
            Root().KeyboardAccelerators().Append(accel);
        }
        // Player shortcuts: Space play/pause, Left/Right seek 5 s, [ / ] lyrics earlier/later.
        // Bubbling KeyDown: focused controls that use these keys themselves (buttons, sliders,
        // text boxes) handle them first.
        Root().KeyDown([this](auto&&, KeyRoutedEventArgs const& e) {
            if (e.Handled() || m_closed || m_settingsShown) return;
            if (OnKey(e.Key(), e.OriginalSource().try_as<DependencyObject>())) e.Handled(true);
        });

        // Volume: inline (device of the focused session) + flyout with both devices
        SetupVolume(m_inlineVolume, Side::Local, InlineVolumeSlider(), InlineMuteButton(), InlineMuteIcon(), InlineVolumeText());
        SetupVolume(m_localVolume, Side::Local, LocalVolumeSlider(), LocalMuteButton(), LocalMuteIcon(), LocalVolumeText());
        SetupVolume(m_remoteVolume, Side::Remote, RemoteVolumeSlider(), RemoteMuteButton(), RemoteMuteIcon(), RemoteVolumeText());
        SetupVolume(m_appVolume, Side::Local, AppVolumeSlider(), AppMuteButton(), AppMuteIcon(), AppVolumeText());
        VolumeFlyout().Opening([this](auto&&, auto&&) { UpdateVolume(); });

        // Transport
        PrevButton().Click([this](auto&&, auto&&) {
            if (m_session) Controller::Get().Media(m_session->side, m_session->key, "previous");
        });
        NextButton().Click([this](auto&&, auto&&) {
            if (m_session) Controller::Get().Media(m_session->side, m_session->key, "next");
        });
        PlayPauseButton().Click([this](auto&&, auto&&) { TogglePlay(); });

        // Seek: only on release (drag) or keyboard changes (coalesced).
        ProgressSlider().SmallChange(5000);
        ProgressSlider().LargeChange(15000);
        ProgressSlider().AddHandler(UIElement::PointerPressedEvent(),
            box_value(PointerEventHandler([this](auto&&, auto&&) { m_dragging = true; })), true);
        auto endDrag = [this](auto&&, auto&&) {
            if (!m_dragging) return;
            m_dragging = false;
            SeekTo(static_cast<int64_t>(ProgressSlider().Value()));
        };
        ProgressSlider().AddHandler(UIElement::PointerReleasedEvent(), box_value(PointerEventHandler(endDrag)), true);
        ProgressSlider().AddHandler(UIElement::PointerCaptureLostEvent(), box_value(PointerEventHandler(endDrag)), true);
        ProgressSlider().ValueChanged([this](auto&&, RangeBaseValueChangedEventArgs const& e) {
            if (m_updatingSlider) return;
            ElapsedLabel().Text(FormatTime(static_cast<int64_t>(e.NewValue())));
            m_lastShownSecond = -2;
            if (m_dragging) return;
            // Keyboard (arrow keys / PageUp / PageDown) on the focused slider.
            if (!m_session || !m_session->CanSeek()) return;
            m_keySeekTarget = static_cast<int64_t>(e.NewValue());
            m_keySeekPending = true;
            m_keySeekTimer.Stop();
            m_keySeekTimer.Start();
        });

        SourceFlyout().Opening([this](auto&&, auto&&) { BuildSourceMenu(); });
        OverlayToggle().Click([this](auto&&, auto&&) {
            Controller::Get().SetLyricsOverlay(OverlayToggle().IsChecked().Value());
        });
        TranslationButton().Click([this](auto&&, auto&&) {
            bool on = TranslationButton().IsChecked().Value();
            ApplyTranslationSetting(on);   // immediately; the host echoes the setting in its next state
            Controller::Get().SetSetting("lyrics.translation", on);
        });
        // Lyrics timing
        LyricsEarlierButton().Click([this](auto&&, auto&&) { AdjustLyricsOffset(-kOffsetStepMs); });
        LyricsLaterButton().Click([this](auto&&, auto&&) { AdjustLyricsOffset(kOffsetStepMs); });
        LyricsOffsetResetButton().Click([this](auto&&, auto&&) {
            if (m_session && !m_session->appId.empty()) Controller::Get().ResetLyricsOffset(m_session->appId);
        });
        LyricsOffsetFlyout().Opening([this](auto&&, auto&&) { UpdateLyricsOffset(); });

        CompactButton().Click([](auto&&, auto&&) { Controller::Get().SwitchToCompact(); });
        SettingsButton().Click([](auto&&, auto&&) { Controller::Get().ShowSettings(L""); });
        EmptyPairButton().Click([](auto&&, auto&&) { Controller::Get().ShowSettings(L"pair"); });
        // Work mode (settings "mode"; "modeChosen" ends the first-run choice).
        EmptyStandaloneButton().Click([](auto&&, auto&&) {
            Controller::Get().SetSettings(json{ { "mode", "standalone" }, { "modeChosen", true } });
        });
        WelcomeLinkedButton().Click([](auto&&, auto&&) {
            Controller::Get().SetSettings(json{ { "mode", "linked" }, { "modeChosen", true } });
            Controller::Get().ShowSettings(L"pair");
        });
        WelcomeStandaloneButton().Click([](auto&&, auto&&) {
            Controller::Get().SetSettings(json{ { "mode", "standalone" }, { "modeChosen", true } });
        });
        WelcomeIcon().Source(BitmapImage(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/MediaDuet.png")));
        // Dismissed per version pair: a different phone / PC version shows it again.
        VersionBar().CloseButtonClick([this](auto&&, auto&&) {
            if (!m_versionPair.empty()) Controller::Get().SetSetting("ui.versionNoticeDismissed", m_versionPair);
        });
        StartHostButton().Click([this](auto&&, auto&&) {
            if (!Controller::Get().LaunchHost()) UpdateLink();
        });

        auto weak = get_weak();
        auto refresh = [weak] {
            if (auto self = weak.get()) self->RefreshAll();
        };
        m_tokens.push_back(c.Subscribe(Event::State, refresh));
        m_tokens.push_back(c.Subscribe(Event::Link, refresh));
        m_tokens.push_back(c.Subscribe(Event::Lyrics, [weak] {
            if (auto self = weak.get())
            {
                if (!self->m_host.IsShowing() || self->m_settingsShown) { self->m_dirty = true; return; }
                self->UpdateLyrics();
                self->UpdateTimer();
            }
        }));
        m_tokens.push_back(c.Subscribe(Event::Appearance, [weak] {
            if (auto self = weak.get()) self->ApplyAppearance();
        }));
        m_tokens.push_back(c.SubscribeResults([weak](bool ok, std::string const& error, std::string const& type) {
            auto self = weak.get();
            if (!self || self->m_closed) return;
            if (!ok && type == "media.command")
            {
                // Roll back optimistic play/pause and seek at once.
                self->m_playHold.Clear();
                self->m_seekHold.Clear();
                self->RefreshAll();
            }
            if (type == "volume.set" && ok)
            {
                // Confirmed: the next state carries the new level, stop ignoring echoes soon.
                double soon = QpcNowMs() + 300;
                for (auto* v : { &self->m_inlineVolume, &self->m_remoteVolume })
                    if (v->side == Side::Remote && !v->dragging && !v->pending) v->ignoreUntil = std::min(v->ignoreUntil, soon);
                self->UpdateVolume();
                return;
            }
            if (type == "volume.set" && !ok)
            {
                // The host refused (e.g. the phone blocks background volume changes): show the real
                // level again instead of the value the slider was dragged to.
                for (auto* v : { &self->m_inlineVolume, &self->m_localVolume, &self->m_remoteVolume, &self->m_appVolume })
                {
                    v->pending.reset();
                    v->ignoreUntil = 0;
                    if (v->timer) v->timer.Stop();
                }
                self->UpdateVolume();
                return;   // shown as an inline hint, not as an error bar
            }
            if (!ok && (type == "media.command" || type == "focus.set" || type == "lyrics.overlay" || type == "lyrics.offset"))
                self->ShowError(error);
        }));

        Closed([this](auto&&, auto&&) { OnClosed(); });

        render::LogLine(L"main: appearance");
        ApplyAppearance();
        render::LogLine(L"main: refresh");
        RefreshAll();
        render::LogLine(L"main: ready");
    }

    // ---- Placement -------------------------------------------------------------------------
    //
    // ui.main.{x, y, width, height} (physical pixels) and ui.main.maximized are restored; a saved
    // rectangle that is no longer on any monitor falls back to the centred default.

    void MainWindow::PlaceInitially()
    {
        if (render::Active()) return;   // placed off-screen by the render test
        double scale = m_host.Scale();
        int w = static_cast<int>(1040 * scale), h = static_cast<int>(760 * scale);
        auto const& settings = Controller::Get().Model().settings;
        auto x = JPath(settings, "ui.main.x");
        auto y = JPath(settings, "ui.main.y");
        auto sw = JPath(settings, "ui.main.width");
        auto sh = JPath(settings, "ui.main.height");
        if (sw && sh && sw->is_number() && sh->is_number())
        {
            w = std::max(static_cast<int>(760 * scale), static_cast<int>(sw->get<double>()));
            h = std::max(static_cast<int>(560 * scale), static_cast<int>(sh->get<double>()));
        }
        RECT r{};
        if (x && y && x->is_number() && y->is_number())
        {
            r = RECT{ x->get<LONG>(), y->get<LONG>(), x->get<LONG>() + w, y->get<LONG>() + h };
            if (IsRectOnAnyMonitor(r)) r = ClampToWorkArea(r);
            else r = RECT{};
        }
        if (r.right == 0 && r.bottom == 0)
        {
            POINT origin{ 0, 0 };
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
            auto const& wa = mi.rcWork;
            w = std::min<int>(w, wa.right - wa.left);
            h = std::min<int>(h, wa.bottom - wa.top);
            r.left = wa.left + (wa.right - wa.left - w) / 2;
            r.top = wa.top + (wa.bottom - wa.top - h) / 2;
            r.right = r.left + w;
            r.bottom = r.top + h;
        }
        AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ r.left, r.top, r.right - r.left, r.bottom - r.top });
        if (Controller::Get().Model().Setting("ui.main.maximized", false))
            if (auto presenter = AppWindow().Presenter().try_as<OverlappedPresenter>()) presenter.Maximize();
    }

    void MainWindow::SavePosition()
    {
        if (render::Active()) return;
        HWND hwnd = m_host.Hwnd();
        if (!hwnd || IsIconic(hwnd)) return;
        auto const& st = Controller::Get().Model();
        if (IsZoomed(hwnd))
        {
            // Keep the last normal rectangle; only remember that the window was maximised.
            if (!st.Setting("ui.main.maximized", false)) Controller::Get().SetSettings(json{ { "ui", { { "main", { { "maximized", true } } } } } });
            return;
        }
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return;
        auto same = [&](char const* path, LONG v) {
            auto j = JPath(st.settings, path);
            return j && j->is_number() && j->get<LONG>() == v;
        };
        if (same("ui.main.x", r.left) && same("ui.main.y", r.top) && same("ui.main.width", r.right - r.left) &&
            same("ui.main.height", r.bottom - r.top) && !st.Setting("ui.main.maximized", false))
            return;
        Controller::Get().SetSettings(json{ { "ui", { { "main", { { "x", r.left }, { "y", r.top }, { "width", r.right - r.left },
            { "height", r.bottom - r.top }, { "maximized", false } } } } } });
    }

    void MainWindow::OnClosed()
    {
        m_closed = true;
        SavePosition();
        if (m_timer) m_timer.Stop();
        if (m_frameTimer) m_frameTimer.Stop();
        if (m_playHoldTimer) m_playHoldTimer.Stop();
        if (m_keySeekTimer) m_keySeekTimer.Stop();
        if (m_holdTimer) m_holdTimer.Stop();
        if (m_layoutTimer) m_layoutTimer.Stop();
        m_rendering = false;
        auto& c = Controller::Get();
        for (auto t : m_tokens) c.Unsubscribe(t);
        m_tokens.clear();
        m_host.Detach();
        m_ambient.Detach();
        m_lyricsView.Detach();
        m_art.Clear();
        ArtBrush().ImageSource(nullptr);
        if (m_settings)
        {
            m_settings->Close();
            m_settings = nullptr;
        }
        for (auto* v : { &m_inlineVolume, &m_localVolume, &m_remoteVolume, &m_appVolume })
            if (v->timer) v->timer.Stop();
        c.WindowClosed(WindowKind::Main, *this);
    }

    // ---- Appearance ------------------------------------------------------------------------

    void MainWindow::ApplyAppearance()
    {
        auto& c = Controller::Get();
        m_host.ApplyTheme(c.Theme());
        bool animations = c.AnimationsEnabled();
        auto transition = [animations]() -> ScalarTransition {
            if (!animations) return nullptr;
            ScalarTransition t;
            t.Duration(std::chrono::milliseconds(350));
            return t;
        };
        ArtImageBorder().OpacityTransition(transition());
        m_ambient.SetAnimations(animations);
        // Line transitions depend on the animation setting: rebuild the lyric visuals.
        if (!m_lyricsIdentity.empty())
        {
            m_lyricsIdentity.clear();
            UpdateLyrics();
        }
        if (auto presenter = AppWindow().Presenter().try_as<OverlappedPresenter>(); presenter && !render::Active())
            presenter.IsAlwaysOnTop(c.Model().Setting("ui.alwaysOnTop", false));
    }

    void MainWindow::UpdateLayout()
    {
        if (m_closed) return;
        // Responsive composition (Apple Music desktop / full-screen player): the content is capped
        // and centred in large windows, the card column and the artwork grow with the window, the
        // lyrics column gets a comfortable reading width (LyricsHost MaxWidth) centred in its column.
        double pageWidth = PlayerPage().ActualWidth();
        double maxContent = m_lyricsShown ? 1760.0 : 1240.0;
        if (std::abs(PlayerGrid().MaxWidth() - maxContent) > 0.5) PlayerGrid().MaxWidth(maxContent);
        double content = std::max(0.0, std::min(pageWidth, maxContent) - 44.0);   // grid padding
        double cardColumn = std::clamp(content * 0.38, 392.0, 720.0);
        CardColumn().Width(m_lyricsShown ? GridLength{ cardColumn, GridUnitType::Pixel } : GridLength{ 1, GridUnitType::Star });
        LyricsColumn().Width(m_lyricsShown ? GridLength{ 1, GridUnitType::Star } : GridLength{ 0, GridUnitType::Pixel });
        double viewport = CardScroller().ActualHeight() - 16;   // card margin
        double available = CardScroller().ActualWidth();
        if (viewport <= 0 || available <= 0) return;
        // Explicit width: the card never relies on stretching inside the ScrollViewer, so its right
        // edge can never be clipped. It grows with the window (360 -> 680 epx).
        double cardMax = std::clamp(std::min(available - 24.0, viewport * 0.62), 360.0, 680.0);
        double cardWidth = std::clamp(available - 24.0, 200.0, cardMax);
        if (std::abs(Card().Width() - cardWidth) > 0.5) Card().Width(cardWidth);
        // Text grows with the card (22 / 26 / 30 epx titles).
        int step = cardWidth >= 600 ? 2 : cardWidth >= 440 ? 1 : 0;
        double titleSize = 22.0 + 4.0 * step;
        if (TitleLabel().FontSize() != titleSize)
        {
            TitleLabel().FontSize(titleSize);
            ArtistLabel().FontSize(15.0 + 2.0 * step);
            AlbumLabel().FontSize(13.0 + 1.0 * step);
            StatusLabel().FontSize(12.0 + 1.0 * step);
        }

        // Short windows: drop the least important rows first (album; inline volume, which is also in
        // the volume flyout); the artwork then shrinks; only below that the card scrolls.
        bool compact = viewport < 600;
        if (compact != m_cardCompact)
        {
            m_cardCompact = compact;
            TitleLabel().MaxLines(compact ? 1 : 2);
            TitleLabel().Margin(Thickness{ 0, compact ? 12.0 : 24.0, 0, 0 });
            UpdateCard();
            UpdateVolume();
            return;   // re-run from Card().SizeChanged with the new content height
        }
        double nonArt = Card().ActualHeight() - ArtFrame().ActualHeight();
        if (nonArt <= 0) return;
        double art = std::clamp(viewport - nonArt, 120.0, cardWidth);
        if (std::abs(ArtFrame().Width() - art) > 1.0)
        {
            ArtFrame().Width(art);
            ArtFrame().Height(art);
        }
    }

    winrt::Windows::Foundation::Point MainWindow::CardOrigin()
    {
        // The card is centred in its ScrollViewer, and the ScrollViewer applies that centring as a
        // render offset (not visible in layout offsets right after UpdateLayout()), so the card's
        // place is taken as the centre of its scroller - which is where it is drawn.
        auto o = CardScroller().ActualOffset();
        return { static_cast<float>(o.x + CardScroller().ActualWidth() / 2), static_cast<float>(o.y + CardScroller().ActualHeight() / 2) };
    }

    bool MainWindow::TransitionsEnabled() const
    {
        return Controller::Get().AnimationsEnabled() && SystemAnimationsEnabled() && m_host.IsShowing();
    }

    // One column (no lyrics) <-> two columns (card + lyrics), without a layout jump: the lyrics pane
    // fades / slides, and the card moves from its old place to its new one (FLIP: the layout
    // changes at once, a composition animation runs the card from its old offset back to zero).
    void MainWindow::SetLyricsLayout(bool on, bool animate)
    {
        if (m_layoutTimer.IsRunning())
        {
            // A pending "lyrics off" (pane fading out) is cancelled by "lyrics on" and vice versa.
            m_layoutTimer.Stop();
            if (on && m_lyricsShown)
            {
                if (animate && TransitionsEnabled()) AnimateVisualOpacity(LyricsHost(), 0.0f, 1.0f, 220);
                else ResetVisual(LyricsHost());
                return;
            }
        }
        if (on == m_lyricsShown) return;
        animate = animate && TransitionsEnabled();
        auto apply = [this](bool show) {
            auto before = CardOrigin();
            m_lyricsShown = show;
            LyricsHost().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            UpdateLayout();
            PlayerGrid().UpdateLayout();
            UpdateLayout();   // card / artwork sizes depend on the new column width
            PlayerGrid().UpdateLayout();
            auto after = CardOrigin();
            return winrt::Windows::Foundation::Numerics::float3{ before.X - after.X, before.Y - after.Y, 0 };
        };
        if (!animate)
        {
            apply(on);
            ResetVisual(LyricsHost());
            ResetVisual(CardScroller());
            if (!on) m_lyricsView.Clear();
            return;
        }
        if (on)
        {
            auto delta = apply(true);
            render::LogLine(L"layout on: card moved by " + std::to_wstring(delta.x) + L", " + std::to_wstring(delta.y));
            AnimateVisualTranslation(CardScroller(), delta, { 0, 0, 0 }, 420);
            AnimateVisualOpacity(LyricsHost(), 0.0f, 1.0f, 360, 120);
            AnimateVisualTranslation(LyricsHost(), { 40, 0, 0 }, { 0, 0, 0 }, 420, 60);
            return;
        }
        // Off: the pane fades out first, then the card glides to the centre.
        AnimateVisualOpacity(LyricsHost(), 1.0f, 0.0f, 200);
        AnimateVisualTranslation(LyricsHost(), { 0, 0, 0 }, { 32, 0, 0 }, 220);
        m_layoutTimer.Interval(std::chrono::milliseconds(static_cast<int>(220 * render::SlowMotion())));
        m_layoutTimer.Start();
    }

    // ---- State -----------------------------------------------------------------------------

    void MainWindow::RefreshAll()
    {
        if (m_closed) return;
        if (!m_host.IsShowing() || m_settingsShown)
        {
            // The player is not visible: only the title-bar connection pill is kept current.
            if (m_host.IsShowing() && Controller::Get().Link() == HostLink::Connected) UpdateConnection();
            else if (Controller::Get().Link() != HostLink::Connected) ConnectionPill().Visibility(Visibility::Collapsed);
            m_dirty = true;
            return;
        }
        m_dirty = false;
        UpdateLink();
        UpdateConnection();
        UpdateCard();
        UpdateWelcome();
        UpdateVersionBar();
        UpdatePolicy();
        UpdateLyrics();
        auto const& st = Controller::Get().Model();
        OverlayToggle().IsChecked(st.overlayVisible);
        UpdateVolume();
        UpdateTimer();
    }

    void MainWindow::UpdateLink()
    {
        auto& c = Controller::Get();
        bool connected = c.Link() == HostLink::Connected && c.Model().valid;
        HostPanel().Visibility(connected ? Visibility::Collapsed : Visibility::Visible);
        OverlayToggle().IsEnabled(connected);
        if (connected)
        {
            HostProgress().IsActive(false);
            return;
        }
        PlayerGrid().Visibility(Visibility::Collapsed);
        EmptyPanel().Visibility(Visibility::Collapsed);
        ConnectionPill().Visibility(Visibility::Collapsed);
        PolicyPanel().Visibility(Visibility::Collapsed);

        bool connecting = c.Link() == HostLink::Connecting;
        HostProgress().IsActive(connecting && m_host.IsShowing());
        HostProgress().Visibility(connecting ? Visibility::Visible : Visibility::Collapsed);
        HostIcon().Visibility(connecting ? Visibility::Collapsed : Visibility::Visible);
        HostTitle().Text(connecting ? Str(L"HostConnecting") : Str(L"HostNotRunningTitle"));
        bool exists = c.HostExecutableExists();
        HostText().Text(connecting ? hstring{} : (exists ? Str(L"HostNotRunningText") : Str(L"HostMissingText")));
        HostText().Visibility(connecting ? Visibility::Collapsed : Visibility::Visible);
        StartHostButton().Visibility(connecting ? Visibility::Collapsed : Visibility::Visible);
        StartHostButton().IsEnabled(exists);
    }

    void MainWindow::UpdateConnection()
    {
        auto& c = Controller::Get();
        if (c.Link() != HostLink::Connected || !c.Model().valid) return;
        auto const& st = c.Model();
        auto const& cn = st.connection;
        if (st.standalone)
        {
            // This PC only: there is no phone to report on.
            ConnectionPill().Visibility(Visibility::Collapsed);
            return;
        }
        std::wstring peer = Widen(st.PeerDisplayName());
        if (peer.empty()) peer = Str(L"YourPhone");

        std::wstring text;
        wchar_t const* glyph = L"\xE8EA";
        SolidColorBrush dot = MakeBrush(255, 138, 138, 138);
        if (cn.status == "connected")
        {
            glyph = cn.transport == "bt" ? L"\xE702" : L"\xE839";
            dot = MakeBrush(255, 46, 173, 75);
            text = peer + L"  \x00B7  " + TransportName(cn.transport);
            if (!cn.secondary.empty() && cn.secondary != cn.transport) text += L" + " + TransportName(cn.secondary);
            if (cn.rttMs >= 0) text += L"  \x00B7  " + StrF(L"Milliseconds", { std::to_wstring(cn.rttMs) });
        }
        else if (cn.status == "connecting")
        {
            glyph = L"\xE895";
            dot = MakeBrush(255, 227, 162, 26);
            text = StrF(L"ConnConnectingTo", { peer });
        }
        else if (cn.status == "disconnected")
        {
            text = StrF(L"ConnDisconnectedFrom", { peer });
        }
        else
        {
            text = Str(L"ConnNoPeer");
        }
        ConnectionIcon().Glyph(glyph);
        ConnectionDot().Fill(dot);
        ConnectionText().Text(text);
        ToolTipService::SetToolTip(ConnectionPill(), box_value(cn.lastError.empty() ? text : text + L"\n" + Widen(cn.lastError)));
        ConnectionPill().Visibility(Visibility::Visible);
    }

    void MainWindow::UpdateCard()
    {
        auto& c = Controller::Get();
        if (c.Link() != HostLink::Connected || !c.Model().valid)
        {
            m_session.reset();
            m_clock.reset();
            return;
        }
        auto const& st = c.Model();
        auto s = st.FocusedSession();
        if (!s)
        {
            m_session.reset();
            m_clock.reset();
            m_seekHold.Clear();
            m_playHold.Clear();
            PlayerGrid().Visibility(Visibility::Collapsed);
            EmptyPanel().Visibility(Visibility::Visible);
            bool noPeer = !st.standalone && st.connection.status == "no_peer" && st.devices.empty();
            auto peer = Widen(st.PeerDisplayName());
            EmptyHint().Text(st.standalone ? std::wstring(Str(L"EmptyHintStandalone"))
                             : noPeer      ? std::wstring(Str(L"EmptyHintNoPeer"))
                                           : StrF(L"EmptyHint", { peer.empty() ? std::wstring(Str(L"YourPhone")) : peer }));
            EmptyPairButton().Visibility(noPeer ? Visibility::Visible : Visibility::Collapsed);
            EmptyStandaloneButton().Visibility(noPeer ? Visibility::Visible : Visibility::Collapsed);
            m_art.Request(DispatcherQueue(), {}, 0, nullptr, nullptr);
            ArtBrush().ImageSource(nullptr);
            m_ambient.SetPalette({});
            return;
        }

        // A new trackId for the same song (players re-reporting the duration) is not a track change.
        bool trackChanged = !m_session || !SameSong(*m_session, *s);
        if (trackChanged) m_clockTrack = s->key + "|" + s->trackId;
        double now = QpcNowMs();
        double shownBefore = m_session && !trackChanged ? m_session->PositionPrecise(now) : -1;
        m_session = *s;
        if (trackChanged)
        {
            m_seekHold.Clear();
            m_playHold.Clear();
            m_keySeekPending = false;
        }
        // Optimistic play/pause: keep showing the requested state until the host confirms it
        // (an optimistic pause keeps the position where it was frozen).
        bool hostPlaying = s->IsPlaying();
        bool shownPlaying = m_playHold.Resolve(hostPlaying, now);
        if (shownPlaying != hostPlaying)
        {
            double pos = !shownPlaying && shownBefore >= 0 ? shownBefore : s->PositionPrecise(now);
            m_session->status = shownPlaying ? "playing" : "paused";
            if (pos >= 0) m_session->Rebase(static_cast<int64_t>(std::llround(pos)));
        }
        // Optimistic seek: a report that predates the seek must not throw the position back.
        if (m_seekHold.Active())
        {
            bool playing = m_session->IsPlaying();
            if (m_seekHold.Hold(m_session->PositionPrecise(now), now, playing, m_session->rate))
                m_session->Rebase(static_cast<int64_t>(std::llround(m_seekHold.Expected(now, playing, m_session->rate))));
        }
        // Lyrics use the smoothed clock (the progress bar stays exact).
        // Same clock as the host overlay (md::lyrics::LyricsClock); a track change snaps it.
        m_clock.update(SessionReport(*m_session, m_clockTrack), now);

        EmptyPanel().Visibility(Visibility::Collapsed);
        PlayerGrid().Visibility(Visibility::Visible);

        TitleLabel().Text(s->title.empty() ? std::wstring(Str(L"UnknownTitle")) : Widen(s->title));
        ToolTipService::SetToolTip(TitleLabel(), s->title.empty() ? nullptr : box_value(Widen(s->title)));
        ArtistLabel().Text(Widen(s->artist));
        ArtistLabel().Visibility(s->artist.empty() ? Visibility::Collapsed : Visibility::Visible);
        AlbumLabel().Text(Widen(s->album));
        AlbumLabel().Visibility(s->album.empty() || m_cardCompact ? Visibility::Collapsed : Visibility::Visible);

        std::wstring device;
        if (s->side == Side::Local)
        {
            DeviceIcon().Glyph(L"\xE977");
            device = Str(L"ThisPC");
        }
        else
        {
            DeviceIcon().Glyph(L"\xE8EA");
            device = Widen(st.PeerDisplayName());
            if (device.empty()) device = Str(L"YourPhone");
        }
        DeviceLabel().Text(device);
        ToolTipService::SetToolTip(DeviceLabel(), box_value(device));
        auto app = Widen(s->DisplayApp());
        AppLabel().Text(app);
        ToolTipService::SetToolTip(AppLabel(), app.empty() ? nullptr : box_value(app));
        SourceButtonText().Text(st.focus.automatic ? StrF(L"SourceLabelAuto", { app, device }) : StrF(L"SourceLabel", { app, device }));

        hstring status;
        if (s->policyState == "policy_paused") status = Str(L"StatusPolicyPaused");
        else if (s->policyState == "remote_paused") status = Str(L"StatusRemotePaused");
        else if (s->status == "playing") status = Str(L"StatusPlaying");
        else if (s->status == "paused") status = Str(L"StatusPaused");
        else if (s->status == "stopped") status = Str(L"StatusStopped");
        else if (s->status == "buffering") status = Str(L"StatusBuffering");
        // Compat sessions (players without a media session) are marked subtly in the status line.
        bool compat = s->source == "compat";
        std::wstring statusText(status);
        if (compat) statusText = statusText.empty() ? std::wstring(Str(L"CompatBadge")) : statusText + L"  \x00B7  " + std::wstring(Str(L"CompatBadge"));
        StatusLabel().Text(statusText);
        ToolTipService::SetToolTip(StatusLabel(), compat ? box_value(Str(L"CompatTooltip")) : nullptr);
        StatusLabel().Visibility(statusText.empty() ? Visibility::Collapsed : Visibility::Visible);

        UpdatePlayState();
        PrevButton().IsEnabled(s->Has("previous"));
        NextButton().IsEnabled(s->Has("next"));

        m_updatingSlider = true;
        ProgressSlider().Maximum(s->durationMs > 0 ? static_cast<double>(s->durationMs) : 1.0);
        m_updatingSlider = false;
        ProgressSlider().IsEnabled(s->CanSeek());
        DurationLabel().Text(FormatTime(s->durationMs > 0 ? s->durationMs : -1));
        m_lastShownSecond = -2;
        UpdateProgress(TickNow());
        if (trackChanged) m_lyricsView.ResumeFollow();

        // Artwork: decoded once per path at the displayed size.
        int decode = static_cast<int>(300 * m_host.Scale());
        auto weak = get_weak();
        m_art.Request(DispatcherQueue(), s->artworkPath, decode,
            [weak](BitmapImage const& image) {
                if (auto self = weak.get())
                {
                    self->ArtBrush().ImageSource(image);
                    self->ArtImageBorder().Opacity(image ? 1.0 : 0.0);
                }
            },
            [weak](std::vector<Color> const& palette) {
                if (auto self = weak.get(); self && !self->m_closed) self->m_ambient.SetPalette(palette);
            });
    }

    void MainWindow::UpdatePlayState()
    {
        if (!m_session) return;
        bool playing = m_session->IsPlaying();
        PlayPauseIcon().Glyph(playing ? L"\xE769" : L"\xE768");
        PlayPauseButton().IsEnabled(playing ? m_session->CanPause() : m_session->CanPlay());
        ToolTipService::SetToolTip(PlayPauseButton(), box_value(playing ? Str(L"Pause") : Str(L"Play")));
    }

    void MainWindow::UpdateWelcome()
    {
        // First run: the host has no paired phone and no work mode was chosen yet. Only hosts that
        // know the setting send "modeChosen": an older host never triggers the choice.
        auto& c = Controller::Get();
        auto const& st = c.Model();
        auto chosen = JPath(st.settings, "modeChosen");
        bool show = c.Link() == HostLink::Connected && st.valid && chosen && chosen->is_boolean() && !chosen->get<bool>() &&
            st.devices.empty() && !st.standalone;
        WelcomePanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        BottomBar().Visibility(show ? Visibility::Collapsed : Visibility::Visible);
        if (!show) return;
        PlayerGrid().Visibility(Visibility::Collapsed);
        EmptyPanel().Visibility(Visibility::Collapsed);
        ConnectionPill().Visibility(Visibility::Collapsed);
    }

    void MainWindow::UpdateVersionBar()
    {
        // Connected phone on another major/minor version than this PC (host: md::version).
        auto& c = Controller::Get();
        auto const& st = c.Model();
        auto const& peer = st.connection.peer;
        bool mismatch = c.Link() == HostLink::Connected && st.valid && !st.standalone && st.connection.status == "connected" &&
            peer.versionMatch == "mismatch";
        std::string pc = st.self.app.empty() ? c.HostVersion() : st.self.app;
        m_versionPair = mismatch ? pc + "|" + peer.app : std::string{};
        bool dismissed = mismatch && st.Setting("ui.versionNoticeDismissed", std::string{}) == m_versionPair;
        if (mismatch && !dismissed)
        {
            VersionBar().Title(Str(L"VersionMismatchTitle"));
            VersionBar().Message(StrF(L"VersionMismatchMessage", { Widen(peer.app), Widen(pc) }));
        }
        VersionBar().IsOpen(mismatch && !dismissed);
    }

    void MainWindow::UpdatePolicy()
    {
        auto& c = Controller::Get();
        auto const& st = c.Model();
        if (c.Link() != HostLink::Connected || !st.valid || st.standalone)   // standalone: nothing is arbitrated
        {
            PolicyPanel().Visibility(Visibility::Collapsed);
            return;
        }
        auto const& p = st.policy;
        std::wstring peer = Widen(st.PeerDisplayName());
        if (peer.empty()) peer = Str(L"YourPhone");
        std::wstring text;
        if (p.mode == "off" || !p.enabled)
        {
            text = Str(L"PolicyOff");
        }
        else if (p.suspended)
        {
            text = Str(L"PolicySuspended");
        }
        else if (p.gated)
        {
            auto out = st.DefaultOutput();
            text = StrF(L"PolicyGated", { out ? Widen(out->name) : std::wstring(Str(L"OutputUnknown")) });
        }
        else if (p.lastAction)
        {
            auto const& a = *p.lastAction;
            std::wstring app = Widen(a.app);
            if (a.what == "pause_remote")
                text = app.empty() ? StrF(L"PolicyPausedRemoteGeneric", { peer }) : StrF(L"PolicyPausedRemote", { peer, app });
            else if (a.what == "pause_local")
                text = app.empty() ? StrF(L"PolicyPausedLocalGeneric", { peer }) : StrF(L"PolicyPausedLocal", { app, peer });
            else if (a.what == "resume_remote")
                text = StrF(L"PolicyResumedRemote", { peer });
            else if (a.what == "resume_local")
                text = app.empty() ? std::wstring(Str(L"PolicyResumedLocalGeneric")) : StrF(L"PolicyResumedLocal", { app });
            else
                text = StrF(L"PolicyModeActive", { Str(L"PolicyMode_" + Widen(p.mode)) });
        }
        else
        {
            text = StrF(L"PolicyModeActive", { Str(L"PolicyMode_" + Widen(p.mode)) });
        }
        PolicyText().Text(text);
        ToolTipService::SetToolTip(PolicyPanel(), box_value(text));
        PolicyPanel().Visibility(Visibility::Visible);
    }

    // ---- Transport ---------------------------------------------------------------------------

    void MainWindow::TogglePlay()
    {
        if (!m_session) return;
        bool playing = m_session->IsPlaying();
        if (playing ? !m_session->CanPause() : !m_session->CanPlay()) return;
        std::string action = playing ? (m_session->Has("pause") ? "pause" : "toggle")
                                     : (m_session->Has("play") ? "play" : "toggle");
        Controller::Get().Media(m_session->side, m_session->key, action);
        // Optimistic: the button, progress and lyrics react now; rolled back if not confirmed.
        double now = QpcNowMs();
        double pos = m_session->PositionPrecise(now);
        m_playHold.Begin(!playing, now);
        m_session->status = playing ? "paused" : "playing";
        if (pos >= 0) m_session->Rebase(static_cast<int64_t>(std::llround(pos)));
        m_clock.update(SessionReport(*m_session, m_clockTrack), now, true);
        m_playHoldTimer.Stop();
        m_playHoldTimer.Start();
        UpdatePlayState();
        UpdateTimer();
    }

    void MainWindow::SeekTo(int64_t positionMs)
    {
        if (!m_session || !m_session->CanSeek()) return;
        positionMs = std::clamp<int64_t>(positionMs, 0, std::max<int64_t>(0, m_session->durationMs));
        Controller::Get().Media(m_session->side, m_session->key, "seek", positionMs);
        double now = QpcNowMs();
        m_session->Rebase(positionMs);
        m_seekHold.Begin(static_cast<double>(positionMs), now);
        m_clock.update(SessionReport(*m_session, m_clockTrack), now, true);
        m_keySeekPending = false;
        m_lastShownSecond = -2;
        UpdateProgress(TickNow());
        m_lyricsView.ResumeFollow();
        UpdateLyricsPosition(now, false);
        UpdateFrameLoop();
    }

    void MainWindow::SeekBy(int64_t deltaMs)
    {
        if (!m_session || !m_session->CanSeek()) return;
        int64_t base = m_keySeekPending ? m_keySeekTarget : m_session->PositionAt(TickNow());
        if (base < 0) return;
        m_keySeekTarget = std::clamp<int64_t>(base + deltaMs, 0, m_session->durationMs);
        m_keySeekPending = true;
        m_updatingSlider = true;
        ProgressSlider().Value(static_cast<double>(m_keySeekTarget));
        m_updatingSlider = false;
        ElapsedLabel().Text(FormatTime(m_keySeekTarget));
        m_lastShownSecond = -2;
        m_keySeekTimer.Stop();
        m_keySeekTimer.Start();
    }

    void MainWindow::AdjustLyricsOffset(int64_t deltaMs)
    {
        if (!m_session || !m_session->hasLyricsDelay || m_session->appId.empty()) return;
        int64_t current = Controller::Get().Model().LyricsOffset(m_session->appId);
        int64_t next = std::clamp<int64_t>(current + deltaMs, -kOffsetLimitMs, kOffsetLimitMs);
        if (next == current) return;
        Controller::Get().AdjustLyricsOffset(m_session->appId, next - current);   // raises State -> refresh
    }

    bool MainWindow::OnKey(VirtualKey key, DependencyObject const& source)
    {
        // Ctrl / Alt combinations belong to accelerators (Alt+Left = back).
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
        bool textInput = source && (source.try_as<TextBox>() || source.try_as<AutoSuggestBox>() || source.try_as<NumberBox>());
        if (textInput) return false;
        switch (key)
        {
        case VirtualKey::Space:
            // A focused button / toggle uses Space itself.
            if (source && (source.try_as<ButtonBase>() || source.try_as<ToggleSwitch>())) return false;
            TogglePlay();
            return true;
        case VirtualKey::Left:
        case VirtualKey::Right:
            if (source && source.try_as<RangeBase>()) return false;   // the focused slider steps itself
            SeekBy(key == VirtualKey::Left ? -kKeySeekMs : kKeySeekMs);
            return true;
        default:
            break;
        }
        if (key == kVkOpenBracket || key == kVkCloseBracket)
        {
            if (!m_lyricsShown || !m_lyricsSynced) return false;
            AdjustLyricsOffset(key == kVkOpenBracket ? -kOffsetStepMs : kOffsetStepMs);
            return true;
        }
        return false;
    }

    // ---- Progress & timer ------------------------------------------------------------------

    void MainWindow::UpdateProgress(uint64_t now)
    {
        if (!m_session) return;
        auto pos = m_session->PositionAt(now);
        if (!m_dragging && !m_keySeekPending)
        {
            m_updatingSlider = true;
            ProgressSlider().Value(pos < 0 ? 0.0 : static_cast<double>(pos));
            m_updatingSlider = false;
            int64_t second = pos < 0 ? -1 : pos / 1000;
            if (second != m_lastShownSecond)
            {
                // Compat sessions only know an approximate position.
                ElapsedLabel().Text(m_session->positionEstimated ? L"\x2248 " + FormatTime(pos) : FormatTime(pos));
                m_lastShownSecond = second;
            }
        }
        // Low-power mode: no per-frame loop; the lyrics advance with this ~10 fps timer instead.
        if (m_lyricsShown && m_lyricsSynced && Controller::Get().LowPower()) UpdateLyricsPosition(QpcNowMs(), false);
    }

    void MainWindow::UpdateTimer()
    {
        if (!m_timer) return;
        auto& c = Controller::Get();
        bool want = m_host.IsShowing() && !m_settingsShown && m_session && m_session->IsPlaying() && c.Link() == HostLink::Connected;
        UpdateFrameLoop();
        if (!want)
        {
            if (m_timer.IsRunning()) m_timer.Stop();
            return;
        }
        int ms = c.LowPower() ? (m_lyricsShown && m_lyricsSynced ? 100 : 1000) : 250;
        auto interval = std::chrono::milliseconds(ms);
        if (m_timer.Interval() != interval) m_timer.Interval(interval);
        if (!m_timer.IsRunning()) m_timer.Start();
    }

    // ---- Lyrics ----------------------------------------------------------------------------
    //
    // LyricsView draws the Apple-Music-style view. Its position is the smoothed clock minus the
    // session's lyricsDelayMs (audio output latency + per-app offset, from the host), so lyrics
    // match what is heard through e.g. Bluetooth headphones. Per-frame work runs from a ~60 fps
    // frame clock only while the window is showing, playback is running and not in low-power mode.

    void MainWindow::UpdateLyrics()
    {
        auto const& lyrics = Controller::Get().CurrentLyrics();
        mdui::Session const* s = m_session ? &*m_session : nullptr;
        bool connected = Controller::Get().Link() == HostLink::Connected;
        bool show = connected && lyrics.MatchesAndShowable(s);
        // The next track's lyrics are not known yet (the lyrics message for it has not arrived or
        // says "loading"): keep the current layout for a moment instead of jumping to one column.
        // Lyrics for the same song under another trackId (the state and the lyrics message carrying
        // a player's new trackId arrive in either order) count as this track's: no hold, no rebuild.
        bool forTrack = lyrics.BelongsTo(s);
        bool pending = connected && s && (!forTrack || lyrics.status == "loading");
        if (show)
        {
            bool faded = m_holding;   // the old lines faded out for a track change: build again
            m_holdTimer.Stop();
            m_holding = false;
            // Content identity, not the trackId: the same lyrics re-keyed to a new trackId stay as they are.
            std::string identity = lyrics.ContentKey();
            if (identity != m_lyricsIdentity || faded)
            {
                m_lyricsIdentity = identity;
                m_translationOn = Controller::Get().Model().Setting("lyrics.translation", true);
                m_lyricsSynced = lyrics.synced;
                if (!m_lyricsShown)
                {
                    // Make the pane visible first so the view knows its size, then build.
                    SetLyricsLayout(true, true);
                }
                m_lyricsView.Build(lyrics, m_translationOn);   // fades the new lines in one by one
                std::wstring source;
                if (lyrics.source == "lrclib" || lyrics.source == "netease" || lyrics.source == "qqmusic")
                    source = Str(L"Provider_" + Widen(lyrics.source));
                else if (lyrics.source == "local") source = Str(L"LyricsSourceLocal");
                else source = Widen(lyrics.source);
                LyricsSourceLabel().Text(source.empty() ? std::wstring{} : StrF(L"LyricsSource", { source }));
                UpdateLyricsPosition(QpcNowMs(), true);
            }
            SetLyricsLayout(true, true);
        }
        else if (m_lyricsShown && !m_lyricsIdentity.empty() && pending && !m_holdExpired)
        {
            // Hold (<= 1.5 s): the old lines fade out, the layout stays.
            if (!m_holding)
            {
                m_holding = true;
                m_lyricsView.FadeOut();
                m_holdTimer.Start();
            }
        }
        else if (m_lyricsShown || !m_lyricsIdentity.empty())
        {
            m_holdTimer.Stop();
            m_holding = false;
            m_lyricsIdentity.clear();
            if (m_lyricsShown) SetLyricsLayout(false, true);   // clears the view once the pane is out
            else m_lyricsView.Clear();
        }
        if (!pending) m_holdExpired = false;
        bool hasTranslation = show && lyrics.hasTranslation;
        if (!m_holding)   // while holding, the pane keeps its controls (no jump in the bottom row)
            TranslationButton().Visibility(hasTranslation ? Visibility::Visible : Visibility::Collapsed);
        if (hasTranslation) ApplyTranslationSetting(Controller::Get().Model().Setting("lyrics.translation", true));
        UpdateLyricsOffset();
        if (show && m_session && m_lyricsSynced) UpdateLyricsPosition(QpcNowMs(), false);
        UpdateFrameLoop();
    }

    void MainWindow::UpdateLyricsOffset()
    {
        auto const& st = Controller::Get().Model();
        bool shown = m_lyricsShown && m_lyricsSynced && m_session && m_session->hasLyricsDelay && !m_session->appId.empty();
        LyricsOffsetButton().Visibility(shown ? Visibility::Visible : Visibility::Collapsed);
        if (!shown) return;
        int64_t manual = st.LyricsOffset(m_session->appId);
        LyricsOffsetButtonText().Text(manual == 0 ? std::wstring{} : OffsetText(manual));
        LyricsOffsetButtonText().Visibility(manual == 0 ? Visibility::Collapsed : Visibility::Visible);
        LyricsOffsetValue().Text(OffsetText(manual));
        LyricsEarlierButton().IsEnabled(manual > -kOffsetLimitMs);
        LyricsLaterButton().IsEnabled(manual < kOffsetLimitMs);
        LyricsOffsetResetButton().IsEnabled(manual != 0);
        std::wstring detail = StrF(L"LyricsOffsetDetail", { Widen(m_session->DisplayApp()) });
        if (m_session->outputLatencyMs > 0)
            detail += L"\n" + StrF(L"LyricsOffsetLatency", { std::to_wstring(m_session->outputLatencyMs) });
        LyricsOffsetDetail().Text(detail);
    }

    void MainWindow::ApplyTranslationSetting(bool on)
    {
        if (TranslationButton().IsChecked().Value() != on) TranslationButton().IsChecked(on);
        if (on == m_translationOn) return;
        m_translationOn = on;
        m_lyricsView.SetTranslation(on);   // the view re-centres once the new line heights are known
    }

    double MainWindow::LyricPosition(double now) const
    {
        if (!m_session) return -1;
        return lyrics::LyricPosition(m_clock, now, m_session->lyricsDelayMs);
    }

    void MainWindow::UpdateLyricsPosition(double now, bool force)
    {
        if (!m_lyricsShown || !m_lyricsSynced) return;
        m_lyricsView.Update(LyricPosition(now), force);
    }

    void MainWindow::OnFrame()
    {
        if (m_closed) return;
        double now = QpcNowMs();
        // Cap visual updates at ~60 fps: high-refresh displays (120-300 Hz) would otherwise multiply
        // the text re-rendering cost without a visible benefit.
        if (now - m_lastFrameMs < 12.0) return;
        m_lastFrameMs = now;
        if (m_lyricsShown && m_lyricsSynced && m_session) UpdateLyricsPosition(now, false);
        UpdateFrameLoop();
    }

    void MainWindow::UpdateFrameLoop()
    {
        auto& c = Controller::Get();
        bool playing = m_session && m_session->IsPlaying() && c.Link() == HostLink::Connected;
        bool want = !m_closed && m_host.IsShowing() && !m_settingsShown && m_lyricsShown && m_lyricsSynced && !c.LowPower() && playing;
        if (want && !m_rendering)
        {
            if (!m_frameTimer)
            {
                // ~60 fps frame clock. A DispatcherQueueTimer is used instead of
                // CompositionTarget::Rendering: Rendering fires at the display refresh rate (measured
                // 300 Hz here), which costs CPU every vsync even when nothing visible changes.
                // Scrolling and scaling are composition animations and need no frames from here.
                m_frameTimer = DispatcherQueue().CreateTimer();
                m_frameTimer.Interval(std::chrono::milliseconds(16));
                m_frameTimer.IsRepeating(true);
                m_frameTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                    if (auto self = weak.get()) self->OnFrame();
                });
            }
            m_frameTimer.Start();
            m_rendering = true;
        }
        else if (!want && m_rendering)
        {
            m_frameTimer.Stop();
            m_rendering = false;
        }
    }

    // ---- Page stack: player <-> settings -----------------------------------------------------

    void MainWindow::AnimatePageIn(UIElement const& page, float fromX)
    {
        if (!Controller::Get().AnimationsEnabled()) return;
        using namespace winrt::Microsoft::UI::Xaml::Hosting;
        Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(page, true);
        auto visual = ElementCompositionPreview::GetElementVisual(page);
        auto compositor = visual.Compositor();
        auto ease = compositor.CreateCubicBezierEasingFunction({ 0.1f, 0.9f }, { 0.2f, 1.0f });
        auto slide = compositor.CreateVector3KeyFrameAnimation();
        slide.InsertKeyFrame(0.0f, { fromX, 0.0f, 0.0f });
        slide.InsertKeyFrame(1.0f, { 0.0f, 0.0f, 0.0f }, ease);
        slide.Duration(std::chrono::milliseconds(280));
        visual.StartAnimation(L"Translation", slide);
        auto fade = compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f, ease);
        fade.Duration(std::chrono::milliseconds(220));
        visual.StartAnimation(L"Opacity", fade);
    }

    void MainWindow::ShowSettings(std::wstring_view page, bool animate)
    {
        if (m_closed) return;
        if (!m_settings)
        {
            // Created on first use so that opening the player stays cheap.
            m_settingsView = MediaDuetUI::SettingsPage();
            Grid::SetRow(m_settingsView, 1);
            m_settingsView.Visibility(Visibility::Collapsed);
            Root().Children().Append(m_settingsView);
            m_settings = get_self<SettingsPage>(m_settingsView);
            m_settings->SetOwner(m_host.Hwnd());
        }
        if (!page.empty()) m_settings->Navigate(page);
        if (m_settingsShown) return;
        m_settingsShown = true;
        PlayerPage().Visibility(Visibility::Collapsed);
        m_settingsView.Visibility(Visibility::Visible);
        BackButton().Visibility(Visibility::Visible);
        DragRegion().Padding(Thickness{ 4, 0, 0, 0 });
        TitleText().Text(Str(L"SettingsTitle"));
        HostProgress().IsActive(false);
        if (animate) AnimatePageIn(m_settingsView, 48.0f);
        m_settings->SetActive(m_host.IsShowing());
        UpdateTimer();
    }

    void MainWindow::ShowPlayer(bool animate)
    {
        if (m_closed || !m_settingsShown) return;
        m_settingsShown = false;
        if (m_settings) m_settings->SetActive(false);
        m_settingsView.Visibility(Visibility::Collapsed);
        PlayerPage().Visibility(Visibility::Visible);
        BackButton().Visibility(Visibility::Collapsed);
        DragRegion().Padding(Thickness{ 16, 0, 0, 0 });
        TitleText().Text(Str(L"AppName"));
        if (animate) AnimatePageIn(PlayerPage(), -48.0f);
        RefreshAll();
    }

    std::wstring MainWindow::SettingsPageTag() const
    {
        return m_settings ? m_settings->CurrentPage() : std::wstring{};
    }

    // ---- Volume ------------------------------------------------------------------------------

    void MainWindow::SetupVolume(VolumeUi& v, Side side, Slider const& slider, Button const& mute, FontIcon const& icon,
        TextBlock const& text)
    {
        v.side = side;
        v.slider = slider;
        v.icon = icon;
        v.text = text;
        v.timer = DispatcherQueue().CreateTimer();
        v.timer.IsRepeating(false);
        auto* pv = &v;
        auto send = [this, pv]() {
            if (!pv->pending) return;
            Controller::Get().SetVolume(pv->side, *pv->pending, std::nullopt, pv->key);
            pv->lastSent = QpcNowMs();
            pv->pending.reset();
        };
        v.timer.Tick([send](auto&&, auto&&) { send(); });
        slider.AddHandler(UIElement::PointerPressedEvent(),
            box_value(PointerEventHandler([pv](auto&&, auto&&) { pv->dragging = true; })), true);
        auto release = [pv, send](auto&&, auto&&) {
            if (!pv->dragging) return;
            pv->dragging = false;
            pv->timer.Stop();
            send();   // commit on release
            pv->ignoreUntil = QpcNowMs() + EchoWindowMs(pv->side);
        };
        slider.AddHandler(UIElement::PointerReleasedEvent(), box_value(PointerEventHandler(release)), true);
        slider.AddHandler(UIElement::PointerCaptureLostEvent(), box_value(PointerEventHandler(release)), true);
        slider.ValueChanged([this, pv, send](auto&&, RangeBaseValueChangedEventArgs const& e) {
            if (pv->applying) return;
            double level = e.NewValue() / 100.0;
            pv->pending = level;
            pv->ignoreUntil = QpcNowMs() + EchoWindowMs(pv->side);
            ShowVolume(*pv, level, false);
            // Throttle to ~10 messages/s while dragging; the release commits the final value.
            double since = QpcNowMs() - pv->lastSent;
            if (since >= 100.0)
            {
                pv->timer.Stop();
                send();
            }
            else if (!pv->timer.IsRunning())
            {
                pv->timer.Interval(std::chrono::milliseconds(static_cast<int64_t>(100.0 - since) + 1));
                pv->timer.Start();
            }
        });
        mute.Click([this, pv](auto&&, auto&&) {
            auto const& st = Controller::Get().Model();
            bool muted = (pv->side == Side::Local ? st.localVolume : st.remoteVolume).muted;
            if (!pv->key.empty())
                if (auto s = st.Find(Side::Local, pv->key)) muted = s->appVolume.muted;
            Controller::Get().SetVolume(pv->side, std::nullopt, !muted, pv->key);
            UpdateVolume();
        });
    }

    void MainWindow::ShowVolume(VolumeUi& v, double level, bool muted)
    {
        wchar_t const* glyph = muted ? L"\xE74F" : level <= 0.001 ? L"\xE992" : level < 0.34 ? L"\xE993" : level < 0.67 ? L"\xE994" : L"\xE995";
        v.icon.Glyph(glyph);
        v.text.Text(muted ? std::wstring(Str(L"VolumeMuted")) : std::to_wstring(static_cast<int>(std::lround(level * 100))) + L"%");
    }

    void MainWindow::ApplyVolume(VolumeUi& v, mdui::Volume const& vol)
    {
        if (!vol.available) return;
        bool userBusy = v.dragging || QpcNowMs() < v.ignoreUntil;
        if (!userBusy)
        {
            v.applying = true;
            v.slider.Value(vol.level * 100.0);
            v.applying = false;
        }
        ShowVolume(v, userBusy ? v.slider.Value() / 100.0 : vol.level, vol.muted);
    }

    void MainWindow::UpdateVolume()
    {
        auto& c = Controller::Get();
        auto const& st = c.Model();
        bool connected = c.Link() == HostLink::Connected && st.valid;
        auto const& local = st.localVolume;
        auto const& remote = st.remoteVolume;

        // Inline: the device that plays the focused session.
        Side side = m_session ? m_session->side : Side::Local;
        auto const& focused = side == Side::Local ? local : remote;
        if (m_inlineVolume.side != side)
        {
            m_inlineVolume.side = side;
            m_inlineVolume.pending.reset();
            m_inlineVolume.ignoreUntil = 0;
        }
        bool inlineShown = connected && m_session && focused.available && !m_cardCompact;
        InlineVolume().Visibility(inlineShown ? Visibility::Visible : Visibility::Collapsed);
        if (inlineShown) ApplyVolume(m_inlineVolume, focused);

        // The phone blocked a volume change while MediaDuet is in the background there.
        bool restricted = connected && c.RemoteVolumeRestricted();
        if (restricted)
        {
            auto hint = Str(L"VolumeBackgroundRestricted");
            InlineVolumeHintText().Text(hint);
            RemoteVolumeHintText().Text(hint);
        }
        InlineVolumeHint().Visibility(restricted && inlineShown && side == Side::Remote ? Visibility::Visible : Visibility::Collapsed);
        RemoteVolumeHint().Visibility(restricted ? Visibility::Visible : Visibility::Collapsed);

        LocalVolumeRow().Visibility(connected && local.available ? Visibility::Visible : Visibility::Collapsed);
        RemoteVolumeRow().Visibility(connected && remote.available ? Visibility::Visible : Visibility::Collapsed);
        VolumeUnavailableText().Visibility(connected && (local.available || remote.available) ? Visibility::Collapsed : Visibility::Visible);
        std::wstring peer = Widen(st.PeerDisplayName());
        RemoteVolumeName().Text(peer.empty() ? std::wstring(Str(L"YourPhone")) : peer);
        ApplyVolume(m_localVolume, local);
        ApplyVolume(m_remoteVolume, remote);

        // Per-app volume of the focused Windows session (or Windows' active session when the
        // phone is focused).
        mdui::Session const* app = nullptr;
        if (connected)
        {
            if (m_session && m_session->side == Side::Local) app = st.Find(Side::Local, m_session->key);
            else if (!st.local.active.empty()) app = st.Find(Side::Local, st.local.active);
        }
        bool appShown = app && app->appVolume.available;
        AppVolumeRow().Visibility(appShown ? Visibility::Visible : Visibility::Collapsed);
        if (appShown)
        {
            if (m_appVolume.key != app->key)
            {
                m_appVolume.key = app->key;
                m_appVolume.pending.reset();
                m_appVolume.ignoreUntil = 0;
            }
            AppVolumeName().Text(StrF(L"AppVolumeName", { Widen(app->DisplayApp()) }));
            ApplyVolume(m_appVolume, app->appVolume);
        }
        if (appShown) VolumeUnavailableText().Visibility(Visibility::Collapsed);
        VolumeButton().IsEnabled(connected);
    }

    // ---- Source switcher -------------------------------------------------------------------

    void MainWindow::BuildSourceMenu()
    {
        auto& c = Controller::Get();
        auto const& st = c.Model();
        auto items = SourceFlyout().Items();
        items.Clear();

        RadioMenuFlyoutItem automatic;
        automatic.Text(Str(L"SourceAutomatic"));
        automatic.GroupName(L"source");
        automatic.IsChecked(st.focus.automatic);
        automatic.Click([](auto&&, auto&&) { Controller::Get().SetFocus(std::nullopt, {}, true); });
        FontIcon autoIcon;
        autoIcon.Glyph(L"\xE8AB");
        automatic.Icon(autoIcon);
        items.Append(automatic);

        bool any = false;
        auto peer = Widen(st.PeerDisplayName());
        if (peer.empty()) peer = Str(L"YourPhone");
        for (auto const* side : { &st.local, &st.remote })
        {
            for (auto const& s : side->sessions)
            {
                if (!any) items.Append(MenuFlyoutSeparator());
                any = true;
                RadioMenuFlyoutItem item;
                std::wstring label = Widen(s.DisplayApp());
                if (!s.title.empty()) label += L"  \x2014  " + Widen(s.title);
                item.Text(label);
                item.GroupName(L"source");
                item.IsChecked(!st.focus.automatic && st.focus.hasSide && st.focus.side == s.side && st.focus.key == s.key);
                FontIcon icon;
                icon.Glyph(s.side == Side::Local ? L"\xE977" : L"\xE8EA");
                item.Icon(icon);
                ToolTipService::SetToolTip(item, box_value(s.side == Side::Local ? std::wstring(Str(L"ThisPC")) : peer));
                Side sessionSide = s.side;
                std::string key = s.key;
                item.Click([sessionSide, key](auto&&, auto&&) { Controller::Get().SetFocus(sessionSide, key, false); });
                items.Append(item);
            }
        }
        if (!any)
        {
            items.Append(MenuFlyoutSeparator());
            MenuFlyoutItem none;
            none.Text(Str(L"SourceNone"));
            none.IsEnabled(false);
            items.Append(none);
        }
    }

    void MainWindow::ShowError(std::string const& error)
    {
        ErrorBar().Title(Str(L"CommandFailed"));
        ErrorBar().Message(error.empty() ? hstring{} : hstring(Widen(error)));
        ErrorBar().IsOpen(true);
    }
}
