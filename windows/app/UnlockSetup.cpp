#include "UnlockSetup.h"

#include "md/unlock/Component.h"
#include "md/util/Log.h"
#include "md/util/Text.h"
#include "BootStore.h"
#include "IpcServer.h"
#include "Stores.h"
#include <shellapi.h>

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace md::app {

namespace fs = std::filesystem;
namespace comp = unlock::component;

namespace {

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

fs::path installDir() {
    PWSTR pf = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf))) out = fs::path(pf) / comp::kInstallSubdir;
    CoTaskMemFree(pf);
    return out;
}

bool validSid(const std::wstring& s) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(s.c_str(), &sid)) return false;
    LocalFree(sid);
    return true;
}

bool setString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

std::wstring getString(const std::wstring& key, const wchar_t* name) {
    wchar_t buf[512];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &cb) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

bool keyExists(const std::wstring& key) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::optional<DWORD> subkeyCount(const std::wstring& key) {
    HKEY k = nullptr;
    auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k);
    if (status == ERROR_FILE_NOT_FOUND) return 0;
    if (status != ERROR_SUCCESS) return std::nullopt;
    DWORD n = 0;
    status = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &n, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return status == ERROR_SUCCESS ? std::optional<DWORD>(n) : std::nullopt;
}

// LogonUI keeps a loaded DLL until it exits: move the old one aside and delete it at reboot.
bool replaceFile(const fs::path& src, const fs::path& dst) {
    DWORD attrs = GetFileAttributesW(dst.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) return false;
    fs::path old = dst;
    old += L".old-" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

void removeFile(const fs::path& p) {
    if (DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
    MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// An existing folder might have been created with permissive explicit ACLs. LogonUI
// loads this binary as SYSTEM, so secure it even on upgrades and reject redirections.
bool securePath(const fs::path& path, bool directory) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const wchar_t* sddl = directory ? L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)"
                                    : L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    bool ok = SetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                              PROTECTED_DACL_SECURITY_INFORMATION, sd) != FALSE;
    LocalFree(sd); return ok;
}

bool stopService(SC_HANDLE service) {
    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int i = 0; i < 150; ++i) {
        if (!QueryServiceStatus(service, &status)) return false;
        if (status.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(100);
    }
    return false;
}
bool installService(const fs::path& directory) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, comp::kServiceName, SERVICE_ALL_ACCESS);
    std::wstring command = L"\"" + (directory / comp::kServiceExe).wstring() + L"\"";
    if (!service) service = CreateServiceW(scm, comp::kServiceName, L"MediaDuet phone sign-in", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr,
        L"Tcpip\0Dnscache\0", L"LocalSystem", nullptr);
    bool ok = service && ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, L"Tcpip\0Dnscache\0", L"LocalSystem", nullptr, nullptr);
    if (ok) {
        SC_ACTION actions[] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_RESTART, 30000}};
        SERVICE_FAILURE_ACTIONSW recovery{86400, nullptr, nullptr, 3, actions};
        SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
        SERVICE_DELAYED_AUTO_START_INFO delayed{FALSE};
        ok = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &recovery) &&
             ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag) &&
             ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
        if (ok && !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) ok = false;
        SERVICE_STATUS state{};
        for (int i = 0; ok && i < 200; ++i) {
            if (!QueryServiceStatus(service, &state) || state.dwCurrentState == SERVICE_STOPPED) { ok = false; break; }
            if (state.dwCurrentState == SERVICE_RUNNING) break;
            Sleep(100);
        }
        ok = ok && state.dwCurrentState == SERVICE_RUNNING;
    }
    if (service) CloseServiceHandle(service); CloseServiceHandle(scm); return ok;
}
bool serviceConfigured(const fs::path& directory) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, comp::kServiceName, SERVICE_QUERY_CONFIG);
    DWORD size = 0;
    if (service) QueryServiceConfigW(service, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data());
    auto expected = L"\"" + (directory / comp::kServiceExe).wstring() + L"\"";
    bool ok = service && QueryServiceConfigW(service, config, size, &size) &&
        config->dwStartType == SERVICE_AUTO_START && config->dwServiceType == SERVICE_WIN32_OWN_PROCESS &&
        _wcsicmp(config->lpBinaryPathName, expected.c_str()) == 0 && _wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0;
    if (service) CloseServiceHandle(service); CloseServiceHandle(scm); return ok;
}
int install(const std::wstring& sid, const std::wstring& payload) {
    fs::path src = exeDir() / comp::kDllName;
    fs::path dir = installDir();
    if (dir.empty() || !fs::exists(src) || !fs::exists(exeDir() / comp::kServiceExe)) return 3;
    std::optional<unlock::BootRecord> record;
    if (!payload.empty()) {
        record = unlock::readBootRecord(payload);
        if (!record || record->userSid != text::toUtf8(sid)) return 2;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !securePath(dir, true)) return 4;
    fs::path dst = dir / comp::kDllName;
    DWORD attrs = GetFileAttributesW(dst.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return 4;
    if (!replaceFile(src, dst) || !securePath(dst, false)) return 4;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 5;
    SC_HANDLE service = OpenServiceW(scm, comp::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool stopped = !service || stopService(service);
    if (service) CloseServiceHandle(service); CloseServiceHandle(scm);
    if (!stopped || !replaceFile(exeDir() / comp::kServiceExe, dir / comp::kServiceExe) ||
        !securePath(dir / comp::kServiceExe, false)) return 4;
    if (record && !unlock::writeBootRecord(*record)) return 5;

    std::wstring clsid = comp::kClsidKey;
    bool ok = setString(HKEY_LOCAL_MACHINE, clsid, nullptr, L"MediaDuet unlock") &&
              setString(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", nullptr, dst.wstring()) &&
              setString(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              setString(HKEY_LOCAL_MACHINE, comp::kProviderKey, nullptr, L"MediaDuet") &&
              setString(HKEY_LOCAL_MACHINE, comp::kStateKey, L"Version", MD_VERSION_W) &&
              setString(HKEY_LOCAL_MACHINE, comp::kStateKey, L"ComponentRevision", comp::kComponentRevision) &&
              setString(HKEY_LOCAL_MACHINE, std::wstring(comp::kUsersKey) + L"\\" + sid, nullptr, L"");
    if (ok && record) ok = setString(HKEY_LOCAL_MACHINE, std::wstring(comp::kUsersKey) + L"\\" + sid, L"BootVersion", MD_VERSION_W);
    if (ok && record) {
        HKEY key = nullptr; DWORD revoked = 0;
        auto path = std::wstring(comp::kUsersKey) + L"\\" + sid;
        ok = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS;
        if (ok) {
            ok = RegSetValueExW(key, L"Revoked", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&revoked), sizeof(revoked)) == ERROR_SUCCESS;
            RegCloseKey(key);
        }
    }
    return ok && installService(dir) ? 0 : 5;
}

int remove(const std::wstring& sid) {
    // This is also used by uninstall. Revoke boot access even when the normal
    // tray host was never started in this session.
    auto data = unlock::bootDataDir();
    if (!data.empty() && fs::exists(data) && !unlock::eraseBootRecord(text::toUtf8(sid))) return 5;
    auto status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(comp::kUsersKey) + L"\\" + sid).c_str(), KEY_WOW64_64KEY, 0);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return 5;
    auto users = subkeyCount(comp::kUsersKey);
    if (!users) return 5;
    if (*users > 0) return 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE service = OpenServiceW(scm, comp::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
        if (service) {
            if (!stopService(service) || !DeleteService(service)) { CloseServiceHandle(service); CloseServiceHandle(scm); return 5; }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(scm);
    }
    // Nobody uses it any more: unregister first, so LogonUI never loads a half-removed provider.
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, comp::kProviderKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, comp::kProviderKey, KEY_WOW64_64KEY, 0);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, comp::kClsidKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, comp::kClsidKey, KEY_WOW64_64KEY, 0);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, comp::kStateKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, comp::kStateKey, KEY_WOW64_64KEY, 0);
    fs::path dir = installDir();
    if (!dir.empty()) {
        removeFile(dir / comp::kDllName);
        removeFile(dir / comp::kServiceExe);
        std::error_code ec;
        fs::remove(dir, ec); // only if empty
    }
    return 0;
}

} // namespace

