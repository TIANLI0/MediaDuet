#include "pch.h"
#include "Karaoke.h"

#include "LyricWrapPanel.h"
#include "RenderTest.h"

#include <winrt/Microsoft.UI.Text.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Windows::UI::Color;

namespace mdui
{
    md::lyrics::Line ToCoreLine(LyricLine const& line)
    {
        md::lyrics::Line l;
        l.t = line.t;
        l.d = line.d;
        l.text = line.text;
        l.translation = line.tr;
        for (auto const& w : line.words) l.words.push_back({ w.t, w.d, w.text });
        return l;
    }

    md::lyrics::Lyrics ToCoreLyrics(Lyrics const& lyrics)
    {
        md::lyrics::Lyrics l;
        l.synced = lyrics.synced;
        for (auto const& line : lyrics.lines) l.lines.push_back(ToCoreLine(line));
        return l;
    }

    md::lyrics::LyricsClock::Report SessionReport(Session const& s, std::string const& trackKey)
    {
        md::lyrics::LyricsClock::Report r;
        if (s.positionMs >= 0) r.positionMs = s.positionMs;
        r.atMs = s.anchorQpcMs;
        r.rate = s.rate;
        r.playing = s.IsPlaying();
        r.trackId = trackKey.empty() ? s.key + "|" + s.trackId : trackKey;
        return r;
    }

    bool LineIsBlank(LyricLine const& line)
    {
        return md::lyrics::buildKaraoke(ToCoreLine(line)).text.empty();
    }

    namespace
    {
        struct LyricFontFile
        {
            wchar_t const* file;
            uint16_t weight;
        };
        constexpr LyricFontFile kLyricFonts[] = {
            { L"HarmonyOS_Sans_SC_Medium.ttf", 500 },
            { L"HarmonyOS_Sans_SC_Bold.ttf", 700 },
        };
        constexpr wchar_t kLyricFamily[] = L"HarmonyOS Sans SC";

        std::wstring ExeDir()
        {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            std::wstring path(buf, n);
            auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
        }

        // nullptr when the file is missing (checked once per weight).
        FontFamily LyricFamily(LyricWeight w)
        {
            static FontFamily families[2]{ nullptr, nullptr };
            static bool checked[2]{};
            int i = static_cast<int>(w);
            if (!checked[i])
            {
                checked[i] = true;
                std::wstring path = ExeDir() + L"\\Assets\\Fonts\\" + kLyricFonts[i].file;
                DWORD attr = GetFileAttributesW(path.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                    families[i] = FontFamily(std::wstring(L"ms-appx:///Assets/Fonts/") + kLyricFonts[i].file + L"#" + kLyricFamily);
            }
            return families[i];
        }

        winrt::Windows::UI::Text::FontWeight LyricFontWeight(LyricWeight w, bool translation)
        {
            if (LyricFamily(w)) return { kLyricFonts[static_cast<int>(w)].weight };
            return translation ? winrt::Microsoft::UI::Text::FontWeights::Normal() : winrt::Microsoft::UI::Text::FontWeights::SemiBold();
        }

        // Width of one space for a font size / weight: a whitespace-only TextBlock measures 0 wide
        // (trailing whitespace), so it is measured as "a a" minus "aa" - in the segments' own font.
        double SpaceWidth(double fontSize, LyricWeight weight)
        {
            static std::map<std::pair<int, int>, double> cache;
            auto key = std::make_pair(static_cast<int>(std::lround(fontSize * 10)), static_cast<int>(weight));
            if (auto it = cache.find(key); it != cache.end()) return it->second;
            auto measure = [&](wchar_t const* s) {
                TextBlock tb;
                tb.FontSize(fontSize);
                ApplyLyricFont(tb, weight);
                tb.Text(s);
                tb.Measure({ 1e6f, 1e6f });
                return static_cast<double>(tb.DesiredSize().Width);
            };
            double w = measure(L"a a") - measure(L"aa");
            if (!(w > 0.5)) w = fontSize * 0.28;
            cache[key] = w;
            return w;
        }

        double WhitespaceWidth(std::wstring_view s, double fontSize, LyricWeight weight)
        {
            double w = 0;
            for (wchar_t c : s)
            {
                if (c == 0x3000) w += fontSize;                    // ideographic space
                else if (c == 0x200B || c == 0xFEFF) w += 0;       // zero width
                else if (c == L'\t') w += 4 * SpaceWidth(fontSize, weight);
                else w += SpaceWidth(fontSize, weight);
            }
            return w;
        }
    }

