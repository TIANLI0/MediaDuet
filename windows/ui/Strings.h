// Localized strings for code (XAML uses x:Uid). Backed by MRT Core and Strings\<lang>\Resources.resw.
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace mdui
{
    // Must be called once before the first Str() (empty = follow the OS UI language).
    void InitStrings(std::wstring const& languageOverride);
    // Resolved language tag actually used for code strings ("en-US" / "zh-CN").
    std::wstring const& UiLanguage();
    // "zh-CN" or "en-US" for an override (empty = OS UI language).
    std::wstring ResolveLanguage(std::wstring const& languageOverride);

    winrt::hstring Str(std::wstring_view key);
    // Replaces {0}, {1}, ... with the given arguments.
    std::wstring StrF(std::wstring_view key, std::initializer_list<std::wstring_view> args);
    std::wstring Format(std::wstring_view pattern, std::initializer_list<std::wstring_view> args);
}
