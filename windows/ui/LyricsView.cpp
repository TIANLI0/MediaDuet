#include "pch.h"
#include "LyricsView.h"

#include "Controller.h"
#include "RenderTest.h"

#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <algorithm>
#include <cmath>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Windows::UI::Color;

namespace mdui
{
    namespace
    {
        constexpr double kFollowAt = 0.38;          // current line centre, fraction of the height
        constexpr float kInactiveScale = 0.93f;
        constexpr int kResumeMs = 4000;             // manual scrolling pauses following this long
        constexpr double kPlainPadding = 16.0;
        constexpr int kRevealStepMs = 34;           // staggered fade-in of new lyrics
        constexpr double kDragSlop = 8.0;           // touch / pen travel before a drag counts as scrolling

        // Follow: lines further below the current one take a little longer (wave); the current line
        // and everything above it move together. Wheel: short.
        Vector3Transition MakeTransition(int bucket)
        {
            Vector3Transition t;
            int ms = bucket == 0 ? 200 : 480 + 40 * (bucket - 1);
            t.Duration(std::chrono::milliseconds(ms));
            return t;
        }
    }

    double LyricsView::FontSizeFor(double height) const
    {
        // 26 epx in short windows, growing to 34 epx in tall ones (Apple Music full-screen feel).
        if (height <= 0) return 26;
        return std::round(std::clamp(26.0 + (height - 520.0) * (8.0 / 680.0), 26.0, 34.0));
    }

    void LyricsView::Attach(Grid const& viewport, Canvas const& canvas, StackPanel const& panel, Button const& followButton,
        Callbacks callbacks)
    {
        m_viewport = viewport;
        m_canvas = canvas;
        m_panel = panel;
        m_followButton = followButton;
        m_callbacks = std::move(callbacks);
        m_alive = std::make_shared<bool>(true);
        m_clip = RectangleGeometry();
        m_viewport.Clip(m_clip);
        auto alive = m_alive;
        auto queue = viewport.DispatcherQueue();

        m_resumeTimer = queue.CreateTimer();
        m_resumeTimer.IsRepeating(false);
        m_resumeTimer.Interval(std::chrono::milliseconds(kResumeMs));
        m_resumeTimer.Tick([this, alive](auto&&, auto&&) {
            if (*alive) ResumeFollow();
        });
        m_revealTimer = queue.CreateTimer();
        m_revealTimer.IsRepeating(true);
        m_revealTimer.Interval(std::chrono::milliseconds(kRevealStepMs));
        m_revealTimer.Tick([this, alive](auto&&, auto&&) {
            if (*alive) RevealStep();
        });
        m_fontTimer = queue.CreateTimer();
        m_fontTimer.IsRepeating(false);
        m_fontTimer.Interval(std::chrono::milliseconds(200));
        m_fontTimer.Tick([this, alive](auto&&, auto&&) {
            if (*alive && FontSizeFor(m_height) != m_fontSize && !m_lines.empty()) Rebuild();
        });

        m_viewportSizeToken = viewport.SizeChanged([this, alive](auto&&, SizeChangedEventArgs const& e) {
            if (*alive) OnViewportSize(e.NewSize().Width, e.NewSize().Height);
        });
        // Line heights change with wrapping, the translation toggle or new lyrics: re-place.
        m_panelSizeToken = panel.SizeChanged([this, alive](auto&&, auto&&) {
            if (!*alive || m_lines.empty()) return;
            if (!m_placed) Place();
            else if (m_synced && !m_followPaused) Follow(Mode::Direct);
            else ApplyOffset(ClampOffset(m_offset), Mode::Direct);
        });

        m_wheelToken = viewport.PointerWheelChanged([this, alive](auto&&, PointerRoutedEventArgs const& e) {
            if (!*alive || m_lines.empty()) return;
            auto props = e.GetCurrentPoint(m_viewport).Properties();
            if (props.IsHorizontalMouseWheel()) return;
            int delta = props.MouseWheelDelta();
            if (delta == 0) return;
            e.Handled(true);
            double target = ClampOffset(m_offset - delta * 0.75);
            if (std::abs(target - m_offset) < 0.5) return;   // already at the end: nothing scrolls
            PauseFollow();
            ApplyOffset(target, Mode::Wheel);
            UpdateFollowButton();
        });
        // Touch / pen drag scrolling. The mouse scrolls with the wheel only: a click (with the
        // slight jitter of a real click) must never pause following or show the follow button.
        viewport.ManipulationMode(ManipulationModes::TranslateY | ManipulationModes::TranslateInertia);
        m_manipStartToken = viewport.ManipulationStarted([this, alive](auto&&, ManipulationStartedRoutedEventArgs const& e) {
            if (!*alive) return;
            m_dragging = false;
            m_dragTravel = 0;
            if (e.PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse || m_lines.empty())
            {
                e.Complete();
                return;
            }
            m_dragging = true;
        });
        m_manipDeltaToken = viewport.ManipulationDelta([this, alive](auto&&, ManipulationDeltaRoutedEventArgs const& e) {
            if (!*alive || !m_dragging || m_lines.empty()) return;
            double dy = e.Delta().Translation.Y;
            if (!m_followPaused)
            {
                m_dragTravel += std::abs(dy);
                if (m_dragTravel < kDragSlop) return;   // a tap, not a drag
                PauseFollow();
            }
            ApplyOffset(ClampOffset(m_offset - dy), Mode::Direct);
            UpdateFollowButton();
            m_resumeTimer.Stop();
            m_resumeTimer.Start();
        });
        m_manipDoneToken = viewport.ManipulationCompleted([this, alive](auto&&, auto&&) {
            if (!*alive) return;
            m_dragging = false;
            if (m_followPaused)
            {
                m_resumeTimer.Stop();
                m_resumeTimer.Start();
            }
        });
        m_followClickToken = followButton.Click([this, alive](auto&&, auto&&) {
            if (*alive) ResumeFollow();
        });
        // Theme switches while paused (no frames running) still recolour the lines.
        m_themeToken = viewport.ActualThemeChanged([this, alive](auto&&, auto&&) {
            if (*alive) ApplyColors();
        });
        followButton.Visibility(Visibility::Collapsed);
    }

