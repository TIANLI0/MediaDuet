#include "Autostart.h"

#include <windows.h>

#include <string>

namespace md::app {

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kValue = L"MediaDuet";

std::wstring command() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return L"\"" + std::wstring(path, n) + L"\" --background";
}
} // namespace

bool setAutostart(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS st;
    if (enabled) {
        auto cmd = command();
        st = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool autostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return command() == buf;
}

} // namespace md::app
