#include "pch.h"
#include "SettingsPage.xaml.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

#include "Controller.h"
#include "PlaybackClock.h"
#include "QrCode.h"
#include "RenderTest.h"
#include "Strings.h"
#include "Version.h"

#include <dwrite.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace mdui;
using winrt::Windows::UI::Color;

namespace
{
    std::wstring TransportName(std::string const& t)
    {
        if (t == "bt") return std::wstring(Str(L"TransportBluetooth"));
        if (t == "lan") return std::wstring(Str(L"TransportLan"));
        return t.empty() ? std::wstring(L"\x2014") : Widen(t);
    }

    std::wstring Dash(std::wstring const& s)
    {
        return s.empty() ? std::wstring(L"\x2014") : s;
    }

    std::wstring FormatDuration(int64_t seconds)
    {
        if (seconds < 0) return L"\x2014";
        int64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
        if (h > 0) return StrF(L"DurationHM", { std::to_wstring(h), std::to_wstring(m) }).c_str();
        if (m > 0) return StrF(L"DurationMS", { std::to_wstring(m), std::to_wstring(s) }).c_str();
        return StrF(L"DurationS", { std::to_wstring(s) }).c_str();
    }

    std::wstring FormatLocalTime(int64_t unixMs)
    {
        if (unixMs <= 0) return {};
        ULARGE_INTEGER t;
        t.QuadPart = static_cast<ULONGLONG>(unixMs) * 10000ull + 116444736000000000ull;
        FILETIME ft{ t.LowPart, t.HighPart }, local{};
        SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
        auto const& lang = UiLanguage();
        wchar_t date[64]{}, time[64]{};
        GetDateFormatEx(lang.c_str(), DATE_SHORTDATE, &st, nullptr, date, 64, nullptr);
        GetTimeFormatEx(lang.c_str(), TIME_NOSECONDS, &st, nullptr, time, 64);
        return std::wstring(date) + L" " + time;
    }

    std::optional<Color> ParseColor(std::string const& s)
    {
        if (s.size() != 9 && s.size() != 7) return std::nullopt;
        if (s[0] != '#') return std::nullopt;
        unsigned long v = 0;
        try
        {
            v = std::stoul(s.substr(1), nullptr, 16);
        }
        catch (...)
        {
            return std::nullopt;
        }
        if (s.size() == 7) v |= 0xFF000000u;
        return Color{ static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v) };
    }

    std::string FormatColor(Color c)
    {
        char buf[16];
        sprintf_s(buf, "#%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
        return buf;
    }

    std::string NormalizeProcess(std::wstring text)
    {
        auto trim = [](std::wstring& s) {
            s.erase(0, s.find_first_not_of(L" \t\""));
            auto end = s.find_last_not_of(L" \t\"");
            s.erase(end == std::wstring::npos ? 0 : end + 1);
        };
        trim(text);
        if (text.empty()) return {};
        if (text.find(L'.') == std::wstring::npos) text += L".exe";
        return Narrow(text);
    }

    std::wstring PolicyStateName(std::string const& s)
    {
        if (s.empty() || s == "unknown") return L"\x2014";
        auto v = Str(L"PolicyState_" + Widen(s));
        return std::wstring(v);
    }

    std::wstring PairError(std::string const& code)
    {
        // Stable codes reported by the host in pairing.error (docs/ipc.md); anything else -> generic text.
        static const char* known[] = { "connect_failed", "unknown_device", "invalid_address", "connection_closed", "timeout",
            "rejected", "pairing_disabled", "mac_mismatch", "busy", "protocol", "bad_message" };
        for (auto k : known)
            if (code == k) return std::wstring(Str(L"PairErr_" + Widen(code)));
        return std::wstring(Str(L"PairErrUnknown"));
    }

    FontIcon MakeIcon(wchar_t const* glyph, double size = 20)
    {
        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(size);
        return icon;
    }
    // Screen readers: name a settings control after the title of the card it sits in.
    void NameFromCard(FrameworkElement const& control)
    {
        if (!Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(control).empty()) return;
        DependencyObject current = control;
        for (int depth = 0; depth < 4 && current; ++depth)
        {
            auto fe = current.try_as<FrameworkElement>();
            if (!fe) return;
            auto parent = fe.Parent();
            auto check = [&](Panel const& panel) -> bool {
                for (auto const& child : panel.Children())
                {
                    if (child == current) continue;
                    if (auto tb = child.try_as<TextBlock>(); tb && !tb.Text().empty() && panel.try_as<StackPanel>())
                    {
                        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(control, tb.Text());
                        return true;
                    }
                    if (auto sp = child.try_as<StackPanel>(); sp && sp.Children().Size() > 0)
                    {
                        if (auto tb = sp.Children().GetAt(0).try_as<TextBlock>(); tb && !tb.Text().empty())
                        {
                            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(control, tb.Text());
                            return true;
                        }
                    }
                }
                return false;
            };
            if (auto panel = parent.try_as<Panel>(); panel && check(panel)) return;
            current = parent;
        }
    }
}


