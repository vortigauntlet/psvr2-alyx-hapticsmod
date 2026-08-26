#include "config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace psvr2 {
namespace {

std::string Trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool Truthy(const std::string& v) {
    const std::string s = Upper(Trim(v));
    return s == "1" || s == "TRUE" || s == "ON" || s == "YES";
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

bool PathExists(const std::string& p) {
    if (p.empty()) return false;
    return GetFileAttributesW(Widen(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool PathExistsW(const std::wstring& p) {
    if (p.empty()) return false;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY key{};
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    }
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const auto rc = RegQueryValueExW(key, value, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};

    std::wstring w(buf);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, n)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

} // namespace

std::string FindSteamRoot() {
    struct Probe { HKEY root; const wchar_t* subkey; const wchar_t* value; };
    static const Probe probes[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"},
    };
    for (const auto& p : probes) {
        const std::string found = ReadRegistryString(p.root, p.subkey, p.value);
        if (PathExists(found)) return found;
    }
    for (const char* guess : {R"(C:\Program Files (x86)\Steam)", R"(C:\Program Files\Steam)"}) {
        if (PathExists(guess)) return guess;
    }
    return {};
}

std::string FindHalfLifeAlyx() {
    const std::string steam = FindSteamRoot();
    std::vector<std::string> libraries;
    if (!steam.empty()) libraries.push_back(steam);

    // Steam records every library root in libraryfolders.vdf. Rather than
    // parsing VDF properly we only need the "path" values, which are
    // unambiguous in this file.
    if (!steam.empty()) {
        std::ifstream f(steam + R"(\steamapps\libraryfolders.vdf)");
        std::string line;
        while (std::getline(f, line)) {
            const auto k = line.find("\"path\"");
            if (k == std::string::npos) continue;
            const auto q1 = line.find('"', k + 6);
            if (q1 == std::string::npos) continue;
            const auto q2 = line.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            std::string p = line.substr(q1 + 1, q2 - q1 - 1);
            // VDF escapes backslashes.
            std::string unescaped;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') { unescaped += '\\'; ++i; }
                else unescaped += p[i];
            }
            if (!unescaped.empty()) libraries.push_back(unescaped);
        }
    }

    for (const auto& lib : libraries) {
        const std::string candidate = lib + R"(\steamapps\common\Half-Life Alyx)";
        if (PathExists(candidate + R"(\game\hlvr)")) return candidate;
    }
    return {};
}

std::wstring FindToolkitDll() {
    const std::string steam = FindSteamRoot();
    std::vector<std::string> roots;
    if (!steam.empty()) roots.push_back(steam);
    roots.push_back(R"(C:\Program Files (x86)\Steam)");
    roots.push_back(R"(C:\Program Files\Steam)");

    for (const auto& r : roots) {
        const std::string p = r +
            R"(\steamapps\common\PlayStation VR2 App\SteamVR_Plug-In\bin\win64\psvr2_toolkit_capi.dll)";
        if (PathExists(p)) return Widen(p);
    }
    return {};
}

bool Config::Load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "Could not open config file: " + path;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // BOM handling, permanently. PowerShell 5.1's `Set-Content -Encoding UTF8`
    // writes a UTF-8 BOM, which previously made the very first key unparseable
    // and produced a "could not load config" error even though the key was
    // plainly visible in the file.
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    } else if (content.size() >= 2 &&
               ((static_cast<unsigned char>(content[0]) == 0xFF &&
                 static_cast<unsigned char>(content[1]) == 0xFE) ||
                (static_cast<unsigned char>(content[0]) == 0xFE &&
                 static_cast<unsigned char>(content[1]) == 0xFF))) {
        error = "Config file is UTF-16. Save it as UTF-8 (or plain ASCII) and try again: " + path;
        return false;
    }

