#include "StreamTransport.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Storage::Streams;

namespace {
constexpr uint32_t kReadChunk = 64 * 1024;
}

StreamTransport::StreamTransport(IScheduler& sched, Windows::Networking::Sockets::StreamSocket socket,
                                 link::TransportKind kind, std::string description, std::string remoteHost)
    : sched_(sched), socket_(std::move(socket)), kind_(kind), desc_(std::move(description)),
      host_(std::move(remoteHost)) {}

StreamTransport::~StreamTransport() {
    try {
        socket_.Close();
    } catch (...) {
    }
}

void StreamTransport::start(Listener* listener) {
    listener_ = listener;
    readLoop();
}

void StreamTransport::readLoop() {
    auto self = shared_from_this();
    // Detached coroutine; keeps the transport alive until the socket closes.
    [](std::shared_ptr<StreamTransport> self) -> fire_and_forget {
        co_await resume_background();
        auto input = self->socket_.InputStream();
        Buffer buf(kReadChunk);
        std::string reason = "connection closed by peer";
        try {
            while (!self->closed_) {
                auto result = co_await input.ReadAsync(buf, kReadChunk, InputStreamOptions::Partial);
                uint32_t n = result.Length();
                if (n == 0) break; // EOF
                Bytes data(result.data(), result.data() + n);
                self->sched_.post([self, d = std::move(data)] {
                    if (!self->reported_ && self->listener_) self->listener_->onTransportData(d);
                });
            }
        } catch (hresult_error const& e) {
            reason = self->closed_ ? "closed locally" : text::toUtf8(e.message());
        } catch (...) {
            reason = "read failed";
        }
        self->reportClosed(self->closed_ ? "closed locally" : reason);
    }(self);
}

void StreamTransport::send(Bytes data) {
    if (closed_) return;
    {
        std::lock_guard lock(m_);
        queue_.push_back(std::move(data));
        ++pending_;
        if (writing_) return;
        writing_ = true;
    }
    pumpWrites();
}

void StreamTransport::pumpWrites() {
    [](std::shared_ptr<StreamTransport> self) -> fire_and_forget {
        co_await resume_background();
        auto out = self->socket_.OutputStream();
        try {
            for (;;) {
                Bytes next;
                {
                    std::lock_guard lock(self->m_);
                    if (self->queue_.empty() || self->closed_) {
                        self->writing_ = false;
                        break;
                    }
                    next = std::move(self->queue_.front());
                    self->queue_.pop_front();
                }
                Buffer b(static_cast<uint32_t>(next.size()));
                memcpy(b.data(), next.data(), next.size());
                b.Length(static_cast<uint32_t>(next.size()));
                co_await out.WriteAsync(b);
                if (--self->pending_ == 0) {
                    self->sched_.post([self] {
                        if (!self->reported_ && self->listener_) self->listener_->onTransportWritable();
                    });
                }
            }
        } catch (hresult_error const& e) {
            {
                std::lock_guard lock(self->m_);
                self->writing_ = false;
            }
            if (!self->closed_) {
                log::warn("transport", "{} write failed: {}", self->desc_, text::toUtf8(e.message()));
                self->close();
            }
        }
    }(shared_from_this());
}

void StreamTransport::close() {
    if (closed_.exchange(true)) return;
    {
        std::lock_guard lock(m_);
        pending_ -= queue_.size();
        queue_.clear();
    }
    try {
        socket_.Close(); // cancels the pending read, which then reports closure
    } catch (...) {
    }
}

void StreamTransport::reportClosed(const std::string& reason) {
    closed_ = true;
    auto self = shared_from_this();
    sched_.post([self, reason] {
        if (self->reported_.exchange(true)) return;
        auto* l = self->listener_;
        self->listener_ = nullptr;
        if (l) l->onTransportClosed(reason);
    });
}

} // namespace md::platform
