#include "pch.h"
#include "CompactWindow.xaml.h"
#if __has_include("CompactWindow.g.cpp")
#include "CompactWindow.g.cpp"
#endif

#include "Controller.h"
#include "Karaoke.h"
#include "RenderTest.h"
#include "Strings.h"

#include <dwmapi.h>

#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace mdui;
using winrt::Windows::UI::Color;

namespace
{
    constexpr int kWidthDip = 340;
    constexpr int kHeightDip = 96;
    constexpr int kSnapDip = 16;
    constexpr int kMarginDip = 24;
}

namespace winrt::MediaDuetUI::implementation
{
    void CompactWindow::InitializeComponent()
    {
        CompactWindowT::InitializeComponent();
        auto& c = Controller::Get();
        Title(Str(L"CompactTitle"));

        // Borderless-looking: keep the thin DWM border + rounded corners, drop the caption.
        OverlappedPresenter presenter = OverlappedPresenter::Create();
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
        presenter.SetBorderAndTitleBar(true, false);
        AppWindow().SetPresenter(presenter);

        m_host.Attach(*this, Root(), SIZE{ kWidthDip, kHeightDip });
        m_host.extraHook = [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return OnMessage(hwnd, msg, wp, lp); };
        m_host.showingChanged = [this] {
            if (m_host.IsShowing() && m_dirty) RefreshAll();
            UpdateTimer();
        };
        PlaceInitially();
        m_ambient.Attach(TintLayer(), ambient::Shape::Compact);

        m_timer = DispatcherQueue().CreateTimer();
        m_timer.IsRepeating(true);
        m_timer.Tick([this](auto&&, auto&&) { Tick(TickNow()); });

        // Drag anywhere that is not a button.
        Root().PointerPressed([this](auto&&, PointerRoutedEventArgs const& e) {
            auto point = e.GetCurrentPoint(Root());
            if (!point.Properties().IsLeftButtonPressed()) return;
            ReleaseCapture();
            SendMessageW(m_host.Hwnd(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        });
        // Controls appear on hover and while one of them has keyboard focus.
        // PointerExited also bubbles from children and is not raised reliably for fast exits, so the
        // real cursor position decides; while hovered a light check (400 ms) catches missed exits.
        // Leaving hides them only after a short grace period (no flicker when the cursor brushes
        // the edge or briefly passes over a tooltip).
        m_hoverTimer = DispatcherQueue().CreateTimer();
        m_hoverTimer.Interval(std::chrono::milliseconds(400));
        m_hoverTimer.IsRepeating(true);
        m_hoverTimer.Tick([this](auto&&, auto&&) { CheckHover(); });
        m_leaveTimer = DispatcherQueue().CreateTimer();
        m_leaveTimer.Interval(std::chrono::milliseconds(350));
        m_leaveTimer.IsRepeating(false);
        m_leaveTimer.Tick([this](auto&&, auto&&) {
            if (m_closed) return;
            POINT pt{};
            RECT r{};
            if (GetCursorPos(&pt) && GetWindowRect(m_host.Hwnd(), &r) && PtInRect(&r, pt)) return;   // came back
            m_hover = false;
            m_hoverTimer.Stop();
            UpdateControlsVisibility();
        });
        Root().PointerEntered([this](auto&&, auto&&) {
            m_leaveTimer.Stop();
            if (m_hover) return;
            m_hover = true;
            if (!m_hoverTimer.IsRunning()) m_hoverTimer.Start();
            UpdateControlsVisibility();
        });
        Root().PointerExited([this](auto&&, auto&&) { CheckHover(); });
        m_playHoldTimer = DispatcherQueue().CreateTimer();
        m_playHoldTimer.IsRepeating(false);
        m_playHoldTimer.Interval(std::chrono::milliseconds(static_cast<int64_t>(OptimisticFlag::kTimeoutMs) + 50));
        m_playHoldTimer.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get(); self && !self->m_closed) self->RefreshAll();
        });
        m_pillHideTimer = DispatcherQueue().CreateTimer();
        m_pillHideTimer.IsRepeating(false);
        m_pillHideTimer.Interval(std::chrono::milliseconds(220));
        m_pillHideTimer.Tick([this](auto&&, auto&&) {
            if (!m_closed && !m_controlsShown) ControlsPill().Background(nullptr);
        });
        ControlsPill().Background(nullptr);
        for (auto const& button : { PrevButton(), PlayPauseButton(), NextButton(), ExpandButton(), CloseButton() })
        {
            // Focus events also fire while the window is being torn down: use weak references.
            button.GotFocus([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get()) self->UpdateControlsVisibility();
            });
            button.LostFocus([weak = get_weak()](auto&&, auto&&) {
                auto self = weak.get();
                if (!self || self->m_closed) return;
                // Focus may be moving to a sibling control; evaluate once it has settled.
                self->DispatcherQueue().TryEnqueue([weak] {
                    if (auto s = weak.get()) s->UpdateControlsVisibility();
                });
            });
        }

        PrevButton().Click([this](auto&&, auto&&) {
            if (m_session) Controller::Get().Media(m_session->side, m_session->key, "previous");
        });
        NextButton().Click([this](auto&&, auto&&) {
            if (m_session) Controller::Get().Media(m_session->side, m_session->key, "next");
        });
        PlayPauseButton().Click([this](auto&&, auto&&) { TogglePlay(); });
        ExpandButton().Click([](auto&&, auto&&) { Controller::Get().SwitchToMain(); });
        CloseButton().Click([this](auto&&, auto&&) { Close(); });
        StartHostButton().Click([this](auto&&, auto&&) {
            if (!Controller::Get().LaunchHost()) RefreshAll();
        });

        // Right-click menu
        MenuFlyout menu;
        MenuFlyoutItem openMain;
        openMain.Text(Str(L"MenuOpenMain"));
        openMain.Click([](auto&&, auto&&) { Controller::Get().SwitchToMain(); });
        menu.Items().Append(openMain);
        MenuFlyoutItem settings;
        settings.Text(Str(L"MenuSettings"));
        settings.Click([](auto&&, auto&&) { Controller::Get().ShowSettings(L""); });
        menu.Items().Append(settings);
        ToggleMenuFlyoutItem onTop;
        onTop.Text(Str(L"MenuAlwaysOnTop"));
        onTop.Click([onTop](auto&&, auto&&) {
            Controller::Get().SetSetting("ui.compact.alwaysOnTop", onTop.IsChecked());
        });
        menu.Items().Append(onTop);
        menu.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem close;
        close.Text(Str(L"MenuClose"));
        close.Click([this](auto&&, auto&&) { Close(); });
        menu.Items().Append(close);
        menu.Opening([onTop](auto&&, auto&&) {
            onTop.IsChecked(Controller::Get().Model().Setting("ui.compact.alwaysOnTop", true));
        });
        Root().ContextFlyout(menu);

        auto weak = get_weak();
        auto refresh = [weak] {
            if (auto self = weak.get()) self->RefreshAll();
        };
        m_tokens.push_back(c.Subscribe(Event::State, refresh));
        m_tokens.push_back(c.Subscribe(Event::Link, refresh));
        m_tokens.push_back(c.Subscribe(Event::Lyrics, [weak] {
            if (auto self = weak.get())
            {
                if (!self->m_host.IsShowing())
                {
                    self->m_dirty = true;
                    return;
                }
                self->UpdateLyrics();
                self->UpdateTimer();
            }
        }));
        m_tokens.push_back(c.Subscribe(Event::Appearance, [weak] {
            if (auto self = weak.get()) self->ApplyAppearance();
        }));
        m_tokens.push_back(c.SubscribeResults([weak](bool ok, std::string const&, std::string const& type) {
            // A refused play/pause rolls the optimistic button back at once.
            if (auto self = weak.get(); self && !self->m_closed && !ok && type == "media.command")
            {
                self->m_playHold.Clear();
                self->RefreshAll();
            }
        }));

        Closed([this](auto&&, auto&&) { OnClosed(); });

        ApplyAppearance();
        RefreshAll();
    }

    // ---- Placement -------------------------------------------------------------------------

    void CompactWindow::PlaceInitially()
    {
        if (render::Active()) return;   // placed off-screen by the render test
        double scale = m_host.Scale();
        int w = static_cast<int>(kWidthDip * scale), h = static_cast<int>(kHeightDip * scale);
        AppWindow().ResizeClient(winrt::Windows::Graphics::SizeInt32{ w, h });
        RECT wnd{};
        GetWindowRect(m_host.Hwnd(), &wnd);
        int outerW = wnd.right - wnd.left, outerH = wnd.bottom - wnd.top;

        auto const& settings = Controller::Get().Model().settings;
        auto x = JPath(settings, "ui.compact.x");
        auto y = JPath(settings, "ui.compact.y");
        RECT r{};
        bool placed = false;
        if (x && y && x->is_number() && y->is_number())
        {
            r = RECT{ x->get<LONG>(), y->get<LONG>(), x->get<LONG>() + outerW, y->get<LONG>() + outerH };
            if (IsRectOnAnyMonitor(r))
            {
                // The monitor layout may have changed since the position was saved: keep it fully visible.
                r = ClampToWorkArea(r);
                placed = true;
            }
        }
        if (!placed)
        {
            POINT origin{ 0, 0 };
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
            int margin = static_cast<int>(kMarginDip * scale);
            r.left = mi.rcWork.right - outerW - margin;
            r.top = mi.rcWork.bottom - outerH - margin;
        }
        AppWindow().Move(winrt::Windows::Graphics::PointInt32{ r.left, r.top });
    }

    void CompactWindow::EnsureOnScreen()
    {
        if (render::Active()) return;   // must stay off-screen
        HWND hwnd = m_host.Hwnd();
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return;
        RECT clamped = ClampToWorkArea(r);
        if (!IsRectOnAnyMonitor(r) || clamped.left != r.left || clamped.top != r.top)
            SetWindowPos(hwnd, nullptr, clamped.left, clamped.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void CompactWindow::SavePosition()
    {
        if (render::Active()) return;
        RECT r{};
        if (!GetWindowRect(m_host.Hwnd(), &r)) return;
        auto const& settings = Controller::Get().Model().settings;
        auto x = JPath(settings, "ui.compact.x");
        auto y = JPath(settings, "ui.compact.y");
        if (x && y && x->is_number() && y->is_number() && x->get<LONG>() == r.left && y->get<LONG>() == r.top) return;
        Controller::Get().SetSettings(json{ { "ui", { { "compact", { { "x", r.left }, { "y", r.top } } } } } });
    }

    std::optional<LRESULT> CompactWindow::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOVING:
        {
            if (!Controller::Get().Model().Setting("ui.compact.snap", true)) break;
            auto proposed = reinterpret_cast<RECT*>(lp);
            // Snap the *visible* frame (DWM extended frame bounds) to the work-area edges.
            RECT wnd{}, frame{};
            GetWindowRect(hwnd, &wnd);
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) frame = wnd;
            LONG dl = frame.left - wnd.left, dt = frame.top - wnd.top, dr = wnd.right - frame.right, db = wnd.bottom - frame.bottom;
            RECT visible{ proposed->left + dl, proposed->top + dt, proposed->right - dr, proposed->bottom - db };
            RECT wa = WorkAreaFor(visible);
            LONG threshold = static_cast<LONG>(kSnapDip * ScaleFor(hwnd));
            LONG dx = 0, dy = 0;
            if (std::abs(visible.left - wa.left) < threshold) dx = wa.left - visible.left;
            else if (std::abs(visible.right - wa.right) < threshold) dx = wa.right - visible.right;
            if (std::abs(visible.top - wa.top) < threshold) dy = wa.top - visible.top;
            else if (std::abs(visible.bottom - wa.bottom) < threshold) dy = wa.bottom - visible.bottom;
            OffsetRect(proposed, dx, dy);
            return TRUE;
        }
        case WM_EXITSIZEMOVE:
            if (Controller::Get().Model().Setting("ui.compact.snap", true)) EnsureOnScreen();
            SavePosition();
            break;
        case WM_DPICHANGED:
        {
            // Per-monitor DPI: keep the same DIP size on the new monitor.
            auto suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            double scale = HIWORD(wp) / 96.0;
            AppWindow().ResizeClient(winrt::Windows::Graphics::SizeInt32{
                static_cast<int32_t>(kWidthDip * scale), static_cast<int32_t>(kHeightDip * scale) });
            return 0;
        }
        case WM_DISPLAYCHANGE:
            EnsureOnScreen();
            break;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) EnsureOnScreen();
            break;
        default:
            break;
        }
        return std::nullopt;
    }

    void CompactWindow::OnClosed()
    {
        m_closed = true;
        if (m_timer) m_timer.Stop();
        if (m_hoverTimer) m_hoverTimer.Stop();
        if (m_leaveTimer) m_leaveTimer.Stop();
        if (m_pillHideTimer) m_pillHideTimer.Stop();
        if (m_playHoldTimer) m_playHoldTimer.Stop();
        if (m_frameTimer) m_frameTimer.Stop();
        m_rendering = false;
        auto& c = Controller::Get();
        for (auto t : m_tokens) c.Unsubscribe(t);
        m_tokens.clear();
        m_host.Detach();
        m_ambient.Detach();
        m_art.Clear();
        ArtBrush().ImageSource(nullptr);
        c.WindowClosed(WindowKind::Compact, *this);
    }

    // ---- Appearance --------------------------------------------------------------------------

    void CompactWindow::ApplyAppearance()
    {
        auto& c = Controller::Get();
        m_host.ApplyTheme(c.Theme());
        bool animations = c.AnimationsEnabled();
        auto fade = [animations]() -> ScalarTransition {
            if (!animations) return nullptr;
            ScalarTransition t;
            t.Duration(std::chrono::milliseconds(160));
            return t;
        };
        ControlsPill().OpacityTransition(fade());
        CloseButton().OpacityTransition(fade());
        ArtImageBorder().OpacityTransition(fade());
        m_ambient.SetAnimations(animations);
        if (animations)
        {
            Vector3Transition slide;
            slide.Duration(std::chrono::milliseconds(180));
            ControlsPill().TranslationTransition(slide);
        }
        else
        {
            ControlsPill().TranslationTransition(nullptr);
        }
        ApplyLyricColors();
        if (auto presenter = AppWindow().Presenter().try_as<OverlappedPresenter>(); presenter && !render::Active())
            presenter.IsAlwaysOnTop(c.Model().Setting("ui.compact.alwaysOnTop", true));
    }

    void CompactWindow::CheckHover()
    {
        if (m_closed) return;
        POINT pt{};
        RECT r{};
        bool inside = GetCursorPos(&pt) && GetWindowRect(m_host.Hwnd(), &r) && PtInRect(&r, pt);
        if (inside)
        {
            m_leaveTimer.Stop();
            return;
        }
        if (m_hover && !m_leaveTimer.IsRunning()) m_leaveTimer.Start();
    }

    Brush CompactWindow::PillBrush()
    {
        // In-app acrylic matching AcrylicInAppFillColorDefaultBrush for the window's *actual* theme
        // (theme resources resolved from code would follow the app theme instead).
        bool dark = m_host.IsDark();
        if (m_pillBrush && m_pillBrushDark == dark) return m_pillBrush;
        AcrylicBrush brush;
        brush.TintColor(dark ? Color{ 255, 0x2C, 0x2C, 0x2C } : Color{ 255, 0xFC, 0xFC, 0xFC });
        brush.TintOpacity(dark ? 0.15 : 0.0);
        brush.TintLuminosityOpacity(dark ? 0.96 : 0.85);
        brush.FallbackColor(dark ? Color{ 255, 0x2C, 0x2C, 0x2C } : Color{ 255, 0xF9, 0xF9, 0xF9 });
        m_pillBrush = brush;
        m_pillBrushDark = dark;
        return m_pillBrush;
    }

    bool CompactWindow::ControlsHaveKeyboardFocus()
    {
        for (auto const& b : { PrevButton(), PlayPauseButton(), NextButton(), ExpandButton(), CloseButton() })
            if (b.FocusState() == FocusState::Keyboard) return true;
        return false;
    }

    void CompactWindow::UpdateControlsVisibility()
    {
        if (m_closed) return;
        bool show = m_hover || ControlsHaveKeyboardFocus();
        bool pill = show && m_session.has_value();
        if (pill)
        {
            m_pillHideTimer.Stop();
            ControlsPill().Background(PillBrush());
        }
        else if (m_controlsShown)
        {
            // An acrylic brush is recomposed whenever the content below it changes (the lyric line
            // animates every frame), even at opacity 0: drop it once the fade-out has finished.
            m_pillHideTimer.Stop();
            m_pillHideTimer.Start();
        }
        m_controlsShown = pill;
        ControlsPill().Opacity(pill ? 1.0 : 0.0);
        ControlsPill().Translation(pill ? winrt::Windows::Foundation::Numerics::float3{ 0, 0, 0 }
                                        : winrt::Windows::Foundation::Numerics::float3{ 14, 0, 0 });
        ControlsPill().IsHitTestVisible(pill);
        CloseButton().Opacity(show ? 1.0 : 0.0);
        CloseButton().IsHitTestVisible(show);
    }

    // ---- State -----------------------------------------------------------------------------

    void CompactWindow::RefreshAll()
    {
        if (!m_host.IsShowing())
        {
            m_dirty = true;
            return;
        }
        m_dirty = false;
        auto& c = Controller::Get();
        auto const& st = c.Model();
        bool connected = c.Link() == HostLink::Connected && st.valid;
        auto s = connected ? st.FocusedSession() : nullptr;

        if (!s)
        {
            m_session.reset();
            m_clock.reset();
            m_playHold.Clear();
            PlayerGrid().Visibility(Visibility::Collapsed);
            Progress().Visibility(Visibility::Collapsed);
            MessageGrid().Visibility(Visibility::Visible);
            bool connecting = c.Link() == HostLink::Connecting;
            bool notRunning = c.Link() == HostLink::NotRunning;
            MessageProgress().IsActive(connecting);
            MessageProgress().Visibility(connecting ? Visibility::Visible : Visibility::Collapsed);
            MessageIcon().Visibility(connecting ? Visibility::Collapsed : Visibility::Visible);
            MessageIcon().Glyph(notRunning ? L"\xE7BA" : L"\xE8D6");
            MessageText().Text(connecting ? Str(L"HostConnecting") : notRunning ? Str(L"HostNotRunningTitle") : Str(L"NothingPlaying"));
            StartHostButton().Visibility(notRunning && c.HostExecutableExists() ? Visibility::Visible : Visibility::Collapsed);
            m_art.Request(DispatcherQueue(), {}, 0, nullptr, nullptr);
            ArtBrush().ImageSource(nullptr);
            m_ambient.SetPalette({});
            UpdateLyrics();
            UpdateControlsVisibility();
            UpdateTimer();
            return;
        }

        // A new trackId for the same song (players re-reporting the duration) is not a track change.
        bool trackChanged = !m_session || !SameSong(*m_session, *s);
        if (trackChanged) m_clockTrack = s->key + "|" + s->trackId;
        double nowQpc = QpcNowMs();
        double shownBefore = m_session && !trackChanged ? m_session->PositionPrecise(nowQpc) : -1;
        m_session = *s;
        if (trackChanged) m_playHold.Clear();
        // Optimistic play/pause (see MainWindow): show the requested state until confirmed.
        bool shownPlaying = m_playHold.Resolve(s->IsPlaying(), nowQpc);
        if (shownPlaying != s->IsPlaying())
        {
            double pos = !shownPlaying && shownBefore >= 0 ? shownBefore : s->PositionPrecise(nowQpc);
            m_session->status = shownPlaying ? "playing" : "paused";
            if (pos >= 0) m_session->Rebase(static_cast<int64_t>(std::llround(pos)));
        }
        // Same clock as the host overlay (md::lyrics::LyricsClock); a track change snaps it.
        m_clock.update(SessionReport(*m_session, m_clockTrack), nowQpc);
        MessageProgress().IsActive(false);
        MessageGrid().Visibility(Visibility::Collapsed);
        PlayerGrid().Visibility(Visibility::Visible);
        TitleLabel().Text(s->title.empty() ? std::wstring(Str(L"UnknownTitle")) : Widen(s->title));
        SubLabel().Text(Widen(s->artist.empty() ? s->album : s->artist));
        ToolTipService::SetToolTip(TitleLabel(), s->title.empty() ? nullptr : box_value(Widen(s->title)));
        m_hasArtistLine = !(s->artist.empty() && s->album.empty());
        UpdateSubLabel();

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
        DeviceLabel().Text(StrF(L"SourceLabel", { Widen(s->DisplayApp()), device }));

        UpdatePlayState();
        NextButton().IsEnabled(s->Has("next"));
        PrevButton().IsEnabled(s->Has("previous"));

        Progress().Visibility(s->durationMs > 0 ? Visibility::Visible : Visibility::Collapsed);

        int decode = static_cast<int>(68 * m_host.Scale());
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
        UpdateLyrics();
        Tick(TickNow());
        UpdateControlsVisibility();
        UpdateTimer();
    }

    // ---- Current lyric line ----------------------------------------------------------------
    //
    // Two KaraokeLine slots: on a line change the outgoing line slides up and fades while the new
    // one slides in from below (240 ms ease-out). The current line fills progressively per frame.

    void CompactWindow::EnsureLyricSlots()
    {
        if (m_slots[0]) return;
        KaraokeLine::Style style;
        style.fontSize = 12.5;
        style.weight = LyricWeight::Medium;   // Bold is too heavy at 12.5 epx next to the title
        style.wrap = false;
        style.translationScale = 0.88;   // 11 px: legible at this size
        style.scrollLongLine = true;      // long lines scroll with the fill, like the desktop lyrics
        for (auto& slot : m_slots)
        {
            slot = std::make_unique<KaraokeLine>(style);
            slot->Root().Opacity(0);
            slot->Root().IsHitTestVisible(false);
            LyricSlot().Children().Append(slot->Root());
        }
        ApplyLyricColors();
    }

    void CompactWindow::ApplyLyricColors()
    {
        if (!m_slots[0]) return;
        bool dark = m_host.IsDark();
        winrt::Windows::UI::ViewManagement::UISettings ui;
        Color sung = ui.GetColorValue(dark ? winrt::Windows::UI::ViewManagement::UIColorType::AccentLight2
                                           : winrt::Windows::UI::ViewManagement::UIColorType::AccentDark1);
        Color unsung = dark ? Color{ 0x80, 255, 255, 255 } : Color{ 0x80, 0, 0, 0 };
        Color translation = dark ? Color{ 0xC8, 255, 255, 255 } : Color{ 0x9E, 0, 0, 0 };
        for (auto& slot : m_slots)
        {
            slot->SetColors(sung, unsung);
            slot->SetTranslationColor(translation);
        }
        m_lyricsDark = dark;
    }

    void CompactWindow::UpdateLyrics()
    {
        auto const& lyrics = Controller::Get().CurrentLyrics();
        mdui::Session const* s = m_session ? &*m_session : nullptr;
        bool show = Controller::Get().Link() == HostLink::Connected && lyrics.synced && lyrics.MatchesAndShowable(s);
        if (show)
        {
            // Content identity, not the trackId: the same lyrics re-keyed to a new trackId stay as they are.
            std::string identity = lyrics.ContentKey();
            if (identity != m_lyricsIdentity)
            {
                m_lyricsIdentity = identity;
                m_lines = lyrics.lines;
                m_rows = md::lyrics::buildRows(ToCoreLyrics(lyrics));
                m_activeLine = -2;
            }
            EnsureLyricSlots();
            bool on = Controller::Get().Model().Setting("lyrics.translation", true);
            if (on != m_translationOn)
            {
                m_translationOn = on;
                for (auto& slot : m_slots) slot->SetTranslationEnabled(on);
            }
        }
        else
        {
            m_lines.clear();
            m_rows = {};
            m_lyricsIdentity.clear();
            m_activeLine = -2;
            m_transitionStart = -1;
        }
        m_lyricsShown = show;
        if (!show)
        {
            if (LyricSlot().Visibility() == Visibility::Visible)
            {
                LyricSlot().Visibility(Visibility::Collapsed);
                DeviceRow().Visibility(Visibility::Visible);
                if (Controller::Get().AnimationsEnabled() && SystemAnimationsEnabled() && m_host.IsShowing())
                    AnimateVisualOpacity(DeviceRow(), 0.0f, 1.0f, 240);
            }
            m_lyricLineVisible = false;
            UpdateSubLabel();
        }
        else if (m_session)
        {
            double now = QpcNowMs();
            UpdateLyricLine(LyricPosition(now), now);
        }
        UpdateFrameLoop();
    }

    void CompactWindow::Tick(uint64_t now)
    {
        if (!m_session) return;
        auto pos = m_session->PositionAt(now);
        if (m_session->durationMs > 0)
            ProgressScale().ScaleX(pos < 0 ? 0.0 : std::clamp(static_cast<double>(pos) / static_cast<double>(m_session->durationMs), 0.0, 1.0));
        // Low-power mode has no per-frame loop: the line advances with this ~10 fps timer.
        if (m_lyricsShown && Controller::Get().LowPower())
        {
            double q = QpcNowMs();
            UpdateLyricLine(LyricPosition(q), q);
        }
    }

    void CompactWindow::UpdateLyricLine(double positionMs, double now)
    {
        if (!m_slots[0]) return;
        if (m_host.IsDark() != m_lyricsDark) ApplyLyricColors();
        // The shared row model: no line before the lyrics, in instrumental gaps and after their end.
        int index = positionMs >= 0 ? m_rows.lineAt(static_cast<int64_t>(positionMs)) : -1;
        bool hasText = index >= 0 && index < static_cast<int>(m_lines.size());
        bool animations = Controller::Get().AnimationsEnabled();
        // Before the first line, in instrumental gaps and after the lyrics the source line is shown instead; the
        // switch cross-fades (no jump between the two rows).
        bool hadLine = m_lyricLineVisible && LyricSlot().Visibility() == Visibility::Visible;
        auto want = hasText ? Visibility::Visible : Visibility::Collapsed;
        if (LyricSlot().Visibility() != want)
        {
            LyricSlot().Visibility(want);
            DeviceRow().Visibility(hasText ? Visibility::Collapsed : Visibility::Visible);
            if (animations && SystemAnimationsEnabled() && m_host.IsShowing())
                AnimateVisualOpacity(hasText ? static_cast<UIElement>(LyricSlot()) : static_cast<UIElement>(DeviceRow()), 0.0f, 1.0f, 240);
        }
        m_lyricLineVisible = hasText;

        if (index != m_activeLine)
        {
            m_activeLine = index;
            if (hasText)
            {
                auto& outgoing = m_slots[m_front];
                int back = 1 - m_front;
                auto& incoming = m_slots[back];
                incoming->SetTranslationEnabled(m_translationOn);
                incoming->SetLine(m_lines[index]);
                incoming->SetActive(true);
                outgoing->SetActive(false);
                m_front = back;
                if (animations && hadLine)
                {
                    m_transitionStart = now;
                }
                else
                {
                    m_transitionStart = -1;
                    incoming->Root().Opacity(1);
                    incoming->Root().Translation({ 0, 0, 0 });
                    outgoing->Root().Opacity(0);
                }
            }
        }
        if (m_transitionStart >= 0)
        {
            constexpr double kDuration = 240.0;
            double t = std::clamp((now - m_transitionStart) / kDuration, 0.0, 1.0);
            double e = 1.0 - std::pow(1.0 - t, 3.0);   // ease-out cubic
            auto in = m_slots[m_front]->Root();
            auto out = m_slots[1 - m_front]->Root();
            in.Opacity(e);
            in.Translation({ 0, static_cast<float>((1.0 - e) * 9.0), 0 });
            out.Opacity(1.0 - e);
            out.Translation({ 0, static_cast<float>(-e * 9.0), 0 });
            if (t >= 1.0) m_transitionStart = -1;
        }
        if (hasText) m_slots[m_front]->Update(positionMs, animations);
        UpdateSubLabel();
    }

    void CompactWindow::UpdateSubLabel()
    {
        bool translation = m_lyricsShown && m_lyricLineVisible && m_slots[0] && m_slots[m_front]->TranslationShown();
        auto want = m_hasArtistLine && !translation ? Visibility::Visible : Visibility::Collapsed;
        if (SubLabel().Visibility() != want) SubLabel().Visibility(want);
    }

    void CompactWindow::OnFrame()
    {
        if (m_closed) return;
        double now = QpcNowMs();
        if (m_lyricsShown && m_session) UpdateLyricLine(LyricPosition(now), now);
        UpdateFrameLoop();
    }

    double CompactWindow::LyricPosition(double now) const
    {
        if (!m_session) return -1;
        return lyrics::LyricPosition(m_clock, now, m_session->lyricsDelayMs);
    }

    void CompactWindow::UpdatePlayState()
    {
        if (!m_session) return;
        bool playing = m_session->IsPlaying();
        PlayPauseIcon().Glyph(playing ? L"\xE769" : L"\xE768");
        PlayPauseButton().IsEnabled(playing ? m_session->CanPause() : m_session->CanPlay());
        ToolTipService::SetToolTip(PlayPauseButton(), box_value(playing ? Str(L"Pause") : Str(L"Play")));
    }

    void CompactWindow::TogglePlay()
    {
        if (!m_session) return;
        bool playing = m_session->IsPlaying();
        if (playing ? !m_session->CanPause() : !m_session->CanPlay()) return;
        std::string action = playing ? (m_session->Has("pause") ? "pause" : "toggle")
                                     : (m_session->Has("play") ? "play" : "toggle");
        Controller::Get().Media(m_session->side, m_session->key, action);
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

    void CompactWindow::UpdateFrameLoop()
    {
        auto& c = Controller::Get();
        bool playing = m_session && m_session->IsPlaying() && c.Link() == HostLink::Connected;
        bool want = !m_closed && m_host.IsShowing() && m_lyricsShown && !c.LowPower() && (playing || m_transitionStart >= 0);
        if (want && !m_rendering)
        {
            if (!m_frameTimer)
            {
                // ~30 fps frame clock: the 12.5 px line moves a fraction of a pixel per frame, so
                // 30 fps is visually continuous here at half the cost (see also
                // MainWindow::UpdateFrameLoop for why not CompositionTarget::Rendering).
                m_frameTimer = DispatcherQueue().CreateTimer();
                m_frameTimer.Interval(std::chrono::milliseconds(33));
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

    void CompactWindow::UpdateTimer()
    {
        if (!m_timer) return;
        UpdateFrameLoop();
        auto& c = Controller::Get();
        bool want = m_host.IsShowing() && m_session && m_session->IsPlaying() &&
            (m_session->durationMs > 0 || m_lyricsShown) && c.Link() == HostLink::Connected;
        if (!want)
        {
            if (m_timer.IsRunning()) m_timer.Stop();
            return;
        }
        // Progress line: 1 s. Low-power mode also drives the lyric line at ~10 fps.
        int ms = c.LowPower() && m_lyricsShown ? 100 : 1000;
        auto interval = std::chrono::milliseconds(ms);
        if (m_timer.Interval() != interval) m_timer.Interval(interval);
        if (!m_timer.IsRunning()) m_timer.Start();
    }
}