    void ApplyLyricFont(TextBlock const& tb, LyricWeight weight, bool translation)
    {
        if (auto family = LyricFamily(weight)) tb.FontFamily(family);
        tb.FontWeight(LyricFontWeight(weight, translation));
    }

    bool LyricFontAvailable()
    {
        return LyricFamily(LyricWeight::Medium) && LyricFamily(LyricWeight::Bold);
    }

    KaraokeLine::KaraokeLine(Style const& style) : m_style(style)
    {
        if (m_style.lineHeight <= 0) m_style.lineHeight = std::round(m_style.fontSize * 1.36);
        auto panel = winrt::make<winrt::MediaDuetUI::implementation::LyricWrapPanel>();
        m_panelImpl = winrt::get_self<winrt::MediaDuetUI::implementation::LyricWrapPanel>(panel);
        m_panelImpl->layoutChanged = [this] {
            // New widths: the fill's line x-space changed.
            m_lastEdge = -1e9;
            for (auto& s : m_segs) s.applied = false;
        };
        m_panel = panel;
        m_panel.HorizontalAlignment(HorizontalAlignment::Left);
        m_panel.IsHitTestVisible(false);

        // Translation row: static, smaller, Medium weight (lighter than the Bold line), below the original text.
        m_tr.FontSize(std::round(m_style.fontSize * m_style.translationScale * 2.0) / 2.0);
        m_tr.TextWrapping(m_style.wrap ? TextWrapping::Wrap : TextWrapping::NoWrap);
        m_tr.TextTrimming(m_style.wrap ? TextTrimming::None : TextTrimming::CharacterEllipsis);
        m_tr.Margin(Thickness{ 0, std::max(1.0, m_style.fontSize * 0.12), 0, 0 });
        ApplyLyricFont(m_tr, LyricWeight::Medium, true);
        m_tr.Foreground(m_trBrush);
        m_tr.Opacity(0.6);
        m_tr.IsHitTestVisible(false);
        m_tr.Visibility(Visibility::Collapsed);
        Grid::SetRow(m_tr, 1);

        // Interlude dots (row 0, instead of the text).
        double dot = std::max(5.0, std::round(m_style.fontSize * 0.36));
        m_dots.Orientation(Orientation::Horizontal);
        m_dots.Spacing(dot * 0.9);
        m_dots.VerticalAlignment(VerticalAlignment::Center);
        m_dots.HorizontalAlignment(HorizontalAlignment::Left);
        m_dots.Margin(Thickness{ 2, 0, 0, 0 });
        m_dots.Height(m_style.lineHeight);
        m_dots.IsHitTestVisible(false);
        m_dots.Visibility(Visibility::Collapsed);
        for (auto& e : m_dot)
        {
            e = Shapes::Ellipse();
            e.Width(dot);
            e.Height(dot);
            e.VerticalAlignment(VerticalAlignment::Center);
            e.Fill(m_sungBrush);
            m_dots.Children().Append(e);
        }
        m_dots.SizeChanged([this](auto&&, SizeChangedEventArgs const& e) {
            // Breathe around the dots' centre.
            m_dots.CenterPoint({ static_cast<float>(e.NewSize().Width / 2), static_cast<float>(e.NewSize().Height / 2), 0.0f });
        });

        RowDefinition textRow;
        textRow.Height(GridLength{ 1, GridUnitType::Auto });
        RowDefinition trRow;
        trRow.Height(GridLength{ 1, GridUnitType::Auto });
        m_root.RowDefinitions().Append(textRow);
        m_root.RowDefinitions().Append(trRow);
        if (m_style.scrollLongLine)
        {
            // Single row that may scroll: the panel sits in a clipped Canvas (no layout clipping
            // of the panel itself, so its Translation can move the text under the clip).
            Canvas host;
            host.Height(m_style.lineHeight);
            RectangleGeometry clip;
            host.Clip(clip);
            host.SizeChanged([clip](auto&&, SizeChangedEventArgs const& e) {
                clip.Rect({ 0, -4, e.NewSize().Width, e.NewSize().Height + 8 });
            });
            host.Children().Append(m_panel);
            m_root.Children().Append(host);
        }
        else
        {
            m_root.Children().Append(m_panel);
        }
        m_root.Children().Append(m_dots);
        m_root.Children().Append(m_tr);
        m_root.Background(SolidColorBrush(Color{ 0, 0, 0, 0 }));   // whole line is clickable
        SetColors(Color{ 255, 255, 255, 255 }, Color{ 0x66, 255, 255, 255 });
    }

