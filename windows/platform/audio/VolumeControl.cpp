#include "VolumeControl.h"

#include "md/util/Log.h"

#include "md/util/Text.h"

#include <unknwn.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
// Defines PKEY_Device_FriendlyName in this translation unit.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <winrt/base.h>

#include <algorithm>

namespace md::platform {

using winrt::com_ptr;

struct VolumeControl::Impl {
    IScheduler& sched;
    std::function<void(const State&)> onChange;
    com_ptr<IMMDeviceEnumerator> enumerator;
    com_ptr<IAudioEndpointVolume> volume;
    std::wstring deviceId;
    std::string deviceName;
    bool bluetooth = false;
    com_ptr<IAudioEndpointVolumeCallback> volumeCb;
    com_ptr<IMMNotificationClient> deviceCb;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    ScopedTimer rebind;

    Impl(IScheduler& s, std::function<void(const State&)> f) : sched(s), onChange(std::move(f)), rebind(&s) {}

    struct VolumeCb : winrt::implements<VolumeCb, IAudioEndpointVolumeCallback> {
        Impl* impl;
        std::weak_ptr<bool> alive;
        explicit VolumeCb(Impl* i) : impl(i), alive(i->alive) {}
        HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
            auto i = impl;
            auto a = alive;
            i->sched.post([i, a] {
                if (!a.expired()) i->notify();
            });
            return S_OK;
        }
    };

    struct DeviceCb : winrt::implements<DeviceCb, IMMNotificationClient> {
        Impl* impl;
        std::weak_ptr<bool> alive;
        explicit DeviceCb(Impl* i) : impl(i), alive(i->alive) {}
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
            if (flow != eRender || role != eMultimedia) return S_OK;
            auto i = impl;
            auto a = alive;
            i->sched.post([i, a] {
                if (!a.expired()) i->rebind.start(300, [i] {
                    i->bind();
                    i->notify();
                });
            });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
            auto i = impl;
            auto a = alive;
            i->sched.post([i, a] {
                if (!a.expired()) i->notify();
            });
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    };

    void unbind() {
        if (volume && volumeCb) volume->UnregisterControlChangeNotify(volumeCb.get());
        volume = nullptr;
    }

    void bind() {
        unbind();
        com_ptr<IMMDevice> dev;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.put()))) return;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            deviceId = id;
            CoTaskMemFree(id);
        }
        deviceName = friendlyName(dev.get());
        // Only on start and default-device changes.
        bluetooth = isBluetooth(dev.get());
        log::info("audio", "default output: {}{}", deviceName, bluetooth ? " (Bluetooth)" : "");
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.put_void()))) {
            volume = nullptr;
            return;
        }
        volume->RegisterControlChangeNotify(volumeCb.get());
    }

    // Bluetooth endpoints: the endpoint's enumerator is BTHENUM (A2DP), BTHHFENUM
    // (hands-free) or BTHLE* (LE Audio); otherwise look at the KS filter the
    // endpoint's connector leads to.
    static bool isBluetooth(IMMDevice* dev) {
        bool bt = false;
        com_ptr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_EnumeratorName, &pv)) && pv.vt == VT_LPWSTR)
                bt = _wcsnicmp(pv.pwszVal, L"BTH", 3) == 0;
            PropVariantClear(&pv);
        }
        if (bt) return true;
        com_ptr<IDeviceTopology> topo;
        com_ptr<IConnector> con;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topo.put_void())) &&
            SUCCEEDED(topo->GetConnector(0, con.put())) && SUCCEEDED(con->GetDeviceIdConnectedTo(&id)) && id) {
            std::string s = text::toLowerAscii(text::toUtf8(id));
            bt = s.find("bthenum") != std::string::npos || s.find("bthhfenum") != std::string::npos ||
                 s.find("bthle") != std::string::npos;
            CoTaskMemFree(id);
        }
        return bt;
    }

    static std::string friendlyName(IMMDevice* dev) {
        com_ptr<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return {};
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string out;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) out = text::toUtf8(pv.pwszVal);
        PropVariantClear(&pv);
        return out;
    }

    State read() const {
        State s;
        s.deviceId = deviceId;
        s.deviceName = deviceName;
        s.bluetooth = bluetooth;
        if (!volume) return s;
        s.available = true;
        volume->GetMasterVolumeLevelScalar(&s.level);
        BOOL m = FALSE;
        volume->GetMute(&m);
        s.muted = m != FALSE;
        UINT step = 0, count = 0;
        if (SUCCEEDED(volume->GetVolumeStepInfo(&step, &count))) s.steps = static_cast<int>(count > 0 ? count - 1 : 0);
        return s;
    }

    void notify() {
        if (onChange) onChange(read());
    }
};

VolumeControl::VolumeControl(IScheduler& sched, std::function<void(const State&)> onChange)
    : impl_(std::make_unique<Impl>(sched, std::move(onChange))) {}

VolumeControl::~VolumeControl() {
    *impl_->alive = false;
    impl_->unbind();
    if (impl_->enumerator && impl_->deviceCb) impl_->enumerator->UnregisterEndpointNotificationCallback(impl_->deviceCb.get());
}

bool VolumeControl::start() {
    auto& i = *impl_;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(i.enumerator.put()))))
        return false;
    i.volumeCb = winrt::make<Impl::VolumeCb>(&i);
    i.deviceCb = winrt::make<Impl::DeviceCb>(&i);
    i.enumerator->RegisterEndpointNotificationCallback(i.deviceCb.get());
    i.bind();
    return i.volume != nullptr;
}

VolumeControl::State VolumeControl::state() const { return impl_->read(); }

std::vector<VolumeControl::Output> VolumeControl::outputs() const {
    std::vector<Output> out;
    if (!impl_->enumerator) return out;
    com_ptr<IMMDeviceCollection> coll;
    if (FAILED(impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, coll.put()))) return out;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        com_ptr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        Output o;
        o.id = id;
        CoTaskMemFree(id);
        o.name = Impl::friendlyName(dev.get());
        o.isDefault = o.id == impl_->deviceId;
        out.push_back(std::move(o));
    }
    return out;
}

void VolumeControl::setLevel(float level) {
    if (!impl_->volume) return;
    level = std::clamp(level, 0.f, 1.f);
    impl_->volume->SetMasterVolumeLevelScalar(level, nullptr);
    if (level > 0.f) impl_->volume->SetMute(FALSE, nullptr);
}

void VolumeControl::setMuted(bool muted) {
    if (impl_->volume) impl_->volume->SetMute(muted ? TRUE : FALSE, nullptr);
}

void VolumeControl::step(int delta) {
    if (!impl_->volume) return;
    for (int k = 0; k < std::abs(delta) && k < 50; ++k) {
        if (delta > 0) impl_->volume->VolumeStepUp(nullptr);
        else impl_->volume->VolumeStepDown(nullptr);
    }
}

} // namespace md::platform
