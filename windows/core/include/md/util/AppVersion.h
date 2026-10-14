#pragma once

// App versions of the two ends (DeviceInfo.app, spec §5) and the mismatch rule shown
// to the user (Settings > Devices / About, the player's InfoBar).

#include <optional>
#include <string>
#include <string_view>

namespace md::version {

struct AppVersion {
    int major = 0, minor = 0, patch = 0;
    bool operator==(const AppVersion&) const = default;
};

// "major.minor.patch" (a missing patch counts as 0); a leading "v" and any suffix
// after the numbers ("-beta.2", "+build5", " (debug)") are ignored. nullopt when the
// text does not start with at least "major.minor".
std::optional<AppVersion> parse(std::string_view s);

enum class Match {
    Unknown,      // one side's version is empty or unreadable: show "unknown", no warning
    Equal,        // nothing to show
    PatchDiffers, // informational only
    Mismatch,     // major or minor differ: warning (features may not work)
};

Match compare(std::string_view self, std::string_view peer);
const char* toString(Match m); // unknown | equal | patch | mismatch

} // namespace md::version