int runUnlockComponent(const std::wstring& action, const std::wstring& userSid, const std::wstring& payload) {
    if (!validSid(userSid)) return 2;
    try {
        if (action == L"install") return install(userSid, payload);
        if (action == L"remove") return remove(userSid);
    } catch (...) { return 6; }
    return 2;
}

int removeUnlockForCurrentUser() {
    auto sid = currentUserSid();
    auto userKey = std::wstring(comp::kUsersKey) + L"\\" + sid;
    auto reply = unlock::bootControl(sid, {{"type", "remove"}});
    bool revoked = reply && reply->value("ok", false) && !reply->value("enabled", true);
    if (!keyExists(userKey)) {
        auto file = platform::appDataDir() / L"unlock.json";
        return DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 5;
    }
    wchar_t exe[32768]; GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    auto args = L"--unlock-component remove " + sid;
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas"; info.lpFile = exe; info.lpParameters = args.c_str(); info.nShow = SW_HIDE;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DWORD code = 1;
    if (ShellExecuteExW(&info)) {
        WaitForSingleObject(info.hProcess, INFINITE); GetExitCodeProcess(info.hProcess, &code); CloseHandle(info.hProcess);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!revoked && code != 0) return 5;
    auto file = platform::appDataDir() / L"unlock.json";
    if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return 5;
    return 0;
}

ComponentState unlockComponentState(const std::wstring& userSid) {
    fs::path dll = installDir() / comp::kDllName;
    std::wstring registered = getString(std::wstring(comp::kClsidKey) + L"\\InprocServer32", nullptr);
    std::error_code ec;
    if (_wcsicmp(registered.c_str(), dll.c_str()) != 0 || !keyExists(comp::kProviderKey) || !fs::exists(dll, ec) ||
        !keyExists(std::wstring(comp::kUsersKey) + L"\\" + userSid))
        return ComponentState::Missing;
    return getString(comp::kStateKey, L"Version") == MD_VERSION_W &&
        getString(comp::kStateKey, L"ComponentRevision") == comp::kComponentRevision && serviceConfigured(installDir()) &&
        getString(std::wstring(comp::kUsersKey) + L"\\" + userSid, L"BootVersion") == MD_VERSION_W
        ? ComponentState::Ok : ComponentState::Outdated;
}

} // namespace md::app
