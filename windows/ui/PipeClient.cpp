#include "pch.h"
#include "PipeClient.h"

#include <sddl.h>

#include <algorithm>
#include <memory>

namespace mdui
{
    namespace
    {
        constexpr size_t kMaxLine = 4u * 1024u * 1024u;   // docs/ipc.md: max 4 MiB per line
        constexpr DWORD kReadChunk = 64 * 1024;
        constexpr DWORD kMinBackoffMs = 250;
        constexpr DWORD kMaxBackoffMs = 3000;

        struct HandleCloser
        {
            void operator()(HANDLE h) const noexcept
            {
                if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        };
        using UniqueHandle = std::unique_ptr<void, HandleCloser>;
    }

    PipeClient::PipeClient(std::wstring pipeName, Callbacks callbacks)
        : m_name(std::move(pipeName)), m_callbacks(std::move(callbacks))
    {
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_retryEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    PipeClient::~PipeClient()
    {
        Stop();
        CloseHandle(m_stopEvent);
        CloseHandle(m_retryEvent);
        CloseHandle(m_writeEvent);
    }

    std::wstring PipeClient::DefaultPipeName()
    {
        std::wstring sidString;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            if (size > 0)
            {
                auto buffer = std::make_unique<BYTE[]>(size);
                if (GetTokenInformation(token, TokenUser, buffer.get(), size, &size))
                {
                    auto user = reinterpret_cast<TOKEN_USER*>(buffer.get());
                    LPWSTR str = nullptr;
                    if (ConvertSidToStringSidW(user->User.Sid, &str))
                    {
                        sidString = str;
                        LocalFree(str);
                    }
                }
            }
            CloseHandle(token);
        }
        return L"\\\\.\\pipe\\MediaDuet.UI." + sidString;
    }

    void PipeClient::Start()
    {
        if (m_thread.joinable()) return;
        ResetEvent(m_stopEvent);
        m_thread = std::thread([this] { Run(); });
    }

    void PipeClient::Stop()
    {
        if (!m_thread.joinable()) return;
        SetEvent(m_stopEvent);
        m_thread.join();
        std::lock_guard lock(m_mutex);
        for (auto& message : m_queue) SecureZeroMemory(message.data(), message.size());
        m_queue.clear();
    }