    double LyricsView::EdgeFactor(int index) const
    {
        // Fades a line out as its centre approaches the top / bottom edge of the viewport (at the
        // target scroll position; the opacity transition runs alongside the scroll).
        if (m_height <= 0) return 1.0;
        auto root = m_lines[index]->Root();
        double y = root.ActualOffset().y + root.ActualHeight() / 2 - m_offset;
        auto ramp = [](double x) {
            x = std::clamp(x, 0.0, 1.0);
            return x * x * (3 - 2 * x);
        };
        double top = m_height * 0.12, bottom = m_height * 0.18;
        return ramp(y / top) * ramp((m_height - y) / bottom);
    }

    void LyricsView::Detach()
    {
        if (!m_alive) return;
        *m_alive = false;
        if (m_resumeTimer) m_resumeTimer.Stop();
        if (m_revealTimer) m_revealTimer.Stop();
        if (m_fontTimer) m_fontTimer.Stop();
        if (m_viewport)
        {
            m_viewport.SizeChanged(m_viewportSizeToken);
            m_viewport.PointerWheelChanged(m_wheelToken);
            m_viewport.ManipulationStarted(m_manipStartToken);
            m_viewport.ManipulationDelta(m_manipDeltaToken);
            m_viewport.ManipulationCompleted(m_manipDoneToken);
            m_viewport.ActualThemeChanged(m_themeToken);
        }
        if (m_panel) m_panel.SizeChanged(m_panelSizeToken);
        if (m_followButton) m_followButton.Click(m_followClickToken);
        Clear();
    }

