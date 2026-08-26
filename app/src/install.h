// Game-side addon deployment and game launching.
//
// The addon installs to game\hlvr_addons\psvr2_haptics\ and touches no Valve
// file. Half-Life: Alyx automatically executes scripts/vscripts/game/gameinit.lua
// from the addon search path for every map, including the base campaign, which
// is what makes a no-injection, no-manifest-edit install possible.
//
// Consequences that matter:
//   * Uninstalling is deleting one folder.
//   * A Half-Life: Alyx update cannot overwrite the mod, because the mod does
//     not live in Valve's directories.
//   * The previous approach (appending `script_reload_code psvr2_haptics.lua`
//     to game\hlvr\cfg\skill_manifest.cfg and dropping a script into Valve's
//     own vscripts folder) is undone by any game update and leaves edits behind.

#pragma once

#include <string>
#include <vector>

namespace psvr2 {

struct InstallReport {
    bool ok = false;
    std::vector<std::string> written;
    std::vector<std::string> unchanged;
    std::string error;
};

// Writes the embedded addon into <hlaPath>\game\hlvr_addons\psvr2_haptics.
// Idempotent: files whose contents already match are left alone.
InstallReport InstallAddon(const std::string& hlaPath);

// Removes the addon folder. Also reports whether a legacy install from the
// older manifest-editing approach is still present.
InstallReport UninstallAddon(const std::string& hlaPath);

// True when Valve's skill_manifest.cfg still carries our old script_reload_code
// line, or the old script is still in Valve's vscripts folder.
bool HasLegacyInstall(const std::string& hlaPath, std::vector<std::string>& found);

// Removes the legacy manifest line and script.
bool RemoveLegacyInstall(const std::string& hlaPath, std::string& error);

// Truncates console.log so the tailer never replays a previous session's
// events on startup.
void ResetConsoleLog(const std::string& hlaPath);

// Launches Half-Life: Alyx through Steam with -condebug so console.log is
// written. Returns false with an explanation if Steam could not be invoked.
bool LaunchGame(std::string& error);

// True if an hlvr process appears to be running.
bool GameIsRunning();

} // namespace psvr2
