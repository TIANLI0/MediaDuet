#include "BootStore.h"
#include "md/unlock/Component.h"
#include "md/crypto/Crypto.h"
#include "md/util/Text.h"
#include <dpapi.h>
#include <sddl.h>
#include <shlobj.h>

namespace md::unlock {
namespace fs = std::filesystem;
namespace {
const char entropy[] = "MediaDuet-unlock-boot-v2";
bool ordinaryPath(const fs::path& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool writeBlob(const fs::path& path, ByteView bytes, const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
std::optional<Bytes> encryptRecord(const BootRecord& r) {
    auto j = bootRecordJson(r);
    auto s = j.dump();
    auto bytes = protectBootBytes(view(s));
    SecureZeroMemory(s.data(), s.size());
    auto& key = j["peer"]["key"].get_ref<std::string&>(); SecureZeroMemory(key.data(), key.size());
    return bytes;
}
}
std::optional<Bytes> protectBootBytes(ByteView bytes) {
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())}, out{};
    DATA_BLOB ent{sizeof(entropy)-1, reinterpret_cast<BYTE*>(const_cast<char*>(entropy))};
    if (!CryptProtectData(&in, L"MediaDuet boot unlock", &ent, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    Bytes result(out.pbData, out.pbData + out.cbData); LocalFree(out.pbData); return result;
}
std::optional<Bytes> unprotectBootBytes(ByteView bytes) {
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())}, out{};
    DATA_BLOB ent{sizeof(entropy)-1, reinterpret_cast<BYTE*>(const_cast<char*>(entropy))};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    Bytes result(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
}
fs::path bootDataDir() {
    PWSTR p = nullptr; fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p))) result = fs::path(p) / L"MediaDuet" / L"Unlock";
    CoTaskMemFree(p); return result;
}
bool secureBootDirectory(const fs::path& path) {
    if (path.empty()) return false;
    auto parent = path.parent_path();
    if (!ordinaryPath(parent)) {
        std::error_code ec; fs::create_directory(parent, ec);
        if (ec || !ordinaryPath(parent)) return false;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                                                              SDDL_REVISION_1, &sd, nullptr)) return false;
    // Secure the parent too: a writable ProgramData\MediaDuet would let its owner
    // replace the protected child directory through parent DELETE_CHILD rights.
    bool ok = SetFileSecurityW(parent.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                              PROTECTED_DACL_SECURITY_INFORMATION, sd) != FALSE;
    std::error_code ec;
    if (ok) fs::create_directory(path, ec);
    ok = ok && !ec && ordinaryPath(path) && SetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd); return ok;
}
bool writeBootRecord(const BootRecord& r) {
    if (!parseBootRecord(bootRecordJson(r))) return false;
    auto dir = bootDataDir();
    if (!secureBootDirectory(dir)) return false;
    auto bytes = encryptRecord(r); if (!bytes) return false;
    auto dst = dir / (text::toWide(r.userSid) + L".bin");
    auto temp = dir / (text::toWide(crypto::randomHex(16)) + L".tmp");
    if (!writeBlob(temp, *bytes, L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")) return false;
    bool ok = MoveFileExW(temp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
std::optional<BootRecord> readBootRecord(const fs::path& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    bool valid = GetFileInformationByHandle(f, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                 GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= 65536;
    Bytes bytes(valid ? static_cast<size_t>(size.QuadPart) : 0); DWORD n = 0;
    bool read = valid && ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size();
    CloseHandle(f); if (!read) return {};
    auto plain = unprotectBootBytes(bytes); if (!plain) return {};
    auto j = nlohmann::json::parse(plain->begin(), plain->end(), nullptr, false); secureZero(*plain);
    auto record = parseBootRecord(j);
    if (j.is_object() && j.contains("peer") && j["peer"].is_object() && j["peer"].contains("key") && j["peer"]["key"].is_string()) {
        auto& key = j["peer"]["key"].get_ref<std::string&>(); SecureZeroMemory(key.data(), key.size());
    }
    return record;
}
bool eraseBootRecord(const std::string& sid) {
    if (!safeSid(sid)) return false;
    auto dir = bootDataDir(); if (dir.empty() || !ordinaryPath(dir) || !ordinaryPath(dir.parent_path())) return false;
    auto file = dir / (text::toWide(sid) + L".bin");
    if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    HKEY key = nullptr;
    auto path = std::wstring(component::kUsersKey) + L"\\" + text::toWide(sid);
    auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) return false;
    DWORD revoked = 1;
    status = RegSetValueExW(key, L"Revoked", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&revoked), sizeof(revoked));
    RegCloseKey(key); return status == ERROR_SUCCESS;
}
bool bootEnrollmentRevoked(const std::wstring& sid) {
    auto path = std::wstring(component::kUsersKey) + L"\\" + sid;
    DWORD revoked = 0, size = sizeof(revoked);
    return RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"Revoked", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                       nullptr, &revoked, &size) == ERROR_SUCCESS && revoked != 0;
}
std::optional<fs::path> stageBootRecord(const BootRecord& r, const fs::path& directory) {
    if (!safeSid(r.userSid) || !ordinaryPath(directory)) return {};
    auto bytes = encryptRecord(r); if (!bytes) return {};
    auto path = directory / (L"unlock-" + text::toWide(crypto::randomHex(16)) + L".tmp");
    auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + text::toWide(r.userSid) + L")";
    if (!writeBlob(path, *bytes, sddl)) return {};
    return path;
}
bool isBootServiceProcess(ULONG pid) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, component::kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    SERVICE_STATUS_PROCESS status{}; DWORD n = 0;
    bool ok = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &n) &&
              status.dwProcessId == pid && pid != 0 && status.dwCurrentState == SERVICE_RUNNING;
    DWORD size = 0;
    if (ok) QueryServiceConfigW(service, nullptr, 0, &size);
    Bytes configBytes(size);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBytes.data());
    ok = ok && QueryServiceConfigW(service, config, size, &size) &&
         config->dwServiceType == SERVICE_WIN32_OWN_PROCESS && _wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0;
    if (service) CloseServiceHandle(service); CloseServiceHandle(scm); return ok;
}
bool isBootServicePipe(HANDLE pipe) {
    // SCM authenticates both the running PID and its LocalSystem account. A
    // normal control client needs no access to another account's process token.
    ULONG pid = 0; return GetNamedPipeServerProcessId(pipe, &pid) && isBootServiceProcess(pid);
}
std::optional<nlohmann::json> bootControl(const std::wstring& sid, const nlohmann::json& request) {
    if (!safeSid(text::toUtf8(sid))) return {};
    std::wstring name = std::wstring(component::kControlPipePrefix) + sid;
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return {};
    std::optional<nlohmann::json> result;
    if (isBootServicePipe(pipe)) {
        auto io = [&](bool write, char* data, DWORD size, DWORD& n) {
            OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = write ? WriteFile(pipe, data, size, &n, &ov) : ReadFile(pipe, data, size, &n, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
                ok = GetOverlappedResult(pipe, &ov, &n, TRUE);
            }
            CloseHandle(ov.hEvent); return ok != FALSE;
        };
        auto line = request.dump() + "\n"; DWORD n = 0;
        if (io(true, line.data(), static_cast<DWORD>(line.size()), n) && n == line.size()) {
            std::string response; char data[2048];
            while (response.size() < 8192 && io(false, data, sizeof(data), n) && n) {
                response.append(data, n);
                auto pos = response.find('\n');
                if (pos != std::string::npos) {
                    auto j = nlohmann::json::parse(response.substr(0, pos), nullptr, false);
                    if (j.is_object() && j.value("sid", std::string()) == text::toUtf8(sid)) result = std::move(j);
                    break;
                }
            }
        }
    }
    CloseHandle(pipe); return result;
}
}