namespace winrt::MediaDuetUI::implementation
{
    void SettingsPage::InitializeComponent()
    {
        SettingsPageT::InitializeComponent();
        auto& c = Controller::Get();

        BitmapImage icon(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/MediaDuet.png"));
        AboutIcon().Source(icon);
        BitmapImage avatar;
        avatar.DecodePixelWidth(192);   // the source size: crisp up to 340 % scale at 56 epx
        avatar.UriSource(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/Developer.png"));
        DeveloperAvatar().ImageSource(avatar);
        // The bundled lyrics font's licence (shipped next to the font files, opened in the default editor).
        HarmonyLicenseLink().Click([](auto&&, auto&&) {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            std::wstring path(buf, n);
            path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"Assets\\Fonts\\HarmonyOS_Sans_LICENSE.txt";
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });

        // ---- Navigation
        Nav().SelectionChanged([this](auto&&, NavigationViewSelectionChangedEventArgs const& e) {
            if (auto item = e.SelectedItem().try_as<NavigationViewItem>())
                ShowPage(std::wstring(unbox_value_or<hstring>(item.Tag(), L"general")));
        });
        Nav().SelectedItem(Nav().MenuItems().GetAt(0));

        // ---- Bindings: settings paths and documented defaults (docs/ipc.md)
        Bind(LanguageCombo(), "language", "auto");
        Bind(ThemeCombo(), "theme", "system");
        Bind(AnimationsToggle(), "ui.animations", true);
        Bind(LowPowerToggle(), "ui.lowPower", false);
        Bind(MainOnTopToggle(), "ui.alwaysOnTop", false);
        Bind(CompactOnTopToggle(), "ui.compact.alwaysOnTop", true);
        Bind(CompactSnapToggle(), "ui.compact.snap", true);
        Bind(AutostartToggle(), "autostart", true);
        Bind(WorkModeRadios(), "mode", "linked");   // the host also marks the choice as made (modeChosen)
        Bind(ArtworkOnlineToggle(), "artwork.online", true);
        Bind(ConnModeRadios(), "connection.mode", "auto");
        Bind(SwitchBackBox(), "connection.switchBackDelaySec", 10);
        Bind(PolicyModeRadios(), "policy.mode", "windows_first");
        Bind(AutoResumeToggle(), "policy.autoResume", true);
        Bind(ResumeDelayBox(), "policy.resumeDelaySec", 5);
        Bind(ActivationBox(), "policy.activationMs", 1500);
        Bind(SensitivitySlider(), SensitivityValue(), "policy.sensitivity", 0.02, 0);
        Bind(HeadsetAssistToggle(), "policy.headsetAssist", true);
        Bind(PauseForCallsToggle(), "policy.pauseForCalls", true);
        Bind(OnlineLyricsToggle(), "lyrics.online", true);
        Bind(TranslationToggle(), "lyrics.translation", true);
        Bind(OverlayLockToggle(), "lyrics.overlay.locked", false);
        Bind(OverlayClickThroughToggle(), "lyrics.overlay.clickThrough", false);
        Bind(OverlayAutoHideToggle(), "lyrics.overlay.autoHide", true);
        Bind(OverlayFontSizeBox(), "lyrics.overlay.fontSize", 32);
        Bind(OverlayOpacitySlider(), OverlayOpacityValue(), "lyrics.overlay.opacity", 0.92, 1);
        Bind(OverlayKaraokeToggle(), "lyrics.overlay.karaoke", true);
        Bind(OverlayTwoLinesToggle(), "lyrics.overlay.twoLines", true);
        Bind(OverlayTopmostToggle(), "lyrics.overlay.topmost", true);
        Bind(OverlayHideFullscreenToggle(), "lyrics.overlay.hideInFullscreen", false);
        Bind(OverlayLayoutCombo(), "lyrics.overlay.layout", "alternate");
        Bind(LatencyAutoToggle(), "lyrics.latency.auto", true);
        Bind(LatencyBtSlider(), LatencyBtValue(), "lyrics.latency.bluetoothMs", 180, 3);
        Bind(SmtcToggle(), "smtcBridge", true);
        Bind(TaskbarEnabledToggle(), "taskbar.enabled", true);
        Bind(TaskbarContentCombo(), "taskbar.content", "auto");
        Bind(TaskbarPositionCombo(), "taskbar.position", "left");
        Bind(TaskbarAlignCombo(), "taskbar.align", "auto");
        Bind(TaskbarOffsetSlider(), TaskbarOffsetValue(), "taskbar.offset", 0, 2);
        Bind(TaskbarOffsetBox(), "taskbar.offset", 0);   // typed values; the slider follows
        Bind(TaskbarWidthBox(), "taskbar.width", 360);
        Bind(TaskbarFontSizeBox(), "taskbar.fontSize", 13);
        Bind(NeteaseDesktopToggle(), "compat.neteaseDesktop", true);
        Bind(TaskbarResumeToggle(), "handoff.taskbarResume", true);
        Bind(UpdateAutoToggle(), "update.auto", true);

        // Every switch: knob at the right edge, our own "On"/"Off" label (MediaDuet's language) left
        // of it. Rebuilt once the page is loaded (the XAML parents are not wired up before that).
        m_alignedToggles = { AnimationsToggle(), LowPowerToggle(), MainOnTopToggle(), CompactOnTopToggle(), CompactSnapToggle(),
                 AutostartToggle(), AutoResumeToggle(), PauseForCallsToggle(), OnlineLyricsToggle(), OverlayShowToggle(),
                 OverlayLockToggle(), OverlayClickThroughToggle(), OverlayAutoHideToggle(), OverlayKaraokeToggle(),
                 OverlayTwoLinesToggle(), SmtcToggle(), A2dpToggle(), LanSendToggle(), DiagnosticsToggle(),
                 HeadsetAssistToggle(), TaskbarEnabledToggle(), NeteaseDesktopToggle(), TranslationToggle(), OverlayTopmostToggle(),
                 OverlayHideFullscreenToggle(), LatencyAutoToggle(), ArtworkOnlineToggle(), UpdateAutoToggle(), TaskbarResumeToggle() };

        // ---- Commands that are not settings
        OverlayShowToggle().Toggled([this](auto&&, auto&&) {
            if (!m_applying && OverlayShowToggle().IsOn() != Controller::Get().Model().overlayVisible)
                Controller::Get().SetLyricsOverlay(OverlayShowToggle().IsOn());
        });
        A2dpToggle().Toggled([this](auto&&, auto&&) {
            if (!m_applying && A2dpToggle().IsOn() != Controller::Get().Model().a2dpSink.IsOn())
                Controller::Get().SetA2dpSink(A2dpToggle().IsOn());
        });
        LanSendToggle().Toggled([this](auto&&, auto&&) {
            if (!m_applying && LanSendToggle().IsOn() != Controller::Get().Model().lanSend.IsOn())
                Controller::Get().SetLanSend(LanSendToggle().IsOn());
        });
        DiagnosticsToggle().Toggled([this](auto&&, auto&&) {
            if (m_applying || DiagnosticsToggle().IsOn() == Controller::Get().Model().diagnosticsEnabled) return;
            Controller::Get().SetDiagnostics(DiagnosticsToggle().IsOn());
            UpdateDiagnostics();
        });
        QuitHostButton().Click([](auto&&, auto&&) { Controller::Get().ExitHost(); });
        UpdateButton().Click([](auto&&, auto&&) {
            auto const& u = Controller::Get().Model().update;
            if (u.phase == "available" || (u.phase == "error" && !u.latest.empty() && u.latest != u.current)) Controller::Get().InstallUpdate();
            else Controller::Get().CheckUpdate();
        });
        UpdateNotesLink().Click([](auto&&, auto&&) {
            auto const& u = Controller::Get().Model().update;
            std::wstring url = Widen(u.page.empty() ? std::string("https://github.com/TIANLI0/MediaDuet/releases/latest") : u.page);
            if (url.rfind(L"https://", 0) == 0) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        StartHostButton().Click([this](auto&&, auto&&) {
            Controller::Get().LaunchHost();
            UpdateHostBar();
        });
        // Font family
        OverlayFontCombo().SelectionChanged([this](auto&&, auto&&) {
            if (m_applying) return;
            if (auto item = OverlayFontCombo().SelectedItem())
                Controller::Get().SetSetting("lyrics.overlay.fontFamily", Narrow(unbox_value_or<hstring>(item, L"")));
        });
        BrowseFolderButton().Click([this](auto&&, auto&&) { PickFolder(); });
        ClearFolderButton().Click([](auto&&, auto&&) { Controller::Get().SetSetting("lyrics.localFolder", ""); });
        OverlayResetButton().Click([](auto&&, auto&&) {
            Controller::Get().SetSettings(json{ { "lyrics", { { "overlay", { { "x", nullptr }, { "y", nullptr } } } } } });
        });
        ActiveColorFlyout().Opening([this](auto&&, auto&&) {
            if (auto col = ParseColor(Controller::Get().Model().Setting("lyrics.overlay.colorActive", std::string("#FFFFFFFF"))))
                ActiveColorPicker().Color(*col);
        });
        InactiveColorFlyout().Opening([this](auto&&, auto&&) {
            if (auto col = ParseColor(Controller::Get().Model().Setting("lyrics.overlay.colorInactive", std::string("#B3FFFFFF"))))
                InactiveColorPicker().Color(*col);
        });
        ActiveColorApply().Click([this](auto&&, auto&&) {
            ApplyColor("lyrics.overlay.colorActive", ActiveColorPicker(), ActiveColorFlyout());
        });
        InactiveColorApply().Click([this](auto&&, auto&&) {
            ApplyColor("lyrics.overlay.colorInactive", InactiveColorPicker(), InactiveColorFlyout());
        });

        // Lists
        BlacklistAdd().Click([this](auto&&, auto&&) { AddToList("policy.blacklist", BlacklistInput()); });
        WhitelistAdd().Click([this](auto&&, auto&&) { AddToList("policy.whitelist", WhitelistInput()); });
        PriorityAdd().Click([this](auto&&, auto&&) {
            std::wstring app(PriorityAppInput().Text());
            app.erase(0, app.find_first_not_of(L" \t"));
            app.erase(app.find_last_not_of(L" \t") + 1);
            double v = PriorityValueBox().Value();
            if (app.empty() || std::isnan(v)) return;
            // App ids contain dots (chrome.exe, com.example.app): build the patch explicitly.
            Controller::Get().SetSettings(json{ { "policy", { { "priorities", { { Narrow(app), static_cast<int>(std::lround(v)) } } } } } });
            PriorityAppInput().Text(L"");
            m_prioritySignature.clear();
            UpdateArbitration();
        });

        // Manual connection
        auto manual = [this](bool pair) {
            std::wstring host(ManualHost().Text());
            host.erase(0, host.find_first_not_of(L" \t"));
            host.erase(host.find_last_not_of(L" \t") + 1);
            double port = ManualPort().Value();
            if (host.empty() || std::isnan(port) || port < 1 || port > 65535)
            {
                ManualError().Text(Str(L"ManualInvalid"));
                ManualError().Visibility(Visibility::Visible);
                return;
            }
            ManualError().Visibility(Visibility::Collapsed);
            auto p = static_cast<int>(port);
            if (pair) Controller::Get().PairStart("lan", Narrow(host) + ":" + std::to_string(p));
            else Controller::Get().ConnectManual(Narrow(host), p);
        };
        ManualPairButton().Click([manual](auto&&, auto&&) { manual(true); });
        QrShowButton().Click([this](auto&&, auto&&) { ShowQr(true); });
        QrHideButton().Click([this](auto&&, auto&&) { ShowQr(false); });
        ManualConnectButton().Click([manual](auto&&, auto&&) { manual(false); });

        // Diagnostics rows (labels are static; values filled from "diagnostics" messages).
        struct Row { wchar_t const* label; char const* key; };
        static const Row rows[] = {
            { L"DiagCpu", "cpuPercent" }, { L"DiagWorkingSet", "workingSetMB" }, { L"DiagPrivate", "privateMB" },
            { L"DiagThreads", "threads" }, { L"DiagTransport", "transport" }, { L"DiagConnectedFor", "connectedForSec" },
            { L"DiagRtt", "rttMs" }, { L"DiagLatency", "lastCommandLatencyMs" }, { L"DiagBytes", "bytes" },
            { L"DiagMessages", "msgs" }, { L"DiagSessions", "sessions" }, { L"DiagAudioSessions", "audioSessionsActive" },
            { L"DiagUiMemory", "ui" },
        };
        int r = 0;
        for (auto const& row : rows)
        {
            DiagGrid().RowDefinitions().Append(RowDefinition());
            TextBlock label;
            label.Text(Str(row.label));
            label.Style(Application::Current().Resources().Lookup(box_value(L"ValueLabelStyle")).as<Microsoft::UI::Xaml::Style>());
            Grid::SetRow(label, r);
            TextBlock value;
            value.Style(Application::Current().Resources().Lookup(box_value(L"ValueTextStyle")).as<Microsoft::UI::Xaml::Style>());
            Grid::SetRow(value, r);
            Grid::SetColumn(value, 1);
            DiagGrid().Children().Append(label);
            DiagGrid().Children().Append(value);
            m_diagValues.emplace_back(value, row.key);
            ++r;
        }

        UiVersionValue().Text(MDUI_VERSION_WSTRING);

        auto weak = get_weak();
        m_tokens.push_back(c.Subscribe(Event::State, [weak] {
            if (auto self = weak.get()) self->RefreshAll();
        }));
        m_tokens.push_back(c.Subscribe(Event::Link, [weak] {
            if (auto self = weak.get())
            {
                self->RefreshAll();
                self->SyncDiscovery();
            }
        }));
        m_tokens.push_back(c.Subscribe(Event::Diagnostics, [weak] {
            if (auto self = weak.get())
                if (self->m_active && self->m_page == L"diagnostics") self->UpdateDiagnostics();
        }));
        m_tokens.push_back(c.SubscribeResults([weak](bool ok, std::string const& error, std::string const& type) {
            if (auto self = weak.get())
                // Only failures of actions the user just took on this page; background requests
                // (discovery, diagnostics) never raise a banner.
                if (!ok && type != "media.command" && type != "focus.set" && type != "discover" && type != "diagnostics" &&
                    type != "volume.set" && (type != "lyrics.offset" || self->m_active))
                    self->ShowResultError(error);
        }));

        // A pending pairing confirmation needs a XamlRoot, which exists only once the content is loaded.
        Root().Loaded([weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || self->m_closed) return;
            for (auto const& t : self->m_alignedToggles) self->RightAlignToggle(t);
            self->m_alignedToggles.clear();
            // Screen-reader names from the card titles (their x:Uid text is in place once loaded).
            for (auto const& b : self->m_toggles) NameFromCard(b.control);
            for (auto const& b : self->m_numbers) NameFromCard(b.control);
            for (auto const& b : self->m_combos) NameFromCard(b.control);
            for (auto const& b : self->m_radios) NameFromCard(b.control);
            for (auto const& b : self->m_sliders) NameFromCard(b->control);
            for (auto const& c2 : std::initializer_list<FrameworkElement>{ self->OverlayShowToggle(), self->A2dpToggle(),
                     self->LanSendToggle(), self->DiagnosticsToggle(), self->OverlayFontCombo(), self->QuitHostButton() })
                NameFromCard(c2);
            self->UpdatePairingDialog();
        });

        RefreshAll();
    }

    // Windows-Settings-style switch: the "On"/"Off" text sits left of the knob and the knob ends at
    // the card's right edge, in line with the combo boxes and number boxes. (WinUI draws its text
    // right of the knob, which leaves the knob ~100 px left of every other control.) The label is
    // ours, so it also follows MediaDuet's language instead of the OS language. The wrapper is a
    // Grid, not a StackPanel, so NameFromCard still names the switch after the card title.
    void SettingsPage::RightAlignToggle(ToggleSwitch const& toggle)
    {
        auto parent = toggle.Parent().try_as<Panel>();
        uint32_t index = 0;
        if (!parent || !parent.Children().IndexOf(toggle, index)) return;
        Grid row;
        row.ColumnSpacing(10);
        row.HorizontalAlignment(HorizontalAlignment::Right);
        row.VerticalAlignment(VerticalAlignment::Center);
        for (int i = 0; i < 2; ++i)
        {
            ColumnDefinition c;
            c.Width(GridLength{ 1, GridUnitType::Auto });
            row.ColumnDefinitions().Append(c);
        }
        Grid::SetColumn(row, Grid::GetColumn(toggle));
        Grid::SetRow(row, Grid::GetRow(toggle));
        Grid::SetColumnSpan(row, Grid::GetColumnSpan(toggle));
        TextBlock state;
        state.VerticalAlignment(VerticalAlignment::Center);
        // TryLookup: a missing key must never throw out of the constructor.
        auto key = box_value(L"ValueTextStyle");
        auto style = Resources().TryLookup(key);
        if (!style) style = Application::Current().Resources().TryLookup(key);
        if (auto st = style.try_as<Microsoft::UI::Xaml::Style>()) state.Style(st);
        parent.Children().RemoveAt(index);
        row.Children().Append(state);
        Grid::SetColumn(toggle, 1);
        Grid::SetRow(toggle, 0);
        Grid::SetColumnSpan(toggle, 1);
        // No built-in content: the template then only keeps its knob plus a fixed gap after it.
        toggle.OnContent(box_value(hstring{}));
        toggle.OffContent(box_value(hstring{}));
        toggle.MinWidth(0);
        toggle.Margin(Thickness{ 0, 0, -12, 0 });
        toggle.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(toggle);
        parent.Children().InsertAt(index, row);
        auto update = [state, toggle] {
            state.Text(toggle.IsOn() ? Str(L"On") : Str(L"Off"));
            state.Opacity(toggle.IsEnabled() ? 1.0 : 0.45);
        };
        update();
        toggle.Toggled([update](auto&&, auto&&) { update(); });
        toggle.IsEnabledChanged([update](auto&&, auto&&) { update(); });
    }

    // ---- Bindings ----------------------------------------------------------------------------

    void SettingsPage::Bind(ToggleSwitch const& control, std::string path, bool def)
    {
        control.Toggled([this, control, path, def](auto&&, auto&&) {
            // Only user changes are sent (programmatic updates and echoes are equal to the current value).
            if (!m_applying && Controller::Get().Model().Setting(path, def) != control.IsOn())
                Controller::Get().SetSetting(path, control.IsOn());
        });
        m_toggles.push_back({ control, std::move(path), def });
    }

    void SettingsPage::Bind(NumberBox const& control, std::string path, double def)
    {
        control.ValueChanged([this, path, def](NumberBox const& sender, NumberBoxValueChangedEventArgs const& e) {
            if (m_applying) return;
            double v = e.NewValue();
            if (std::isnan(v))
            {
                // Empty input: restore the current value instead of sending garbage.
                m_applying = true;
                sender.Value(e.OldValue());
                m_applying = false;
                return;
            }
            v = std::clamp(v, sender.Minimum(), sender.Maximum());
            if (Controller::Get().Model().SettingNumber(path, def) == v) return;
            if (v == std::floor(v)) Controller::Get().SetSetting(path, static_cast<int64_t>(v));
            else Controller::Get().SetSetting(path, v);
        });
        m_numbers.push_back({ control, std::move(path), def });
    }

    void SettingsPage::Bind(ComboBox const& control, std::string path, std::string def)
    {
        control.SelectionChanged([this, control, path, def](auto&&, auto&&) {
            if (m_applying) return;
            if (auto item = control.SelectedItem().try_as<ComboBoxItem>())
            {
                auto value = Narrow(unbox_value_or<hstring>(item.Tag(), L""));
                if (Controller::Get().Model().Setting(path, def) != value) Controller::Get().SetSetting(path, value);
            }
        });
        m_combos.push_back({ control, std::move(path), std::move(def) });
    }

    void SettingsPage::Bind(RadioButtons const& control, std::string path, std::string def)
    {
        // RadioButtons may raise SelectionChanged later (when its items are realised), so compare with
        // the current value instead of relying on m_applying alone.
        control.SelectionChanged([this, control, path, def](auto&&, auto&&) {
            if (m_applying) return;
            if (auto item = control.SelectedItem().try_as<RadioButton>())
            {
                auto value = Narrow(unbox_value_or<hstring>(item.Tag(), L""));
                if (Controller::Get().Model().Setting(path, def) != value) Controller::Get().SetSetting(path, value);
            }
        });
        m_radios.push_back({ control, std::move(path), std::move(def) });
    }

    static void FormatSlider(TextBlock const& label, double v, int format)
    {
        wchar_t buf[32];
        if (format == 1) swprintf_s(buf, L"%d%%", static_cast<int>(std::lround(v * 100)));
        else if (format == 2) swprintf_s(buf, L"%s", StrF(L"Pixels", { std::to_wstring(static_cast<int>(std::lround(v))) }).c_str());
        else if (format == 3) swprintf_s(buf, L"%s", StrF(L"Milliseconds", { std::to_wstring(static_cast<int>(std::lround(v))) }).c_str());
        else swprintf_s(buf, L"%.3f", v);
        label.Text(buf);
    }

    void SettingsPage::Bind(Slider const& control, TextBlock const& label, std::string path, double def, int format)
    {
        auto binding = std::make_unique<SliderBinding>();
        binding->control = control;
        binding->label = label;
        binding->path = std::move(path);
        binding->def = def;
        binding->format = format;
        binding->debounce = DispatcherQueue().CreateTimer();
        binding->debounce.IsRepeating(false);
        binding->debounce.Interval(std::chrono::milliseconds(300));
        auto raw = binding.get();
        binding->debounce.Tick([raw](auto&&, auto&&) {
            bool integer = raw->format >= 2;
            double v = integer ? std::round(raw->control.Value()) : std::round(raw->control.Value() * 1000.0) / 1000.0;
            if (std::abs(Controller::Get().Model().SettingNumber(raw->path, raw->def) - v) <= 1e-9) return;
            if (integer) Controller::Get().SetSetting(raw->path, static_cast<int64_t>(v));
            else Controller::Get().SetSetting(raw->path, v);
        });
        control.ValueChanged([this, raw](auto&&, RangeBaseValueChangedEventArgs const& e) {
            FormatSlider(raw->label, e.NewValue(), raw->format);
            if (m_applying) return;
            raw->debounce.Stop();
            raw->debounce.Start();   // send once the user stops dragging
        });
        m_sliders.push_back(std::move(binding));
    }

    void SettingsPage::ApplySettings()
    {
        auto const& st = Controller::Get().Model();
        m_applying = true;
        for (auto& b : m_toggles)
        {
            bool v = st.Setting(b.path, b.def);
            if (b.control.IsOn() != v) b.control.IsOn(v);
        }
        for (auto& b : m_numbers)
        {
            if (b.control.FocusState() != FocusState::Unfocused) continue;   // don't fight the user's typing
            double v = st.SettingNumber(b.path, b.def);
            if (b.control.Value() != v) b.control.Value(v);
        }
        for (auto& b : m_combos)
        {
            auto v = Widen(st.Setting(b.path, b.def));
            for (uint32_t i = 0; i < b.control.Items().Size(); ++i)
            {
                auto item = b.control.Items().GetAt(i).try_as<ComboBoxItem>();
                if (item && unbox_value_or<hstring>(item.Tag(), L"") == v)
                {
                    if (b.control.SelectedIndex() != static_cast<int32_t>(i)) b.control.SelectedIndex(i);
                    break;
                }
            }
        }
        for (auto& b : m_radios)
        {
            auto v = Widen(st.Setting(b.path, b.def));
            for (uint32_t i = 0; i < b.control.Items().Size(); ++i)
            {
                auto item = b.control.Items().GetAt(i).try_as<RadioButton>();
                if (item && unbox_value_or<hstring>(item.Tag(), L"") == v)
                {
                    if (b.control.SelectedIndex() != static_cast<int32_t>(i)) b.control.SelectedIndex(i);
                    break;
                }
            }
        }
        for (auto& b : m_sliders)
        {
            if (b->debounce.IsRunning()) continue;   // a change is about to be sent
            double v = st.SettingNumber(b->path, b->def);
            if (std::abs(b->control.Value() - v) > 1e-9) b->control.Value(v);
            FormatSlider(b->label, v, b->format);
        }
        m_applying = false;
    }

    // ---- Pages -------------------------------------------------------------------------------

    void SettingsPage::Navigate(std::wstring_view page)
    {
        std::wstring tag(page == L"pair" ? L"devices" : page);
        if (tag.empty()) return;
        auto select = [&](auto const& items) -> bool {
            for (auto const& obj : items)
            {
                auto item = obj.template try_as<NavigationViewItem>();
                if (item && unbox_value_or<hstring>(item.Tag(), L"") == tag)
                {
                    Nav().SelectedItem(item);
                    return true;
                }
            }
            return false;
        };
        if (!select(Nav().MenuItems())) select(Nav().FooterMenuItems());
        if (page == L"pair")
        {
            // Bring "Pair a new phone" into view once layout has happened.
            DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()] {
                if (auto self = weak.get()) self->PairNewHeader().StartBringIntoView();
            });
        }
    }

    void SettingsPage::ShowPage(std::wstring const& tag)
    {
        if (m_closed) return;
        m_page = tag;
        ResultBar().IsOpen(false);   // a notice belongs to the page where it happened
        std::pair<wchar_t const*, StackPanel> pages[] = {
            { L"general", PageGeneral() }, { L"devices", PageDevices() }, { L"connection", PageConnection() },
            { L"arbitration", PageArbitration() }, { L"lyrics", PageLyrics() }, { L"taskbar", PageTaskbar() },
            { L"integration", PageIntegration() }, { L"compat", PageCompat() }, { L"audio", PageAudio() },
            { L"diagnostics", PageDiagnostics() }, { L"about", PageAbout() },
        };
        for (auto& [name, panel] : pages)
            panel.Visibility(tag == name ? Visibility::Visible : Visibility::Collapsed);
        PageScroller().ChangeView(nullptr, 0.0, nullptr, true);
        if (tag == L"lyrics") LoadFonts();
        SyncDiscovery();
        RefreshAll();
    }

    void SettingsPage::SyncDiscovery()
    {
        bool want = m_page == L"devices" && m_active && Controller::Get().Link() == HostLink::Connected &&
            !Controller::Get().Model().standalone;
        bool started = want && !m_discoveryHeld;
        if (started) Controller::Get().AcquireDiscovery();
        if (!want && m_discoveryHeld) Controller::Get().ReleaseDiscovery();
        m_discoveryHeld = want;
        // The pairing QR code lives as long as the page: shown at once while no phone is paired.
        if (started && Controller::Get().Model().devices.empty()) ShowQr(true);
        else if (!want && (m_qrShown || !Controller::Get().Model().pairing.qrStatus.empty())) ShowQr(false);
        if (!m_spinnerTimer)
        {
            m_spinnerTimer = DispatcherQueue().CreateTimer();
            m_spinnerTimer.IsRepeating(false);
            m_spinnerTimer.Interval(std::chrono::seconds(15));
            m_spinnerTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get(); self && !self->m_closed) self->DiscoveryRing().IsActive(false);
            });
        }
        // Discovery keeps running for as long as the page is open; only the spinner is limited to
        // the first seconds of each session ("Searching…" stays visible).
        if (started)
        {
            DiscoveryRing().IsActive(true);
            m_spinnerTimer.Stop();
            m_spinnerTimer.Start();
        }
        else if (!want)
        {
            m_spinnerTimer.Stop();
            DiscoveryRing().IsActive(false);
        }
    }

    void SettingsPage::SetActive(bool active)
    {
        if (m_closed || m_active == active) return;
        m_active = active;
        if (active && m_dirty) RefreshAll();
        SyncDiscovery();
    }

    void SettingsPage::Close()
    {
        if (m_closed) return;
        m_closed = true;
        auto& c = Controller::Get();
        if (m_spinnerTimer) m_spinnerTimer.Stop();
        if (m_discoveryHeld)
        {
            c.ReleaseDiscovery();
            m_discoveryHeld = false;
        }
        if (m_qrShown) c.PairQr(false);
        for (auto& s : m_sliders)
        {
            if (s->debounce.IsRunning())
            {
                s->debounce.Stop();
                if (s->format >= 2) c.SetSetting(s->path, static_cast<int64_t>(std::round(s->control.Value())));
                else c.SetSetting(s->path, std::round(s->control.Value() * 1000.0) / 1000.0);
            }
        }
        for (auto t : m_tokens) c.Unsubscribe(t);
        m_tokens.clear();
        m_active = false;
    }

    // ---- Refresh -----------------------------------------------------------------------------

    void SettingsPage::RefreshAll()
    {
        if (m_closed) return;
        if (!m_active)
        {
            m_dirty = true;
            return;
        }
        m_dirty = false;
        UpdateHostBar();
        UpdateWorkMode();
        ApplySettings();
        UpdatePairingDialog();
        if (m_page == L"devices") { UpdateDevices(); UpdateDiscovered(); }
        else if (m_page == L"connection") UpdateConnection();
        else if (m_page == L"arbitration")
        {
            UpdateArbitration();
            UpdateOutputs();
        }
        else if (m_page == L"lyrics")
        {
            UpdateLyricsPage();
            UpdateProviders();
        }
        else if (m_page == L"audio") UpdateAudio();
        else if (m_page == L"diagnostics") UpdateDiagnostics();
        else if (m_page == L"about") UpdateAbout();
        else if (m_page == L"taskbar") UpdateTaskbar();
    }

    void SettingsPage::UpdateHostBar()
    {
        auto& c = Controller::Get();
        bool connected = c.Link() == HostLink::Connected && c.Model().valid;
        Pages().IsHitTestVisible(connected);
        Pages().Opacity(connected ? 1.0 : 0.5);
        if (connected)
        {
            HostBar().IsOpen(false);
            return;
        }
        bool connecting = c.Link() == HostLink::Connecting;
        HostBar().Severity(connecting ? InfoBarSeverity::Informational : InfoBarSeverity::Warning);
        HostBar().Title(connecting ? Str(L"HostConnecting") : Str(L"HostNotRunningTitle"));
        HostBar().Message(connecting ? hstring{} : (c.HostExecutableExists() ? Str(L"HostNotRunningShort") : Str(L"HostMissingText")));
        StartHostButton().Visibility(!connecting && c.HostExecutableExists() ? Visibility::Visible : Visibility::Collapsed);
        HostBar().IsOpen(true);
    }

    void SettingsPage::ShowResultError(std::string const& error)
    {
        ResultBar().Title(Str(L"CommandFailed"));
        ResultBar().Message(Widen(error));
        ResultBar().IsOpen(true);
    }

    // ---- Devices -----------------------------------------------------------------------------

    void SettingsPage::UpdateDevices()
    {
        auto const& st = Controller::Get().Model();
        std::string sig;
        for (auto const& d : st.devices)
            sig += d.id + "|" + d.name + "|" + d.platform + "|" + d.btAddress + "|" + d.lanHost + "|" +
                (d.connected ? "1" : "0") + d.transport + "|" + std::to_string(d.lastSeen) + "|" + d.app + "|" + d.versionMatch + "\n";
        UpdateDevicesVersionBar();
        UpdateQr();
        if (sig == m_devicesSignature) return;
        m_devicesSignature = sig;

        PairedList().Children().Clear();
        PairedEmpty().Visibility(st.devices.empty() ? Visibility::Visible : Visibility::Collapsed);
        auto cardStyle = Application::Current().Resources().Lookup(box_value(L"CardStyle")).as<Microsoft::UI::Xaml::Style>();
        auto descStyle = Application::Current().Resources().Lookup(box_value(L"CardDescriptionStyle")).as<Microsoft::UI::Xaml::Style>();
        for (auto const& d : st.devices)
        {
            Border card;
            card.Style(cardStyle);
            Grid grid;
            grid.ColumnSpacing(12);
            grid.ColumnDefinitions().Append(ColumnDefinition());
            grid.ColumnDefinitions().Append(ColumnDefinition());
            grid.ColumnDefinitions().Append(ColumnDefinition());
            grid.ColumnDefinitions().GetAt(0).Width(GridLength{ 0, GridUnitType::Auto });
            grid.ColumnDefinitions().GetAt(2).Width(GridLength{ 0, GridUnitType::Auto });

            auto icon = MakeIcon(L"\xE8EA");
            icon.Margin(Thickness{ 2, 0, 6, 0 });
            icon.VerticalAlignment(VerticalAlignment::Center);
            grid.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            TextBlock name;
            name.Text(Widen(d.name.empty() ? d.id : d.name));
            name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            text.Children().Append(name);
            TextBlock status;
            status.Style(descStyle);
            std::wstring s = d.connected ? StrF(L"DeviceConnectedVia", { TransportName(d.transport) }).c_str()
                                         : std::wstring(Str(L"DeviceNotConnected"));
            if (!d.connected && d.lastSeen > 0) s += L"  \x00B7  " + StrF(L"DeviceLastSeen", { FormatLocalTime(d.lastSeen) });
            status.Text(s);
            text.Children().Append(status);
            // "MediaDuet Android 1.2.1" (last version the phone reported) and how it compares with this PC.
            TextBlock ver;
            ver.Style(descStyle);
            ver.Text(DeviceVersionText(d.platform, d.app, d.versionMatch));   // the warning itself is the InfoBar above
            text.Children().Append(ver);
            std::wstring addr;
            if (!d.btAddress.empty()) addr += StrF(L"DeviceBt", { Widen(d.btAddress) });
            if (!d.lanHost.empty()) addr += (addr.empty() ? L"" : L"  \x00B7  ") + StrF(L"DeviceLan", { Widen(d.lanHost) });
            if (!addr.empty())
            {
                TextBlock a;
                a.Style(descStyle);
                a.Text(addr);
                a.IsTextSelectionEnabled(true);
                text.Children().Append(a);
            }
            Grid::SetColumn(text, 1);
            grid.Children().Append(text);

            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(8);
            buttons.VerticalAlignment(VerticalAlignment::Center);
            Button connect;
            std::string id = d.id;
            if (d.connected)
            {
                connect.Content(box_value(Str(L"Disconnect")));
                connect.Click([id](auto&&, auto&&) { Controller::Get().DeviceDisconnect(id); });
            }
            else
            {
                connect.Content(box_value(Str(L"Connect")));
                connect.Click([id](auto&&, auto&&) { Controller::Get().DeviceConnect(id); });
            }
            buttons.Children().Append(connect);
            Button forget;
            forget.Content(box_value(Str(L"Forget")));
            std::wstring displayName = Widen(d.name.empty() ? d.id : d.name);
            forget.Click([this, id, displayName](auto&&, auto&&) { ConfirmForget(id, displayName); });
            buttons.Children().Append(forget);
            Grid::SetColumn(buttons, 2);
            grid.Children().Append(buttons);

            card.Child(grid);
            PairedList().Children().Append(card);
        }
    }

    std::wstring SettingsPage::DeviceVersionText(std::string const& platform, std::string const& app, std::string const& match)
    {
        if (app.empty()) return std::wstring(Str(L"DeviceVersionUnknown"));
        std::wstring os = platform == "android" ? L"Android" : platform == "windows" ? L"Windows" : Widen(platform);
        std::wstring t = StrF(L"DeviceVersion", { os, Widen(app) });
        if (match == "patch") t += L"  \x00B7  " + std::wstring(Str(L"VersionPatchNote"));
        else if (match == "mismatch") t += L"  \x00B7  " + std::wstring(Str(L"VersionMismatchShort"));
        return t;
    }

    void SettingsPage::ShowQr(bool show)
    {
        m_qrShown = show;
        Controller::Get().PairQr(show);
    }

    void SettingsPage::UpdateQr()
    {
        auto const& p = Controller::Get().Model().pairing;
        bool waiting = p.qrStatus == "waiting" && !p.qrPayload.empty();
        if (!waiting) m_qrPayload.clear();
        else if (p.qrPayload != m_qrPayload)
        {
            m_qrPayload = p.qrPayload;
            QrPath().Data(QrGeometry(m_qrPayload));
        }
        if (p.qrStatus == "succeeded" || p.qrStatus == "expired") m_qrShown = false;
        QrFrame().Visibility(waiting ? Visibility::Visible : Visibility::Collapsed);
        QrIcon().Visibility(waiting ? Visibility::Collapsed : Visibility::Visible);
        if (waiting) QrPairText().Text(Str(L"QrPairWaiting"));
        else if (p.qrStatus == "succeeded") QrPairText().Text(StrF(L"QrPairSucceeded", { Widen(p.qrPeerName) }));
        else if (p.qrStatus == "expired") QrPairText().Text(Str(L"QrPairExpired"));
        else QrPairText().Text(Str(L"QrPairIdle"));
        bool again = p.qrStatus == "succeeded" || p.qrStatus == "expired";
        QrShowButton().Content(box_value(Str(again ? L"QrShowAgain" : L"QrShowButton")));
        QrShowButton().Visibility(waiting ? Visibility::Collapsed : Visibility::Visible);
        QrHideButton().Visibility(waiting ? Visibility::Visible : Visibility::Collapsed);
    }

    void SettingsPage::UpdateDevicesVersionBar()
    {
        // A warning when a paired phone (the connected one first) runs another major/minor version.
        auto& c = Controller::Get();
        auto const& st = c.Model();
        mdui::Device const* bad = nullptr;
        for (auto const& d : st.devices)
            if (d.versionMatch == "mismatch" && (!bad || d.connected)) bad = &d;
        if (!bad)
        {
            DevicesVersionBar().IsOpen(false);
            return;
        }
        std::string pc = st.self.app.empty() ? c.HostVersion() : st.self.app;
        DevicesVersionBar().Severity(InfoBarSeverity::Warning);
        DevicesVersionBar().Title(Str(L"VersionMismatchTitle"));
        DevicesVersionBar().Message(StrF(L"VersionMismatchMessage", { Widen(bad->app), Widen(pc) }));
        DevicesVersionBar().IsOpen(true);
    }

    void SettingsPage::UpdateWorkMode()
    {
        // This PC only: pages and cards that need a phone are hidden (General > Work mode switches back).
        bool standalone = Controller::Get().Model().standalone;
        auto vis = standalone ? Visibility::Collapsed : Visibility::Visible;
        for (auto const& item : { NavDevicesItem(), NavConnectionItem(), NavArbitrationItem(), NavAudioItem() })
            item.Visibility(vis);
        SmtcCard().Visibility(vis);
        SmtcNoteText().Visibility(vis);
        if (standalone && (m_page == L"devices" || m_page == L"connection" || m_page == L"arbitration" || m_page == L"audio"))
            Nav().SelectedItem(Nav().MenuItems().GetAt(0));
    }

    void SettingsPage::UpdateDiscovered()
    {
        auto const& st = Controller::Get().Model();
        std::string sig;
        for (auto const& d : st.discovered)
            sig += d.kind + "|" + d.id + "|" + d.name + "|" + d.detail + (d.paired ? "1" : "0") + (d.ready ? "1" : "0") + "\n";
        if (sig == m_discoveredSignature) return;
        m_discoveredSignature = sig;

        DiscoveredList().Children().Clear();
        DiscoveredEmpty().Visibility(st.discovered.empty() ? Visibility::Visible : Visibility::Collapsed);
        auto cardStyle = Application::Current().Resources().Lookup(box_value(L"CardStyle")).as<Microsoft::UI::Xaml::Style>();
        auto descStyle = Application::Current().Resources().Lookup(box_value(L"CardDescriptionStyle")).as<Microsoft::UI::Xaml::Style>();
        for (auto const& d : st.discovered)
        {
            Border card;
            card.Style(cardStyle);
            Grid grid;
            grid.ColumnSpacing(12);
            for (int i = 0; i < 3; ++i) grid.ColumnDefinitions().Append(ColumnDefinition());
            grid.ColumnDefinitions().GetAt(0).Width(GridLength{ 0, GridUnitType::Auto });
            grid.ColumnDefinitions().GetAt(2).Width(GridLength{ 0, GridUnitType::Auto });

            auto icon = MakeIcon(d.kind == "bt" ? L"\xE702" : L"\xE839", 18);
            icon.Margin(Thickness{ 2, 0, 6, 0 });
            icon.VerticalAlignment(VerticalAlignment::Center);
            grid.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            TextBlock name;
            name.Text(Widen(d.name.empty() ? d.id : d.name));
            text.Children().Append(name);
            TextBlock detail;
            detail.Style(descStyle);
            detail.Text((d.kind == "bt" ? std::wstring(Str(L"TransportBluetooth")) : std::wstring(Str(L"TransportLan"))) +
                (d.detail.empty() ? L"" : L"  \x00B7  " + Widen(d.detail)));
            text.Children().Append(detail);
            Grid::SetColumn(text, 1);
            grid.Children().Append(text);

            if (!d.ready && !d.paired)
            {
                TextBlock notReady;
                notReady.Style(descStyle);
                notReady.Text(Str(L"DiscoveredNotReady"));
                notReady.VerticalAlignment(VerticalAlignment::Center);
                notReady.TextWrapping(TextWrapping::Wrap);
                notReady.MaxWidth(220);
                Grid::SetColumn(notReady, 2);
                grid.Children().Append(notReady);
            }
            else if (d.paired)
            {
                TextBlock paired;
                paired.Style(descStyle);
                paired.Text(Str(L"AlreadyPaired"));
                paired.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetColumn(paired, 2);
                grid.Children().Append(paired);
            }
            else
            {
                Button pair;
                pair.Content(box_value(Str(L"Pair")));
                pair.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Microsoft::UI::Xaml::Style>());
                pair.VerticalAlignment(VerticalAlignment::Center);
                std::string kind = d.kind, target = d.id;
                pair.Click([kind, target](auto&&, auto&&) { Controller::Get().PairStart(kind, target); });
                Grid::SetColumn(pair, 2);
                grid.Children().Append(pair);
            }
            card.Child(grid);
            DiscoveredList().Children().Append(card);
        }
    }
}

