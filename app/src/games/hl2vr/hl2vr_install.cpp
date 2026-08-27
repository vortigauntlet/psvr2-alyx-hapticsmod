#include "games/hl2vr/hl2vr_install.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fstream>
#include <sstream>

namespace psvr2 {
namespace hl2vr {
namespace {

constexpr const char* kAppId = "658920"; // Half-Life 2: VR Mod
constexpr const char* kDllName = "psvr2_haptics_plugin.dll";

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

std::string ParentOf(const std::string& p) {
    const auto slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : p.substr(0, slash);
}

bool MakeDirs(const std::string& path) {
    std::string acc;
    std::istringstream ss(path);
    std::string part;
    bool first = true;
    while (std::getline(ss, part, '\\')) {
        if (!first) acc += '\\';
        acc += part;
        first = false;
        if (acc.empty() || acc.back() == ':') continue;
        CreateDirectoryW(Widen(acc).c_str(), nullptr);
    }
    return Exists(path);
}

// The mod's content directory. HL2VR ships as a Source mod whose game directory
// is "hl2", which is where addons\ is read from.
std::string AddonDir(const std::string& root) {
    return root + R"(\hl2\addons)";
}

// The plugin descriptor Source reads at startup.
//
// Format verified against Valve Developer Community documentation for server
// plugins and against the SDK's own plugin sample: a top-level "Plugin" block
// with a "file" key holding the module path relative to the game directory,
// with no extension so the engine appends the platform's own.
std::string VdfContents() {
    return
        "\"Plugin\"\r\n"
        "{\r\n"
        "\t\"file\"\t\"addons/psvr2_haptics_plugin\"\r\n"
        "}\r\n";
}

bool WriteIfChanged(const std::string& path, const std::string& contents,
                    Hl2vrInstallReport& report) {
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
        std::stringstream ss;
        ss << existing.rdbuf();
        if (ss.str() == contents) {
            report.unchanged.push_back(path);
            return true;
        }
    }
    existing.close();
    if (!MakeDirs(ParentOf(path))) {
        report.error = "Could not create " + ParentOf(path);
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        report.error = "Could not write " + path;
        return false;
    }
    out << contents;
    report.written.push_back(path);
    return true;
}

bool CopyIfChanged(const std::string& from, const std::string& to,
                   Hl2vrInstallReport& report) {
    // Compared by content rather than by timestamp, so reinstalling over an
    // identical file reports "unchanged" instead of churning a DLL the engine
    // may currently have loaded.
    std::ifstream src(from, std::ios::binary);
    if (!src) {
        report.error = "Could not read " + from;
        return false;
    }
    std::stringstream ss;
    ss << src.rdbuf();
    const std::string data = ss.str();
    return WriteIfChanged(to, data, report);
}

} // namespace

std::vector<std::string> PluginSearchPaths(const std::string& exeDir) {
    return {
        exeDir + "\\" + kDllName,
        exeDir + "\\plugin\\" + kDllName,
        exeDir + "\\hl2vr\\" + kDllName,
    };
}

Hl2vrInstallReport InstallPlugin(const std::string& hl2vrPath) {
    Hl2vrInstallReport r;
    if (hl2vrPath.empty()) {
        r.error = "Half-Life 2 VR path is not set and could not be detected.";
        return r;
    }

    const std::string addons = AddonDir(hl2vrPath);
    if (!MakeDirs(addons)) {
        r.error = "Could not create " + addons;
        return r;
    }

    // The descriptor always goes down: it is what makes the engine look for the
    // plugin at all, and writing it before the DLL exists is harmless - Source
    // reports a missing module and carries on.
    if (!WriteIfChanged(addons + "\\psvr2_haptics.vdf", VdfContents(), r)) {
        return r;
    }

    // The DLL is a separate build against the Source SDK and will not exist on
    // a machine where it has not been built. Say so plainly instead of
    // reporting a successful install of nothing.
    wchar_t exePathW[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring w(exePathW);
    const auto slash = w.find_last_of(L"\\/");
    std::string exeDir;
    if (slash != std::wstring::npos) {
        const std::wstring dir = w.substr(0, slash);
        const int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0,
                                          nullptr, nullptr);
        if (n > 0) {
            exeDir.resize(static_cast<size_t>(n) - 1);
            WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, exeDir.data(), n,
                                nullptr, nullptr);
        }
    }

    std::string found;
    for (const auto& candidate : PluginSearchPaths(exeDir)) {
        if (Exists(candidate)) { found = candidate; break; }
    }
    if (found.empty()) {
        r.pluginMissing = true;
        r.ok = true; // the descriptor did install
        return r;
    }
    if (!CopyIfChanged(found, addons + "\\" + kDllName, r)) return r;

    r.ok = true;
    return r;
}

Hl2vrInstallReport UninstallPlugin(const std::string& hl2vrPath) {
    Hl2vrInstallReport r;
    if (hl2vrPath.empty()) {
        r.error = "Half-Life 2 VR path is not set.";
        return r;
    }
    const std::string addons = AddonDir(hl2vrPath);
    for (const char* name : {"psvr2_haptics.vdf", kDllName}) {
        const std::string path = addons + "\\" + name;
        if (!Exists(path)) continue;
        if (DeleteFileW(Widen(path).c_str())) {
            r.written.push_back(path);
        } else {
            r.error = "Could not delete " + path +
                      " (is Half-Life 2 VR still running?)";
            return r;
        }
    }
    r.ok = true;
    return r;
}

bool LaunchGame(std::string& error) {
    const std::string url = std::string("steam://rungameid/") + kAppId;
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL));
    if (rc <= 32) {
        error = "Could not launch Half-Life 2 VR through Steam (code " +
                std::to_string(rc) + ").";
        return false;
    }
    return true;
}

bool GameIsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring name(entry.szExeFile);
            for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
            // HL2VR ships its own launcher alongside the usual Source
            // executable name, so both are worth looking for.
            if (name == L"hl2vr.exe" || name == L"hl2.exe") {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

} // namespace hl2vr
} // namespace psvr2
