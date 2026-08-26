#include "install.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "lua_embed.h"

namespace psvr2 {
namespace {

constexpr const char* kAppId = "546560"; // Half-Life: Alyx

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

bool Exists(const std::string& p) {
    return GetFileAttributesW(Widen(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string AddonRoot(const std::string& hlaPath) {
    return hlaPath + R"(\game\hlvr_addons\psvr2_haptics)";
}

// Creates every missing component of a directory path.
bool MakeDirs(const std::string& path) {
    std::string acc;
    std::istringstream ss(path);
    std::string part;
    bool first = true;
    while (std::getline(ss, part, '\\')) {
        if (first) { acc = part; first = false; }
        else { acc += "\\" + part; }
        if (acc.empty() || acc.back() == ':') continue;
        if (!CreateDirectoryW(Widen(acc).c_str(), nullptr)) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool WriteAll(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

std::string ParentOf(const std::string& path) {
    const auto slash = path.find_last_of('\\');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool RemoveTree(const std::string& dir) {
    if (!Exists(dir)) return true;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(Widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::wstring nameW = fd.cFileName;
        if (nameW == L"." || nameW == L"..") continue;
        char nameA[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, nameW.c_str(), -1, nameA, sizeof(nameA), nullptr, nullptr);
        const std::string child = dir + "\\" + nameA;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(child);
        else DeleteFileW(Widen(child).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return RemoveDirectoryW(Widen(dir).c_str()) != 0;
}

std::string LegacyManifest(const std::string& hlaPath) {
    return hlaPath + R"(\game\hlvr\cfg\skill_manifest.cfg)";
}

std::string LegacyScript(const std::string& hlaPath) {
    return hlaPath + R"(\game\hlvr\scripts\vscripts\psvr2_haptics.lua)";
}

} // namespace

namespace {

// The engine source, installed at BOTH locations (see InstallAddon).
const char* EngineSource() {
    for (const auto& f : kAddonFiles) {
        if (std::string(f.relativePath).find("psvr2_haptics/core.lua") != std::string::npos) {
            return f.contents;
        }
    }
    return nullptr;
}

std::string DirectScript(const std::string& hlaPath) {
    return LegacyScript(hlaPath);
}

constexpr const char* kManifestLine = "script_reload_code psvr2_haptics.lua";

// Adds our line to skill_manifest.cfg if it is not already there. Idempotent,
// and re-applied on every run so a Half-Life: Alyx update that restores Valve's
// original file is repaired automatically the next time this is launched.
bool EnsureManifest(const std::string& hlaPath, bool& changed, std::string& error) {
    changed = false;
    const std::string manifest = LegacyManifest(hlaPath);
    std::string body = Exists(manifest) ? ReadAll(manifest) : std::string{};

    if (body.find("psvr2_haptics") != std::string::npos) return true;

    if (!body.empty() && body.back() != '\n') body += "\n";
    body += kManifestLine;
    body += "\n";

    if (!MakeDirs(ParentOf(manifest))) {
        error = "Could not create " + ParentOf(manifest);
        return false;
    }
    if (!WriteAll(manifest, body)) {
        error = "Could not write " + manifest +
                "\n  (close Half-Life: Alyx first, or this may need admin rights)";
        return false;
    }
    changed = true;
    return true;
}

} // namespace

// Installs the game-side engine by TWO routes, because only one of them
// actually works for the base campaign.
//
//   1. game\hlvr\scripts\vscripts\psvr2_haptics.lua, loaded by a
//      `script_reload_code` line in game\hlvr\cfg\skill_manifest.cfg.
//      This is the route that works, and it is what the bHaptics Alyx
//      integration uses.
//
//   2. game\hlvr_addons\psvr2_haptics\..., the tidy addon layout.
//
// Route 2 alone was tried first and does NOT work: Half-Life: Alyx only mounts
// addons that are *enabled*, and a hand-installed folder never appears in
// default_enabled_addons_list. The engine log shows it plainly - every level
// load reports `addons []`, and no script output is ever produced. Route 2 is
// still written so the same install works if the addon is ever published to the
// Workshop and enabled, but route 1 is what makes it run today.
InstallReport InstallAddon(const std::string& hlaPath) {
    InstallReport r;

    // --- route 2: addon layout (harmless, forward-looking) ---
    const std::string root = AddonRoot(hlaPath);
    for (const auto& file : kAddonFiles) {
        std::string dest = root + "\\" + file.relativePath;
        std::replace(dest.begin(), dest.end(), '/', '\\');
        if (!MakeDirs(ParentOf(dest))) {
            r.error = "Could not create folder: " + ParentOf(dest);
            return r;
        }
        const std::string want = file.contents;
        if (Exists(dest) && ReadAll(dest) == want) { r.unchanged.push_back(dest); continue; }
        if (!WriteAll(dest, want)) { r.error = "Could not write: " + dest; return r; }
        r.written.push_back(dest);
    }

    // --- route 1: the one that actually loads ---
    const char* engine = EngineSource();
    if (engine == nullptr) {
        r.error = "Internal: embedded engine source missing";
        return r;
    }
    const std::string direct = DirectScript(hlaPath);
    if (!MakeDirs(ParentOf(direct))) {
        r.error = "Could not create folder: " + ParentOf(direct);
        return r;
    }
    const std::string want = engine;
    if (Exists(direct) && ReadAll(direct) == want) {
        r.unchanged.push_back(direct);
    } else if (!WriteAll(direct, want)) {
        r.error = "Could not write: " + direct;
        return r;
    } else {
        r.written.push_back(direct);
    }

    bool manifestChanged = false;
    if (!EnsureManifest(hlaPath, manifestChanged, r.error)) return r;
    if (manifestChanged) r.written.push_back(LegacyManifest(hlaPath));

    r.ok = true;
    return r;
}

bool HasLegacyInstall(const std::string& hlaPath, std::vector<std::string>& found);
bool RemoveLegacyInstall(const std::string& hlaPath, std::string& error);

// Removes both install routes: the addon folder, and the direct script plus its
// skill_manifest.cfg entry.
InstallReport UninstallAddon(const std::string& hlaPath) {
    InstallReport r;
    const std::string root = AddonRoot(hlaPath);

    if (Exists(root)) {
        if (!RemoveTree(root)) {
            r.error = "Could not fully remove: " + root;
            return r;
        }
        r.written.push_back(root);
    }

    std::string e;
    std::vector<std::string> found;
    if (HasLegacyInstall(hlaPath, found)) {
        if (!RemoveLegacyInstall(hlaPath, e)) {
            r.error = e;
            return r;
        }
        for (auto& f : found) r.written.push_back(std::move(f));
    }

    r.ok = true;
    return r;
}

bool HasLegacyInstall(const std::string& hlaPath, std::vector<std::string>& found) {
    found.clear();
    const std::string manifest = LegacyManifest(hlaPath);
    if (Exists(manifest)) {
        const std::string body = ReadAll(manifest);
        if (body.find("psvr2_haptics") != std::string::npos) found.push_back(manifest);
    }
    if (Exists(LegacyScript(hlaPath))) found.push_back(LegacyScript(hlaPath));
    return !found.empty();
}

bool RemoveLegacyInstall(const std::string& hlaPath, std::string& error) {
    const std::string manifest = LegacyManifest(hlaPath);
    if (Exists(manifest)) {
        const std::string body = ReadAll(manifest);
        if (body.find("psvr2_haptics") != std::string::npos) {
            std::istringstream ss(body);
            std::string line;
            std::string out;
            while (std::getline(ss, line)) {
                if (line.find("psvr2_haptics") != std::string::npos) continue;
                out += line;
                if (!ss.eof()) out += "\n";
            }
            if (!WriteAll(manifest, out)) {
                error = "Could not rewrite " + manifest;
                return false;
            }
        }
    }
    if (Exists(LegacyScript(hlaPath))) {
        if (!DeleteFileW(Widen(LegacyScript(hlaPath)).c_str())) {
            error = "Could not delete " + LegacyScript(hlaPath);
            return false;
        }
    }
    return true;
}

void ResetConsoleLog(const std::string& hlaPath) {
    const std::string log = hlaPath + R"(\game\hlvr\console.log)";
    if (!Exists(log)) return;
    // Truncate rather than delete: the game keeps the handle open across runs.
    std::ofstream f(log, std::ios::binary | std::ios::trunc);
}

bool LaunchGame(std::string& error) {
    // Going through Steam preserves the user's own launch options (notably
    // -condebug) instead of overriding them.
    const std::string url = std::string("steam://rungameid/") + kAppId;
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        error = "Could not ask Steam to start Half-Life: Alyx (code " +
                std::to_string(static_cast<long>(rc)) + "). Start the game yourself.";
        return false;
    }
    return true;
}

bool GameIsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == L"hlvr.exe") { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace psvr2
