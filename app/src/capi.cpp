#include "capi.h"

#include <chrono>
#include <cstring>
#include <map>

namespace psvr2 {

const char* ResultName(int rc) {
    switch (rc) {
        case kResultOk: return "OK";
        case kResultDriverInactive: return "DRIVER_INACTIVE";
        case kResultNoSlot: return "NO_SLOT";
        case kResultTimeout: return "TIMEOUT";
        case kResultInvalidParameter: return "INVALID_PARAMETER";
        default: return "UNKNOWN";
    }
}

bool SameCommand(const TriggerCommand& a, const TriggerCommand& b) {
    // The union carries trailing padding that we never write, so a raw memcmp
    // over the whole struct is safe only because both sides are value
    // initialised. Compare the mode first to keep the common case cheap.
    if (a.mode != b.mode) return false;
    return std::memcmp(&a.data, &b.data, sizeof(TriggerCommandData)) == 0;
}

bool Capi::LoadOne(const std::wstring& candidate) {
    // psvr2_toolkit_capi.dll links against libcrossipc.dll and libusb-1.0.dll,
    // which live beside it in the SteamVR plug-in folder. A plain LoadLibraryW
    // on an absolute path does NOT search that folder for the dependencies, so
    // the load fails with ERROR_MOD_NOT_FOUND even though the file is right
    // there. LOAD_WITH_ALTERED_SEARCH_PATH makes the DLL's own directory the
    // first place its imports are resolved from, which is what lets us load it
    // in place instead of copying three DLLs next to the executable.
    HMODULE m = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        lastError_ = GetLastError();
        lastTried_ = candidate;
        return false;
    }

    auto init = reinterpret_cast<FnInit>(GetProcAddress(m, "psvr2_toolkit_init"));
    auto deinit = reinterpret_cast<FnDeinit>(GetProcAddress(m, "psvr2_toolkit_deinit"));
    auto active = reinterpret_cast<FnDriverActive>(GetProcAddress(m, "psvr2_toolkit_get_driver_active"));
    auto write = reinterpret_cast<FnWritePcm>(GetProcAddress(m, "psvr2_toolkit_write_pcm"));
    auto wait = reinterpret_cast<FnWaitForPcm>(GetProcAddress(m, "psvr2_toolkit_wait_for_pcm"));
    auto trigger = reinterpret_cast<FnSetTrigger>(GetProcAddress(m, "psvr2_toolkit_set_trigger_effect"));

    if (!init || !deinit || !active || !write || !wait || !trigger) {
        lastError_ = ERROR_PROC_NOT_FOUND;
        lastTried_ = candidate;
        FreeLibrary(m);
        return false;
    }

    module_ = m;
    path_ = candidate;
    init_ = init;
    deinit_ = deinit;
    driverActive_ = active;
    writePcm_ = write;
    waitForPcm_ = wait;
    setTrigger_ = trigger;
    return true;
}

bool Capi::Load(const std::wstring& configured, std::string& error) {
    std::vector<std::wstring> candidates;
    if (!configured.empty()) candidates.push_back(configured);
    candidates.push_back(L"psvr2_toolkit_capi.dll");
    candidates.push_back(LR"(C:\Program Files (x86)\Steam\steamapps\common\PlayStation VR2 App\SteamVR_Plug-In\bin\win64\psvr2_toolkit_capi.dll)");
    candidates.push_back(LR"(C:\Program Files\Steam\steamapps\common\PlayStation VR2 App\SteamVR_Plug-In\bin\win64\psvr2_toolkit_capi.dll)");

    for (const auto& c : candidates) {
        if (LoadOne(c)) return true;
    }
    char narrow[512]{};
    if (!lastTried_.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, lastTried_.c_str(), -1, narrow, sizeof(narrow),
                            nullptr, nullptr);
    }
    error = "Could not load psvr2_toolkit_capi.dll.\n  last tried: ";
    error += narrow[0] ? narrow : "(none)";
    error += "\n  win32 error " + std::to_string(lastError_);
    switch (lastError_) {
        case ERROR_MOD_NOT_FOUND:
            error += " (a dependency of the DLL is missing - is the PSVR2Toolkit "
                     "install complete?)";
            break;
        case ERROR_FILE_NOT_FOUND:
            error += " (file not found - set toolkit_dll= in the config)";
            break;
        case ERROR_PROC_NOT_FOUND:
            error += " (loaded, but the expected exports are absent - update PSVR2Toolkit)";
            break;
        case ERROR_BAD_EXE_FORMAT:
            error += " (architecture mismatch - this build is x64)";
            break;
        default:
            break;
    }
    return false;
}

bool Capi::Init(std::string& error) {
    if (!module_) {
        error = "CAPI not loaded";
        return false;
    }
    const int rc = init_();
    if (rc != kResultOk) {
        error = std::string("psvr2_toolkit_init failed: ") + ResultName(rc);
        return false;
    }
    initialised_ = true;
    if (!driverActive_()) {
        error = "PSVR2Toolkit driver is not active. Start SteamVR with the PSVR2 "
                "headset connected, then run this again.";
        return false;
    }
    return true;
}

Capi::PcmCapabilities Capi::Calibrate(int cycles) {
    PcmCapabilities caps;
    if (!module_) return caps;

    const std::array<uint8_t, kChunkSize> silence{};
    std::map<int, int> writeCodes;
    int ready = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < cycles; ++i) {
        const int wrc = WaitForPcm();
        if (!WaitIsReady(wrc)) continue;
        ++ready;
        caps.waitReadyValue = wrc;
        ++writeCodes[WritePcm(Controller::Left, silence)];
        ++writeCodes[WritePcm(Controller::Right, silence)];
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    if (ms > 0 && ready > 0) caps.measuredHz = ready * 1000.0 / static_cast<double>(ms);
    const double expected = static_cast<double>(kSampleRate) / kChunkSize;
    caps.waitBlocks = caps.measuredHz > expected * 0.5 && caps.measuredHz < expected * 1.8;

    // Deciding whether write_pcm returns a real result code has to be
    // deterministic. A void-returning build leaves a stale register value that
    // is constant within a run but arbitrary between runs, so "is it inside the
    // documented range" decides by luck - it was observed flipping between runs
    // on the same machine.
    //
    // The only unambiguous positive signal is that every write returned exactly
    // OK. Anything else is treated as unreportable:
    //   * a constant garbage value  -> genuinely meaningless, ignore it
    //   * a constant documented error -> ambiguous between a real failure and a
    //     stale register, so surface it once here instead of logging it forever
    // Either way we never spam false errors and never silently swallow a real
    // one, because the observed value is reported in `writeObserved`.
    caps.writeCodesMeaningful =
        writeCodes.size() == 1 && writeCodes.count(kResultOk) == 1;
    caps.writeObserved = writeCodes.size() == 1 ? writeCodes.begin()->first : 0;
    caps.writeCodesVaried = writeCodes.size() > 1;

    pcm_ = caps;
    return caps;
}

void Capi::Shutdown() {
    if (module_ && deinit_ && initialised_) deinit_();
    initialised_ = false;
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
}

} // namespace psvr2