    bool PipeClient::Send(std::string line)
    {
        if (!m_connected.load()) { SecureZeroMemory(line.data(), line.size()); return false; }
        line.push_back('\n');
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(line));
        }
        SetEvent(m_writeEvent);
        return true;
    }

    void PipeClient::RetryNow()
    {
        SetEvent(m_retryEvent);
    }

    void PipeClient::Run()
    {
        DWORD backoff = kMinBackoffMs;
        while (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
        {
            HANDLE pipe = CreateFileW(m_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_BUSY)
                {
                    // The host is replacing the previous client; wait briefly for an instance.
                    WaitNamedPipeW(m_name.c_str(), 1000);
                    continue;
                }
                if (m_callbacks.disconnected) m_callbacks.disconnected(true);
                HANDLE waits[] = { m_stopEvent, m_retryEvent };
                DWORD r = WaitForMultipleObjects(2, waits, FALSE, backoff);
                if (r == WAIT_OBJECT_0) break;
                backoff = (r == WAIT_OBJECT_0 + 1) ? kMinBackoffMs : std::min(backoff * 2, kMaxBackoffMs);
                continue;
            }

            backoff = kMinBackoffMs;
            {
                std::lock_guard lock(m_mutex);
                for (auto& message : m_queue) SecureZeroMemory(message.data(), message.size());
                m_queue.clear();
            }
            m_connected = true;
            if (m_callbacks.connected) m_callbacks.connected();
            Session(pipe);
            m_connected = false;
            CloseHandle(pipe);
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;
            if (m_callbacks.disconnected) m_callbacks.disconnected(false);
            HANDLE waits[] = { m_stopEvent, m_retryEvent };
            if (WaitForMultipleObjects(2, waits, FALSE, kMinBackoffMs) == WAIT_OBJECT_0) break;
        }
    }

    bool PipeClient::WriteAll(HANDLE pipe, std::string const& data, bool watchStop)
    {
        UniqueHandle evt(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!evt) return false;
        size_t offset = 0;
        while (offset < data.size())
        {
            OVERLAPPED ov{};
            ov.hEvent = evt.get();
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(pipe, data.data() + offset, chunk, nullptr, &ov))
            {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                HANDLE waits[] = { evt.get(), m_stopEvent };
                DWORD r = WaitForMultipleObjects(watchStop ? 2 : 1, waits, FALSE, watchStop ? 10000 : 500);
                if (r != WAIT_OBJECT_0)
                {
                    CancelIoEx(pipe, &ov);
                    GetOverlappedResult(pipe, &ov, &written, TRUE);
                    return false;
                }
            }
            if (!GetOverlappedResult(pipe, &ov, &written, TRUE)) return false;
            offset += written;
        }
        return true;
    }

    void PipeClient::Session(HANDLE pipe)
    {
        UniqueHandle readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!readEvent) return;
        auto buffer = std::make_unique<char[]>(kReadChunk);
        std::string pending;
        OVERLAPPED rov{};
        bool readPending = false;

        auto issueRead = [&]() -> bool {
            rov = {};
            rov.hEvent = readEvent.get();
            ResetEvent(readEvent.get());
            if (!ReadFile(pipe, buffer.get(), kReadChunk, nullptr, &rov))
            {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) return false;
            }
            readPending = true;
            return true;
        };

        bool alive = issueRead();
        while (alive)
        {
            HANDLE waits[] = { m_stopEvent, readEvent.get(), m_writeEvent };
            DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0)
            {
                // Flush what the UI queued right before exiting (e.g. "discover":false).
                std::deque<std::string> batch;
                {
                    std::lock_guard lock(m_mutex);
                    batch.swap(m_queue);
                }
                for (auto& msg : batch) {
                    bool ok = WriteAll(pipe, msg, false);
                    SecureZeroMemory(msg.data(), msg.size());
                    if (!ok) break;
                }
                for (auto& msg : batch) SecureZeroMemory(msg.data(), msg.size());
                break;
            }
            if (r == WAIT_OBJECT_0 + 1)
            {
                DWORD n = 0;
                readPending = false;
                if (!GetOverlappedResult(pipe, &rov, &n, FALSE))
                {
                    if (GetLastError() != ERROR_MORE_DATA) break;
                }
                pending.append(buffer.get(), n);
                size_t start = 0;
                for (;;)
                {
                    auto nl = pending.find('\n', start);
                    if (nl == std::string::npos) break;
                    std::string line = pending.substr(start, nl - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    start = nl + 1;
                    if (!line.empty() && m_callbacks.line) m_callbacks.line(std::move(line));
                }
                pending.erase(0, start);
                if (pending.size() > kMaxLine) break;   // protocol violation; reconnect
                alive = issueRead();
            }
            else if (r == WAIT_OBJECT_0 + 2)
            {
                std::deque<std::string> batch;
                {
                    std::lock_guard lock(m_mutex);
                    batch.swap(m_queue);
                }
                for (auto& msg : batch)
                {
                    bool ok = WriteAll(pipe, msg, true);
                    SecureZeroMemory(msg.data(), msg.size());
                    if (!ok)
                    {
                        alive = false;
                        break;
                    }
                }
                for (auto& msg : batch) SecureZeroMemory(msg.data(), msg.size());
            }
            else
            {
                break;
            }
        }

        if (readPending)
        {
            DWORD n = 0;
            CancelIoEx(pipe, &rov);
            GetOverlappedResult(pipe, &rov, &n, TRUE);
        }
    }
}
