#include "LoopbackSender.h"

#include "md/link/SecureChannel.h"
#include "md/util/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <cmath>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace md::platform {

using winrt::com_ptr;

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrameSamples = kRate / 100; // 10 ms
constexpr size_t kFrameBytes = kFrameSamples * kChannels * 2;

SOCKET connectWithTimeout(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        err = "cannot resolve " + host;
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, nullptr, &tv) == 1) {
            int so = 0, len = sizeof(so);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
            if (so == 0) break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) err = "cannot connect to the phone's audio port";
    return s;
}

// Sends all bytes or reports would-block (caller drops the frame) / error.
enum class SendResult { Ok, WouldBlock, Error };
SendResult sendAll(SOCKET s, const uint8_t* p, size_t n) {
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        int r = send(s, reinterpret_cast<const char*>(p + off), static_cast<int>(n - off), 0);
        if (r > 0) {
            off += static_cast<size_t>(r);
            continue;
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return SendResult::Error;
        if (off == 0) return SendResult::WouldBlock;
        // Partially written frame: must finish it to keep framing intact.
        if (++spins > 200) return SendResult::Error;
        Sleep(1);
    }
    return SendResult::Ok;
}

int16_t toS16(float f) {
    if (f > 1.f) f = 1.f;
    if (f < -1.f) f = -1.f;
    return static_cast<int16_t>(std::lrintf(f * 32767.f));
}

} // namespace

LoopbackSender::LoopbackSender(IScheduler& sched, std::function<void()> changed)
    : sched_(sched), changed_(std::move(changed)) {}

LoopbackSender::~LoopbackSender() {
    *alive_ = false;
    stop();
}

LoopbackSender::Status LoopbackSender::status() const {
    return {state_, error_, sent_.load(), dropped_.load()};
}

void LoopbackSender::report(std::string state, std::string error) {
    std::weak_ptr<bool> alive = alive_;
    sched_.post([this, alive, state, error] {
        if (alive.expired()) return;
        state_ = state;
        error_ = error;
        if (changed_) changed_();
    });
}

void LoopbackSender::start(const std::string& host, uint16_t port, Bytes streamId, Bytes key, Bytes iv) {
    stop();
    stop_ = false;
    sent_ = 0;
    dropped_ = 0;
    state_ = "connecting";
    error_.clear();
    if (changed_) changed_();
    thread_ = std::thread([this, host, port, s = std::move(streamId), k = std::move(key), i = std::move(iv)]() mutable {
        run(host, port, std::move(s), std::move(k), std::move(i));
    });
}

void LoopbackSender::stop(const std::string& reason) {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (state_ != "off" && state_ != "error") {
        state_ = "off";
        if (!reason.empty()) log::info("loopback", "stopped: {}", reason);
        if (changed_) changed_();
    }
}

void LoopbackSender::run(std::string host, uint16_t port, Bytes streamId, Bytes key, Bytes iv) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::string err;
    SOCKET sock = INVALID_SOCKET;
    HANDLE ev = nullptr;
    DWORD task = 0;
    HANDLE mmcss = nullptr;
    auto cleanup = [&] {
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (ev) CloseHandle(ev);
        if (sock != INVALID_SOCKET) closesocket(sock);
        WSACleanup();
        if (SUCCEEDED(co)) CoUninitialize();
    };
    try {
        sock = connectWithTimeout(host, port, 5000, err);
        if (sock == INVALID_SOCKET) throw std::runtime_error(err);
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        int sndbuf = 64 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
        if (sendAll(sock, streamId.data(), streamId.size()) != SendResult::Ok) throw std::runtime_error("handshake send failed");

        com_ptr<IMMDeviceEnumerator> en;
        winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.put())));
        com_ptr<IMMDevice> dev;
        winrt::check_hresult(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
        com_ptr<IAudioClient> client;
        winrt::check_hresult(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
        WAVEFORMATEX* mix = nullptr;
        winrt::check_hresult(client->GetMixFormat(&mix));
        // Ask the audio engine to convert to 48 kHz stereo float for us.
        WAVEFORMATEXTENSIBLE want{};
        want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        want.Format.nChannels = kChannels;
        want.Format.nSamplesPerSec = kRate;
        want.Format.wBitsPerSample = 32;
        want.Format.nBlockAlign = kChannels * 4;
        want.Format.nAvgBytesPerSec = kRate * want.Format.nBlockAlign;
        want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        want.Samples.wValidBitsPerSample = 32;
        want.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        want.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        CoTaskMemFree(mix);
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 20 * 10000, 0,
                                                reinterpret_cast<WAVEFORMATEX*>(&want), nullptr));
        winrt::check_hresult(client->SetEventHandle(ev));
        com_ptr<IAudioCaptureClient> cap;
        winrt::check_hresult(client->GetService(IID_PPV_ARGS(cap.put())));
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        winrt::check_hresult(client->Start());
        report("streaming");
        log::info("loopback", "streaming PC audio to {}:{}", host, port);

        link::RecordCipher cipher(key, iv, proto::FrameType::Audio);
        std::vector<int16_t> pending;
        pending.reserve(kFrameSamples * kChannels * 4);
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        while (!stop_) {
            // Loopback signals only when something renders; a timeout keeps stop responsive.
            WaitForSingleObject(ev, 50);
            UINT32 packet = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames * kChannels; ++i)
                    pending.push_back((fl & AUDCLNT_BUFFERFLAGS_SILENT) ? int16_t(0) : toS16(f[i]));
                cap->ReleaseBuffer(frames);
            }
            while (pending.size() >= kFrameSamples * kChannels) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                uint64_t us = static_cast<uint64_t>(now.QuadPart * 1'000'000 / freq.QuadPart);
                Bytes pt(8 + kFrameBytes);
                putU64BE(pt.data(), us);
                memcpy(pt.data() + 8, pending.data(), kFrameBytes);
                pending.erase(pending.begin(), pending.begin() + kFrameSamples * kChannels);
                // Seal only frames we will actually send: sequence numbers must stay contiguous.
                fd_set w;
                FD_ZERO(&w);
                FD_SET(sock, &w);
                timeval tv{0, 0};
                if (select(0, nullptr, &w, nullptr, &tv) != 1) {
                    ++dropped_;
                    continue;
                }
                Bytes frame = proto::encodeFrame(proto::FrameType::Audio, cipher.seal(pt));
                auto r = sendAll(sock, frame.data(), frame.size());
                if (r == SendResult::Error) throw std::runtime_error("phone closed the audio stream");
                if (r == SendResult::WouldBlock) {
                    // Cannot drop an already sealed frame without breaking the sequence; treat as fatal.
                    throw std::runtime_error("network too slow for audio streaming");
                }
                ++sent_;
            }
        }
        client->Stop();
        cleanup();
        report("off");
        return;
    } catch (const winrt::hresult_error& e) {
        err = "audio capture failed (0x" + std::to_string(static_cast<uint32_t>(e.code())) + ")";
    } catch (const std::exception& e) {
        err = e.what();
    }
    log::warn("loopback", "{}", err);
    cleanup();
    report("error", err);
}

} // namespace md::platform
