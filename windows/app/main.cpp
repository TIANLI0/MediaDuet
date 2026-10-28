// MediaDuet host entry point (tray app; see docs/architecture.md).

#include "HostApp.h"
#include "Strings.h"
#include "TrayIcon.h"
#include "AppIdentity.h"
#include "UnlockSetup.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    bool background = false;
    std::string show;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc == 2 && std::wstring(argv[1]) == L"--unlock-remove") {
        LocalFree(argv); return md::app::removeUnlockForCurrentUser();
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--unlock-component") {
        int rc = argc == 4 || argc == 5 ? md::app::runUnlockComponent(argv[2], argv[3], argc == 5 ? argv[4] : L"") : 2;
        LocalFree(argv);
        return rc;
    }
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--background") background = true;
        else if (a == L"--show" && i + 1 < argc) {
            show = md::text::toUtf8(argv[++i]);
        }
    }
    LocalFree(argv);

    // Maintenance switch (no UI, no single-instance check): registers the app
    // identity so Windows shows "MediaDuet" instead of "Unknown app".
    if (wcsstr(GetCommandLineW(), L"--register-identity")) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        md::platform::ensureAppIdentity(L"MediaDuet");
        auto name = md::platform::resolvedAppName();
        return name.empty() ? 2 : 0;
    }

    // Single instance per user session: forward activation to the running host.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MediaDuet.Host.v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND h = FindWindowW(md::app::TrayIcon::kWindowClass, nullptr)) {
            if (!background) PostMessageW(h, md::app::TrayIcon::kMsgActivate, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    SetCurrentProcessExplicitAppUserModelID(L"MediaDuet.Host");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    int rc = 0;
    try {
        md::app::HostApp app;
        rc = app.run(background, show);
    } catch (const std::exception& e) {
        md::log::error("host", "fatal: {}", e.what());
        rc = 1;
    } catch (const winrt::hresult_error& e) {
        md::log::error("host", "fatal WinRT error 0x{:08x}", static_cast<uint32_t>(e.code()));
        rc = 1;
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}