namespace winrt::MediaDuetUI::implementation
{
    // ---- Connection --------------------------------------------------------------------------

    void SettingsPage::UpdateConnection()
    {
        auto const& st = Controller::Get().Model();
        auto const& cn = st.connection;
        ConnStatusValue().Text(Str(L"ConnState_" + Widen(cn.status)));
        std::wstring peer = Widen(st.PeerDisplayName());
        if (!cn.peer.platform.empty() && !peer.empty()) peer += L" (" + Widen(cn.peer.platform) + L")";
        ConnPeerValue().Text(Dash(peer));
        std::wstring transport = cn.status == "connected" ? TransportName(cn.transport) : std::wstring(L"\x2014");
        if (cn.status == "connected" && !cn.secondary.empty() && cn.secondary != cn.transport)
            transport = StrF(L"TransportWithBackup", { transport, TransportName(cn.secondary) });
        ConnTransportValue().Text(transport);
        ConnRttValue().Text(cn.rttMs >= 0 && cn.status == "connected" ? StrF(L"Milliseconds", { std::to_wstring(cn.rttMs) }) : std::wstring(L"\x2014"));
        uint64_t now = ::mdui::TickNow();
        ConnSinceValue().Text(cn.status == "connected" && cn.sinceTick > 0 && now >= cn.sinceTick
            ? FormatDuration(static_cast<int64_t>((now - cn.sinceTick) / 1000)) : std::wstring(L"\x2014"));
        ConnTrafficValue().Text(StrF(L"TrafficFormat", { FormatBytes(cn.bytesIn), FormatBytes(cn.bytesOut) }));
        // lastError is English diagnostic text from the host: shown only as secondary detail.
        ConnErrorValue().Text(Dash(Widen(cn.lastError)));
        std::wstring bt;
        if (!cn.btKnown) bt = L"\x2014";
        else if (!cn.btAdapter) bt = Str(L"BtNoAdapter");
        else bt = cn.btOn ? std::wstring(Str(L"BtOn")) : std::wstring(Str(L"BtOff"));
        if (cn.btKnown && !cn.btAdapterName.empty()) bt += L"  \x00B7  " + Widen(cn.btAdapterName);
        ConnBtValue().Text(bt);
    }

