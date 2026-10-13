#pragma once

#include <string>
#include <string_view>

namespace md::app {

// Host-side UI strings (tray, overlay menu, dialogs). The WinUI process has its
// own .resw resources.
enum class Lang { En, Zh };

void setLanguage(std::string_view setting); // "auto" | "zh-CN" | "en-US"
Lang language();
std::string languageTag(); // "zh-CN" | "en-US"
const wchar_t* tr(std::string_view key);

} // namespace md::app