    KaraokeLine::~KaraokeLine()
    {
        if (m_panelImpl) m_panelImpl->layoutChanged = nullptr;
    }

    void KaraokeLine::SetLine(LyricLine const& line)
    {
        m_line = line;
        m_interlude = false;
        m_karaoke = md::lyrics::buildKaraoke(ToCoreLine(line));
        bool blank = m_karaoke.text.empty();
        // A blank line shown as text (plain lyrics) keeps a small note so it is not empty; synced
        // views never show blank lines (gaps are interlude rows, LyricTiming BuildDisplayRows).
        if (blank)
        {
            m_karaoke.text = L"\x266A";
            m_karaoke.words.clear();
            m_karaoke.codePoints = 1;
        }
        m_text = m_karaoke.text;
        BuildSegments();
        m_panel.Visibility(Visibility::Visible);
        m_dots.Visibility(Visibility::Collapsed);
        m_tr.Text(blank ? std::wstring{} : Widen(line.tr));
        ApplyTranslation();
        m_lastEdge = -1e9;
        m_wholeSung = false;
        SetAllSegments(m_active ? m_unsungBrush : m_sungBrush);
    }

    void KaraokeLine::BuildSegments()
    {
        // Word spans must start / end on segment boundaries so x(from) and x(to) are exact.
        std::vector<int> cuts;
        for (auto const& w : m_karaoke.words)
        {
            cuts.push_back(static_cast<int>(w.start));
            cuts.push_back(static_cast<int>(w.start + w.len));
        }
        m_segments = lyrics::Segmentize(m_text, cuts);
        auto children = m_panel.Children();
        children.Clear();
        m_segs.clear();
        m_segs.reserve(m_segments.size());
        for (auto const& s : m_segments)
        {
            Seg seg;
            TextBlock tb;
            tb.FontSize(m_style.fontSize);
            ApplyLyricFont(tb, m_style.weight);
            tb.LineHeight(m_style.lineHeight);
            tb.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            tb.TextWrapping(TextWrapping::NoWrap);
            tb.IsHitTestVisible(false);
            std::wstring_view part(m_text.data() + s.start, static_cast<size_t>(s.len));
            if (s.space)
            {
                // Whitespace measures as 0 wide in a TextBlock: give it its real advance instead.
                tb.Width(WhitespaceWidth(part, m_style.fontSize, m_style.weight));
            }
            else
            {
                tb.Text(winrt::hstring(part));   // a copy: the view is not null-terminated
            }
            tb.Foreground(m_sungBrush);
            children.Append(tb);
            seg.text = tb;
            m_segs.push_back(std::move(seg));
        }
        m_panelImpl->SetSegments(m_segments, m_style.lineHeight, m_style.wrap);
        if (m_style.scrollLongLine) m_panel.Translation({ 0, 0, 0 });
        m_lastScroll = 0;
    }