    // ---- Arbitration -------------------------------------------------------------------------

    void SettingsPage::UpdateArbitration()
    {
        auto const& st = Controller::Get().Model();
        auto const& p = st.policy;
        std::wstring peer = Widen(st.PeerDisplayName());
        if (peer.empty()) peer = Str(L"YourPhone");
        std::wstring apps;
        for (auto const& a : p.activeApps) apps += (apps.empty() ? L"" : L", ") + Widen(a);
        std::wstring live = p.windowsAudioActive
            ? (apps.empty() ? std::wstring(Str(L"PolicyLiveWinActiveGeneric")) : StrF(L"PolicyLiveWinActive", { apps }).c_str())
            : std::wstring(Str(L"PolicyLiveWinIdle"));
        if (p.suspended) live += L"\n" + std::wstring(Str(L"PolicySuspended"));
        if (p.gated)
        {
            auto out = st.DefaultOutput();
            live += L"\n" + StrF(L"PolicyGated", { out ? Widen(out->name) : std::wstring(Str(L"OutputUnknown")) });
        }
        live += L"\n";
        live += StrF(L"PolicyLiveStates", { PolicyStateName(p.local), peer, PolicyStateName(p.remote) });
        PolicyLiveText().Text(live);

        UpdateStringList(BlacklistPanel(), "policy.blacklist", m_blacklistSignature);
        UpdateStringList(WhitelistPanel(), "policy.whitelist", m_whitelistSignature);

        // Custom priorities
        json priorities = json::object();
        if (auto v = JPath(st.settings, "policy.priorities"); v && v->is_object()) priorities = *v;
        auto sig = priorities.dump();
        if (sig == m_prioritySignature) return;
        m_prioritySignature = sig;
        PriorityPanel().Children().Clear();
        if (priorities.empty())
        {
            TextBlock none;
            none.Text(Str(L"ListEmpty"));
            none.Opacity(0.6);
            PriorityPanel().Children().Append(none);
        }
        for (auto it = priorities.begin(); it != priorities.end(); ++it)
        {
            if (!it->is_number()) continue;
            std::string id = it.key();
            Grid row;
            row.ColumnSpacing(8);
            for (int i = 0; i < 3; ++i) row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(1).Width(GridLength{ 0, GridUnitType::Auto });
            row.ColumnDefinitions().GetAt(2).Width(GridLength{ 0, GridUnitType::Auto });
            TextBlock name;
            name.Text(Widen(id));
            name.VerticalAlignment(VerticalAlignment::Center);
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            row.Children().Append(name);
            NumberBox value;
            value.Width(100);
            value.Minimum(-100);
            value.Maximum(100);
            value.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
            value.Value(it->get<double>());
            value.ValueChanged([id](NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
                if (std::isnan(e.NewValue())) return;
                Controller::Get().SetSettings(json{ { "policy", { { "priorities", { { id, static_cast<int>(std::lround(e.NewValue())) } } } } } });
            });
            Grid::SetColumn(value, 1);
            row.Children().Append(value);
            Button remove;
            remove.Content(MakeIcon(L"\xE74D", 14));
            ToolTipService::SetToolTip(remove, box_value(Str(L"Remove")));
            remove.Click([this, id](auto&&, auto&&) {
                Controller::Get().SetSettings(json{ { "policy", { { "priorities", { { id, nullptr } } } } } });
                m_prioritySignature.clear();
                UpdateArbitration();
            });
            Grid::SetColumn(remove, 2);
            row.Children().Append(remove);
            PriorityPanel().Children().Append(row);
        }
    }

