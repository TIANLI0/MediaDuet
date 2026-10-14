#pragma once

#include "SettingsPage.g.h"

#include "Model.h"
#include "WindowUtil.h"

namespace winrt::MediaDuetUI::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage() = default;
        void InitializeComponent();
        // general, devices, pair, connection, arbitration, lyrics, integration, audio, diagnostics, about
        void Navigate(std::wstring_view page);
        std::wstring CurrentPage() const { return m_page; }
        // Hosted inside MainWindow: owner window (for the folder picker) and whether the page is
        // currently visible (settings shown, window not minimised). Close() unsubscribes.
        void SetOwner(HWND owner) { m_owner = owner; }
        void SetActive(bool active);
        void Close();

    private:
        struct ToggleBinding { Microsoft::UI::Xaml::Controls::ToggleSwitch control; std::string path; bool def; };
        struct NumberBinding { Microsoft::UI::Xaml::Controls::NumberBox control; std::string path; double def; };
        struct ComboBinding { Microsoft::UI::Xaml::Controls::ComboBox control; std::string path; std::string def; };
        struct RadioBinding { Microsoft::UI::Xaml::Controls::RadioButtons control; std::string path; std::string def; };
        struct SliderBinding
        {
            Microsoft::UI::Xaml::Controls::Slider control;
            Microsoft::UI::Xaml::Controls::TextBlock label;
            std::string path;
            double def;
            int format;   // 0 decimal, 1 percent, 2 integer pixels, 3 integer milliseconds
            Microsoft::UI::Dispatching::DispatcherQueueTimer debounce{ nullptr };
        };

        void Bind(Microsoft::UI::Xaml::Controls::ToggleSwitch const& c, std::string path, bool def);
        void Bind(Microsoft::UI::Xaml::Controls::NumberBox const& c, std::string path, double def);
        void Bind(Microsoft::UI::Xaml::Controls::ComboBox const& c, std::string path, std::string def);
        void Bind(Microsoft::UI::Xaml::Controls::RadioButtons const& c, std::string path, std::string def);
        void Bind(Microsoft::UI::Xaml::Controls::Slider const& c, Microsoft::UI::Xaml::Controls::TextBlock const& label,
            std::string path, double def, int format);

        void ShowPage(std::wstring const& tag);
        void SyncDiscovery();
        void RefreshAll();
        void ApplySettings();
        void UpdateHostBar();
        void UpdateDevices();
        void UpdateDiscovered();
        void UpdateConnection();
        void UpdateArbitration();
        void UpdateLyricsPage();
        void UpdateLyricsOffsets();
        void UpdateAudio();
        void UpdateDiagnostics();
        void UpdateAbout();
        void UpdateWorkMode();
        void UpdateDevicesVersionBar();
        static std::wstring DeviceVersionText(std::string const& platform, std::string const& app, std::string const& match);
        void UpdateTaskbar();
        void UpdateProviders();
        void UpdateOutputs();
        void SendProviders(std::vector<std::string> const& order, std::vector<std::string> const& enabled);
        void UpdatePairingDialog();
        void UpdateStringList(Microsoft::UI::Xaml::Controls::StackPanel const& panel, std::string const& path, std::string& signature);
        void AddToList(std::string const& path, Microsoft::UI::Xaml::Controls::TextBox const& input);
        void LoadFonts();
        void PickFolder();
        void ApplyColor(std::string const& path, Microsoft::UI::Xaml::Controls::ColorPicker const& picker,
            Microsoft::UI::Xaml::Controls::Flyout const& flyout);
        void ConfirmForget(std::string id, std::wstring name);
        void ShowResultError(std::string const& error);


        HWND m_owner{ nullptr };
        bool m_active{ false };
        std::vector<uint32_t> m_tokens;
        bool m_applying{ false };
        bool m_dirty{ false };
        bool m_closed{ false };
        std::wstring m_page{ L"general" };
        bool m_discoveryHeld{ false };
        // Stops the discovery spinner after a while: an indeterminate ProgressRing redraws at the
        // display refresh rate (~120 ms CPU/s measured on a 300 Hz display).
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_spinnerTimer{ nullptr };
        bool m_fontsLoaded{ false };

        std::vector<ToggleBinding> m_toggles;
        std::vector<NumberBinding> m_numbers;
        std::vector<ComboBinding> m_combos;
        std::vector<RadioBinding> m_radios;
        std::vector<std::unique_ptr<SliderBinding>> m_sliders;

        std::string m_devicesSignature;
        std::string m_discoveredSignature;
        std::string m_blacklistSignature;
        std::string m_whitelistSignature;
        std::string m_prioritySignature;
        std::string m_providersSignature;
        std::string m_outputsSignature;
        std::string m_offsetsSignature;
        std::vector<std::pair<Microsoft::UI::Xaml::Controls::TextBlock, std::string>> m_diagValues;

        Microsoft::UI::Xaml::Controls::ContentDialog m_pairDialog{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_pairTitle{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_pairBody{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_pairCode{ nullptr };
        Microsoft::UI::Xaml::Controls::ProgressRing m_pairProgress{ nullptr };
        std::string m_pairDialogState;
        std::string m_pairDismissedState;
        bool m_dialogOpen{ false };
    };
}

namespace winrt::MediaDuetUI::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}
