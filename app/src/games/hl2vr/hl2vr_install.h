// Half-Life 2 VR game-side deployment.
//
// The plugin installs to <hl2vr>\hl2\addons\ and touches no Valve or HL2VR
// file. Source loads every addons\*.vdf at startup, which is what makes a
// no-injection, no-file-edit install possible - the same property the Alyx
// addon install has, arrived at by a different route.
//
// Consequences that matter:
//   * Uninstalling is deleting two files.
//   * A Half-Life 2 VR update cannot overwrite the mod, because the mod does
//     not live in anything the updater replaces.
//
// IMPORTANT, AND THE REASON THIS FILE IS SHORTER THAN ITS ALYX COUNTERPART:
// the Alyx addon is Lua, so it is embedded in this executable and written out
// on demand. The Half-Life 2 VR plugin is a compiled 32-bit DLL that must be
// built against the Source SDK 2013 (see games/hl2vr/plugin/README.md), so
// there is nothing to embed. This installer deploys a DLL the user already has
// and reports honestly when they do not, rather than pretending to install
// something that does not exist.

#pragma once

#include <string>
#include <vector>

namespace psvr2 {
namespace hl2vr {

struct Hl2vrInstallReport {
    bool ok = false;
    std::vector<std::string> written;
    std::vector<std::string> unchanged;
    // Set when the plugin DLL itself could not be found. Not an error in the
    // usual sense: it is the expected state until the plugin has been built,
    // and the message says how to build it.
    bool pluginMissing = false;
    std::string error;
};

// Writes addons\psvr2_haptics.vdf, and copies the plugin DLL beside it when a
// built one can be found next to this executable.
Hl2vrInstallReport InstallPlugin(const std::string& hl2vrPath);

// Removes the .vdf and the DLL.
Hl2vrInstallReport UninstallPlugin(const std::string& hl2vrPath);

// Where the plugin DLL is looked for, in order. Exposed so the CLI can tell the
// user exactly which paths were tried rather than just "not found".
std::vector<std::string> PluginSearchPaths(const std::string& exeDir);

// Launches Half-Life 2 VR through Steam. appid 658920.
bool LaunchGame(std::string& error);

// True if an hl2vr process appears to be running.
bool GameIsRunning();

} // namespace hl2vr
} // namespace psvr2
