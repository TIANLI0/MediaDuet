#include "pch.h"
#include "Strings.h"

#include "Model.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#include <optional>

using namespace winrt::Microsoft::Windows::ApplicationModel::Resources;

namespace mdui
{
    namespace
    {
        struct StringTable
        {
            ResourceManager manager{ nullptr };
            ResourceContext context{ nullptr };
            ResourceMap map{ nullptr };
        };

        std::optional<StringTable>& Table()
        {
            static std::optional<StringTable> table;
            return table;
        }

        std::wstring& Language()
        {
            static std::wstring lang;
            return lang;
        }
    }

    void InitStrings(std::wstring const& languageOverride)
    {
        StringTable t;
        t.manager = ResourceManager();
        t.context = t.manager.CreateResourceContext();
        if (!languageOverride.empty())
            t.context.QualifierValues().Insert(L"Language", languageOverride);
        t.map = t.manager.MainResourceMap().GetSubtree(L"Resources");
        Table() = std::move(t);

        Language() = ResolveLanguage(languageOverride);
    }

    std::wstring ResolveLanguage(std::wstring const& languageOverride)
    {
        std::wstring lang = languageOverride;
        if (lang.empty())
        {
            wchar_t buf[LOCALE_NAME_MAX_LENGTH * 4]{};
            ULONG count = 0, size = ARRAYSIZE(buf);
            if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf, &size) && buf[0])
                lang = buf;
        }
        return (_wcsnicmp(lang.c_str(), L"zh", 2) == 0) ? L"zh-CN" : L"en-US";
    }

    std::wstring const& UiLanguage()
    {
        return Language();
    }

    winrt::hstring Str(std::wstring_view key)
    {
        auto& t = Table();
        if (!t) InitStrings({});
        try
        {
            auto candidate = t->map.TryGetValue(key, t->context);
            if (candidate) return candidate.ValueAsString();
        }
        catch (winrt::hresult_error const&)
        {
        }
        return winrt::hstring(key);
    }

    std::wstring Format(std::wstring_view pattern, std::initializer_list<std::wstring_view> args)
    {
        std::wstring out;
        out.reserve(pattern.size() + 32);
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 2] == L'}' &&
                pattern[i + 1] >= L'0' && pattern[i + 1] <= L'9')
            {
                size_t idx = static_cast<size_t>(pattern[i + 1] - L'0');
                if (idx < args.size()) out.append(*(args.begin() + idx));
                i += 2;
                continue;
            }
            out.push_back(pattern[i]);
        }
        return out;
    }

    std::wstring StrF(std::wstring_view key, std::initializer_list<std::wstring_view> args)
    {
        auto pattern = Str(key);
        return Format(pattern, args);
    }

    std::wstring A2dpErrorText(std::string const& code, std::string const& fallback)
    {
        for (auto known : { "no_device", "ambiguous", "timeout", "denied", "failed" })
            if (code == known) return std::wstring(Str(L"A2dpError_" + Widen(code)));
        return fallback.empty() ? std::wstring(Str(L"A2dpError_failed")) : Widen(fallback);
    }
}
