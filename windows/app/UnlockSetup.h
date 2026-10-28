#pragma once

#include <string>

namespace md::app {

// `MediaDuet.exe --unlock-component install|remove <userSid>` (docs/unlock.md), run elevated:
// installs / removes the credential provider for one user. Returns the process exit code.
int runUnlockComponent(const std::wstring& action, const std::wstring& userSid, const std::wstring& payload = {});
// Unelevated uninstall entry point: revoke first, then offer removal of the
// system component. A cancellation cannot silently leave usable boot credentials.
int removeUnlockForCurrentUser();

// Read-only checks for the (unelevated) host.
enum class ComponentState { Ok, Missing, Outdated };
ComponentState unlockComponentState(const std::wstring& userSid);

} // namespace md::app
