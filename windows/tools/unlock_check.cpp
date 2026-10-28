// Read-only enrollment/service probe. Never requests a password or phone approval,
// controls a service, installs a provider, locks a session, or restarts Windows.
#include "BootStore.h"
#include "IpcServer.h"
#include "md/util/Text.h"
#include <shlobj.h>
#include <fstream>
#include <iostream>

int main() {
    using nlohmann::json;
    auto sid = md::app::currentUserSid();
    json result{{"controlAuthenticated", false}, {"localEnrollment", false}};
    auto status = md::unlock::bootControl(sid, {{"type", "status"}});
    result["controlAuthenticated"] = status.has_value();
    if (status) {
        result["enabled"] = status->value("enabled", false);
        result["stale"] = status->value("stale", false);
        result["lastUnlockAt"] = status->value("lastUnlockAt", int64_t(0));
    }
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        std::ifstream in(std::filesystem::path(local) / L"MediaDuet" / L"unlock.json");
        auto enrollment = json::parse(in, nullptr, false);
        if (enrollment.is_object()) {
            result["localEnrollment"] = true;
            result["sameEnrollment"] = status && status->value("enrollment", std::string()) == enrollment.value("enrollment", std::string());
            result["accountBound"] = enrollment.value("sid", std::string()) == md::text::toUtf8(sid);
            result["localStale"] = enrollment.value("stale", false);
        }
        CoTaskMemFree(local);
    }
    std::cout << result.dump(2) << '\n';
    return status && result.value("enabled", false) && result.value("sameEnrollment", false) &&
           result.value("accountBound", false) && !result.value("stale", true) ? 0 : 1;
}
