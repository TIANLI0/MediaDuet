#pragma once

#include "App.xaml.g.h"
#include "Controller.h"

namespace winrt::MediaDuetUI::implementation
{
    struct App : AppT<App>
    {
        explicit App(mdui::LaunchOptions options);

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        mdui::LaunchOptions m_options;
    };
}
