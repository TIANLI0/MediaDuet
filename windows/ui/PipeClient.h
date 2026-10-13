// Named-pipe JSON-lines client for the host IPC (docs/ipc.md).
// All I/O happens on one background thread using overlapped I/O; callbacks are invoked on that
// thread (the Controller marshals them to the UI thread).
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mdui
{
    class PipeClient
    {
    public:
        struct Callbacks
        {
            std::function<void()> connected;
            // hostMissing: true when the pipe does not exist (host not running), false when an
            // established connection was lost.
            std::function<void(bool hostMissing)> disconnected;
            std::function<void(std::string&& line)> line;
        };

        PipeClient(std::wstring pipeName, Callbacks callbacks);
        ~PipeClient();
        PipeClient(PipeClient const&) = delete;
        PipeClient& operator=(PipeClient const&) = delete;

        void Start();
        void Stop();
        // Queues one JSON object (without trailing newline). Returns false when not connected.
        bool Send(std::string line);
        // Skips the current reconnect back-off (e.g. right after launching the host).
        void RetryNow();
        bool IsConnected() const { return m_connected.load(); }
        std::wstring const& Name() const { return m_name; }

        // \\.\pipe\MediaDuet.UI.<current user SID>
        static std::wstring DefaultPipeName();

    private:
        void Run();
        void Session(HANDLE pipe);
        bool WriteAll(HANDLE pipe, std::string const& data, bool watchStop);

        std::wstring m_name;
        Callbacks m_callbacks;
        std::thread m_thread;
        HANDLE m_stopEvent{ nullptr };
        HANDLE m_retryEvent{ nullptr };
        HANDLE m_writeEvent{ nullptr };
        std::mutex m_mutex;
        std::deque<std::string> m_queue;
        std::atomic<bool> m_connected{ false };
    };
}
