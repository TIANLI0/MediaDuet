// Entry point: single instance (AppInstance key + activation redirection), language override, XAML start.
#include "pch.h"
#include "App.xaml.h"
#include "Controller.h"
#include "PipeClient.h"
#include "RenderTest.h"
#include "Strings.h"

#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.Globalization.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

using namespace winrt;
using namespace winrt::Microsoft::Windows::AppLifecycle;

namespace
{
    std::wstring CommandLineFromArgs(AppActivationArguments const& args)
    {
        if (args.Kind() == ExtendedActivationKind::Launch)
        {
            if (auto launch = args.Data().try_as<winrt::Windows::ApplicationModel::Activation::ILaunchActivatedEventArgs>())
                return std::wstring(launch.Arguments());
        }
        return {};
    }

    // Returns true when this process should exit because an existing instance took the activation.
    bool RedirectToExistingInstance(std::wstring const& key)
    {
        AppInstance instance{ nullptr };
        try
        {
            instance = AppInstance::FindOrRegisterForKey(key);
        }
        catch (hresult_error const&)
        {
            return false;   // AppLifecycle unavailable: run as an independent instance.
        }
        if (instance.IsCurrent())
        {
            instance.Activated([](auto&&, AppActivationArguments const& args) {
                // Arrives on a background thread; the Controller marshals to the UI thread.
                auto cmd = CommandLineFromArgs(args);
                if (cmd.empty()) cmd = L"MediaDuetUI.exe";
                mdui::Controller::Get().QueueActivation(cmd);
            });
            return false;
        }

        AllowSetForegroundWindow(instance.ProcessId());
        auto args = AppInstance::GetCurrent().GetActivatedEventArgs();
        // RedirectActivationToAsync must not block the STA; wait on a worker and pump COM.
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread worker([&] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                instance.RedirectActivationToAsync(args).get();
            }
            catch (hresult_error const&)
            {
            }
            SetEvent(done);
            winrt::uninit_apartment();
        });
        DWORD index = 0;
        CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, &done, &index);
        worker.join();
        CloseHandle(done);
        return true;
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    auto options = mdui::ParseCommandLine(GetCommandLineW());

    if (!options.renderTest.empty())
    {
        // Invisible render test: only against a private test pipe (never a running MediaDuet), no
        // single-instance redirection (that would bring a real UI window up).
        auto def = mdui::PipeClient::DefaultPipeName();
        if (options.pipe.empty() || options.pipe == def || (L"\\\\.\\pipe\\" + options.pipe) == def) return 2;
        mdui::render::SetActive(options.renderTest, options.renderOnly);
        if (!options.languageGiven)
        {
            options.language = L"en-US";
            options.languageGiven = true;
        }
    }
    else
    {
        // One UI process per user session (per pipe when --pipe is used for testing).
        std::wstring key = L"MediaDuetUI";
        auto pipe = options.pipe.empty() ? mdui::PipeClient::DefaultPipeName() : options.pipe;
        for (auto c : pipe) key.push_back(iswalnum(c) || c == L'-' ? c : L'_');
        if (RedirectToExistingInstance(key)) return 0;
    }

    // Language must be decided before any XAML (x:Uid) is loaded.
    std::wstring language = options.languageGiven ? options.language : mdui::ReadPersistedLanguage();
    if (language != L"zh-CN" && language != L"en-US") language = L"auto";
    if (language != L"auto")
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(language);
    mdui::InitStrings(language == L"auto" ? std::wstring{} : language);

    ::winrt::Microsoft::UI::Xaml::Application::Start([options](auto&&) {
        ::winrt::make<::winrt::MediaDuetUI::implementation::App>(options);
    });

    mdui::Controller::Get().Shutdown();
    return 0;
}
