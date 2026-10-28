#include "IpcServer.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <sddl.h>

#include <vector>

namespace md::app {

namespace {

constexpr size_t kMaxLine = 4 * 1024 * 1024;

} // namespace

std::wstring currentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> buf(len);
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return out;
}

std::wstring IpcServer::pipeName() { return L"\\\\.\\pipe\\MediaDuet.UI." + currentUserSid(); }

IpcServer::IpcServer(IScheduler& sched, Callbacks cb)
    : IpcServer(sched, std::move(cb), Options{pipeName(), L"D:P(A;;GA;;;" + currentUserSid() + L")", nullptr}) {}

IpcServer::IpcServer(IScheduler& sched, Callbacks cb, Options opts) : sched_(sched), cb_(std::move(cb)), opts_(std::move(opts)) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sendEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

IpcServer::~IpcServer() {
    *alive_ = false;
    stop();
    for (auto& s : outbox_) SecureZeroMemory(s.data(), s.size());
    CloseHandle(stopEvent_);
    CloseHandle(sendEvent_);
}

bool IpcServer::start() {
    if (thread_.joinable()) return true;
    ResetEvent(stopEvent_);
    thread_ = std::thread([this] { run(); });
    return true;
}

void IpcServer::stop() {
    SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
}

void IpcServer::send(std::string line) {
    if (!connected_) { SecureZeroMemory(line.data(), line.size()); return; }
    line.push_back('\n');
    {
        std::lock_guard lock(m_);
        // Bound memory if the UI stops reading.
        if (outbox_.size() > 512) {
            SecureZeroMemory(outbox_.front().data(), outbox_.front().size()); outbox_.pop_front();
        }
        outbox_.push_back(std::move(line));
    }
    SetEvent(sendEvent_);
}

void IpcServer::run() {
    const std::wstring& name = opts_.name;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(opts_.sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(),
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       64 * 1024, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("ipc", "CreateNamedPipe failed ({})", GetLastError());
            if (WaitForSingleObject(stopEvent_, 2000) == WAIT_OBJECT_0) break;
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = ioEvent;
        ResetEvent(ioEvent);
        bool ok = ConnectNamedPipe(pipe, &ov) != 0;
        DWORD err = GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            HANDLE hs[] = {stopEvent_, ioEvent};
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &ov);
                DWORD ignored;
                GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                CloseHandle(pipe);
                break;
            }
            DWORD dummy;
            ok = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != 0;
        } else if (!ok && err == ERROR_PIPE_CONNECTED) {
            ok = true;
        }
        if (!ok) {
            CloseHandle(pipe);
            continue;
        }
        {
            std::lock_guard lock(m_);
            for (auto& s : outbox_) SecureZeroMemory(s.data(), s.size());
            outbox_.clear();
        }
        connected_ = true;
        sched_.post([alive = alive_, cb = cb_.onConnected] {
            if (*alive && cb) cb();
        });

        std::string partial;
        std::vector<char> buf(64 * 1024);
        bool readPending = false;
        bool verified = !opts_.verifyClient;
        OVERLAPPED rov{};
        rov.hEvent = ioEvent;
        bool alive = true;
        while (alive) {
            if (!readPending) {
                ResetEvent(ioEvent);
                DWORD n = 0;
                if (ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &n, &rov)) {
                    SetEvent(ioEvent); // completed synchronously; handled below
                } else if (GetLastError() != ERROR_IO_PENDING) {
                    break;
                }
                readPending = true;
            }
            HANDLE hs[] = {stopEvent_, ioEvent, sendEvent_};
            DWORD w = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                alive = false;
                break;
            }
            if (w == WAIT_OBJECT_0 + 1) {
                DWORD n = 0;
                if (!GetOverlappedResult(pipe, &rov, &n, FALSE)) break;
                readPending = false;
                if (n == 0) continue;
                if (!verified) {
                    if (!opts_.verifyClient(pipe)) {
                        log::warn("ipc", "refused a client of {}", text::toUtf8(name));
                        break;
                    }
                    verified = true;
                }
                partial.append(buf.data(), n);
                size_t start = 0, nl;
                while ((nl = partial.find('\n', start)) != std::string::npos) {
                    std::string line = partial.substr(start, nl - start);
                    start = nl + 1;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    if (line.size() > kMaxLine) { alive = false; break; }
                    if (opts_.prepareLine) opts_.prepareLine(line, pipe);
                    sched_.post([guard = alive_, cb = cb_.onLine, l = std::move(line)]() mutable {
                        if (*guard && cb) cb(l);
                        SecureZeroMemory(l.data(), l.size());
                    });
                }
                SecureZeroMemory(partial.data(), start);
                partial.erase(0, start);
                if (partial.size() > kMaxLine) {
                    log::warn("ipc", "client sent an over-long line; disconnecting");
                    break;
                }
            }
            if (w == WAIT_OBJECT_0 + 2) {
                std::deque<std::string> batch;
                {
                    std::lock_guard lock(m_);
                    batch.swap(outbox_);
                }
                for (auto& s : batch) {
                    OVERLAPPED wov{};
                    wov.hEvent = writeEvent;
                    ResetEvent(writeEvent);
                    DWORD written = 0;
                    if (!WriteFile(pipe, s.data(), static_cast<DWORD>(s.size()), &written, &wov) &&
                        GetLastError() != ERROR_IO_PENDING) {
                        SecureZeroMemory(s.data(), s.size());
                        alive = false;
                        break;
                    }
                    HANDLE waits[] = {stopEvent_, writeEvent};
                    if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0 + 1) CancelIoEx(pipe, &wov);
                    bool done = GetOverlappedResult(pipe, &wov, &written, TRUE) != 0 && written == s.size();
                    // Lines of the credential provider's pipe may carry a password.
                    SecureZeroMemory(s.data(), s.size());
                    if (!done) {
                        alive = false;
                        break;
                    }
                }
                for (auto& s : batch) SecureZeroMemory(s.data(), s.size());
            }
        }
        if (readPending) {
            CancelIoEx(pipe, &rov);
            DWORD n;
            GetOverlappedResult(pipe, &rov, &n, TRUE);
        }
        connected_ = false;
        SecureZeroMemory(buf.data(), buf.size());
        SecureZeroMemory(partial.data(), partial.size());
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        sched_.post([guard = alive_, cb = cb_.onDisconnected] {
            if (*guard && cb) cb();
        });
        if (!alive && WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
    }
    CloseHandle(ioEvent);
    CloseHandle(writeEvent);
    if (sd) LocalFree(sd);
}

} // namespace md::app