    void SettingsPage::UpdateStringList(StackPanel const& panel, std::string const& path, std::string& signature)
    {
        auto const& st = Controller::Get().Model();
        json list = json::array();
        if (auto v = JPath(st.settings, path); v && v->is_array()) list = *v;
        auto sig = list.dump();
        if (sig == signature) return;
        signature = sig;
        panel.Children().Clear();
        if (list.empty())
        {
            TextBlock none;
            none.Text(Str(L"ListEmpty"));
            none.Opacity(0.6);
            panel.Children().Append(none);
            return;
        }
        for (auto const& item : list)
        {
            if (!item.is_string()) continue;
            std::string value = item.get<std::string>();
            Grid row;
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(1).Width(GridLength{ 0, GridUnitType::Auto });
            TextBlock name;
            name.Text(Widen(value));
            name.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(name);
            Button remove;
            remove.Content(MakeIcon(L"\xE74D", 14));
            ToolTipService::SetToolTip(remove, box_value(Str(L"Remove")));
            remove.Click([this, path, value](auto&&, auto&&) {
                json current = json::array();
                if (auto v = JPath(Controller::Get().Model().settings, path); v && v->is_array()) current = *v;
                json next = json::array();
                for (auto const& x : current)
                    if (!(x.is_string() && x.get<std::string>() == value)) next.push_back(x);
                Controller::Get().SetSetting(path, next);
                UpdateArbitration();
            });
            Grid::SetColumn(remove, 1);
            row.Children().Append(remove);
            panel.Children().Append(row);
        }
    }