    void KaraokeLine::SetInterlude(int64_t t, int64_t d)
    {
        m_line = LyricLine{};
        m_line.t = t;
        m_line.d = d;
        m_interlude = true;
        m_interludeT = t;
        m_interludeD = std::max<int64_t>(1, d);
        m_karaoke = md::lyrics::KaraokeLine{};
        m_text.clear();
        m_segments.clear();
        m_segs.clear();
        m_panel.Children().Clear();
        m_panelImpl->SetSegments({}, m_style.lineHeight, m_style.wrap);
        m_panel.Visibility(Visibility::Collapsed);
        m_dots.Visibility(Visibility::Visible);
        m_tr.Text(L"");
        ApplyTranslation();
        m_lastDotScale = -1;
        UpdateInterlude(-1, false);
    }

    void KaraokeLine::SetColors(Color sung, Color unsung)
    {
        m_sung = sung;
        m_unsung = unsung;
        m_sungBrush.Color(sung);
        m_unsungBrush.Color(unsung);
        for (auto& s : m_segs)
        {
            if (!s.gradient) continue;
            s.on.Color(sung);
            s.off.Color(unsung);
        }
        if (!m_trCustomColor) m_trBrush.Color(sung);
    }

    void KaraokeLine::SetTranslationColor(Color color)
    {
        m_trCustomColor = true;
        m_trBrush.Color(color);
    }

    void KaraokeLine::SetTranslationEnabled(bool enabled)
    {
        if (m_trEnabled == enabled) return;
        m_trEnabled = enabled;
        ApplyTranslation();
    }

    void KaraokeLine::ApplyTranslation()
    {
        bool show = m_trEnabled && !m_tr.Text().empty();
        m_trShown = show;
        m_tr.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    }

    void KaraokeLine::SetAllSegments(Brush const& brush)
    {
        for (auto& s : m_segs)
        {
            s.text.Foreground(brush);
            s.applied = false;
        }
    }

    void KaraokeLine::SetActive(bool active)
    {
        m_active = active;
        if (m_interlude)
        {
            // Becoming current: dots start unlit (the next Update() fills them for the position).
            UpdateInterlude(-1, true);
            return;
        }
        m_tr.Opacity(active ? 0.8 : 0.6);
        m_lastEdge = -1e9;
        m_wholeSung = false;
        // Inactive lines are plain sung colour (the view dims them); an active line starts unsung
        // until the first Update() places the edge.
        SetAllSegments(active ? m_unsungBrush : m_sungBrush);
        if (!active && m_style.scrollLongLine && m_lastScroll != 0)
        {
            m_panel.Translation({ 0, 0, 0 });
            m_lastScroll = 0;
        }
    }

    void KaraokeLine::UpdateInterlude(double p, bool animate)
    {
        // Like Android: only the current interlude shows its dots (progressive + breathing); others
        // keep their place in the list but are invisible.
        double scale = 1.0;
        double opacity[3] = { 0.0, 0.0, 0.0 };
        if (m_active && p >= 0 && animate)
        {
            double elapsed = p - static_cast<double>(m_interludeT);
            double d = static_cast<double>(m_interludeD);
            double progress = std::clamp(elapsed / d, 0.0, 1.0);
            for (int i = 0; i < 3; ++i) opacity[i] = lyrics::InterludeDotOpacity(elapsed, d, lyrics::InterludeDotFill(progress, i));
            scale = lyrics::InterludeScale(elapsed, d);
        }
        else if (m_active)
        {
            // No animations (or no position yet): three steady dots.
            for (auto& o : opacity) o = animate ? 0.0 : 1.0;
        }
        for (int i = 0; i < 3; ++i)
            if (std::abs(m_dot[i].Opacity() - opacity[i]) > 0.004) m_dot[i].Opacity(opacity[i]);
        if (std::abs(scale - m_lastDotScale) > 0.002)
        {
            m_lastDotScale = scale;
            float s = static_cast<float>(scale);
            m_dots.Scale({ s, s, 1.0f });
        }
    }

