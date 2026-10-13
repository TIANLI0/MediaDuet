#pragma once

#include <format>
#include <string>
#include <string_view>

namespace md::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Optional file sink. Without it, messages go to OutputDebugString only.
// Files rotate at 1 MiB, keeping one previous file. Never log key material.
void init(const std::wstring& directory, Level minLevel);
void setLevel(Level minLevel);
void write(Level level, std::string_view tag, std::string_view message);

template <class... Args>
void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, tag, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace md::log
