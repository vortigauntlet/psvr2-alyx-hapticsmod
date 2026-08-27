#pragma once

#include <map>
#include <string>
#include <vector>

namespace psvr2 {

struct Config {
    // Which game this session is for: "alyx" or "hl2vr".
    //
    // Selected with --game, or by `game=` in the config file. Defaults to Alyx
    // so every existing install and shortcut keeps behaving exactly as it did
    // before the second integration existed.
    std::string game = "alyx";

    // Paths. All are auto-detected when left blank.
    std::string hlaPath;
    std::string hl2vrPath;
    std::wstring toolkitDll;

    // Levels
    float master = 1.0f;        // PCM master
    float triggerMaster = 1.0f; // adaptive trigger strength
    std::map<std::string, float> eventGain;  // gain.<EVENT>
    std::map<std::string, float> weaponGain; // weapon.<NAME>

    // Behaviour
    std::string handedness = "right";
    bool adaptiveTriggers = true;
    bool physics = true;
    // Door interaction is inferred by polling, not reported by a game event,
    // so it is the most heuristic thing in the build. Off switch kept close to
    // hand in case it misfires on doors that swing by themselves.
    bool doors = true;

    // Headset rumble. OFF by default and deliberately so: on PC the PSVR2's
    // headset motor only responds after the headset has been jailbroken, and
    // most people have not done that. Enabling it for them would mean a
    // feature that silently does nothing, which is worse than an honest
    // opt-in.
    //
    // Nothing breaks when it is on and the headset is not jailbroken - the
    // value is simply ignored by hardware that is not listening. See hmd.h.
    bool hmd = false;

    // Minimum damage before a hit reaches the HEADSET, as opposed to the
    // controllers. Deliberately higher than minDamage: a graze that is worth a
    // flinch in the hands is not worth a jolt to the face.
    float hmdMinDamage = 12.0f;
    bool debug = false;
    int pollMs = 12;

    // Physics tuning
    float minImpactImpulse = 150.0f;
    int impactCooldownMs = 80;
    // Damage below this is ignored: chip damage should not interrupt whatever
    // the player's hands are doing.
    float minDamage = 4.0f;

    // Lines that parsed but are no longer meaningful. Reported, never fatal,
    // so an old config file still launches.
    std::vector<std::string> obsoleteKeys;
    std::vector<std::string> warnings;

    // Tolerates UTF-8 BOM (with or without), UTF-16 LE/BE BOM (rejected with a
    // clear message rather than silent garbage), CRLF, comments and blank
    // lines. Returns false only when the file cannot be opened.
    bool Load(const std::string& path, std::string& error);

    // Fills in anything still blank by probing the machine. Appends to
    // warnings when something could not be found.
    void AutoDetect();

    // Verifies the resolved paths actually exist. Returns false with a
    // human-readable explanation rather than a code.
    bool Validate(std::string& error) const;

    float GainFor(const std::string& event) const;
    float GainForWeapon(const std::string& weapon) const;

    std::string consoleLogPath() const;
    std::string addonInstallPath() const;

    // Half-Life 2 VR writes its console.log inside the mod's own game
    // directory, not beside the executable.
    std::string hl2vrConsoleLogPath() const;

    // The game directory for whichever game is selected, for messages.
    std::string gamePath() const;
    bool isHl2vr() const { return game == "hl2vr"; }
};

// Steam / Half-Life: Alyx discovery, used by AutoDetect and the installer.
std::string FindSteamRoot();
std::string FindHalfLifeAlyx();
std::string FindHalfLife2VR();
std::wstring FindToolkitDll();

} // namespace psvr2