    void KaraokeLine::ApplyFill(double edgeX, bool force)
    {
        auto const& lineX = m_panelImpl->LineX();
        if (lineX.size() != m_segs.size() + 1) return;   // not measured yet
        // Feather like the overlay: max(6, 0.5 x font size), centred on the edge.
        double feather = std::max(6.0, m_style.fontSize * 0.5);
        lyrics::SegmentFills(edgeX, feather, lineX, m_fills);
        for (size_t i = 0; i < m_segs.size(); ++i)
        {
            auto& s = m_segs[i];
            auto const& f = m_fills[i];
            if (m_segments[i].space)
            {
                s.mode = f.mode;   // nothing to draw
                continue;
            }
            if (f.mode == lyrics::FillMode::Edge)
            {
                if (!s.gradient)
                {
                    s.gradient = LinearGradientBrush();
                    s.gradient.MappingMode(BrushMappingMode::Absolute);
                    s.on = GradientStop();
                    s.on.Offset(0);
                    s.on.Color(m_sung);
                    s.off = GradientStop();
                    s.off.Offset(1);
                    s.off.Color(m_unsung);
                    s.gradient.GradientStops().Append(s.on);
                    s.gradient.GradientStops().Append(s.off);
                }
                if (force || !s.applied || s.mode != f.mode || std::abs(s.gStart - f.gradientStart) > 0.2)
                {
                    s.gradient.StartPoint({ static_cast<float>(f.gradientStart), 0 });
                    s.gradient.EndPoint({ static_cast<float>(f.gradientEnd), 0 });
                    s.gStart = f.gradientStart;
                    s.gEnd = f.gradientEnd;
                    if (!s.applied || s.mode != f.mode) s.text.Foreground(s.gradient);
                }
            }
            else if (force || !s.applied || s.mode != f.mode)
            {
                s.text.Foreground(f.mode == lyrics::FillMode::Sung ? m_sungBrush : m_unsungBrush);
            }
            s.mode = f.mode;
            s.applied = true;
        }
        // Single-row views (compact window): scroll a line wider than the view like the overlay.
        if (m_style.scrollLongLine)
        {
            double view = m_root.ActualWidth();
            double total = lineX.back();
            double off = view > 0 ? md::lyrics::scrollOffset(static_cast<float>(total), static_cast<float>(view), static_cast<float>(std::clamp(edgeX, 0.0, total))) : 0.0;
            if (std::abs(off - m_lastScroll) > 0.4)
            {
                m_lastScroll = off;
                m_panel.Translation({ static_cast<float>(-off), 0, 0 });
            }
        }
    }

    void KaraokeLine::Update(double positionMs, bool fillEnabled)
    {
        if (!m_active) return;
        if (m_interlude)
        {
            UpdateInterlude(positionMs, fillEnabled);
            return;
        }
        if (!fillEnabled)
        {
            // Simple highlight: the whole current line in the sung colour.
            if (!m_wholeSung)
            {
                SetAllSegments(m_sungBrush);
                m_wholeSung = true;
            }
            return;
        }
        bool force = false;
        if (m_wholeSung)
        {
            m_wholeSung = false;
            force = true;
        }
        auto const& lineX = m_panelImpl->LineX();
        if (lineX.size() != m_segs.size() + 1) return;   // measured on the next layout pass
        auto span = md::lyrics::fillAt(m_karaoke, positionMs);
        double edge = lyrics::EdgeX(span, m_segments, lineX);
        if (span.from >= m_karaoke.text.size()) edge = lineX.back() + 1e4;   // complete
        if (!force && std::abs(edge - m_lastEdge) < 0.25) return;   // sub-pixel movement is invisible
        m_lastEdge = edge;
        ApplyFill(edge, force);
    }
}
