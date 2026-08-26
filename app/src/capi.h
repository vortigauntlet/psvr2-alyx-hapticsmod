// PSVR2Toolkit CAPI binding.
//
// The ABI below was verified two ways:
//   1. exported symbol names read out of the shipped
//      psvr2_toolkit_capi.dll (PlayStation VR2 App / SteamVR_Plug-In)
//   2. declarations in PSVR2Toolkit upstream:
//      projects/psvr2_toolkit_capi/psvr2tk_capi.h
//      projects/common/pad_trigger_effect.h
//      projects/common/common.h
//
// Nothing here is guessed. If the toolkit changes its ABI, Load() fails
// cleanly on the missing export rather than calling through a bad pointer.
//
// psvr2_toolkit_set_hmd_rumble is deliberately NOT bound: headset vibration is
// out of scope for this project.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

// common.h: enum class VRControllerType : uint8_t { Left = 0, Right = 1, Both = 2 };
enum class Controller : uint8_t { Left = 0, Right = 1, Both = 2 };

// common.h
constexpr int kChunkSize = 32;    // k_senseChunkSize
constexpr int kSampleRate = 3000; // k_senseSampleRate

// psvr2tk_capi.h result codes
constexpr int kResultOk = 0;
constexpr int kResultDriverInactive = -1;
constexpr int kResultNoSlot = -2;
constexpr int kResultTimeout = -3;
constexpr int kResultInvalidParameter = -4;

const char* ResultName(int rc);

// ---------------------------------------------------------------------------
// pad_trigger_effect.h
//
// Documented parameter ranges are recorded next to each field. They are
// enforced in triggers.cpp; out-of-range values are rejected by the toolkit
// with kResultInvalidParameter rather than being clamped for us.
// ---------------------------------------------------------------------------

enum TriggerMode : int32_t {
    kTriggerOff = 0,
    kTriggerFeedback = 1,
    kTriggerWeapon = 2,
    kTriggerVibration = 3,
    kTriggerMultiPositionFeedback = 4,
    kTriggerSlopeFeedback = 5,
    kTriggerMultiPositionVibration = 6,
};

struct TriggerOffParam { uint8_t padding[48]{}; };

struct TriggerFeedbackParam {
    uint8_t position{};  // 0..9
    uint8_t strength{};  // 0..8
    uint8_t padding[46]{};
};

struct TriggerWeaponParam {
    uint8_t startPosition{}; // 2..7
    uint8_t endPosition{};   // startPosition+1 .. 8
    uint8_t strength{};      // 0..8
    uint8_t padding[45]{};
};

struct TriggerVibrationParam {
    uint8_t position{};  // 0..9
    uint8_t amplitude{}; // 0..8
    uint8_t frequency{}; // 0..255 Hz
    uint8_t padding[45]{};
};

struct TriggerMultiPositionFeedbackParam {
    uint8_t strength[10]{}; // each 0..8
    uint8_t padding[38]{};
};

struct TriggerSlopeFeedbackParam {
    uint8_t startPosition{};  // 0 .. endPosition
    uint8_t endPosition{};    // startPosition+1 .. 9
    uint8_t startStrength{};  // 1..8
    uint8_t endStrength{};    // 1..8
    uint8_t padding[44]{};
};

struct TriggerMultiPositionVibrationParam {
    uint8_t frequency{};     // 0..255
    uint8_t amplitude[10]{}; // each 0..8
    uint8_t padding[37]{};
};

union TriggerCommandData {
    TriggerOffParam off;
    TriggerFeedbackParam feedback;
    TriggerWeaponParam weapon;
    TriggerVibrationParam vibration;
    TriggerMultiPositionFeedbackParam multiFeedback;
    TriggerSlopeFeedbackParam slope;
    TriggerMultiPositionVibrationParam multiVibration;
    TriggerCommandData() : off{} {}
};

struct TriggerCommand {
    TriggerMode mode = kTriggerOff;
    uint8_t padding[4]{};
    TriggerCommandData data{};
};
static_assert(sizeof(TriggerCommand) == 56, "TriggerCommand ABI mismatch");

