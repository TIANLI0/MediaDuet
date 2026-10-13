#include "pch.h"
#include "App.xaml.h"

#include "RenderTest.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::MediaDuetUI::implementation
{
    App::App(mdui::LaunchOptions options) : m_options(std::move(options))
    {
        if (!m_options.renderTest.empty())
        {
            UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
                mdui::render::LogLine(L"UNHANDLED: " + std::wstring(e.Message()) + L" (0x" +
                    std::to_wstring(static_cast<uint32_t>(e.Exception().value)) + L")");
            });
        }
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        // The Controller decides when the process ends (last window closed, host.exit); windows are
        // also replaced on a language switch, which must not end the dispatcher loop.
        DispatcherShutdownMode(Microsoft::UI::Xaml::DispatcherShutdownMode::OnExplicitShutdown);
        mdui::Controller::Get().Start(m_options);
    }
}