    std::istringstream stream(content);
    std::string line;
    int lineNo = 0;
    while (std::getline(stream, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            warnings.push_back("line " + std::to_string(lineNo) + ": no '=', ignored");
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        auto num = [&](float fallback) {
            try { return std::stof(value); }
            catch (...) {
                warnings.push_back("line " + std::to_string(lineNo) + ": '" + value +
                                   "' is not a number, using " + std::to_string(fallback));
                return fallback;
            }
        };

        if (key == "hla_path") hlaPath = value;
        else if (key == "toolkit_dll") toolkitDll = Widen(value);
        else if (key == "master") master = num(1.0f);
        else if (key == "trigger_master") triggerMaster = num(1.0f);
        else if (key == "handedness") handedness = (Upper(value) == "LEFT") ? "left" : "right";
        else if (key == "adaptive_triggers") adaptiveTriggers = Truthy(value);
        else if (key == "physics") physics = Truthy(value);
        else if (key == "doors") doors = Truthy(value);
        else if (key == "debug") debug = Truthy(value);
        else if (key == "poll_ms") pollMs = std::max(4, static_cast<int>(num(12.0f)));
        else if (key == "min_impact_impulse") minImpactImpulse = num(150.0f);
        else if (key == "min_damage") minDamage = num(4.0f);
        else if (key == "impact_cooldown_ms") impactCooldownMs = static_cast<int>(num(80.0f));
        else if (key.rfind("gain.", 0) == 0) eventGain[Upper(key.substr(5))] = num(1.0f);
        else if (key.rfind("weapon.", 0) == 0) weaponGain[Upper(key.substr(7))] = num(1.0f);
        // Retired keys kept parseable so an old config still starts.
        else if (key == "native_hooks" || key == "native_pipe" || key == "native" ||
                 key == "physics_enabled" || key == "physics_sample_hz" ||
                 key == "physics_min_impact_impulse" || key == "physics_collision_cooldown_ms") {
            obsoleteKeys.push_back(key);
            if (key == "physics_enabled") physics = Truthy(value);
            if (key == "physics_min_impact_impulse") minImpactImpulse = num(150.0f);
            if (key == "physics_collision_cooldown_ms") impactCooldownMs = static_cast<int>(num(80.0f));
        } else {
            warnings.push_back("line " + std::to_string(lineNo) + ": unknown key '" + key + "'");
        }
    }
    return true;
}

void Config::AutoDetect() {
    if (hlaPath.empty() || !PathExists(hlaPath + R"(\game\hlvr)")) {
        const std::string found = FindHalfLifeAlyx();
        if (!found.empty()) {
            if (!hlaPath.empty()) {
                warnings.push_back("configured hla_path did not look like a Half-Life: Alyx "
                                   "install; using detected path instead");
            }
            hlaPath = found;
        }
    }
    if (toolkitDll.empty() || !PathExistsW(toolkitDll)) {
        const std::wstring found = FindToolkitDll();
        if (!found.empty()) toolkitDll = found;
    }
}

bool Config::Validate(std::string& error) const {
    if (hlaPath.empty()) {
        error = "Half-Life: Alyx was not found automatically.\n"
                "  Set hla_path= in the config, e.g.\n"
                R"(  hla_path=C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx)";
        return false;
    }
    if (!PathExists(hlaPath)) {
        error = "hla_path does not exist: " + hlaPath;
        return false;
    }
    if (!PathExists(hlaPath + R"(\game\hlvr)")) {
        error = "hla_path exists but has no game\\hlvr folder, so it is not a "
                "Half-Life: Alyx install: " + hlaPath;
        return false;
    }
    return true;
}

float Config::GainFor(const std::string& event) const {
    const auto it = eventGain.find(event);
    return it == eventGain.end() ? 1.0f : it->second;
}

float Config::GainForWeapon(const std::string& weapon) const {
    const auto it = weaponGain.find(weapon);
    return it == weaponGain.end() ? 1.0f : it->second;
}

std::string Config::consoleLogPath() const {
    return hlaPath + R"(\game\hlvr\console.log)";
}

std::string Config::addonInstallPath() const {
    return hlaPath + R"(\game\hlvr_addons\psvr2_haptics)";
}

} // namespace psvr2
