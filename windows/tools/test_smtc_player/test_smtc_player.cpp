// TEST FIXTURE (not shipped): a silent Windows media session for end-to-end tests.
// Publishes an SMTC session "MediaDuet Fixture Track N" and logs every control
// request it receives to stdout and %TEMP%\mediaduet_fixture.log, so tests can
// verify that commands sent from the phone really reach a Windows player.
//
//   MediaDuetTestPlayer.exe [seconds]

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Media;
using namespace std::chrono;

namespace {
SystemMediaTransportControls g_smtc{nullptr};
int g_track = 1;
bool g_playing = true;
int64_t g_posMs = 0;
int64_t g_anchor = 0;
std::wofstream g_log;

int64_t nowMs() { return static_cast<int64_t>(GetTickCount64()); }
int64_t position() { return g_posMs + (g_playing ? nowMs() - g_anchor : 0); }

void logLine(const std::wstring& s) {
    wprintf(L"%s\n", s.c_str());
    fflush(stdout);
    g_log << s << std::endl;
}

void publish() {
    g_smtc.PlaybackStatus(g_playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
    auto du = g_smtc.DisplayUpdater();
    du.Type(MediaPlaybackType::Music);
    du.MusicProperties().Title(L"MediaDuet Fixture Track " + std::to_wstring(g_track));
    du.MusicProperties().Artist(L"Windows Fixture");
    du.MusicProperties().AlbumTitle(L"E2E");
    du.Update();
    SystemMediaTransportControlsTimelineProperties tl;
    tl.StartTime(winrt::Windows::Foundation::TimeSpan{0});
    tl.EndTime(seconds(180));
    tl.MinSeekTime(winrt::Windows::Foundation::TimeSpan{0});
    tl.MaxSeekTime(seconds(180));
    tl.Position(milliseconds(position()));
    g_smtc.UpdateTimelineProperties(tl);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP + 1) {
        publish();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    init_apartment(apartment_type::multi_threaded);
    SetCurrentProcessExplicitAppUserModelID(L"MediaDuet.TestFixture");
    int seconds = cmd && *cmd ? _wtoi(cmd) : 120;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_log.open(std::wstring(tmp) + L"mediaduet_fixture.log", std::ios::trunc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MediaDuetFixture";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"MediaDuet Fixture", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                nullptr, inst, nullptr);
    auto interop = get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
    check_hresult(interop->GetForWindow(hwnd, guid_of<SystemMediaTransportControls>(), put_abi(g_smtc)));
    g_smtc.IsEnabled(true);
    g_smtc.IsPlayEnabled(true);
    g_smtc.IsPauseEnabled(true);
    g_smtc.IsNextEnabled(true);
    g_smtc.IsPreviousEnabled(true);
    g_anchor = nowMs();
    g_smtc.ButtonPressed([hwnd](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& a) {
        g_posMs = position();
        g_anchor = nowMs();
        switch (a.Button()) {
        case SystemMediaTransportControlsButton::Play: g_playing = true; logLine(L"BUTTON play"); break;
        case SystemMediaTransportControlsButton::Pause: g_playing = false; logLine(L"BUTTON pause"); break;
        case SystemMediaTransportControlsButton::Next: ++g_track; g_posMs = 0; logLine(L"BUTTON next"); break;
        case SystemMediaTransportControlsButton::Previous: g_posMs = 0; logLine(L"BUTTON previous"); break;
        default: logLine(L"BUTTON other"); break;
        }
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
    });
    g_smtc.PlaybackPositionChangeRequested([hwnd](auto&&, PlaybackPositionChangeRequestedEventArgs const& a) {
        g_posMs = duration_cast<milliseconds>(a.RequestedPlaybackPosition()).count();
        g_anchor = nowMs();
        logLine(L"SEEK " + std::to_wstring(g_posMs));
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
    });
    publish();
    logLine(L"READY");
    SetTimer(hwnd, 1, seconds * 1000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) break;
        DispatchMessageW(&msg);
    }
    g_smtc.PlaybackStatus(MediaPlaybackStatus::Closed);
    g_smtc.IsEnabled(false);
    logLine(L"EXIT");
    return 0;
}
