#pragma once

#include <string>

namespace md::platform {

inline constexpr wchar_t kHostAumid[] = L"MediaDuet.Host";

// Makes Windows resolve our explicit AppUserModelID to a name and icon (media
// flyout / SMTC, notifications) for an unpackaged app: a Start-menu shortcut
// carrying System.AppUserModel.ID, plus HKCU\Software\Classes\AppUserModelId.
// Idempotent and cheap; call once at start-up.
void ensureAppIdentity(const std::wstring& displayName);

// True if Windows resolves kHostAumid to an app (diagnostics).
std::wstring resolvedAppName();

} // namespace md::platform