    void LyricsView::OnViewportSize(double width, double height)
    {
        m_width = width;
        m_height = height;
        m_clip.Rect({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
        // The panel is inside a Canvas (no layout clipping, no height constraint): give it the width.
        double panelWidth = std::max(0.0, width);
        if (std::abs(m_panel.Width() - panelWidth) > 0.5 || std::isnan(m_panel.Width())) m_panel.Width(panelWidth);
        if (m_lines.empty()) return;
        if (FontSizeFor(height) != m_fontSize)
        {
            m_fontTimer.Stop();
            m_fontTimer.Start();   // rebuilt with the new size once resizing settles
        }
        if (!m_placed) Place();
        else if (m_synced && !m_followPaused) Follow(Mode::Direct);
        else ApplyOffset(ClampOffset(m_offset), Mode::Direct);
    }

    void LyricsView::Clear()
    {
        if (m_panel) m_panel.Children().Clear();
        m_lines.clear();
        m_rows = {};
        m_bucket.clear();
        m_revealed.clear();
        m_active = -2;
        m_pinned = -1;
        m_placed = false;
        m_followPaused = false;
        if (m_revealTimer) m_revealTimer.Stop();
        if (m_resumeTimer) m_resumeTimer.Stop();
        if (m_followButton) m_followButton.Visibility(Visibility::Collapsed);
    }

    void LyricsView::FadeOut()
    {
        if (!m_panel || m_lines.empty()) return;
        m_revealTimer.Stop();
        if (Controller::Get().AnimationsEnabled())
        {
            ScalarTransition fade;
            fade.Duration(std::chrono::milliseconds(220));
            m_panel.OpacityTransition(fade);
        }
        m_panel.Opacity(0.0);
    }

    void LyricsView::Rebuild()
    {
        // Font size changed: rebuild in place (no fade-in), keeping the scroll / follow state.
        bool paused = m_followPaused;
        auto source = m_source;
        Build(source, m_translationOn);
        for (size_t i = 0; i < m_revealed.size(); ++i) m_revealed[i] = true;
        m_revealTimer.Stop();
        m_followPaused = paused;
        ApplyLineStates();
    }

    void LyricsView::Build(Lyrics const& lyrics, bool translationOn)
    {
        Clear();
        m_source = lyrics;
        m_synced = lyrics.synced;
        m_translationOn = translationOn;
        m_animations = Controller::Get().AnimationsEnabled();
        m_fontSize = FontSizeFor(m_height);
        m_core = ToCoreLyrics(lyrics);
        if (m_synced)
        {
            std::vector<bool> blank;
            blank.reserve(lyrics.lines.size());
            for (auto const& l : lyrics.lines) blank.push_back(LineIsBlank(l));
            m_rows = lyrics::BuildDisplayRows(m_core, blank);
        }
        else
        {
            for (size_t i = 0; i < lyrics.lines.size(); ++i)
                m_rows.rows.push_back({ static_cast<int>(i), lyrics.lines[i].t, lyrics.lines[i].d, lyrics::RowKind::Text });
        }

        KaraokeLine::Style style;
        style.fontSize = m_fontSize;
        style.lineHeight = std::round(m_fontSize * 1.36);
        style.wrap = true;
        m_panel.Spacing(std::round(m_fontSize * 0.5));
        double pad = std::round(m_fontSize * 0.3);
        auto alive = m_alive;
        m_panel.OpacityTransition(nullptr);
        for (size_t i = 0; i < m_rows.rows.size(); ++i)
        {
            auto const& d = m_rows.rows[i];
            auto line = std::make_unique<KaraokeLine>(style);
            line->SetTranslationEnabled(translationOn);
            if (d.kind == lyrics::RowKind::Interlude) line->SetInterlude(d.t, d.d);
            else line->SetLine(lyrics.lines[static_cast<size_t>(d.line)]);
            auto root = line->Root();
            root.Padding(Thickness{ 0, pad, 12, pad });
            if (d.kind == lyrics::RowKind::Empty) root.Visibility(Visibility::Collapsed);
            root.Opacity(0.0);   // revealed line by line once placed
            if (m_synced)
            {
                root.Scale({ kInactiveScale, kInactiveScale, 1.0f });
                // Scale around the left edge, vertically centred.
                root.SizeChanged([root](auto&&, SizeChangedEventArgs const& e) {
                    root.CenterPoint({ 0.0f, static_cast<float>(e.NewSize().Height / 2), 0.0f });
                });
                int index = static_cast<int>(i);
                int64_t t = d.t;
                root.Tapped([this, alive, index, t](auto&&, TappedRoutedEventArgs const& e) {
                    if (!*alive || !m_callbacks.canSeek || !m_callbacks.canSeek()) return;
                    e.Handled(true);
                    // The view shows the clicked line right away (the lyric position reaches it
                    // only after the output delay) and follows it again.
                    m_pinned = index;
                    m_pinUntil = QpcNowMs() + 2500;
                    m_followPaused = false;
                    m_resumeTimer.Stop();
                    m_followButton.Visibility(Visibility::Collapsed);
                    if (m_callbacks.seek) m_callbacks.seek(t);
                });
            }
            if (m_animations)
            {
                // Quick enough that the emphasis of a new current line never lags behind its fill.
                ScalarTransition fade;
                fade.Duration(std::chrono::milliseconds(220));
                root.OpacityTransition(fade);
                if (m_synced)
                {
                    Vector3Transition grow;
                    grow.Duration(std::chrono::milliseconds(260));
                    root.ScaleTransition(grow);
                }
            }
            m_panel.Children().Append(root);
            m_lines.push_back(std::move(line));
            m_bucket.push_back(-2);
            m_revealed.push_back(false);
        }
        m_colorsSet = false;
        ApplyColors();
        m_offset = m_synced ? 0 : -kPlainPadding;
        // Until the panel has been laid out the lines stay invisible (their place is unknown).
        for (auto const& l : m_lines) l->Root().Translation({ 0, static_cast<float>(-m_offset), 0 });
        m_panel.Opacity(m_lines.empty() ? 1.0 : 0.0);
        if (m_width > 0) m_panel.Width(m_width);
        m_panel.UpdateLayout();
        Place();
    }

    void LyricsView::Place()
    {
        if (m_lines.empty() || m_height <= 0) return;   // the viewport's SizeChanged places later
        m_placed = true;
        if (m_synced) Follow(Mode::Direct);
        else ApplyOffset(-kPlainPadding, Mode::Direct);
        m_panel.Opacity(1);
        if (!m_animations)
        {
            std::fill(m_revealed.begin(), m_revealed.end(), true);
            ApplyLineStates();
        }
        else
        {
            m_revealTimer.Start();
        }
    }

    void LyricsView::RevealStep()
    {
        // One visible line per step, top to bottom; once the visible ones are in, the rest at once.
        for (size_t i = 0; i < m_lines.size(); ++i)
        {
            if (m_revealed[i]) continue;
            auto root = m_lines[i]->Root();
            double y = root.ActualOffset().y - m_offset;
            bool visible = root.Visibility() == Visibility::Visible && y + root.ActualHeight() > -40 && y < m_height + 40;
            if (!visible) continue;
            m_revealed[i] = true;
            ApplyLineStates();
            return;
        }
        std::fill(m_revealed.begin(), m_revealed.end(), true);
        ApplyLineStates();
        m_revealTimer.Stop();
    }

    void LyricsView::ApplyColors()
    {
        bool dark = m_viewport && m_viewport.ActualTheme() == ElementTheme::Dark;
        if (m_colorsSet && dark == m_dark) return;
        m_colorsSet = true;
        m_dark = dark;
        // Unsung part of the current line at ~50 %: brighter than every other line (<= 36 %).
        Color sung = dark ? Color{ 255, 255, 255, 255 } : Color{ 0xE6, 0, 0, 0 };
        Color unsung = dark ? Color{ 0x80, 255, 255, 255 } : Color{ 0x6E, 0, 0, 0 };
        for (auto const& l : m_lines) l->SetColors(sung, unsung);
    }

    void LyricsView::SetTranslation(bool on)
    {
        if (m_translationOn == on) return;
        m_translationOn = on;
        for (auto const& l : m_lines) l->SetTranslationEnabled(on);
        // Line heights change: the panel's SizeChanged re-centres the current line.
    }

    void LyricsView::ApplyLineStates()
    {
        for (int i = 0; i < static_cast<int>(m_lines.size()); ++i)
        {
            auto root = m_lines[i]->Root();
            double opacity = 0.8;   // plain lyrics
            float scale = 1.0f;
            if (m_synced)
            {
                int d = m_active < 0 ? 4 : std::abs(i - m_active);
                opacity = lyrics::LineOpacity(d);
                scale = d == 0 ? 1.0f : kInactiveScale;
            }
            if (!m_revealed[i]) opacity = 0.0;
            else opacity *= EdgeFactor(i);
            if (std::abs(root.Opacity() - opacity) > 1e-3) root.Opacity(opacity);
            if (root.Scale().x != scale) root.Scale({ scale, scale, 1.0f });
        }
    }

    void LyricsView::Update(double positionMs, bool force)
    {
        if (m_lines.empty()) return;
        ApplyColors();
        if (!m_synced) return;
        bool animations = Controller::Get().AnimationsEnabled();
        if (animations != m_animations)
        {
            m_animations = animations;
            std::fill(m_bucket.begin(), m_bucket.end(), -2);   // transitions are re-chosen
        }
        int index = lyrics::CurrentRow(m_core, m_rows, positionMs);
        if (m_pinned >= 0)
        {
            if (index >= m_pinned || QpcNowMs() > m_pinUntil || m_pinned >= static_cast<int>(m_lines.size())) m_pinned = -1;
            else index = m_pinned;
        }
        if (index != m_active || force)
        {
            if (m_active >= 0 && m_active < static_cast<int>(m_lines.size()) && m_active != index) m_lines[m_active]->SetActive(false);
            bool changed = index != m_active;
            bool fresh = m_active == -2;   // first position after Build: no scroll animation
            m_active = index;
            if (index >= 0 && (changed || !m_lines[index]->IsActive())) m_lines[index]->SetActive(true);
            ApplyLineStates();
            if (m_placed && changed) Follow(fresh ? Mode::Direct : Mode::Follow);
            if (m_followPaused) UpdateFollowButton();
        }
        if (index >= 0)
        {
            double p = m_pinned == index ? std::max(positionMs, static_cast<double>(m_rows.rows[index].t)) : positionMs;
            m_lines[index]->Update(p, animations);
        }
    }

    double LyricsView::TargetFor(int index) const
    {
        if (m_lines.empty()) return 0;
        index = std::clamp(index, 0, static_cast<int>(m_lines.size()) - 1);
        // An empty (collapsed) row keeps the previous visible line in place.
        while (index > 0 && m_lines[index]->Root().Visibility() != Visibility::Visible) --index;
        auto root = m_lines[index]->Root();
        double top = root.ActualOffset().y;
        return top + root.ActualHeight() / 2 - m_height * kFollowAt;
    }

    double LyricsView::ClampOffset(double offset) const
    {
        if (m_lines.empty()) return 0;
        if (m_synced) return std::clamp(offset, TargetFor(0), std::max(TargetFor(0), TargetFor(static_cast<int>(m_lines.size()) - 1)));
        double max = std::max(-kPlainPadding, m_panel.ActualHeight() - m_height + kPlainPadding);
        return std::clamp(offset, -kPlainPadding, max);
    }

    void LyricsView::ApplyOffset(double offset, Mode mode)
    {
        m_offset = offset;
        float y = static_cast<float>(-offset);
        for (int i = 0; i < static_cast<int>(m_lines.size()); ++i)
        {
            int bucket = -1;   // no transition
            if (m_animations)
            {
                if (mode == Mode::Wheel) bucket = 0;
                else if (mode == Mode::Follow) bucket = 1 + std::clamp(i - std::max(0, m_active), 0, 7);
            }
            auto root = m_lines[i]->Root();
            if (bucket != m_bucket[i])
            {
                m_bucket[i] = bucket;
                root.TranslationTransition(bucket < 0 ? nullptr : MakeTransition(bucket));
            }
            auto current = root.Translation();
            if (std::abs(current.y - y) > 0.01f) root.Translation({ 0, y, 0 });
        }
        if (m_placed) ApplyLineStates();   // the edge fade follows the new position
    }

    void LyricsView::Follow(Mode mode)
    {
        if (!m_synced || m_followPaused || m_lines.empty() || m_height <= 0) return;
        double target = TargetFor(m_active);
        if (std::abs(target - m_offset) < 0.5 && mode != Mode::Direct) return;
        ApplyOffset(target, mode);
    }

    void LyricsView::PauseFollow()
    {
        m_pinned = -1;
        m_followPaused = true;
        m_resumeTimer.Stop();
        m_resumeTimer.Start();
    }

    void LyricsView::UpdateFollowButton()
    {
        // Only while the current line is really out of its place (scrolled at least about a line
        // away); a scroll that ends where following would put it needs no "back" button.
        bool away = m_synced && m_followPaused && m_active >= 0 && std::abs(m_offset - TargetFor(m_active)) > m_fontSize * 1.5;
        auto want = away ? Visibility::Visible : Visibility::Collapsed;
        if (m_followButton.Visibility() != want) m_followButton.Visibility(want);
    }

    void LyricsView::ResumeFollow()
    {
        m_resumeTimer.Stop();
        if (m_followButton) m_followButton.Visibility(Visibility::Collapsed);
        if (!m_followPaused) return;
        m_followPaused = false;
        Follow(Mode::Follow);
    }
}
