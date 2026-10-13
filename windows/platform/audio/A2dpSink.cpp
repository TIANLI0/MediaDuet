#include "A2dpSink.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Media::Audio;

A2dpSink::A2dpSink(IScheduler& sched, std::function<void()> changed) : sched_(sched), changed_(std::move(changed)) {}

A2dpSink::~A2dpSink() {
    *alive_ = false;
    try {
        if (watcher_) watcher_.Stop();
    } catch (...) {
    }
    try {
        if (conn_) conn_.Close();
    } catch (...) {
    }
}

void A2dpSink::setState(std::string state, std::string error) {
    st_.state = std::move(state);
    st_.error = std::move(error);
    if (changed_) changed_();
}

void A2dpSink::start() {
    try {
        watcher_ = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
        std::weak_ptr<bool> alive = alive_;
        watcher_.Added([this, alive](auto&&, DeviceInformation const& info) {
            std::wstring id(info.Id());
            std::string name = text::toUtf8(info.Name());
            sched_.post([this, alive, id, name] {
                if (alive.expired()) return;
                devices_[id] = name;
                st_.available = true;
                if (changed_) changed_();
            });
        });
        watcher_.Removed([this, alive](auto&&, DeviceInformationUpdate const& upd) {
            std::wstring id(upd.Id());
            sched_.post([this, alive, id] {
                if (alive.expired()) return;
                devices_.erase(id);
                st_.available = !devices_.empty();
                if (changed_) changed_();
            });
        });
        watcher_.Start();
    } catch (hresult_error const& e) {
        log::warn("a2dp", "AudioPlaybackConnection not supported: {}", text::toUtf8(e.message()));
        st_.available = false;
    }
}

void A2dpSink::enable(const std::string& deviceName) {
    if (active()) return;
    std::wstring id;
    for (auto& [k, n] : devices_)
        if (n == deviceName) id = k;
    if (id.empty() && devices_.size() == 1) id = devices_.begin()->first;
    if (id.empty()) {
        setState("error", devices_.empty() ? "no paired phone offers Bluetooth audio (A2DP source)"
                                           : "could not decide which phone to receive audio from");
        return;
    }
    st_.device = devices_[id];
    setState("connecting");
    uint64_t attempt = ++attempt_;
    std::weak_ptr<bool> alive = alive_;
    [](A2dpSink* self, std::weak_ptr<bool> alive, std::wstring id, uint64_t attempt) -> fire_and_forget {
        co_await resume_background();
        AudioPlaybackConnection conn{nullptr};
        std::string err;
        bool ok = false;
        try {
            conn = AudioPlaybackConnection::TryCreateFromId(id);
            if (!conn) {
                err = "device is not available for audio playback";
            } else {
                co_await conn.StartAsync();
                auto result = co_await conn.OpenAsync();
                switch (result.Status()) {
                case AudioPlaybackConnectionOpenResultStatus::Success: ok = true; break;
                case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut: err = "the phone did not respond"; break;
                case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem: err = "denied by Windows"; break;
                default: err = "failed (" + std::to_string(static_cast<int>(result.Status())) + ")"; break;
                }
            }
        } catch (hresult_error const& e) {
            err = text::toUtf8(e.message());
        }
        self->sched_.post([self, alive, conn, ok, err, attempt] {
            if (alive.expired()) return;
            if (attempt != self->attempt_) {
                if (conn) conn.Close();
                return;
            }
            if (!ok) {
                if (conn) conn.Close();
                log::warn("a2dp", "audio receive failed: {}", err);
                self->setState("error", err);
                return;
            }
            self->conn_ = conn;
            std::weak_ptr<bool> alive2 = self->alive_;
            self->connTok_ = conn.StateChanged([self, alive2, attempt](AudioPlaybackConnection const& c, auto&&) {
                bool closed = c.State() == AudioPlaybackConnectionState::Closed;
                self->sched_.post([self, alive2, closed, attempt] {
                    if (alive2.expired() || attempt != self->attempt_ || !closed) return;
                    self->conn_ = nullptr;
                    log::info("a2dp", "phone audio connection closed");
                    self->setState("off");
                });
            });
            log::info("a2dp", "receiving Bluetooth audio from {}", self->st_.device);
            self->setState("on");
        });
    }(this, alive, id, attempt);
}

void A2dpSink::disable() {
    ++attempt_;
    if (conn_) {
        try {
            conn_.StateChanged(connTok_);
            conn_.Close();
        } catch (...) {
        }
        conn_ = nullptr;
    }
    if (st_.state != "off") setState("off");
}

} // namespace md::platform