    void SettingsPage::AddToList(std::string const& path, TextBox const& input)
    {
        auto value = NormalizeProcess(std::wstring(input.Text()));
        if (value.empty()) return;
        json current = json::array();
        if (auto v = JPath(Controller::Get().Model().settings, path); v && v->is_array()) current = *v;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return s;
        };
        for (auto const& x : current)
        {
            if (x.is_string() && lower(x.get<std::string>()) == lower(value))
            {
                input.Text(L"");
                return;
            }
        }
        current.push_back(value);
        Controller::Get().SetSetting(path, current);
        input.Text(L"");
        UpdateArbitration();
    }

    // ---- Lyrics ------------------------------------------------------------------------------

    void SettingsPage::UpdateLyricsPage()
    {
        auto const& st = Controller::Get().Model();
        auto folder = st.Setting("lyrics.localFolder", std::string());
        LocalFolderText().Text(folder.empty() ? std::wstring(Str(L"NoFolderSelected")) : Widen(folder));
        ToolTipService::SetToolTip(LocalFolderText(), folder.empty() ? nullptr : box_value(Widen(folder)));
        ClearFolderButton().IsEnabled(!folder.empty());

        m_applying = true;
        if (OverlayShowToggle().IsOn() != st.overlayVisible) OverlayShowToggle().IsOn(st.overlayVisible);
        if (m_fontsLoaded)
        {
            auto family = Widen(st.Setting("lyrics.overlay.fontFamily", std::string("Microsoft YaHei UI")));
            auto items = OverlayFontCombo().Items();
            int32_t found = -1;
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (unbox_value_or<hstring>(items.GetAt(i), L"") == family)
                {
                    found = static_cast<int32_t>(i);
                    break;
                }
            }
            if (found < 0)
            {
                items.InsertAt(0, box_value(family));
                found = 0;
            }
            if (OverlayFontCombo().SelectedIndex() != found) OverlayFontCombo().SelectedIndex(found);
        }
        m_applying = false;

        auto swatch = [&](Border const& b, char const* path, char const* def) {
            auto color = ParseColor(st.Setting(path, std::string(def)));
            b.Background(SolidColorBrush(color.value_or(Color{ 255, 255, 255, 255 })));
        };
        swatch(ActiveColorSwatch(), "lyrics.overlay.colorActive", "#FFFFFFFF");
        swatch(InactiveColorSwatch(), "lyrics.overlay.colorInactive", "#B3FFFFFF");

        // The alternating / centred layout only matters with two lines.
        OverlayLayoutCombo().IsEnabled(st.Setting("lyrics.overlay.twoLines", true));

        // Timing: what the host currently applies for Windows' audio output.
        bool autoLatency = st.Setting("lyrics.latency.auto", true);
        LatencyBtSlider().IsEnabled(autoLatency);
        if (st.outputLatency.known)
        {
            std::wstring status;
            if (!st.outputLatency.bluetooth) status = Str(L"LatencyStatusWired");
            else if (autoLatency) status = StrF(L"LatencyStatusBt", { std::to_wstring(st.outputLatency.ms) });
            else status = Str(L"LatencyStatusBtOff");
            LatencyStatusText().Text(status);
            LatencyStatusText().Visibility(Visibility::Visible);
        }
        else
        {
            LatencyStatusText().Visibility(Visibility::Collapsed);
        }
        UpdateLyricsOffsets();
    }

    void SettingsPage::UpdateLyricsOffsets()
    {
        auto const& st = Controller::Get().Model();
        json offsets = json::object();
        if (auto v = JPath(st.settings, "lyrics.offsets"); v && v->is_object()) offsets = *v;
        // App names come from the current sessions (an app id alone is e.g. "com.netease.cloudmusic").
        auto nameOf = [&](std::string const& id) -> std::wstring {
            for (auto const* side : { &st.local, &st.remote })
                for (auto const& s : side->sessions)
                    if (s.appId == id && !s.appName.empty()) return Widen(s.appName);
            return Widen(id);
        };
        std::string sig = offsets.dump();
        for (auto it = offsets.begin(); it != offsets.end(); ++it) sig += "|" + Narrow(nameOf(it.key()));
        if (sig == m_offsetsSignature) return;
        m_offsetsSignature = sig;
        OffsetsPanel().Children().Clear();
        bool any = false;
        for (auto it = offsets.begin(); it != offsets.end(); ++it)
        {
            if (!it->is_number()) continue;
            auto ms = static_cast<int64_t>(std::llround(it->get<double>()));
            if (ms == 0) continue;
            any = true;
            std::string id = it.key();
            Grid row;
            row.ColumnSpacing(12);
            for (int i = 0; i < 3; ++i) row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(1).Width(GridLength{ 0, GridUnitType::Auto });
            row.ColumnDefinitions().GetAt(2).Width(GridLength{ 0, GridUnitType::Auto });
            TextBlock name;
            name.Text(nameOf(id));
            name.VerticalAlignment(VerticalAlignment::Center);
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            ToolTipService::SetToolTip(name, box_value(Widen(id)));
            row.Children().Append(name);
            TextBlock value;
            value.Text(StrF(L"OffsetSeconds", { FormatOffsetSeconds(ms) }));
            value.VerticalAlignment(VerticalAlignment::Center);
            value.Style(Application::Current().Resources().Lookup(box_value(L"CardDescriptionStyle")).as<Microsoft::UI::Xaml::Style>());
            Grid::SetColumn(value, 1);
            row.Children().Append(value);
            Button reset;
            reset.Content(box_value(Str(L"LyricsOffsetReset")));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(reset, StrF(L"LyricsOffsetResetFor", { nameOf(id) }));
            reset.Click([this, id](auto&&, auto&&) {
                Controller::Get().ResetLyricsOffset(id);
                m_offsetsSignature.clear();
                UpdateLyricsOffsets();
            });
            Grid::SetColumn(reset, 2);
            row.Children().Append(reset);
            OffsetsPanel().Children().Append(row);
        }
        if (!any)
        {
            TextBlock none;
            none.Text(Str(L"ListEmpty"));
            none.Opacity(0.6);
            OffsetsPanel().Children().Append(none);
        }
    }

    void SettingsPage::LoadFonts()
    {
        if (m_fontsLoaded) return;
        m_fontsLoaded = true;
        std::set<std::wstring> names;
        com_ptr<IDWriteFactory> factory;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.put()))))
        {
            com_ptr<IDWriteFontCollection> collection;
            if (SUCCEEDED(factory->GetSystemFontCollection(collection.put())))
            {
                for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i)
                {
                    com_ptr<IDWriteFontFamily> family;
                    com_ptr<IDWriteLocalizedStrings> localized;
                    if (FAILED(collection->GetFontFamily(i, family.put())) || FAILED(family->GetFamilyNames(localized.put()))) continue;
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    localized->FindLocaleName(L"en-us", &index, &exists);
                    if (!exists) index = 0;
                    UINT32 length = 0;
                    if (FAILED(localized->GetStringLength(index, &length))) continue;
                    std::wstring name(length + 1, L'\0');
                    if (FAILED(localized->GetString(index, name.data(), length + 1))) continue;
                    name.resize(length);
                    if (!name.empty() && name[0] != L'@') names.insert(name);
                }
            }
        }
        m_applying = true;
        auto items = OverlayFontCombo().Items();
        items.Clear();
        for (auto const& n : names) items.Append(box_value(n));
        m_applying = false;
        UpdateLyricsPage();
    }

    void SettingsPage::PickFolder()
    {
        try
        {
            auto dialog = create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            auto title = Str(L"PickFolderTitle");
            dialog->SetTitle(title.c_str());
            if (FAILED(dialog->Show(m_owner))) return;   // cancelled
            com_ptr<IShellItem> item;
            if (FAILED(dialog->GetResult(item.put()))) return;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            {
                Controller::Get().SetSetting("lyrics.localFolder", Narrow(path));
                CoTaskMemFree(path);
                UpdateLyricsPage();
            }
        }
        catch (hresult_error const& e)
        {
            ShowResultError(Narrow(e.message()));
        }
    }

    void SettingsPage::ApplyColor(std::string const& path, ColorPicker const& picker, Flyout const& flyout)
    {
        Controller::Get().SetSetting(path, FormatColor(picker.Color()));
        flyout.Hide();
        UpdateLyricsPage();
    }

    // ---- Audio -------------------------------------------------------------------------------

    void SettingsPage::UpdateAudio()
    {
        auto const& st = Controller::Get().Model();
        auto stateName = [](AudioEndpoint const& a) -> std::wstring {
            static const char* known[] = { "off", "connecting", "connected", "on", "streaming", "error" };
            for (auto k : known)
                if (a.state == k) return std::wstring(Str(L"AudioState_" + Widen(a.state)));
            return Widen(a.state);
        };
        m_applying = true;
        auto const& a2dp = st.a2dpSink;
        A2dpToggle().IsOn(a2dp.IsOn());
        A2dpToggle().IsEnabled(a2dp.available || a2dp.IsOn());
        std::wstring s = a2dp.available ? std::wstring(StrF(L"AudioStatus", { stateName(a2dp) })) : std::wstring(Str(L"A2dpUnavailable"));
        if (!a2dp.device.empty()) s += L"  \x00B7  " + Widen(a2dp.device);
        A2dpStatus().Text(s);
        bool a2dpFailed = a2dp.state == "error" || !a2dp.error.empty();
        A2dpError().Message(a2dpFailed ? hstring(A2dpErrorText(a2dp.errorCode, a2dp.error)) : hstring{});
        A2dpError().IsOpen(a2dpFailed);

        auto const& lan = st.lanSend;
        LanSendToggle().IsOn(lan.IsOn());
        LanSendToggle().IsEnabled(lan.available || lan.IsOn());
        LanSendStatus().Text(lan.available ? std::wstring(StrF(L"AudioStatus", { stateName(lan) })) : std::wstring(Str(L"LanSendUnavailable")));
        LanSendError().Message(Widen(lan.error));
        LanSendError().IsOpen(!lan.error.empty());
        m_applying = false;
    }

    // ---- Diagnostics -------------------------------------------------------------------------

    void SettingsPage::UpdateDiagnostics()
    {
        auto& c = Controller::Get();
        auto const& st = c.Model();
        m_applying = true;
        if (DiagnosticsToggle().IsOn() != st.diagnosticsEnabled) DiagnosticsToggle().IsOn(st.diagnosticsEnabled);
        m_applying = false;
        auto const& d = c.Diagnostics();
        bool has = st.diagnosticsEnabled && d.contains("type");
        DiagCard().Visibility(has ? Visibility::Visible : Visibility::Collapsed);
        DiagWaiting().Visibility(st.diagnosticsEnabled && !has ? Visibility::Visible : Visibility::Collapsed);
        if (!has) return;

        auto num = [&](char const* key, wchar_t const* fmt) -> std::wstring {
            if (!d.contains(key) || !d[key].is_number()) return L"\x2014";
            wchar_t buf[64];
            swprintf_s(buf, fmt, d[key].get<double>());
            return buf;
        };
        auto integer = [&](char const* key) -> std::wstring {
            auto v = JInt(d, key, -1);
            return v < 0 ? std::wstring(L"\x2014") : std::to_wstring(v);
        };
        auto ms = [&](char const* key) -> std::wstring {
            auto v = JInt(d, key, -1);
            return v < 0 ? std::wstring(L"\x2014") : StrF(L"Milliseconds", { std::to_wstring(v) });
        };
        for (auto& [value, key] : m_diagValues)
        {
            std::wstring text;
            if (key == "cpuPercent") text = num("cpuPercent", L"%.2f %%");
            else if (key == "workingSetMB") text = num("workingSetMB", L"%.1f MB");
            else if (key == "privateMB") text = num("privateMB", L"%.1f MB");
            else if (key == "threads") text = integer("threads");
            else if (key == "transport") text = TransportName(JStr(d, "transport"));
            else if (key == "connectedForSec") text = FormatDuration(JInt(d, "connectedForSec", -1));
            else if (key == "rttMs") text = ms("rttMs");
            else if (key == "lastCommandLatencyMs") text = ms("lastCommandLatencyMs");
            else if (key == "bytes") text = StrF(L"TrafficFormat", { FormatBytes(JInt(d, "bytesIn", 0)), FormatBytes(JInt(d, "bytesOut", 0)) });
            else if (key == "msgs") text = StrF(L"TrafficFormat", { integer("msgsIn"), integer("msgsOut") });
            else if (key == "sessions") text = StrF(L"DiagSessionsFormat", { integer("localSessions"), integer("remoteSessions") });
            else if (key == "audioSessionsActive") text = integer("audioSessionsActive");
            else if (key == "ui")
            {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                {
                    wchar_t buf[96];
                    swprintf_s(buf, L"%.1f MB  \x00B7  %.1f MB", pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0);
                    text = buf;
                }
            }
            value.Text(text);
        }
    }

    void SettingsPage::UpdateAbout()
    {
        auto& c = Controller::Get();
        auto const& v = c.HostVersion();
        HostVersionValue().Text(c.Link() == HostLink::Connected && !v.empty() ? Widen(v) : std::wstring(Str(L"NotConnected")));
        // The phone: this session's version, else the last one a paired phone reported.
        auto const& st = c.Model();
        std::wstring phone;
        if (c.Link() != HostLink::Connected || !st.valid) phone = Str(L"NotConnected");
        else if (st.standalone) phone = Str(L"WorkModeStandaloneShort");
        else if (st.connection.status == "connected")
            phone = DeviceVersionText(st.connection.peer.platform, st.connection.peer.app, st.connection.peer.versionMatch);
        else
        {
            for (auto const& d : st.devices)
                if (!d.app.empty()) phone = StrF(L"PhoneVersionLastSeen", { Widen(d.app) });
            if (phone.empty()) phone = st.devices.empty() ? std::wstring(Str(L"NoPhonePaired")) : std::wstring(Str(L"DeviceVersionUnknown"));
        }
        PhoneVersionValue().Text(phone);
        UpdateSoftwareUpdate();
    }

    void SettingsPage::UpdateSoftwareUpdate()
    {
        auto& c = Controller::Get();
        auto const& u = c.Model().update;
        bool host = c.Link() == HostLink::Connected && u.valid;
        std::wstring latest = Widen(u.latest), current = Widen(u.current.empty() ? std::string(MDUI_VERSION_STRING) : u.current);
        bool newer = !u.latest.empty() && u.latest != u.current;
        std::wstring title, detail, button(Str(L"UpdateCheckButton"));
        bool busy = false, progress = false;
        if (!host) { title = Str(L"UpdateTitle"); detail = Str(L"NotConnected"); }
        else if (u.phase == "checking") { title = Str(L"UpdateChecking"); busy = true; }
        else if (u.phase == "available") { title = StrF(L"UpdateAvailable", { latest }); detail = StrF(L"UpdateAvailableDetail", { current }); button = Str(L"UpdateInstallButton"); }
        else if (u.phase == "downloading")
        {
            title = StrF(L"UpdateDownloading", { latest });
            wchar_t pct[16];
            swprintf_s(pct, L"%d%%", static_cast<int>(u.progress * 100 + 0.5));
            detail = pct;
            busy = progress = true;
        }
        else if (u.phase == "installing") { title = StrF(L"UpdateInstalling", { latest }); detail = Str(L"UpdateInstallingDetail"); busy = true; }
        else if (u.phase == "current") { title = Str(L"UpdateCurrent"); detail = StrF(L"UpdateCurrentDetail", { current }); }
        else if (u.phase == "error")
        {
            title = Str(L"UpdateFailed");
            std::wstring key = L"UpdateErr_" + Widen(u.error.empty() ? std::string("network") : u.error);
            detail = Str(key);
            if (detail.empty() || detail == key) detail = StrF(L"UpdateErrOther", { Widen(u.error) });
            if (newer) button = Str(L"UpdateRetryButton");
        }
        else { title = Str(L"UpdateTitle"); detail = StrF(L"UpdateCurrentDetail", { current }); }
        UpdateTitle().Text(title);
        UpdateDetail().Text(detail);
        UpdateDetail().Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        UpdateProgress().Visibility(progress ? Visibility::Visible : Visibility::Collapsed);
        UpdateProgress().Value(u.progress);
        UpdateNotesLink().Visibility(host && newer ? Visibility::Visible : Visibility::Collapsed);
        UpdateButton().Content(box_value(button));
        UpdateButton().IsEnabled(host && !busy);
    }

    // ---- Pairing -----------------------------------------------------------------------------

    void SettingsPage::UpdatePairingDialog()
    {
        auto& c = Controller::Get();
        auto const& p = c.Model().pairing;
        std::string state = c.PairingDialogWanted() ? p.state : std::string("idle");
        if (state.empty()) state = "idle";
        if (state == "idle")
        {
            m_pairDismissedState.clear();
            if (m_pairDialog && m_dialogOpen && m_pairDialogState != "succeeded" && m_pairDialogState != "failed")
                m_pairDialog.Hide();
            return;
        }
        if (state == m_pairDismissedState) return;

        if (!m_pairDialog)
        {
            if (!Root().XamlRoot()) return;   // retried from Root().Loaded
            ContentDialog dialog;
            dialog.XamlRoot(Root().XamlRoot());
            dialog.RequestedTheme(Root().ActualTheme());
            dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Microsoft::UI::Xaml::Style>());
            StackPanel content;
            content.Spacing(14);
            content.MinWidth(340);
            m_pairBody = TextBlock();
            m_pairBody.TextWrapping(TextWrapping::Wrap);
            content.Children().Append(m_pairBody);
            m_pairCode = TextBlock();
            m_pairCode.FontSize(44);
            m_pairCode.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            m_pairCode.CharacterSpacing(120);
            m_pairCode.HorizontalAlignment(HorizontalAlignment::Center);
            content.Children().Append(m_pairCode);
            m_pairProgress = ProgressRing();
            m_pairProgress.Width(28);
            m_pairProgress.Height(28);
            content.Children().Append(m_pairProgress);
            dialog.Content(content);

            dialog.PrimaryButtonClick([this](auto&&, ContentDialogButtonClickEventArgs const& e) {
                e.Cancel(true);
                if (m_pairDialogState == "awaiting_user") Controller::Get().PairRespond(true);
            });
            dialog.SecondaryButtonClick([this](auto&&, ContentDialogButtonClickEventArgs const& e) {
                e.Cancel(true);
                if (m_pairDialogState == "awaiting_user") Controller::Get().PairRespond(false);
            });
            dialog.CloseButtonClick([this](auto&&, auto&&) {
                if (m_pairDialogState == "connecting" || m_pairDialogState == "awaiting_user" || m_pairDialogState == "awaiting_peer")
                    Controller::Get().PairCancel();
            });
            dialog.Closed([this](auto&&, auto&&) {
                m_dialogOpen = false;
                if (m_pairDialogState == "succeeded" || m_pairDialogState == "failed")
                    Controller::Get().AcknowledgePairingResult();
                if (m_pairProgress) m_pairProgress.IsActive(false);
                m_pairDismissedState = m_pairDialogState;
                m_pairDialog = nullptr;
            });
            m_pairDialog = dialog;
            try
            {
                m_pairDialog.ShowAsync();
                m_dialogOpen = true;
            }
            catch (hresult_error const&)
            {
                // Another dialog is open (e.g. "forget device"); try again on the next state.
                m_pairDialog = nullptr;
                return;
            }
        }

        m_pairDialogState = state;
        std::wstring peer = Widen(p.peerName);
        if (peer.empty()) peer = Str(L"YourPhone");
        std::wstring sas = Widen(p.sas);
        if (sas.size() == 6) sas = sas.substr(0, 3) + L" " + sas.substr(3);

        hstring title, primary, secondary, close;
        std::wstring body;
        bool showCode = false, busy = false;
        if (state == "connecting")
        {
            title = StrF(L"PairTitleConnecting", { peer });
            body = Str(L"PairBodyConnecting");
            busy = true;
            close = Str(L"Cancel");
        }
        else if (state == "awaiting_user")
        {
            title = Str(L"PairTitleConfirm");
            body = StrF(L"PairBodyConfirm", { peer });
            showCode = true;
            primary = Str(L"PairCodesMatch");
            secondary = Str(L"PairCodesDiffer");
        }
        else if (state == "awaiting_peer")
        {
            title = Str(L"PairTitleConfirm");
            body = StrF(L"PairBodyWaitingPeer", { peer });
            showCode = true;
            busy = true;
            close = Str(L"Cancel");
        }
        else if (state == "succeeded")
        {
            title = Str(L"PairTitleSucceeded");
            body = StrF(L"PairBodySucceeded", { peer });
            close = Str(L"Done");
        }
        else
        {
            title = Str(L"PairTitleFailed");
            body = PairError(p.error);
            close = Str(L"Close");
        }
        m_pairDialog.Title(box_value(title));
        m_pairBody.Text(body);
        m_pairCode.Text(sas);
        m_pairCode.Visibility(showCode && !sas.empty() ? Visibility::Visible : Visibility::Collapsed);
        m_pairCode.Opacity(state == "awaiting_peer" ? 0.55 : 1.0);
        m_pairProgress.IsActive(busy);
        m_pairProgress.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        m_pairDialog.PrimaryButtonText(primary);
        m_pairDialog.SecondaryButtonText(secondary);
        m_pairDialog.CloseButtonText(close);
        // Never make "Codes match" the default button: a stray Enter must not confirm a pairing code.
        m_pairDialog.DefaultButton(primary.empty() ? ContentDialogButton::Close : ContentDialogButton::None);
    }

    void SettingsPage::ConfirmForget(std::string id, std::wstring name)
    {
        ContentDialog dialog;
        dialog.XamlRoot(Root().XamlRoot());
        dialog.RequestedTheme(Root().ActualTheme());
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Microsoft::UI::Xaml::Style>());
        dialog.Title(box_value(StrF(L"ForgetTitle", { name })));
        TextBlock body;
        body.Text(Str(L"ForgetBody"));
        body.TextWrapping(TextWrapping::Wrap);
        dialog.Content(body);
        dialog.PrimaryButtonText(Str(L"Forget"));
        dialog.CloseButtonText(Str(L"Cancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        dialog.PrimaryButtonClick([id](auto&&, auto&&) { Controller::Get().DeviceForget(id); });
        try
        {
            dialog.ShowAsync();
        }
        catch (hresult_error const&)
        {
        }
    }
}

