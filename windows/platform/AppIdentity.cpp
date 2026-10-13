#include "AppIdentity.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include <filesystem>

namespace md::platform {

namespace fs = std::filesystem;
using winrt::com_ptr;

namespace {

fs::path exePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n));
}

fs::path startMenuPrograms() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

// Creates or updates a shortcut to this exe that carries the AUMID.
bool writeShortcut(const fs::path& lnk, const fs::path& target) {
    com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return false;
    auto file = link.as<IPersistFile>();
    bool exists = fs::exists(lnk);
    if (exists && FAILED(file->Load(lnk.c_str(), STGM_READWRITE))) exists = false;
    if (!exists) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(target.parent_path().c_str());
        link->SetIconLocation(target.c_str(), 0);
        link->SetDescription(L"MediaDuet");
    } else {
        // Only adopt shortcuts to a MediaDuet host (e.g. the installer's), never others.
        wchar_t cur[MAX_PATH * 2] = {};
        link->GetPath(cur, static_cast<int>(std::size(cur)), nullptr, 0);
        if (_wcsicmp(fs::path(cur).filename().c_str(), L"MediaDuet.exe") != 0) return false;
    }
    auto store = link.as<IPropertyStore>();
    PROPVARIANT pv;
    if (FAILED(InitPropVariantFromString(kHostAumid, &pv))) return false;
    PROPVARIANT oldPv;
    PropVariantInit(&oldPv);
    bool same = SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &oldPv)) && oldPv.vt == VT_LPWSTR &&
                oldPv.pwszVal && wcscmp(oldPv.pwszVal, kHostAumid) == 0;
    PropVariantClear(&oldPv);
    if (same && exists) {
        PropVariantClear(&pv);
        return true;
    }
    HRESULT hr = store->SetValue(PKEY_AppUserModel_ID, pv);
    PropVariantClear(&pv);
    if (FAILED(hr) || FAILED(store->Commit())) return false;
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    return SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

void writeRegistry(const std::wstring& displayName, const fs::path& icon) {
    std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + kHostAumid;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return;
    auto setStr = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    setStr(L"DisplayName", displayName);
    if (fs::exists(icon)) setStr(L"IconUri", icon.wstring());
    setStr(L"IconBackgroundColor", L"FF5856D6");
    RegCloseKey(h);
}

} // namespace

void ensureAppIdentity(const std::wstring& displayName) {
    fs::path exe = exePath();
    fs::path programs = startMenuPrograms();
    if (!programs.empty()) {
        // Prefer the installer's shortcut; a portable copy gets its own.
        bool ok = writeShortcut(programs / L"MediaDuet" / L"MediaDuet.lnk", exe);
        if (!ok) ok = writeShortcut(programs / L"MediaDuet.lnk", exe);
        if (!ok) log::warn("identity", "could not register the Start-menu shortcut for the app id");
    }
    // Keep the icon in a stable per-user place so the registration survives moves.
    PWSTR local = nullptr;
    fs::path icon;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
        fs::path dir = fs::path(local) / L"MediaDuet";
        CoTaskMemFree(local);
        std::error_code ec;
        fs::create_directories(dir, ec);
        icon = dir / L"MediaDuet.png";
        fs::path src = exe.parent_path() / L"MediaDuet.png";
        if (fs::exists(src, ec)) fs::copy_file(src, icon, fs::copy_options::overwrite_existing, ec);
    }
    writeRegistry(displayName, icon);
}

std::wstring resolvedAppName() {
    // Desktop apps resolve through the shell's AppsFolder (what the media flyout uses).
    com_ptr<IShellItem> item;
    std::wstring path = std::wstring(L"shell:AppsFolder\\") + kHostAumid;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())))) return {};
    PWSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
        out = name;
        CoTaskMemFree(name);
    }
    return out;
}

} // namespace md::platform