bool SameCommand(const TriggerCommand& a, const TriggerCommand& b);

// ---------------------------------------------------------------------------

class Capi {
public:
    ~Capi() { Shutdown(); }

    // Tries the configured path first, then the usual install locations.
    // Returns false and fills `error` if no candidate exposes the full ABI.
    bool Load(const std::wstring& configured, std::string& error);
    bool Init(std::string& error);
    void Shutdown();

    bool loaded() const { return module_ != nullptr; }
    const std::wstring& path() const { return path_; }
    bool DriverActive() const { return driverActive_ && driverActive_(); }

    // --- ABI variance between shipped toolkit builds -----------------------
    //
    // The CAPI is explicitly work-in-progress and the DLL that ships with the
    // PlayStation VR2 App does not always match upstream's source. Two
    // differences have been measured on a real install:
    //
    //   * psvr2_toolkit_wait_for_pcm returns 1, not 0. Upstream returns
    //     PSVR2TK_RESULT_OK (0) / TIMEOUT (-3) / DRIVER_INACTIVE (-1); the
    //     shipped build behaves like an older bool-returning version where 1
    //     means "a chunk is due". Comparing it against 0 makes every wait look
    //     like a failure and silently starves the whole PCM path.
    //
    //   * psvr2_toolkit_write_pcm returns an uninitialised register value
    //     (e.g. 2059534336), consistent with a build whose write_pcm returns
    //     void. Treating that as a result code produces endless bogus errors.
    //
    // Rather than hard-coding either variant, Calibrate() measures the real
    // behaviour once at startup and the rest of the program asks these
    // accessors. A future toolkit that returns proper codes is picked up
    // automatically and gets full error reporting.

    struct PcmCapabilities {
        bool waitBlocks = false;           // pacing matches kSampleRate/kChunkSize
        bool writeCodesMeaningful = false; // every write returned exactly OK
        bool writeCodesVaried = false;     // write returned more than one value
        int writeObserved = 0;             // the constant value it did return
        double measuredHz = 0.0;
        int waitReadyValue = kResultOk;
    };

    // Runs a short silent warm-up. Safe: it only ever writes zero samples.
    PcmCapabilities Calibrate(int cycles = 40);
    const PcmCapabilities& pcm() const { return pcm_; }

    // True when the driver says a chunk is due. Accepts both ABI variants.
    static bool WaitIsReady(int rc) { return rc >= 0; }

    // Blocks until the driver is ready for the next 32-sample chunk.
    int WaitForPcm() const { return waitForPcm_ ? waitForPcm_() : kResultInvalidParameter; }
    int WritePcm(Controller c, const std::array<uint8_t, kChunkSize>& chunk) const {
        return writePcm_ ? writePcm_(c, chunk.data()) : kResultInvalidParameter;
    }
    int SetTriggerEffect(Controller c, const TriggerCommand& cmd) const {
        return setTrigger_ ? setTrigger_(c, cmd) : kResultInvalidParameter;
    }

private:
    bool LoadOne(const std::wstring& path);

    using FnInit = int(__cdecl*)();
    using FnDeinit = void(__cdecl*)();
    using FnDriverActive = bool(__cdecl*)();
    using FnWritePcm = int(__cdecl*)(Controller, const unsigned char*);
    using FnWaitForPcm = int(__cdecl*)();
    using FnSetTrigger = int(__cdecl*)(Controller, const TriggerCommand&);

    HMODULE module_ = nullptr;
    std::wstring path_;
    FnInit init_ = nullptr;
    FnDeinit deinit_ = nullptr;
    FnDriverActive driverActive_ = nullptr;
    FnWritePcm writePcm_ = nullptr;
    FnWaitForPcm waitForPcm_ = nullptr;
    FnSetTrigger setTrigger_ = nullptr;
    bool initialised_ = false;
    PcmCapabilities pcm_;
    // Kept so a failed Load() can explain which candidate failed and why.
    unsigned long lastError_ = 0;
    std::wstring lastTried_;
};

} // namespace psvr2
