#include "md/util/Log.h"

#include "md/util/Text.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace md::log {
namespace {

std::mutex g_mutex;
std::wstring g_path;
FILE* g_file = nullptr;
std::atomic<int> g_level{static_cast<int>(Level::Info)};
constexpr long kMaxBytes = 1 << 20;

const char* levelName(Level l) {
    switch (l) {
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warn: return "W";
    case Level::Error: return "E";
    }
    return "?";
}

void rotateLocked() {
    if (!g_file) return;
    if (std::ftell(g_file) < kMaxBytes) return;
    std::fclose(g_file);
    g_file = nullptr;
    std::error_code ec;
    std::filesystem::rename(g_path, g_path + L".1", ec);
    g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYWR);
}

} // namespace

void init(const std::wstring& directory, Level minLevel) {
    std::lock_guard lock(g_mutex);
    g_level = static_cast<int>(minLevel);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    g_path = (std::filesystem::path(directory) / L"host.log").wstring();
    if (g_file) std::fclose(g_file);
    g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYWR);
}

void setLevel(Level minLevel) { g_level = static_cast<int>(minLevel); }

void write(Level level, std::string_view tag, std::string_view message) {
    if (static_cast<int>(level) < g_level.load()) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = std::format("{:02}-{:02} {:02}:{:02}:{:02}.{:03} {} [{}] {} {}\n", st.wMonth, st.wDay,
                                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, levelName(level),
                                   GetCurrentThreadId(), tag, message);
    OutputDebugStringW(text::toWide(line).c_str());
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
        rotateLocked();
    }
}

} // namespace md::log
