#pragma once

// Names shared by the host, its elevated component setup and the credential provider
// (docs/unlock.md). Header-only: the credential provider DLL does not link md_core.

namespace md::unlock::component {

inline constexpr const wchar_t* kClsid = L"{91835FE0-2B7C-453B-926F-B67713F7DF99}";
inline constexpr const wchar_t* kDllName = L"MediaDuetUnlock.dll";
// Under %ProgramFiles% (administrators-only), never the per-user install folder.
inline constexpr const wchar_t* kInstallSubdir = L"MediaDuet";
inline constexpr const wchar_t* kClsidKey = L"SOFTWARE\\Classes\\CLSID\\{91835FE0-2B7C-453B-926F-B67713F7DF99}";
inline constexpr const wchar_t* kProviderKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{91835FE0-2B7C-453B-926F-B67713F7DF99}";
// Version (REG_SZ) and Users\<sid> (one subkey per user who enabled it).
inline constexpr const wchar_t* kStateKey = L"SOFTWARE\\MediaDuet\\Unlock";
inline constexpr const wchar_t* kUsersKey = L"SOFTWARE\\MediaDuet\\Unlock\\Users";
// + decimal session id.
inline constexpr const wchar_t* kPipePrefix = L"\\\\.\\pipe\\MediaDuet.Unlock.";
inline constexpr int kPipeVersion = 1;
// Detect credential/service fixes shipped within the same application version.
inline constexpr const wchar_t* kComponentRevision = L"3";
inline constexpr const wchar_t* kServiceName = L"MediaDuetUnlock";
inline constexpr const wchar_t* kServiceExe = L"MediaDuetUnlockService.exe";
inline constexpr const wchar_t* kBootPipePrefix = L"\\\\.\\pipe\\MediaDuet.Unlock.Boot.";
inline constexpr const wchar_t* kControlPipePrefix = L"\\\\.\\pipe\\MediaDuet.Unlock.Control.";

} // namespace md::unlock::component