namespace winrt::MediaDuetUI::implementation
{
    // ---- Lyrics providers ------------------------------------------------------------------
    //
    // lyrics.providers is the ordered list of *enabled* online sources; a disabled source is simply
    // absent. The page lists enabled sources in their order, then the disabled ones.

    namespace
    {
        constexpr char const* kProviders[] = { "netease", "qqmusic", "lrclib" };

        std::vector<std::string> EnabledProviders()
        {
            std::vector<std::string> out;
            auto const* v = JPath(Controller::Get().Model().settings, "lyrics.providers");
            if (!v || !v->is_array())
            {
                for (auto p : kProviders) out.emplace_back(p);   // documented default
                return out;
            }
            for (auto const& x : *v)
                if (x.is_string()) out.push_back(x.get<std::string>());
            return out;
        }
    }

    void SettingsPage::SendProviders(std::vector<std::string> const& order, std::vector<std::string> const& enabled)
    {
        json list = json::array();
        for (auto const& id : order)
            if (std::find(enabled.begin(), enabled.end(), id) != enabled.end()) list.push_back(id);
        Controller::Get().SetSetting("lyrics.providers", list);
        m_providersSignature.clear();
        UpdateProviders();
    }

    void SettingsPage::UpdateProviders()
    {
        auto enabled = EnabledProviders();
        std::vector<std::string> order = enabled;
        for (auto p : kProviders)
            if (std::find(order.begin(), order.end(), p) == order.end()) order.emplace_back(p);
        bool online = Controller::Get().Model().Setting("lyrics.online", true);
        std::string sig = std::to_string(online);
        for (auto const& id : order)
            sig += id + (std::find(enabled.begin(), enabled.end(), id) != enabled.end() ? "+" : "-");
        if (sig == m_providersSignature) return;
        m_providersSignature = sig;

        ProvidersPanel().Children().Clear();
        ProvidersPanel().Opacity(online ? 1.0 : 0.5);
        ProvidersPanel().IsHitTestVisible(online);
        for (size_t i = 0; i < order.size(); ++i)
        {
            std::string id = order[i];
            bool on = std::find(enabled.begin(), enabled.end(), id) != enabled.end();
            Grid row;
            row.ColumnSpacing(6);
            for (int k = 0; k < 4; ++k) row.ColumnDefinitions().Append(ColumnDefinition());
            row.ColumnDefinitions().GetAt(0).Width(GridLength{ 0, GridUnitType::Auto });
            row.ColumnDefinitions().GetAt(2).Width(GridLength{ 0, GridUnitType::Auto });
            row.ColumnDefinitions().GetAt(3).Width(GridLength{ 0, GridUnitType::Auto });

            ToggleSwitch toggle;
            toggle.IsOn(on);
            toggle.OnContent(box_value(hstring{}));
            toggle.OffContent(box_value(hstring{}));
            toggle.MinWidth(0);
            toggle.VerticalAlignment(VerticalAlignment::Center);
            auto name = Str(L"Provider_" + Widen(id));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(toggle, name);
            toggle.Toggled([this, id, toggle](auto&&, auto&&) {
                auto en = EnabledProviders();
                auto it = std::find(en.begin(), en.end(), id);
                if (toggle.IsOn() && it == en.end()) en.push_back(id);
                else if (!toggle.IsOn() && it != en.end()) en.erase(it);
                else return;
                std::vector<std::string> ord = EnabledProviders();
                for (auto p : kProviders)
                    if (std::find(ord.begin(), ord.end(), p) == ord.end()) ord.emplace_back(p);
                SendProviders(ord, en);
            });
            row.Children().Append(toggle);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            TextBlock title;
            title.Text(std::wstring(std::to_wstring(i + 1)) + L".  " + std::wstring(name));
            title.Opacity(on ? 1.0 : 0.55);
            text.Children().Append(title);
            Grid::SetColumn(text, 1);
            row.Children().Append(text);

            auto move = [this, i, order](int delta) {
                int j = static_cast<int>(i) + delta;
                if (j < 0 || j >= static_cast<int>(order.size())) return;
                auto ord = order;
                std::swap(ord[i], ord[j]);
                SendProviders(ord, EnabledProviders());
            };
            Button up;
            up.Style(Application::Current().Resources().Lookup(box_value(L"IconButtonStyle")).as<Microsoft::UI::Xaml::Style>());
            up.Width(32);
            up.Height(32);
            up.Content(MakeIcon(L"\xE70E", 12));
            up.IsEnabled(i > 0);
            ToolTipService::SetToolTip(up, box_value(Str(L"MoveUp")));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(up, Str(L"MoveUp"));
            up.Click([move](auto&&, auto&&) { move(-1); });
            Grid::SetColumn(up, 2);
            row.Children().Append(up);
            Button down;
            down.Style(Application::Current().Resources().Lookup(box_value(L"IconButtonStyle")).as<Microsoft::UI::Xaml::Style>());
            down.Width(32);
            down.Height(32);
            down.Content(MakeIcon(L"\xE70D", 12));
            down.IsEnabled(i + 1 < order.size());
            ToolTipService::SetToolTip(down, box_value(Str(L"MoveDown")));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(down, Str(L"MoveDown"));
            down.Click([move](auto&&, auto&&) { move(1); });
            Grid::SetColumn(down, 3);
            row.Children().Append(down);
            ProvidersPanel().Children().Append(row);
        }
    }

