// Phase 0 API feasibility probe for MediaDuet (not part of the product).
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cstdio>
#include <string>

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Rfcomm;

static void p(const wchar_t* fmt, ...) {
    va_list a; va_start(a, fmt); vwprintf(fmt, a); va_end(a); fflush(stdout);
}

static void ProbeGsmtc(const wchar_t* tag) {
    p(L"\n== GSMTC (%s) ==\n", tag);
    auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    auto cur = mgr.GetCurrentSession();
    p(L"current: %s\n", cur ? cur.SourceAppUserModelId().c_str() : L"(none)");
    for (auto const& s : mgr.GetSessions()) {
        auto info = s.GetPlaybackInfo();
        std::wstring title;
        try { auto props = s.TryGetMediaPropertiesAsync().get(); title = std::wstring(props.Title()) + L" / " + std::wstring(props.Artist()); } catch (...) { title = L"(props failed)"; }
        auto tl = s.GetTimelineProperties();
        p(L"  session app=%s status=%d title=%s pos=%lldms end=%lldms canPause=%d canNext=%d canSeek=%d\n",
          s.SourceAppUserModelId().c_str(), (int)info.PlaybackStatus(), title.c_str(),
          tl.Position().count() / 10000, tl.EndTime().count() / 10000,
          (int)info.Controls().IsPauseEnabled(), (int)info.Controls().IsNextEnabled(),
          (int)info.Controls().IsPlaybackPositionEnabled());
    }
}

static void ProbeBluetooth() {
    p(L"\n== Bluetooth ==\n");
    auto adapter = BluetoothAdapter::GetDefaultAsync().get();
    if (!adapter) { p(L"no adapter\n"); return; }
    p(L"adapter classic=%d le=%d central=%d addr=%012llx\n", (int)adapter.IsClassicSupported(), (int)adapter.IsLowEnergySupported(),
      (int)adapter.IsCentralRoleSupported(), adapter.BluetoothAddress());
    auto paired = DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get();
    for (auto const& di : paired) {
        p(L"  paired: %s\n", di.Name().c_str());
        try {
            auto dev = BluetoothDevice::FromIdAsync(di.Id()).get();
            if (!dev) continue;
            auto r = dev.GetRfcommServicesAsync(BluetoothCacheMode::Cached).get();
            p(L"    rfcomm services (cached) err=%d count=%u\n", (int)r.Error(), r.Services().Size());
            for (auto const& svc : r.Services()) {
                auto g = svc.ServiceId().Uuid();
                wchar_t buf[64]; StringFromGUID2(g, buf, 64);
                p(L"      %s\n", buf);
            }
        } catch (hresult_error const& e) { p(L"    error %s\n", e.message().c_str()); }
    }
}

static void ProbeA2dpSink() {
    p(L"\n== AudioPlaybackConnection (A2DP sink) ==\n");
    try {
        auto sel = Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        auto devs = DeviceInformation::FindAllAsync(sel).get();
        p(L"devices eligible: %u\n", devs.Size());
        for (auto const& d : devs) p(L"  %s\n", d.Name().c_str());
    } catch (hresult_error const& e) { p(L"error 0x%08x %s\n", (unsigned)e.code(), e.message().c_str()); }
}

static void ProbeCoreAudio() {
    p(L"\n== Core Audio sessions ==\n");
    com_ptr<IMMDeviceEnumerator> en;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.put_void()));
    com_ptr<IMMDevice> dev; check_hresult(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
    com_ptr<IAudioSessionManager2> mgr;
    check_hresult(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void()));
    com_ptr<IAudioSessionEnumerator> se; check_hresult(mgr->GetSessionEnumerator(se.put()));
    int n = 0; se->GetCount(&n);
    for (int i = 0; i < n; ++i) {
        com_ptr<IAudioSessionControl> c; se->GetSession(i, c.put());
        auto c2 = c.as<IAudioSessionControl2>();
        AudioSessionState st; c->GetState(&st);
        DWORD pid = 0; c2->GetProcessId(&pid);
        float peak = 0; if (auto m = c.try_as<IAudioMeterInformation>()) m->GetPeakValue(&peak);
        p(L"  pid=%lu state=%d peak=%.3f system=%d\n", pid, (int)st, peak, c2->IsSystemSoundsSession() == S_OK);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void ProbeSmtcBridge() {
    p(L"\n== SMTC remote bridge (no audio) ==\n");
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MdProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    auto interop = get_activation_factory<Windows::Media::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
    Windows::Media::SystemMediaTransportControls smtc{ nullptr };
    HRESULT hr = interop->GetForWindow(hwnd, guid_of<Windows::Media::SystemMediaTransportControls>(), put_abi(smtc));
    p(L"GetForWindow hr=0x%08x\n", (unsigned)hr);
    if (FAILED(hr)) return;
    smtc.IsEnabled(true); smtc.IsPlayEnabled(true); smtc.IsPauseEnabled(true); smtc.IsNextEnabled(true);
    smtc.PlaybackStatus(Windows::Media::MediaPlaybackStatus::Playing);
    auto du = smtc.DisplayUpdater();
    du.Type(Windows::Media::MediaPlaybackType::Music);
    du.MusicProperties().Title(L"MediaDuet Probe Title"); du.MusicProperties().Artist(L"Probe Artist");
    du.Update();
    Windows::Media::SystemMediaTransportControlsTimelineProperties tl;
    tl.StartTime({}); tl.EndTime(std::chrono::seconds(200)); tl.Position(std::chrono::seconds(42));
    tl.MinSeekTime({}); tl.MaxSeekTime(std::chrono::seconds(200));
    smtc.UpdateTimelineProperties(tl);
    // pump a little so the session gets published
    for (int i = 0; i < 30; ++i) { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); Sleep(50); }
    ProbeGsmtc(L"after SMTC registration");
    DestroyWindow(hwnd);
}

int main() {
    init_apartment(apartment_type::multi_threaded);
    try { ProbeGsmtc(L"baseline"); } catch (hresult_error const& e) { p(L"gsmtc err %s\n", e.message().c_str()); }
    try { ProbeBluetooth(); } catch (hresult_error const& e) { p(L"bt err %s\n", e.message().c_str()); }
    ProbeA2dpSink();
    try { ProbeCoreAudio(); } catch (hresult_error const& e) { p(L"audio err %s\n", e.message().c_str()); }
    try { ProbeSmtcBridge(); } catch (hresult_error const& e) { p(L"smtc err 0x%08x %s\n", (unsigned)e.code(), e.message().c_str()); }
    return 0;
}