    // ---- Taskbar ----------------------------------------------------------------------------

    void SettingsPage::UpdateTaskbar()
    {
        auto const& st = Controller::Get().Model();
        auto const& tb = st.taskbar;
        bool enabled = st.Setting("taskbar.enabled", true);
        std::wstring text;
        Color dot{ 255, 138, 138, 138 };
        if (!tb.known) text = L"\x2014";
        else if (!tb.supported) text = Str(L"TaskbarStatusUnsupported");
        else if (!enabled) text = Str(L"TaskbarStatusDisabled");
        else if (tb.shown)
        {
            text = Str(L"TaskbarStatusShown");
            dot = Color{ 255, 46, 173, 75 };
        }
        else
        {
            text = Str(L"TaskbarStatusHidden");
            dot = Color{ 255, 227, 162, 26 };
        }
        // taskbar.reason codes (docs/ipc.md) are localised; anything else is shown as is (secondary).
        if (tb.known && !tb.reason.empty() && !(tb.reason == "disabled" && !enabled))
        {
            static const char* known[] = { "disabled", "nothing_playing", "fullscreen", "autohide", "vertical_taskbar", "no_space",
                "taskbar_not_ready", "no_taskbar", "unavailable" };
            std::wstring reason = Widen(tb.reason);
            for (auto k : known)
                if (tb.reason == k) reason = Str(L"TaskbarReason_" + Widen(tb.reason));
            text += L"  \x00B7  " + reason;
        }
        TaskbarStatusText().Text(text);
        TaskbarStatusDot().Fill(SolidColorBrush(dot));
    }
}

namespace winrt::MediaDuetUI::implementation
{
    // ---- "Only intervene with these outputs" (policy.onlyWithOutputs = [{id, name}]) ------------

    void SettingsPage::UpdateOutputs()
    {
        auto const& st = Controller::Get().Model();
        json selected = json::array();
        if (auto v = JPath(st.settings, "policy.onlyWithOutputs"); v && v->is_array()) selected = *v;
        std::string sig = selected.dump() + (st.policy.gated ? "g" : "");
        for (auto const& o : st.audioOutputs) sig += o.id + "|" + o.name + (o.isDefault ? "*" : "") + "\n";
        if (sig == m_outputsSignature) return;
        m_outputsSignature = sig;

        auto isSelected = [&](std::string const& id) {
            for (auto const& x : selected)
                if (JStr(x, "id") == id) return true;
            return false;
        };
        struct Row { std::string id, name; bool present, isDefault; };
        std::vector<Row> rows;
        for (auto const& o : st.audioOutputs) rows.push_back({ o.id, o.name, true, o.isDefault });
        for (auto const& x : selected)
        {
            auto id = JStr(x, "id");
            bool present = std::any_of(rows.begin(), rows.end(), [&](Row const& r) { return r.id == id; });
            if (!id.empty() && !present) rows.push_back({ id, JStr(x, "name"), false, false });
        }

        OutputsPanel().Children().Clear();
        if (rows.empty())
        {
            TextBlock none;
            none.Text(Str(L"OutputsNone"));
            none.Opacity(0.6);
            OutputsPanel().Children().Append(none);
        }
        for (auto const& r : rows)
        {
            CheckBox box;
            std::wstring label = Widen(r.name.empty() ? r.id : r.name);
            if (r.isDefault) label += L"  \x00B7  " + std::wstring(Str(L"OutputCurrent"));
            if (!r.present) label += L"  \x00B7  " + std::wstring(Str(L"OutputNotConnected"));
            box.Content(box_value(label));
            box.IsChecked(isSelected(r.id));
            std::string id = r.id, name = r.name;
            auto changed = [this, id, name](IInspectable const& sender, RoutedEventArgs const&) {
                bool on = sender.as<CheckBox>().IsChecked().Value();
                json current = json::array();
                if (auto v = JPath(Controller::Get().Model().settings, "policy.onlyWithOutputs"); v && v->is_array()) current = *v;
                json next = json::array();
                for (auto const& x : current)
                    if (JStr(x, "id") != id) next.push_back(x);
                if (on) next.push_back(json{ { "id", id }, { "name", name } });
                Controller::Get().SetSetting("policy.onlyWithOutputs", next);
                m_outputsSignature.clear();
            };
            box.Checked(changed);
            box.Unchecked(changed);
            OutputsPanel().Children().Append(box);
        }
        auto out = st.DefaultOutput();
        OutputsGatedText().Text(st.policy.gated
            ? StrF(L"PolicyGated", { out ? Widen(out->name) : std::wstring(Str(L"OutputUnknown")) }) : std::wstring{});
        OutputsGatedText().Visibility(st.policy.gated ? Visibility::Visible : Visibility::Collapsed);
    }
}
