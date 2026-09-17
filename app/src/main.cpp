// PSVR2 Alyx Haptics
//
//   Half-Life: Alyx
//        v  (VScript addon, no injection, no Valve files touched)
//   console.log
//        v
//   this middleware: event router -> HLA tactile profiles
//        |-- adaptive trigger state machine (persistent + transient overlays)
//        '-- PCM voice synthesis and mixer
//        v
//   PSVR2Toolkit CAPI  ->  PSVR2 Sense controllers
//
// Headset rumble is deliberately not used anywhere in this project.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/adapter.h"
#include "core/capi.h"
#include "core/bhaptics_listener.h"
#include "core/config.h"
#include "core/multi_transport.h"
#include "core/events.h"
#include "core/haptics.h"
#include "core/hmd.h"
#include "core/profiles.h"
#include "core/transport.h"
#include "core/triggers.h"
#include "games/alyx/alyx_adapter.h"
#include "games/alyx/alyx_install.h"
#include "games/alyx/netconsole.h"
#include "games/hl2vr/hl2vr_adapter.h"
#include "games/hl2vr/hl2vr_bhaptics.h"
#include "games/hl2vr/hl2vr_install.h"

namespace psvr2 {
namespace {

std::atomic<bool> g_running{true};
void OnSignal(int) { g_running = false; }

// Transport delivery monitor.
//
// The game script emits TICK every 250 ms of GAME time, carrying its own
// clock. Comparing the spacing we observe against the spacing the game
// intended tells us what the transport is doing to our events:
//
//   * spacing matches      -> events are arriving promptly
//   * ticks arrive bunched -> something is buffering, and the bunch size is
//                             roughly the delay being added
//   * counter gaps         -> lines are being dropped outright
//
// This matters more than it might look. Every tactile decision in this project
// assumes the effect lands with the thing that caused it; a gunshot 80 ms late
// is not a gunshot, and no amount of waveform tuning fixes that. Before this,
// latency was completely uninstrumented - we could not have told the
// difference between a badly designed effect and a well designed one arriving
// too late.
struct DeliveryMonitor {
    bool started = false;
    long long lastCounter = 0;
    double lastGameTime = 0.0;
    std::chrono::steady_clock::time_point lastArrival;

    int samples = 0;
    int bunched = 0;   // arrived far sooner than the game spacing implies
    int dropped = 0;   // counter gaps
    double jitterSumMs = 0.0;
    double worstMs = 0.0;

    void OnTick(long long counter, double gameTime) {
        const auto now = std::chrono::steady_clock::now();
        if (started) {
            if (counter > lastCounter + 1) {
                dropped += static_cast<int>(counter - lastCounter - 1);
            }
            const double wallMs =
                std::chrono::duration<double, std::milli>(now - lastArrival).count();
            const double gameMs = (gameTime - lastGameTime) * 1000.0;
            // Only meaningful across a normal, un-paused interval.
            if (gameMs > 1.0 && gameMs < 2000.0) {
                const double jitter = wallMs - gameMs;
                ++samples;
                jitterSumMs += std::fabs(jitter);
                worstMs = std::max(worstMs, std::fabs(jitter));
                // Two ticks landing almost together means the earlier one was
                // held somewhere and released late.
                if (wallMs < gameMs * 0.35) ++bunched;
            }
        }
        started = true;
        lastCounter = counter;
        lastGameTime = gameTime;
        lastArrival = now;
    }

    void Report(const char* transportName) const {
        if (samples < 4) {
            std::cout << "Transport: not enough ticks to judge delivery.\n";
            return;
        }
        const double mean = jitterSumMs / samples;
        std::cout << "Transport (" << transportName << "): "
                  << samples << " ticks, mean jitter " << mean
                  << " ms, worst " << worstMs << " ms";
        if (dropped > 0) std::cout << ", " << dropped << " dropped";
        if (bunched > 0) {
            std::cout << ", " << bunched << " bunched arrivals";
        }
        std::cout << "\n";
        if (bunched * 4 > samples) {
            std::cout << "! Events are arriving in bursts, so something is buffering them.\n"
        "  If this is the console.log route, switch to -netconport 29000.\n";
        } else if (mean < 20.0) {
            std::cout << "  Delivery is prompt.\n";
        }
    }
};

// Session recording and replay.
//
// The brief for this project asked for replay of recorded semantic events, and
// it is the single thing that most changes how fast tactile design can be
// iterated on. Without it every tuning question needs the game launched, a
// headset worn, and the exact situation reproduced by hand.
//
// With it: play Half-Life: Alyx once with record= set, and that session becomes
// a repeatable test case with REAL masses, REAL materials and REAL firing
// cadence. Synthetic self-tests can only exercise what someone thought to
// author; a recording exercises what the game actually does.
//
// The format is deliberately trivial - "<milliseconds>\t<EVENT>:<params>" -
// so a session can be trimmed, spliced or hand-written in any text editor.
class Recorder {
public:
    bool Open(const std::string& path) {
        file_.open(path, std::ios::trunc);
        if (!file_) return false;
        start_ = std::chrono::steady_clock::now();
        file_ << "# PSVR2 Alyx Haptics session recording\n"
        "# <ms since start>\\t<EVENT>:<params>\n";
        return true;
    }
    bool open() const { return file_.is_open(); }
    void Write(const std::string& event, const std::string& param) {
        if (!file_.is_open()) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_).count();
        file_ << ms << '\t' << event;
        if (!param.empty()) file_ << ':' << param;
        file_ << '\n';
        // Flushed per line so a crash, or simply closing the window, still
        // leaves a usable recording behind.
        file_.flush();
        ++count_;
    }
    uint64_t count() const { return count_; }

private:
    std::ofstream file_;
    std::chrono::steady_clock::time_point start_;
    uint64_t count_ = 0;
};

struct Entry { long long ms; std::string event, param; };

bool LoadRecording(const std::string& path, std::vector<Entry>& entries) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Could not open recording: " << path << "\n";
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            std::cout << "! line " << lineNo << ": no tab separator, skipped\n";
            continue;
        }
        Entry e;
        try { e.ms = std::stoll(line.substr(0, tab)); }
        catch (...) {
            std::cout << "! line " << lineNo << ": bad timestamp, skipped\n";
            continue;
        }
        const std::string rest = line.substr(tab + 1);
        const auto colon = rest.find(':');
        e.event = colon == std::string::npos ? rest : rest.substr(0, colon);
        e.param = colon == std::string::npos ? std::string{} : rest.substr(colon + 1);
        entries.push_back(std::move(e));
    }

    if (entries.empty()) {
        std::cerr << "Recording contains no events: " << path << "\n";
        return false;
    }
    return true;
}

// Offline replay: renders a recorded session through the real router and mixer
// with no hardware at all, and reports what it produced.
//
// This is the form that matters most for iterating on tactile design, because
// it needs nothing but the file. It also surfaces the one class of problem no
// synthetic test can: OVERLAP. The self-test fires each signature alone, but a
// recording has you reloading while carrying a crate and taking a hit in the
// middle of a burst, which is exactly when the limiter starts crushing things.
int RunReplayAnalysis(IGameAdapter& router, Mixer& mixer, const std::string& path) {
    std::vector<Entry> entries;
    if (!LoadRecording(path, entries)) return 2;

    const long long span = entries.back().ms - entries.front().ms;
    const long long base = entries.front().ms;

    std::map<std::string, int> counts;
    Mixer::StreamStats left, right;
    long long clock = 0;
    float worstLimit = 1.0f;
    long long worstAt = 0;

    for (const auto& e : entries) {
        const long long due = e.ms - base;
        if (due > clock) {
            // Advance the render to this event's time, so overlap is modelled
            // exactly as it happened rather than collapsed together.
            const int chunks = static_cast<int>(
                (due - clock) * kSampleRate / (1000LL * kChunkSize));
            if (chunks > 0) {
                const float beforeL = left.limitMin, beforeR = right.limitMin;
                mixer.StepOffline(chunks, left, right);
                const float now = std::min(left.limitMin, right.limitMin);
                if (now < worstLimit && (left.limitMin < beforeL || right.limitMin < beforeR)) {
                    worstLimit = now;
                    worstAt = clock;
                }
                clock = due;
            }
        }
        ++counts[e.event];
        router.Handle(e.event, e.param);
    }
    // Let the tail play out.
    mixer.StepOffline(120, left, right);

    std::cout << "Session: " << path << "\n"
              << "  " << entries.size() << " events over " << span / 1000.0 << " s\n\n"
              << "event                    count\n"
        "------------------------------\n";
    for (const auto& [name, n] : counts) {
        std::printf("%-24s %5d\n", name.c_str(), n);
    }
    std::printf("\nrendered output    peak    rms   worst-lim   limiting\n"
        "------------------------------------------------------\n");
    std::printf("LEFT             %6.2f  %5.3f     %6.2f     %5.1f%%\n",
                left.peak, left.rms(), left.limitMin, left.limitedFraction() * 100.0f);
    std::printf("RIGHT            %6.2f  %5.3f     %6.2f     %5.1f%%\n",
                right.peak, right.rms(), right.limitMin, right.limitedFraction() * 100.0f);

    const float worst = std::min(left.limitMin, right.limitMin);
    const float busiest = std::max(left.limitedFraction(), right.limitedFraction());
    std::cout << "\n";

    // How OFTEN the limiter works matters far more than how far it once dipped.
    // A momentary dip where two big events genuinely coincide is correct - that
    // moment should be loud. Limiting engaged much of the time means everything
    // is riding the ceiling and the dynamic range has collapsed, which is the
    // failure this whole design is built to avoid.
    if (busiest > 0.15f) {
        std::printf("! The limiter was engaged %.0f%% of the session.\n"
        "  That is not the odd loud moment - the mix is riding the ceiling, so\n"
        "  effects are being flattened toward each other. Lower master, or find\n"
        "  which effects are overlapping constantly.\n", busiest * 100.0f);
    } else if (worst < 0.45f) {
        std::cout << "! One moment around " << worstAt / 1000.0 << " s pulled the limiter to "
                  << worst << ".\n"
        "  Brief, so it may just be a genuinely big moment - worth feeling with\n"
        "  --replay on hardware before changing anything.\n";
    } else {
        std::printf("Healthy dynamics: limiting engaged %.1f%% of the time, worst %.2f.\n",
                    busiest * 100.0f, worst);
    }
    return 0;
}

int RunReplay(IGameAdapter& router, TriggerManager& triggers, Mixer& mixer,
              const std::string& path, bool debug) {
    std::vector<Entry> entries;
    if (!LoadRecording(path, entries)) return 2;

    const long long span = entries.back().ms - entries.front().ms;
    std::cout << "Replaying " << entries.size() << " events over "
              << span / 1000.0 << " s from " << path << "\n"
              << "Hold both controllers. Ctrl+C to stop.\n\n";

    const auto t0 = std::chrono::steady_clock::now();
    const long long base = entries.front().ms;
    for (const auto& e : entries) {
        if (!g_running) break;
        // Original timing is preserved, because cadence is part of the design:
        // a burst of shotgun fire and the same shots spread over a second are
        // very different things for the mixer to handle.
        const auto due = t0 + std::chrono::milliseconds(e.ms - base);
        while (g_running && std::chrono::steady_clock::now() < due) {
            triggers.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (debug) {
            std::cout << "  " << (e.ms - base) << " ms  " << e.event;
            if (!e.param.empty()) std::cout << ":" << e.param;
            std::cout << "\n";
        }
        router.Handle(e.event, e.param);
    }

    // Let the tail of the last effect actually play out.
    for (int i = 0; i < 60 && g_running; ++i) {
        triggers.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "\nDone. PCM chunks written: " << mixer.chunksWritten() << "\n";
    return 0;
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf);
    const auto slash = w.find_last_of(L'\\');
    if (slash != std::wstring::npos) w = w.substr(0, slash);
    char out[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

// Shown by --version.
//
// A public release needs this. Without it a bug report says "the haptics
// feel wrong" and there is no way to know which build it came from, whether
// the reporter already has the fix for the thing they are reporting, or
// whether their profile file predates the values it overrides.
// The FIRST public release, so it is 1.0 regardless of how many internal
// iterations preceded it. Shipping the private iteration count as the public
// version just invites "where are the other seven releases?" from people who
// can only ever see this one.
//
// The game script keeps its own number (below). It is a compatibility marker
// used to verify what actually landed in the game folder, not a product
// version, and the two are not expected to match.
constexpr const char* kVersion = "1.0";

void PrintVersion() {
    std::cout << "PSVR2 Alyx Haptics " << kVersion << "\n"
              << "  game script : 7.0\n"
              << "  built       : " __DATE__ " " __TIME__ "\n"
              << "  requires    : PSVR2Toolkit (PlayStation VR2 App)\n";
}

void PrintUsage() {
    std::cout <<
        "PSVR2 Haptics  -  Half-Life: Alyx and Half-Life 2 VR\n"
        "\n"
        "  psvr2_alyx_haptics.exe [options] [config-file]\n"
        "\n"
        "  --game <name>    alyx (default) or hl2vr. Overrides game= in the\n"
        "                   config file, so one install can drive either game\n"
        "                   from two shortcuts.\n"
        "\n"
        "  (no options)     install/update the game side, then run\n"
        "  --launch         also start the game through Steam\n"
        "  --install        install/update the game side and exit\n"
        "  --uninstall      remove the game side and exit\n"
        "  --test [name]    play tactile signatures without the game\n"
        "                   (no name = play them all in order)\n"
        "  --list-tests     print the available test names\n"
        "  --version        print the version and exit\n"
        "\n"
        "  tuning:\n"
        "  --dump-profiles  write the current tactile profiles to an editable file\n"
        "  --profiles <f>   load tactile profiles from this file\n"
        "                   (edit, then --test or --analyze - no rebuild needed)\n"
        "\n"
        "  sessions:\n"
        "  --record <file>  log every semantic event to a replayable session\n"
        "  --replay <file>  play a recorded session back with original timing\n"
        "                   (add --analyze to render it offline, no hardware)\n"
        "\n"
        "  hardware benches:\n"
        "  --triggers [gun] hold each weapon's resting trigger profile, then\n"
        "                   fire recoil on a beat - the only way to feel the\n"
        "                   trigger without the game, since the toolkit cannot\n"
        "                   tell us whether you are pulling it\n"
        "  --pcm-format     A/B the PCM byte encoding (signed vs unsigned)\n"
        "  --recoil-lab     six candidate recoil designs back to back - pick one\n"
        "  --deep-test      why recoil weakens with the trigger fully bottomed\n"
        "  --trigger-sweep  measure the trigger MOTOR: which frequency, strength\n"
        "                   and mode actually push back against your finger\n"
        "\n"
        "  diagnostics:\n"
        "  --probe          measure the PCM path and report driver behaviour\n"
        "  --bhaptics-scan  listen for a game's own bHaptics event stream and\n"
        "                   print every pattern it announces and fires. This is\n"
        "                   how the Half-Life 2 VR key names get discovered.\n"
        "  --verify         run the offline self-checks: the event model, the\n"
        "                   codec, the materials, and that every self-test\n"
        "                   case renders (or is asserted silent)\n"
        "  --analyze        render every signature offline and print what it\n"
        "                   actually produces - no headset or hardware needed\n"
        "  --sweep          frequency response check - which Hz you actually feel\n"
        "  --hmd-sweep      headset frequency response - needs hmd=true, and a\n"
        "                   jailbroken headset to be felt at all\n"
        "  --hmd-test       play the four headset patterns\n"
        "  --impacts <f>    read a recording and report where the held-impact\n"
        "                   thresholds should actually sit - no hardware needed\n"
        "  --hands          left/right localisation check\n"
        "\n"
        "  --debug          verbose trigger/PCM/impact diagnostics\n"
        "  --no-install     do not touch the game addon\n"
        "  --help\n";
}

// Measures the real behaviour of the toolkit's PCM pacing on THIS machine.
//
// This exists because the installed CAPI build does not necessarily match
// upstream: upstream's psvr2_toolkit_wait_for_pcm returns 0/-1/-3, but the
// shipped DLL has been observed returning 1, which is consistent with an older
// bool-returning build where 1 means "ready". Treating that as an error is
// catastrophic but silent - the mixer simply never writes PCM and only the
// adaptive triggers are felt.
int RunProbe(Capi& capi) {
    using namespace std::chrono;
    std::cout << "Probing PCM pacing (about 3 seconds)...\n\n";

    std::map<int, int> waitCodes;
    std::map<int, int> writeCodes;
    std::array<uint8_t, kChunkSize> silence{};

    const auto t0 = steady_clock::now();
    int iterations = 0;
    while (steady_clock::now() - t0 < seconds(3) && g_running) {
        const int wrc = capi.WaitForPcm();
        ++waitCodes[wrc];
        if (wrc >= 0) {
            ++writeCodes[capi.WritePcm(Controller::Left, silence)];
            ++writeCodes[capi.WritePcm(Controller::Right, silence)];
        } else {
            std::this_thread::sleep_for(milliseconds(2));
        }
        ++iterations;
    }
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();

    const double hz = elapsed > 0 ? iterations * 1000.0 / static_cast<double>(elapsed) : 0.0;
    const double expected = static_cast<double>(kSampleRate) / kChunkSize;

    std::cout << "  iterations      : " << iterations << " in " << elapsed << " ms\n"
              << "  wait rate       : " << hz << " Hz\n"
              << "  expected rate   : " << expected << " Hz ("
              << kSampleRate << " Hz / " << kChunkSize << " samples)\n"
              << "  wait_for_pcm    :";
    for (auto [code, n] : waitCodes) {
        std::cout << " " << code << "(" << ResultName(code) << ")x" << n;
    }
    std::cout << "\n  write_pcm       :";
    for (auto [code, n] : writeCodes) {
        std::cout << " " << code << "(" << ResultName(code) << ")x" << n;
    }
    std::cout << "\n\n";

    const bool paced = hz > expected * 0.5 && hz < expected * 1.8;

    // A build that genuinely returns result codes can only emit values in the
    // documented range. Anything else is a stale register from a void function.
    bool codesMeaningful = !writeCodes.empty();
    for (const auto& [code, n] : writeCodes) {
        (void)n;
        if (code > kResultOk || code < kResultInvalidParameter) { codesMeaningful = false; break; }
    }
    const bool writesRejected =
        codesMeaningful && !(writeCodes.size() == 1 && writeCodes.count(kResultOk) == 1);

    if (paced) {
        std::cout << "wait_for_pcm is blocking and pacing correctly.\n";
        if (waitCodes.size() == 1 && waitCodes.count(kResultOk) == 0) {
            std::cout << "  (this build signals readiness with "
                      << waitCodes.begin()->first
                      << ", not 0 - comparing it against 0 would starve PCM entirely)\n";
        }
    } else {
        std::cout << "! wait_for_pcm is not pacing at the expected rate. If it is far too\n"
        "  fast it is not blocking, and PCM output will be starved or aliased.\n";
    }
    if (!codesMeaningful) {
        std::cout << "write_pcm does not return a usable result code in this build, so its\n"
        "  return value is ignored rather than reported as a false error.\n";
    } else if (writesRejected) {
        std::cout << "! at least one write_pcm call was rejected (see the tally above).\n";
    } else {
        std::cout << "Every write_pcm call was accepted.\n";
    }
    return (paced && !writesRejected) ? 0 : 1;
}

// Plays a sustained tone on one or both controllers and waits for it to finish.
void PlayTone(Mixer& mixer, TriggerManager& triggers, Controller c,
              float hz, float amp, int ms, bool raw = true) {
    std::vector<Voice> v{raw ? RawTone(hz, amp, static_cast<float>(ms), 15)
                             : Tone(hz, hz, amp, static_cast<float>(ms), 15)};
    mixer.Submit(c, std::move(v));
    for (int i = 0; i < (ms + 250) / 10 && g_running; ++i) {
        triggers.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Frequency response probe.
//
// The Sense controllers' usable vibrotactile band is not documented anywhere,
// and the whole tactile design depends on it: if the actuator rolls off above
// some frequency, every "bright" material signature collapses into the same
// dull thud and effects stop being distinguishable no matter how they are
// designed. This measures it on the actual hardware instead of assuming.
// Impact threshold analysis.
//
// HELD_MIN_SPEED and HELD_MIN_EXCESS in the game script decide whether a held
// object's deceleration counts as a collision. Both have always been
// placeholders - plausible round numbers never compared against a real swing.
//
// The script now reports every NEAR-MISS as PHYS_CANDIDATE, from floors well
// below the real thresholds, so a recording contains the decision boundary
// rather than only the far side of it. That is what makes this measurable:
// logging just the accepted hits can reveal false alarms but never misses, and
// "I bashed it and felt nothing" is the more likely complaint.
//
// This reads such a recording back and reports where the line actually sits.
int RunImpactAnalysis(const std::string& path) {
    std::vector<Entry> entries;
    if (!LoadRecording(path, entries)) return 2;

    struct Cand {
        float impulse, mass, speed, objDrop, handDrop, excess;
        bool passed;
        std::string side, material;
    };
    std::vector<Cand> cands;

    for (const auto& e : entries) {
        if (e.event != "PHYS_CANDIDATE") continue;
        // impulse,mass,side,speed,objDrop,handDrop,excess,passed,material
        std::vector<std::string> f;
        std::string cur;
        for (char ch : e.param) {
            if (ch == ',') { f.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
        f.push_back(cur);
        if (f.size() < 9) continue;
        Cand c{};
        try {
            c.impulse  = std::stof(f[0]);
            c.mass     = std::stof(f[1]);
            c.side     = f[2];
            c.speed    = std::stof(f[3]);
            c.objDrop  = std::stof(f[4]);
            c.handDrop = std::stof(f[5]);
            c.excess   = std::stof(f[6]);
            c.passed   = f[7] == "1";
            c.material = f[8];
        } catch (...) { continue; }
        cands.push_back(c);
    }

    std::cout << "Impact threshold analysis\n"
                 "-------------------------\n"
              << "  " << path << "\n";

    if (cands.empty()) {
        std::cout << "\nNo PHYS_CANDIDATE events in this recording.\n\n"
                     "Either it predates candidate logging (game script 7.1), or\n"
                     "nothing was carried and swung during the session.\n\n"
                     "To collect data: hold a bottle, a can, a crate, and bash them\n"
                     "into walls, floors and enemies. Wave them about WITHOUT hitting\n"
                     "anything too - the non-hits are half the measurement.\n";
        return 1;
    }

    const size_t passed = static_cast<size_t>(std::count_if(
        cands.begin(), cands.end(), [](const Cand& c) { return c.passed; }));
    std::cout << "  " << cands.size() << " candidates, " << passed
              << " passed the current thresholds (speed>110, excess>95)\n\n";

    auto pct = [](std::vector<float> v, double p) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        const size_t i = std::min(v.size() - 1,
            static_cast<size_t>(p * static_cast<double>(v.size() - 1)));
        return v[i];
    };
    std::vector<float> speeds, excesses, impulses;
    for (const auto& c : cands) {
        speeds.push_back(c.speed);
        excesses.push_back(c.excess);
        impulses.push_back(c.impulse);
    }

    std::cout << std::fixed << std::setprecision(0);
    std::cout << "                 p10    p25    p50    p75    p90    p99    max\n";
    auto row = [&](const char* label, std::vector<float>& v) {
        std::cout << "  " << std::left << std::setw(13) << label << std::right;
        for (double p : {0.10, 0.25, 0.50, 0.75, 0.90, 0.99, 1.00})
            std::cout << std::setw(7) << pct(v, p);
        std::cout << "\n";
    };
    row("speed", speeds);
    row("excess", excesses);
    row("impulse", impulses);

    // How the accept count moves with the threshold. The useful shape is a
    // PLATEAU: if the count barely changes across a range, real hits and
    // ordinary arm movement are well separated and anywhere in that range is
    // safe. A smooth fall with no flat part means they overlap and no
    // threshold can separate them - which is itself the finding.
    std::cout << "\nWhat other thresholds would accept:\n"
                 "  excess >    count    (of " << cands.size() << " candidates)\n";
    for (int t : {20, 40, 60, 80, 95, 120, 150, 200, 300}) {
        const size_t n = static_cast<size_t>(std::count_if(cands.begin(), cands.end(),
            [&](const Cand& c) { return c.excess > static_cast<float>(t); }));
        std::cout << "  " << std::setw(8) << t << std::setw(9) << n;
        if (t == 95) std::cout << "   <- current";
        std::cout << "\n";
    }

    // The discriminator itself. In a real collision the object sheds speed the
    // hand did not, so excess/objDrop near 1.0 means "the hand cannot explain
    // this". Near 0 means the hand simply stopped and nothing was struck.
    int strong = 0, weak = 0;
    for (const auto& c : cands) {
        if (c.objDrop <= 0.0f) continue;
        ((c.excess / c.objDrop) > 0.6f ? strong : weak)++;
    }
    std::cout << "\nHow much the HAND explains:\n"
              << "  " << strong << " unexplained by hand movement (likely real contact)\n"
              << "  " << weak << " mostly explained by the hand slowing (likely not)\n";

    std::cout << "\nHeaviest ten:\n"
                 "     impulse   mass   speed  excess  hand-expl  material\n";
    std::vector<Cand> top = cands;
    std::sort(top.begin(), top.end(),
              [](const Cand& a, const Cand& b) { return a.impulse > b.impulse; });
    for (size_t i = 0; i < std::min<size_t>(10, top.size()); ++i) {
        const Cand& c = top[i];
        const float expl = c.objDrop > 0.0f ? (1.0f - c.excess / c.objDrop) : 0.0f;
        std::cout << "  " << std::setw(10) << c.impulse
                  << std::setw(7) << c.mass
                  << std::setw(8) << c.speed
                  << std::setw(8) << c.excess
                  << std::setw(10) << std::setprecision(2) << expl << std::setprecision(0)
                  << "  " << c.material
                  << (c.passed ? "" : "   (MISSED)") << "\n";
    }

    std::cout << "\nReading this:\n"
                 "  * Look for a PLATEAU in the threshold table. A range where the\n"
                 "    count barely moves means real hits and ordinary arm movement\n"
                 "    are cleanly separated, and anywhere in it is a safe value.\n"
                 "  * No plateau means they overlap and no threshold separates\n"
                 "    them. That is a real finding, not a failed measurement - it\n"
                 "    would mean the discriminator needs work, not the numbers.\n"
                 "  * Anything marked (MISSED) is a hit you may have felt nothing\n"
                 "    for. If the heaviest rows are missed, the thresholds are too\n"
                 "    high.\n"
                 "  * Set them in addon/scripts/vscripts/psvr2_haptics/core.lua\n"
                 "    (HELD_MIN_SPEED, HELD_MIN_EXCESS), then rebuild.\n";
    return 0;
}

// Headset frequency response.
//
// The direct counterpart of RunSweep for the grip actuator and of
// --trigger-sweep for the trigger motor. Both of those overturned a design
// assumption the moment they were run: the grip sweep revealed a ~520 Hz
// ceiling that had silently collapsed every material into its body layer, and
// the trigger sweep reversed a frequency rule inferred from an uncontrolled
// three-weapon comparison.
//
// The headset patterns in hmd.cpp have had no such measurement. They are
// reasoned from the single fact that the parameter is a frequency, and nothing
// more. This is how they stop being guesses.
int RunHmdSweep(HmdChannel& hmd) {
    static const int kHz[] = {
        8, 12, 16, 20, 26, 32, 40, 50, 60, 75, 90, 110, 140, 180, 220,
    };

    std::cout <<
        "Headset frequency response check\n"
        "--------------------------------\n"
        "Put the headset ON. Each value runs for about 0.8 s, with a gap.\n"
        "Note which ones you actually FEEL, which read as pressure rather\n"
        "than buzz, and where they stop being felt at all.\n"
        "\n"
        "If you feel nothing for ANY of these, the headset is very likely not\n"
        "jailbroken. Nothing is broken in that case - hardware that is not\n"
        "listening simply ignores the value.\n\n";

    if (!hmd.enabled()) {
        std::cout << "Headset channel is not enabled - nothing to sweep.\n";
        return 1;
    }

    for (int hz : kHz) {
        if (!g_running) break;
        std::cout << "  " << hz << " Hz" << std::endl;
        HmdPattern one{"sweep", {{static_cast<uint8_t>(hz), 800}, {0, 0}}};
        hmd.Play(one, 9);
        const auto t0 = std::chrono::steady_clock::now();
        while (g_running &&
               std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1100)) {
            hmd.Tick(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    hmd.Stop();

    std::cout << "\nDone. Tell me:\n"
        "  1. which values you could feel at all\n"
        "  2. which felt like PRESSURE rather than buzzing\n"
        "  3. the highest one still clearly felt\n"
        "  4. whether any were unpleasant on the face - that is a hard limit,\n"
        "     not a preference, and anything unpleasant gets removed\n";
    return 0;
}

// Plays the four headset patterns in turn, announcing each.
int RunHmdTest(HmdChannel& hmd) {
    std::cout <<
        "Headset patterns\n"
        "----------------\n"
        "Put the headset ON. Each pattern is announced before it plays.\n\n";

    if (!hmd.enabled()) {
        std::cout << "Headset channel is not enabled - nothing to play.\n";
        return 1;
    }

    struct Case { const char* label; HmdPattern pattern; };
    const Case cases[] = {
        {"cover-mouth       your hand over your own face, hiding from Jeff", hmdfx::CoverMouth()},
        {"barnacle          a tongue has you and is hauling you up", hmdfx::Barnacle()},
        {"barnacle-release  it dropped you", hmdfx::BarnacleRelease()},
        {"hurt (light)      a hit worth flinching at", hmdfx::Hurt(0.35f)},
        {"hurt (heavy)      a hit that rattles you", hmdfx::Hurt(1.0f)},
    };

    for (const auto& c : cases) {
        if (!g_running) break;
        std::cout << "  " << c.label << std::endl;
        hmd.Play(c.pattern, 9);
        const auto t0 = std::chrono::steady_clock::now();
        while (g_running &&
               std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1600)) {
            hmd.Tick(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    hmd.Stop();

    std::cout << "\nDone. These are CODE level - reasoned, never measured.\n"
                 "Run --hmd-sweep first if they feel wrong: the response curve\n"
                 "is what decides whether these numbers make any sense.\n";
    return 0;
}

int RunSweep(Mixer& mixer, TriggerManager& triggers) {
    static const float kFreqs[] = {
        40, 60, 80, 100, 120, 150, 180, 220, 260, 300, 350, 400, 500, 600, 800,
    };

    std::cout <<
        "Frequency response check\n"
        "------------------------\n"
        "Hold BOTH controllers. Each tone plays for about 0.6 s on both hands.\n"
        "Note which ones feel STRONGEST, and where they stop being felt at all.\n"
        "All tones are the same amplitude, so any difference you feel is the\n"
        "hardware, not the design.\n\n";

    for (float hz : kFreqs) {
        if (!g_running) break;
        std::cout << "  " << static_cast<int>(hz) << " Hz" << std::endl;
        PlayTone(mixer, triggers, Controller::Both, hz, 0.95f, 600);
    }

    std::cout << "\nDone. Tell me:\n"
        "  1. which frequency felt the strongest\n"
        "  2. the highest one you could still feel clearly\n"
        "  3. whether any felt buzzy/rattly rather than smooth\n";
    return 0;
}

// Left/right localisation check.
int RunHands(Mixer& mixer, TriggerManager& triggers) {
    std::cout <<
        "Left / right check\n"
        "------------------\n"
        "Hold both controllers. Each burst names the hand it should be in.\n\n";

    struct Step { const char* label; Controller c; };
    static const Step steps[] = {
        {"LEFT only",  Controller::Left},
        {"RIGHT only", Controller::Right},
        {"BOTH",       Controller::Both},
        {"LEFT only",  Controller::Left},
        {"RIGHT only", Controller::Right},
    };

    for (const auto& s : steps) {
        if (!g_running) break;
        std::cout << "  " << s.label << std::endl;
        PlayTone(mixer, triggers, s.c, 180.0f, 0.95f, 500);
    }
    std::cout << "\nDone. If a burst was felt in the wrong hand, or not at all,\n"
        "say which step number.\n";
    return 0;
}


// Renders every tactile signature offline and prints what it actually produces.
// No hardware needed. This is how the loudness hierarchy and the pitch spread
// get verified rather than assumed.
// ---------------------------------------------------------------------------
// Self-verification.
//
// The layer with no test is where the rot is - this project has found the same
// silent failure three times now, each time in the part nothing measured. These
// are the checks that need neither hardware nor a game, so there is no excuse
// for not running them, and they are what a change to the event model or to
// either adapter's suite has to survive.
//
// Deliberately NOT a framework. Four assertions that fail loudly and return a
// non-zero exit code is what CI needs and all it needs.
// ---------------------------------------------------------------------------
// Discovering a game's bHaptics event vocabulary.
//
// The one thing about this integration that genuinely cannot be worked out
// without the game is the NAMES Half-Life 2 VR gives its haptic patterns. This
// listens on the bHaptics port, prints every key the game announces and every
// key it fires, and does nothing else - no haptics, no hardware, no install.
//
// One play session turns the unknown list into a known one, at which point the
// substring rules in games/hl2vr/hl2vr_bhaptics.cpp should be replaced with
// exact matches. That is the whole reason this tool exists rather than the
// mapping being guessed and shipped.
int RunBhapticsScan() {
    std::cout <<
        "bHaptics event scan\n"
        "-------------------\n"
        "Listening on 127.0.0.1:" << kBhapticsPort << " for a game with built-in\n"
        "bHaptics support. Nothing is played and no hardware is needed.\n"
        "\n"
        "  1. Make sure the bHaptics Player is NOT running - it owns this port.\n"
        "  2. Start Half-Life 2 VR and turn bHaptics ON in its options.\n"
        "  3. Play for a few minutes: fire every weapon, reload, take damage,\n"
        "     use the gravity gun, hit something with the crowbar.\n"
        "  4. Ctrl+C here, and send me the list.\n\n";

    BhapticsListener listener;
    if (!listener.Connect() && !listener.listening()) {
        if (listener.portInUse()) {
            std::cout << "! Port " << kBhapticsPort << " is already in use.\n"
                         "  That is almost certainly the bHaptics Player. Close it\n"
                         "  from the system tray and run this again.\n";
        } else {
            std::cout << "! Could not open port " << kBhapticsPort << ".\n";
        }
        return 1;
    }
    std::cout << "Listening. Waiting for the game...\n\n";

    std::map<std::string, uint64_t> fired;
    bool sawAnything = false;
    while (g_running) {
        listener.Connect();
        for (const auto& line : listener.Poll()) {
            const auto tag = line.find("[PSVR2H]");
            if (tag == std::string::npos) continue;
            const std::string rest = line.substr(tag + 9);
            const auto colon = rest.find(':');
            if (colon == std::string::npos) continue;
            const std::string event = rest.substr(0, colon);
            const std::string key = rest.substr(colon + 1);
            sawAnything = true;

            if (event == "BH_APP") {
                std::cout << "connected: " << key << "\n\n";
            } else if (event == "BH_REGISTER") {
                const auto m = MapBhapticsKey(key);
                std::printf("  register  %-40s -> %-18s %s\n", key.c_str(),
                            m.event.empty() ? "(unmapped)" : m.event.c_str(),
                            m.rule ? m.rule : "");
                std::fflush(stdout);
            } else if (event == "BH_SUBMIT") {
                if (fired[key]++ == 0) {
                    const auto m = MapBhapticsKey(key);
                    std::printf("  FIRED     %-40s -> %-18s %s\n", key.c_str(),
                                m.event.empty() ? "(unmapped)" : m.event.c_str(),
                                m.rule ? m.rule : "");
                    std::fflush(stdout);
                }
            }
        }
        listener.WaitForData(50);
    }

    std::cout << "\n\nSummary\n-------\n";
    if (!sawAnything) {
        std::cout <<
            "Nothing ever connected.\n\n"
            "That means one of:\n"
            "  * bHaptics is switched off in the game's options\n"
            "  * the bHaptics Player was running and took the port first\n"
            "  * Half-Life 2 VR does not use this transport, and the server\n"
            "    plugin is the only route - see docs/HL2VR.md\n\n"
            "The third is the one worth knowing, and it is the reason this tool\n"
            "reports honestly rather than assuming.\n";
        return 1;
    }

    std::cout << listener.registeredKeys().size() << " pattern(s) announced, "
              << fired.size() << " actually fired.\n\n";
    if (!fired.empty()) {
        std::cout << "Fired, by frequency - these are the ones that matter:\n";
        std::vector<std::pair<std::string, uint64_t>> byCount(fired.begin(), fired.end());
        std::sort(byCount.begin(), byCount.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [key, count] : byCount) {
            const auto m = MapBhapticsKey(key);
            std::printf("  %6llu  %-40s %s\n",
                        static_cast<unsigned long long>(count), key.c_str(),
                        m.event.empty() ? "<-- UNMAPPED" : m.event.c_str());
        }
    }
    return 0;
}

int RunVerify(IGameAdapter& router, Mixer& mixer) {
    int failures = 0;
    auto fail = [&](const std::string& what) {
        ++failures;
        std::cout << "  FAIL  " << what << "\n";
    };

    std::cout << "Self-verification (" << router.gameName() << ")\n"
                 "--------------------------------------------------\n";

    // 1. Every event in the generic model has a name, and every name decodes
    //    back to the event it came from. A new enumerator whose name is missing
    //    is a compile error thanks to the static_assert in events.cpp; this
    //    catches the subtler case of two enumerators sharing a name.
    {
        std::set<std::string> seen;
        int checked = 0;
        for (int i = 0; i < static_cast<int>(Ev::Count); ++i) {
            const Ev e = static_cast<Ev>(i);
            const std::string name = EvName(e);
            if (name == "UNKNOWN" && e != Ev::Unknown) {
                fail("event " + std::to_string(i) + " has no name");
                continue;
            }
            if (!seen.insert(name).second) {
                fail("event name '" + name + "' is used twice");
            }
            if (ParseEv(name) != e) {
                fail("event '" + name + "' does not parse back to itself");
            }
            ++checked;
        }
        std::cout << "  ok    " << checked << " event names are unique and round-trip\n";
    }

    // 2. The text codec preserves every field it claims to.
    //
    //    This is what makes a recording from either game replayable through the
    //    other's adapter, and what a synthetic test harness rides on, so a
    //    silent field drop here would be invisible until a session recorded
    //    over an hour turned out to be missing its masses.
    {
        HapticEvent in;
        in.type = Ev::Impact;
        in.hand = Hand::Left;
        in.material = Material::Glass;
        in.weapon = "MAGNUM";
        in.subtype = "held";
        in.intensity = 0.75f;
        in.energy = 0.5f;
        in.mass = 12.25f;
        in.speed = 640.0f;
        in.spin = 310.0f;
        in.confidence = 0.8f;
        in.count = 3;

        const std::string encoded = Encode(in);
        const auto colon = encoded.find(':');
        HapticEvent out;
        const bool ok = Decode(
            colon == std::string::npos ? encoded : encoded.substr(0, colon),
            colon == std::string::npos ? std::string{} : encoded.substr(colon + 1),
            out);
        if (!ok) {
            fail("codec: '" + encoded + "' did not decode");
        } else {
            auto same = [&](const char* what, bool cond) {
                if (!cond) fail(std::string("codec: ") + what + " lost in '" + encoded + "'");
            };
            same("type", out.type == in.type);
            same("hand", out.hand == in.hand);
            same("material", out.material == in.material);
            same("weapon", out.weapon == in.weapon);
            same("subtype", out.subtype == in.subtype);
            same("intensity", std::fabs(out.intensity - in.intensity) < 0.01f);
            same("energy", std::fabs(out.energy - in.energy) < 0.01f);
            same("mass", std::fabs(out.mass - in.mass) < 0.01f);
            same("speed", std::fabs(out.speed - in.speed) < 1.0f);
            same("spin", std::fabs(out.spin - in.spin) < 1.0f);
            same("confidence", std::fabs(out.confidence - in.confidence) < 0.01f);
            same("count", out.count == in.count);
        }

        // An unknown field must be ignored rather than poisoning the event, so
        // a newer game side can add one without breaking an older middleware.
        HapticEvent forward;
        if (!Decode("IMPACT", "mass=4,newfield=whatever,mat=metal", forward)) {
            fail("codec: an unknown field made the whole event fail to decode");
        } else if (forward.material != Material::Metal ||
                   std::fabs(forward.mass - 4.0f) > 0.01f) {
            fail("codec: an unknown field corrupted its neighbours");
        }
        std::cout << "  ok    codec round-trips every field and ignores unknown ones\n";
    }

    // 3. The WebSocket handshake matches RFC 6455's published test vector.
    //
    //    This is the one piece of the bHaptics transport with an authoritative
    //    answer available offline, and it is worth pinning: a subtly wrong
    //    handshake fails as "the game never connects", which is
    //    indistinguishable from "the game does not use this transport". That
    //    ambiguity would send someone down the wrong road for hours.
    {
        // RFC 6455 section 1.3.
        const std::string got = WebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        const std::string want = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        if (got != want) {
            fail("websocket handshake: got " + got + ", RFC 6455 says " + want);
        } else {
            std::cout << "  ok    websocket handshake matches the RFC 6455 vector\n";
        }
    }

    // 4. Every material class has a distinct name that parses back.
    {
        std::set<std::string> names;
        for (Material m : AllMaterials()) {
            const std::string n = MaterialName(m);
            if (!names.insert(n).second) fail("material name '" + n + "' is used twice");
            if (ParseMaterial(n) != m) fail("material '" + n + "' does not parse back");
        }
        std::cout << "  ok    " << names.size() << " material classes are distinct\n";
    }

    // 5. Every self-test case is claimed by the adapter and produces something.
    //
    //    A name in the suite that no handler answers is a test measuring
    //    nothing, and a case that renders silence is either a bug or a
    //    deliberate assertion - so silence has to be declared rather than
    //    discovered. The two cases below are the ones this project WANTS
    //    silent, and they are the most important assertions in the file:
    //    a melee swing through empty air and standing still holding a crate
    //    must both produce no waveform at all.
    {
        static const std::set<std::string> kMustBeSilent = {
            "melee-swing", "grav-hold-still", "hold-still",
        };
        int played = 0, silent = 0;
        for (const auto& name : router.SelfTestNames()) {
            if (!router.RunSelfTest(name, "right")) {
                fail("self-test '" + name + "' is listed but has no handler");
                continue;
            }
            const auto right = mixer.AnalyzeOffline(Controller::Right, 900);
            const auto left = mixer.AnalyzeOffline(Controller::Left, 900);
            const bool made = right.peak > 0.0f || left.peak > 0.0f;
            const bool wantSilent = kMustBeSilent.count(name) != 0;
            if (made && wantSilent) {
                fail("'" + name + "' must be SILENT but produced a waveform");
            } else if (!made && !wantSilent) {
                fail("'" + name + "' produced no waveform at all");
            }
            if (made) ++played; else ++silent;
        }
        std::cout << "  ok    " << played << " signatures render, " << silent
                  << " are deliberately silent\n";
    }

    // 6. The bHaptics key mapper classifies, drops and reports correctly.
    //
    //    Only meaningful for a game that has bHaptics support, so it is scoped
    //    to that adapter. The three cases are the three OUTCOMES that matter:
    //    a key that should map, a key that should be deliberately dropped
    //    because a controller cannot represent it, and a key nothing should
    //    claim. The third is the one worth guarding - a mapper that guesses
    //    would fire the wrong sensation at the right moment, which is far
    //    harder to notice than silence.
    if (std::string(router.id()) == "hl2vr") {
        struct Case { const char* key; const char* want; const char* why; };
        static const Case kCases[] = {
            // THE ORDERING TRAPS. Every one of these was a real bug at some
            // point in writing the rule table, because the substrings overlap:
            // "DamageFire" contains "fire", "CrowbarHit" contains "hit",
            // "DamageExplosion" contains "damage", "ShotgunReload" contains
            // "shotgun". Each pair below pins one of those decisions.
            {"PistolFire_1",        "HL2_FIRE",       "names a weapon and an action"},
            {"DamageFire_1",        "HL2_DAMAGE",     "burning is NOT a gunshot"},
            {"EnvironmentFire_1",   "HL2_DAMAGE",     "standing in fire is still fire"},
            {"CrowbarHit_1",        "HL2_MELEE_HIT",  "melee beats the word 'hit'"},
            {"DamageBullet_1",      "HL2_DAMAGE",     "being shot is damage, not firing"},
            {"ShockOnHandLeft_1",   "HL2_SHOCK",      "on the hand, not general damage"},
            {"KickbackShotgun_1",   "HL2_FIRE",       "recoil is part of firing"},
            {"PhysCannonLaunch_1",  "HL2_GRAV_LAUNCH","the gravity gun, launching"},
            {"PhysCannonGrab_1",    "HL2_GRAV_GRAB",  "the gravity gun, capturing"},
            {"DamageExplosion_1",   "HL2_EXPLOSION",  "a blast reads as a blast, not as damage"},
            {"ShotgunReload_1",     "HL2_RELOAD",     "reload beats the weapon name"},
            {"Heartbeat_1",         "",               "a vest can, a controller cannot"},
            {"Footstep_1",          "",               "your hands are not your feet"},
            {"AirboatRide_1",       "",               "a vehicle is not a hand"},
            {"ZzzUnknownThing_1",   "",               "nothing should claim this"},
        };
        int mapped = 0, dropped = 0;
        for (const auto& c : kCases) {
            const auto m = MapBhapticsKey(c.key);
            if (m.event != c.want) {
                fail(std::string("bhaptics: ") + c.key + " -> '" + m.event +
                     "', expected '" + c.want + "' (" + c.why + ")");
            }
            if (m.event.empty()) ++dropped; else ++mapped;
            // Every outcome must carry a reason, mapped or not. An unexplained
            // drop is indistinguishable from a bug.
            if (m.rule == nullptr) {
                fail(std::string("bhaptics: ") + c.key + " gave no reason");
            }
        }
        // The damage TYPE has to reach the parameters, not just the event.
        // Mapping DamageFire to HL2_DAMAGE and then losing the "fire" would
        // silently collapse four distinct sensations back into one.
        struct TypeCase { const char* key; const char* wantType; };
        static const TypeCase kTypes[] = {
            {"DamageFire_1",        "fire"},
            {"EnvironmentFire_1",   "fire"},
            {"DamageSpark_1",       "shock"},
            {"DamageLaser_1",       "shock"},
            {"EnvironmentPoison_1", "toxic"},
            {"DamageBullet_1",      ""},
        };
        for (const auto& c : kTypes) {
            const auto m = MapBhapticsKey(c.key);
            const std::string want = std::string("20,0,") + c.wantType;
            if (m.params != want) {
                fail(std::string("bhaptics: ") + c.key + " params '" + m.params +
                     "', expected '" + want + "'");
            }
        }

        // A submit for a mapped key must actually reach the synthesis.
        router.ResetForTest();
        router.ResetEmitTrace();
        router.Handle("BH_SUBMIT", "ShotgunFire_1");
        const auto right = mixer.AnalyzeOffline(Controller::Right, 900);
        const auto left = mixer.AnalyzeOffline(Controller::Left, 900);
        if (right.peak <= 0.0f && left.peak <= 0.0f) {
            fail("bhaptics: a mapped submit produced no waveform");
        }
        // ...and an unmapped one must reach nothing at all.
        router.ResetEmitTrace();
        router.Handle("BH_SUBMIT", "Heartbeat_1");
        const auto quietR = mixer.AnalyzeOffline(Controller::Right, 900);
        const auto quietL = mixer.AnalyzeOffline(Controller::Left, 900);
        if (quietR.peak > 0.0f || quietL.peak > 0.0f) {
            fail("bhaptics: a dropped key still produced a waveform");
        }
        std::cout << "  ok    bhaptics keys: " << mapped << " mapped, " << dropped
                  << " correctly dropped, and a submit reaches synthesis\n";
    }

    // 7. Every profile name the adapters ask for actually exists.
    {
        int missing = 0;
        for (int i = 0; i < kProfileCount; ++i) {
            Profiles probe;
            if (probe.Build(kProfileNames[i]).empty()) {
                fail(std::string("profile '") + kProfileNames[i] + "' builds nothing");
                ++missing;
            }
        }
        if (missing == 0) {
            std::cout << "  ok    " << kProfileCount << " tactile profiles all build\n";
        }
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "All checks passed.\n";
        return 0;
    }
    std::cout << failures << " check(s) FAILED.\n";
    return 1;
}

int RunAnalyze(IGameAdapter& router, Mixer& mixer) {
    std::map<std::string, std::pair<int, float>> measured; // name -> {durMs, domHz}
    std::cout << "signature          hands   peak    rms   dur(ms)  domHz   limiter\n"
        "---------------------------------------------------------------------\n";
    for (const auto& n : router.SelfTestNames()) {
        router.RunSelfTest(n, "right");
        // Which hands the effect really reached. Worth having in the table:
        // a signature that is unexpectedly bilateral is a design question, and
        // without this column the only way to find out was to feel it.
        const uint8_t felt = router.emitTrace();
        const char* where = (felt == 3) ? "both" : (felt == 1) ? "left"
                          : (felt == 2) ? "right" : "trig";
        const auto a = mixer.AnalyzeOffline(Controller::Right, 900);
        const auto other = mixer.AnalyzeOffline(Controller::Left, 900);
        // Bilateral effects are measured on whichever side carries more, so
        // the peak column stays comparable across the table.
        const float peak = std::max(a.peak, other.peak);
        std::printf("%-18s %-6s %5.2f  %5.3f  %6d  %6.0f  %6.2f%s\n",
                    n.c_str(), where, peak, a.rms, a.durationMs, a.centroidHz,
                    a.limitMin, a.limitMin < 0.75f ? "  <-- limiting hard" : "");
        measured[n] = {a.durationMs, a.centroidHz};
    }
    std::cout << "\npeak/rms show the loudness hierarchy; domHz shows pitch spread.\n"
        "'hands' is where the effect was actually emitted - 'both' is deliberate\n"
        "for whole-body events, not a bug.\n"
        "A limiter value well below 1.0 means the effect is being squashed,\n"
        "which flattens it toward every other effect.\n";

    // Perceptual collision check.
    //
    // "Not distinctive enough" was reported repeatedly on hardware and there
    // was no way to act on it except by guessing. This makes it a number.
    //
    // Vibrotactile discrimination is coarse: two vibrations need roughly a
    // 1.5x ratio in duration OR in pitch before skin reads them as different
    // things at all. Any pair inside that ratio on BOTH axes is, perceptually,
    // the same effect - no matter how different the code that made them looks.
    //
    // Effects grouped by family, because what matters is telling apart things
    // you meet in the same moment. Confusing the pistol with the shotgun is a
    // real failure; confusing the pistol with a wooden crate impact is not.
    struct Sig { std::string name; int durMs; float hz; };
    std::vector<Sig> sigs;
    for (const auto& [n, m] : measured) {
        if (m.first > 0 && m.second > 0.0f) sigs.push_back({n, m.first, m.second});
    }
    // The grouping itself lives in the adapter, because it is a statement
    // about one game's moment-to-moment play. Half-Life 2 lets you carry
    // every weapon at once and switch freely, so its weapon family is larger
    // and harder to keep separated than anything in Alyx.
    auto family = [&router](const std::string& n) { return router.AnalyzeFamily(n); };

    std::cout << "\nperceptual collisions (same family, <1.5x apart on BOTH axes)\n"
                 "-------------------------------------------------------------\n";
    int collisions = 0;
    for (size_t i = 0; i < sigs.size(); ++i) {
        for (size_t j = i + 1; j < sigs.size(); ++j) {
            const char* fa = family(sigs[i].name);
            const char* fb = family(sigs[j].name);
            if (fa == nullptr || fb == nullptr || std::string(fa) != fb) continue;
            const float dr = static_cast<float>(std::max(sigs[i].durMs, sigs[j].durMs)) /
                             std::max(1.0f, static_cast<float>(std::min(sigs[i].durMs, sigs[j].durMs)));
            const float pr = std::max(sigs[i].hz, sigs[j].hz) /
                             std::max(1.0f, std::min(sigs[i].hz, sigs[j].hz));
            if (dr >= 1.5f || pr >= 1.5f) continue;
            ++collisions;
            std::printf("  [%-16s] %-16s %4dms/%3.0fHz  ==  %-16s %4dms/%3.0fHz\n",
                        fa, sigs[i].name.c_str(), sigs[i].durMs, sigs[i].hz,
                        sigs[j].name.c_str(), sigs[j].durMs, sigs[j].hz);
        }
    }
    // The same check, on the adaptive trigger.
    //
    // The waveform layer got this treatment after seven effects were found
    // sharing one perceptual cell. The trigger layer then drifted into exactly
    // the same state unnoticed, because the check only ever covered one of the
    // two channels.
    //
    // Reported as a MARGIN rather than a pass/fail, because a hard bar did
    // real damage. Set to 1.75x it forced the SMG's recoil down to 65 ms to
    // satisfy the arithmetic, and that was immediately reported as "WAY too
    // short" - the number was clean and the weapon felt worse.
    //
    // Below 1.5x two effects genuinely are the same to a finger; that stays a
    // failure. Between 1.5x and 1.75x is thin but often fine, especially where
    // the pair differs in ways no ratio can see - the SMG uses a different
    // trigger mode from the pistol and its shots CHAIN into a continuous
    // rattle, which is a larger difference in play than any of these numbers.
    //
    // So: report the margin, flag what is genuinely too close, and let the
    // hand decide the rest.
    constexpr float kTriggerFail = 1.50f;
    constexpr float kTriggerComfortable = 1.75f;
    std::cout << "\nadaptive trigger recoil ladder\n"
                 "------------------------------\n"
                 "weapon      rest   kick   rate   reveal  pulses\n";
    const auto ladder = router.RecoilLadder();
    for (const auto& r : ladder) {
        // Two independent ladders side by side: how hard the trigger is to
        // PULL, and how hard it KICKS. They were tuned blind of each other
        // until the pistol and shotgun were found sitting at identical
        // resistance with no hierarchy to feel at all.
        const float cycles = r.revealMs * r.rateHz / 1000.0f;
        std::printf("%-10s %3d/8  %3d/8 %5d Hz %6d ms  %s\n",
                    r.weapon.c_str(), r.restPeak, r.drive, r.rateHz, r.revealMs,
                    cycles < 1.2f ? "one" : (cycles < 2.5f ? "two" : "rattle"));
    }
    int triggerCollisions = 0;
    for (size_t i = 0; i < ladder.size(); ++i) {
        for (size_t j = i + 1; j < ladder.size(); ++j) {
            auto ratio = [](int a, int b) {
                return static_cast<float>(std::max(a, b)) /
                       std::max(1.0f, static_cast<float>(std::min(a, b)));
            };
            const float dr = ratio(ladder[i].revealMs, ladder[j].revealMs);
            const float rr = ratio(ladder[i].rateHz, ladder[j].rateHz);
            const float sr = ratio(ladder[i].drive, ladder[j].drive);
            const float best = std::max({dr, rr, sr});
            if (best >= kTriggerComfortable) continue;
            const char* axis = (best == rr) ? "rate" : (best == dr ? "duration" : "drive");
            if (best < kTriggerFail) {
                ++triggerCollisions;
                std::printf("  ! %s / %s : only %.2fx apart (%s) - too close\n",
                            ladder[i].weapon.c_str(), ladder[j].weapon.c_str(), best, axis);
            } else {
                std::printf("    %s / %s : %.2fx apart (%s) - thin but workable\n",
                            ladder[i].weapon.c_str(), ladder[j].weapon.c_str(), best, axis);
            }
        }
    }
    if (triggerCollisions == 0) {
        std::cout << "  every weapon is separable on at least one axis.\n";
    } else {
        std::cout << "  Drive cannot fix these (the motor stalls above 7) and rate barely\n"
                     "  can (only ~12-28 Hz kicks at all). Spread them on DURATION.\n";
    }

    if (collisions == 0) {
        std::cout << "\n  none - every effect is separable from its siblings.\n";
    } else {
        std::printf("  %d pair(s) a hand cannot tell apart. Separate them by LENGTH or\n"
                    "  PITCH, or give one a rhythm the other lacks - skin resolves timing\n"
                    "  far better than it resolves pitch.\n", collisions);
        std::cout << "  (This measure is blind to RHYTHM: cardboard's three separate hits\n"
                     "  read as nothing like a single knock of the same length and pitch,\n"
                     "  so check a flagged pair by feel before reshaping it.)\n";
    }
    return 0;
}

// Adaptive trigger bench.
//
// --test cannot demonstrate a trigger effect, and no amount of tuning changes
// that: the toolkit CAPI exposes write_pcm, wait_for_pcm and
// set_trigger_effect and NOTHING inbound - no trigger position, no button
// state. The middleware cannot know whether a finger is on the trigger, so it
// cannot fire recoil in response to a pull.
//
// What it can do is hold a weapon's resting profile indefinitely, so the curve
// can be explored by hand, and fire recoil on a predictable beat so the finger
// is already in place when it lands. That makes the trigger testable without
// launching the game, which is the whole point.
int RunTriggerBench(IGameAdapter& router, TriggerManager& triggers, const std::string& only) {
    std::vector<std::string> weapons = {"PISTOL", "SMG", "SHOTGUN"};
    if (!only.empty()) {
        std::string up = only;
        std::transform(up.begin(), up.end(), up.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        weapons = {up};
    }

    std::cout <<
        "Adaptive trigger bench\n"
        "----------------------\n"
        "Hold the controller in your dominant hand, finger ON the trigger.\n"
        "\n"
        "For each weapon: the resting profile installs first - squeeze slowly\n"
        "and feel where it resists and where it breaks. Then recoil fires five\n"
        "times on a beat. Keep the trigger PRESSED for those.\n"
        "\n"
        "Ctrl+C to stop.\n\n";

    for (const auto& w : weapons) {
        if (!g_running) break;
        router.Handle("WEAPON", w);
        std::cout << "  " << w << " - resting profile. Squeeze slowly for 4 s." << std::endl;
        for (int i = 0; i < 400 && g_running; ++i) {
            triggers.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout << "  " << w << " - recoil x5. Hold the trigger DOWN." << std::endl;
        for (int shot = 0; shot < 5 && g_running; ++shot) {
            router.Handle("FIRE", w + ",right,1H,-1");
            for (int i = 0; i < 90 && g_running; ++i) {
                triggers.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::cout << "\n";
    }
    std::cout << "Done. Which had the strongest kick, and did any feel like a\n"
                 "buzz rather than a shove?\n";
    return 0;
}

// Adaptive trigger response sweep.
//
// The grip actuator's response was measured on hardware early on and the whole
// PCM design was built on it. The TRIGGER was never measured at all - its
// values were reasoned about by analogy, and that analogy turned out to be
// wrong in both directions:
//
//   * "low frequency reads as mechanical" is true of a voice coil and false
//     of the trigger's worm-gear motor, which cannot build force before it
//     has to reverse. Reported: 55 Hz kicked hardest, 24 Hz less, 30 Hz
//     nothing - the exact opposite of the design intent.
//
//   * Feedback mode resists FURTHER PRESSING; it does not push back. With the
//     trigger held down past the wall there is nothing to feel, which is why a
//     full-strength shotgun "shove" at position 1 registered as nothing.
//
// So this is the trigger's equivalent of --sweep: same mode, same strength,
// one variable at a time, reported by feel.
int RunTriggerSweep(TriggerManager& triggers) {
    std::cout <<
        "Adaptive trigger response sweep\n"
        "-------------------------------\n"
        "Hold the RIGHT controller. Rest your finger on the trigger and press it\n"
        "about HALFWAY - not all the way down. Keep it there throughout.\n"
        "\n"
        "Halfway matters: several of these resist movement rather than push, and\n"
        "with the trigger bottomed out there is no travel left to feel them in.\n"
        "\n";

    auto hold = [&](const TriggerCommand& cmd, const char* label, int ms) {
        if (!g_running) return;
        std::cout << "  " << label << std::endl;
        triggers.PushOverlay(Controller::Right, cmd, ms + 200, 9, label);
        for (int i = 0; i < ms / 10 && g_running; ++i) {
            triggers.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        triggers.ClearOverlays(Controller::Right, label);
        triggers.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    };

    std::cout << "PART 1 - Vibration frequency. Same strength (8), same position.\n"
                 "Which pushes back hardest?\n\n";
    static const int kFreqs[] = {10, 20, 30, 40, 50, 60, 80, 100, 130, 170};
    for (int hz : kFreqs) {
        if (!g_running) break;
        char label[64];
        std::snprintf(label, sizeof(label), "%3d Hz", hz);
        hold(trig::Vibration(0, 8, hz), label, 1400);
    }

    std::cout << "\nPART 2 - Strength at a fixed 60 Hz. Where does it stop growing?\n\n";
    for (int amp : {2, 4, 6, 8}) {
        if (!g_running) break;
        char label[64];
        std::snprintf(label, sizeof(label), "strength %d/8", amp);
        hold(trig::Vibration(0, amp, 60), label, 1200);
    }

    std::cout << "\nPART 3 - Different MODES. Which actually resists your finger?\n"
                 "Try pressing further and easing off during each one.\n\n";
    hold(trig::Feedback(0, 8), "FEEDBACK  - a wall from the very start", 1800);
    hold(trig::Feedback(5, 8), "FEEDBACK  - a wall halfway in", 1800);
    hold(trig::Weapon(2, 7, 8), "WEAPON    - resists then breaks", 1800);
    hold(trig::Vibration(0, 8, 60), "VIBRATION - oscillating motor", 1800);
    {
        std::array<int, 10> all8{};
        all8.fill(8);
        hold(trig::MultiVibration(60, all8), "MULTIVIBRATION - same, per-position", 1800);
    }
    hold(trig::Slope(0, 9, 2, 8), "SLOPE     - resistance ramps up with travel", 1800);

    triggers.Reset();
    std::cout <<
        "\nDone. Four answers settle the whole trigger design:\n"
        "  1. which FREQUENCY in part 1 pushed back hardest\n"
        "  2. whether strength kept growing to 8/8, or plateaued early\n"
        "  3. which MODE in part 3 felt most like a force against your finger\n"
        "  4. whether FEEDBACK-halfway felt different from FEEDBACK-from-start\n";
    return 0;
}

// Recoil design bench.
//
// Three separate attempts at trigger recoil have now been reported as "feels
// the same" or "feels like nothing", each time after a single-guess redesign.
// The pattern says the problem is not which numbers were chosen but that the
// design space has never actually been sampled.
//
// One strong clue: in --trigger-sweep every effect was held for 1400-1800 ms
// and all of the resisting modes read clearly. Recoil kicks are 30-90 ms. The
// trigger motor physically drives a lever into position, so there is a slew
// time below which it simply cannot arrive - and a 40 ms pistol kick may be
// cancelled before the motor has meaningfully moved at all.
//
// So this plays six candidate recoils back to back, varying MODE and DURATION
// together, repeating each so it can be felt properly. The answer is one
// letter rather than another round of guessing.
int RunRecoilLab(TriggerManager& triggers) {
    std::cout <<
        "Recoil design bench\n"
        "-------------------\n"
        "Hold the RIGHT controller with the trigger pressed about HALFWAY, and\n"
        "keep it there. Each candidate fires four times with a gap between.\n"
        "\n"
        "Judge only one thing: does it feel like the weapon KICKED BACK against\n"
        "your finger, or does it feel like a buzz / like nothing?\n"
        "\n"
        "Ctrl+C to stop.\n\n";

    struct Candidate {
        const char* label;
        TriggerCommand kick;
        int kickMs;
        bool settle;
    };
    const Candidate cands[] = {
        {"A  Slope, 90 ms      (what shipped, and felt like nothing)",
         trig::Slope(0, 9, 5, 8), 90, false},
        {"B  Slope, 260 ms     (same mode, long enough for the motor to arrive)",
         trig::Slope(0, 9, 5, 8), 260, false},
        {"C  Weapon, 90 ms     (your suggestion, short)",
         trig::Weapon(2, 7, 8), 90, false},
        {"D  Weapon, 260 ms    (your suggestion, long)",
         trig::Weapon(2, 7, 8), 260, false},
        {"E  Feedback 8, 260 ms (a plain hard wall, for reference)",
         trig::Feedback(0, 8), 260, false},
        {"F  Weapon 260 ms + 16 Hz settle (full two-stage)",
         trig::Weapon(2, 7, 8), 260, true},
    };

    for (const auto& c : cands) {
        if (!g_running) break;
        std::cout << "  " << c.label << std::endl;
        for (int shot = 0; shot < 4 && g_running; ++shot) {
            triggers.PushOverlay(Controller::Right, c.kick, c.kickMs, 9, "lab-kick");
            if (c.settle) {
                triggers.PushOverlay(Controller::Right, trig::Vibration(0, 8, 16),
                                     c.kickMs + 230, 8, "lab-settle");
            }
            // Run well past the effect so the trigger fully returns to rest
            // between shots - otherwise each one starts from the last one's
            // state and the comparison is meaningless.
            for (int i = 0; i < 95 && g_running; ++i) {
                triggers.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        triggers.Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << "\n";
    }

    triggers.Reset();
    std::cout <<
        "Done. Which letter felt most like a weapon kicking back?\n"
        "\n"
        "If the LONG ones (B, D, E) beat the short ones, the motor needs time to\n"
        "travel and per-shot recoil has to be slower than the fire rate allows -\n"
        "which is a real constraint worth knowing.\n"
        "If C beat A, the mode was the problem and duration is fine.\n"
        "If none of them kicked, the trigger cannot do impulses at all and\n"
        "recoil belongs entirely in the grip actuator.\n";
    return 0;
}

// Bottomed-trigger bench.
//
// Reported on hardware: at HALF depth every weapon's recoil pushes back, but
// with the trigger held ALL THE WAY DOWN the shotgun and SMG stop pushing and
// just vibrate into the controller body - while the pistol still kicks a
// little. The pistol is the only one at amplitude 7/8; the other two are at
// 8/8.
//
// Physically that adds up. At the stop there is no travel left, so the motor
// has nothing to move; and at full drive it may simply stall against the stop,
// where a slightly weaker command can still oscillate. That is a hypothesis
// though, and the difference matters because the SMG is precisely the weapon
// players DO hold bottomed out.
//
// So: same conditions, trigger held hard against the stop, one variable at a
// time. Does lower drive push better than higher? Does any resistance mode
// shift the trigger back up when there is nowhere left to press?
int RunDeepTest(TriggerManager& triggers) {
    std::cout <<
        "Bottomed-trigger bench\n"
        "----------------------\n"
        "Hold the RIGHT trigger ALL THE WAY DOWN, hard against the stop, and\n"
        "keep it there for the whole test.\n"
        "\n"
        "For each one, judge only: does it PUSH BACK against your finger, or\n"
        "does it just buzz in the controller body?\n"
        "\n"
        "Ctrl+C to stop.\n\n";

    auto hold = [&](const TriggerCommand& cmd, const char* label, int ms) {
        if (!g_running) return;
        std::cout << "  " << label << std::endl;
        triggers.PushOverlay(Controller::Right, cmd, ms + 150, 9, label);
        for (int i = 0; i < ms / 10 && g_running; ++i) {
            triggers.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        triggers.Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    };

    std::cout << "PART 1 - Does DRIVE STRENGTH change it? All 16 Hz.\n"
                 "If the weaker ones push better, the motor is stalling at full drive.\n\n";
    for (int amp : {8, 7, 6, 5, 4}) {
        if (!g_running) break;
        char label[64];
        std::snprintf(label, sizeof(label), "vibration  strength %d/8", amp);
        hold(trig::Vibration(0, amp, 16), label, 1600);
    }

    std::cout << "\nPART 2 - Does RATE change it? All strength 7.\n\n";
    for (int hz : {8, 12, 16, 24, 35}) {
        if (!g_running) break;
        char label[64];
        std::snprintf(label, sizeof(label), "vibration  %2d Hz", hz);
        hold(trig::Vibration(0, 7, hz), label, 1600);
    }

    std::cout << "\nPART 3 - Can anything SHOVE the trigger back up from the stop?\n"
                 "These are walls placed BEHIND your finger. If the motor can drive\n"
                 "the trigger toward them you will feel it lifting.\n\n";
    hold(trig::Feedback(0, 8), "feedback   wall at 0, max", 1600);
    hold(trig::Feedback(2, 8), "feedback   wall at 2, max", 1600);
    hold(trig::Slope(0, 3, 8, 8), "slope      hard across the first third", 1600);
    hold(trig::Weapon(2, 3, 8), "weapon     narrow band, low", 1600);
    {
        std::array<int, 10> low{};
        for (int i = 0; i < 4; ++i) low[i] = 8;
        hold(trig::MultiFeedback(low), "multifeedback  max on the low half only", 1600);
    }

    triggers.Reset();
    std::cout <<
        "\nDone. Three answers:\n"
        "  1. in part 1, did any strength push back better than 8/8?\n"
        "  2. in part 2, did any rate push better than the rest?\n"
        "  3. in part 3, did ANY of them lift the trigger against your finger,\n"
        "     or did all of them do nothing once you were at the stop?\n"
        "\n"
        "If part 3 is all nothing, that is a hardware limit worth knowing: recoil\n"
        "at full depth can only ever be vibration, and the design should stop\n"
        "trying to shove there.\n";
    return 0;
}

// PCM byte-encoding A/B.
//
// Everything here assumes the toolkit wants signed two's complement samples.
// That assumption has never actually been tested, and it is exactly the kind
// of thing that would produce a persistent, unexplainable "it all feels a bit
// weak" - if the device instead reads the byte as UNSIGNED centred on 128,
// then our -127..127 becomes 129..127, a signal one step either side of
// silence, and no amount of amplitude tuning above it would ever help.
//
// Same tone, same amplitude, two encodings. If B is obviously stronger, the
// whole output stage has been wrong from the start.
int RunPcmFormat(Capi& capi) {
    std::cout <<
        "PCM encoding check\n"
        "------------------\n"
        "The same 200 Hz tone at the same amplitude, encoded two ways.\n"
        "Tell me which is STRONGER, or whether they feel identical.\n\n";

    struct Variant { const char* label; bool unsignedCentred; };
    const Variant variants[] = {
        {"A  signed (what the mixer uses today)", false},
        {"B  unsigned, centred on 128",           true},
    };

    for (int round = 0; round < 2 && g_running; ++round) {
        for (const auto& v : variants) {
            if (!g_running) break;
            std::cout << "  " << v.label << std::endl;
            double phase = 0.0;
            const int chunks = 1500 / (1000 * kChunkSize / kSampleRate); // ~1.5 s
            for (int c = 0; c < chunks && g_running; ++c) {
                if (!Capi::WaitIsReady(capi.WaitForPcm())) continue;
                std::array<uint8_t, kChunkSize> buf{};
                for (int i = 0; i < kChunkSize; ++i) {
                    const double s = std::sin(phase) * 0.9;
                    phase += 2.0 * 3.14159265358979 * 200.0 / kSampleRate;
                    if (v.unsignedCentred) {
                        buf[i] = static_cast<uint8_t>(
                            std::clamp(static_cast<int>(std::lrint(128.0 + s * 127.0)), 0, 255));
                    } else {
                        buf[i] = static_cast<uint8_t>(static_cast<int8_t>(
                            std::clamp(static_cast<int>(std::lrint(s * 127.0)), -127, 127)));
                    }
                }
                capi.WritePcm(Controller::Left, buf);
                capi.WritePcm(Controller::Right, buf);
            }
            // Silence between variants so they do not blur together.
            for (int c = 0; c < 20 && g_running; ++c) {
                if (!Capi::WaitIsReady(capi.WaitForPcm())) continue;
                std::array<uint8_t, kChunkSize> quiet{};
                if (v.unsignedCentred) quiet.fill(128);
                capi.WritePcm(Controller::Left, quiet);
                capi.WritePcm(Controller::Right, quiet);
            }
        }
        std::cout << "  --- repeating ---\n";
    }
    std::cout << "\nIf B was clearly stronger, the output stage has been wrong all along\n"
                 "and that is why everything has felt weak. If they matched, the\n"
                 "encoding is fine and the weakness is elsewhere.\n";
    return 0;
}

int RunTests(IGameAdapter& router, TriggerManager& triggers, Mixer& mixer, const std::string& only) {
    auto names = router.SelfTestNames();
    if (!only.empty()) {
        if (std::find(names.begin(), names.end(), only) == names.end()) {
            std::cout << "Unknown test '" << only << "'. Known tests:\n";
            for (const auto& n : names) std::cout << "  " << n << "\n";
            return 2;
        }
        names = {only};
    }

    std::cout << "Playing " << names.size() << " tactile signature(s).\n"
              << "Hold BOTH controllers. Each line names the hand it was FELT in.\n"
              << "Most signatures alternate hands so localisation is exercised;\n"
              << "a few are deliberately bilateral and will report [both].\n\n";

    size_t index = 0;
    for (const auto& n : names) {
        if (!g_running) break;
        // Alternate so no signature is only ever felt on one side.
        const std::string side = (index++ % 2 == 0) ? "right" : "left";
        // The name goes out first so it is on screen as the effect starts, but
        // the HAND is reported afterwards, because only the router knows where
        // an effect really went. Asking for "right" and announcing "right"
        // while a bilateral effect buzzes both hands is how correct behaviour
        // gets mistaken for a bug.
        std::printf("  %-18s", n.c_str());
        std::fflush(stdout);
        router.RunSelfTest(n, side);
        const uint8_t felt = router.emitTrace();
        const char* where = (felt == 3) ? "both"
                          : (felt == 1) ? "left"
                          : (felt == 2) ? "right"
                                        : "trigger only";
        std::printf("[%s]\n", where);
        std::fflush(stdout);
        // Let the effect play out, ticking the trigger scheduler as the main
        // loop would, so overlays expire and restore exactly as in-game.
        for (int i = 0; i < 90 && g_running; ++i) {
            triggers.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // Proof that PCM actually reached the driver. A zero here means the mixer
    // never ran, which is the failure mode that made every previous build feel
    // like adaptive triggers only.
    std::cout << "\nDone. PCM chunks written: " << mixer.chunksWritten();
    if (mixer.writeErrors() > 0) std::cout << ", write errors: " << mixer.writeErrors();
    std::cout << "\n";
    if (mixer.chunksWritten() == 0) {
        std::cout << "! No PCM was written. Run --probe to see what the driver is doing.\n";
        return 1;
    }
    return 0;
}

// Builds the adapter for the selected game.
//
// This is the ONLY place in the middleware that names a game. Everything else
// - the mixer, the trigger scheduler, recording, replay, analysis, the CLI -
// runs against IGameAdapter and would not notice a third integration being
// added beside these two.
std::unique_ptr<IGameAdapter> MakeAdapter(const Config& cfg, Mixer& mixer,
                                          TriggerManager& triggers,
                                          const Profiles& profiles,
                                          HmdChannel* hmd) {
    if (cfg.isHl2vr()) {
        return std::make_unique<Hl2vrAdapter>(mixer, triggers, cfg, profiles, hmd);
    }
    return std::make_unique<AlyxAdapter>(mixer, triggers, cfg, profiles, hmd);
}

int Main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Flush every line.
    //
    // This process runs for a whole play session and its output is the only
    // record of what happened. Left to the default, iostreams fully buffer when
    // stdout is a pipe or a file rather than a console - so redirecting to a
    // log produced an EMPTY file until a clean exit, and nothing at all if the
    // window was closed or the process killed. That is precisely the case a log
    // is wanted for.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::string configPath;
    std::string testName;
    std::string recordPath;
    std::string replayPath;
    std::string impactsPath;
    std::string profilePath;
    bool doDumpProfiles = false;
    bool doTest = false, doInstallOnly = false, doUninstall = false;
    bool doLaunch = false, noInstall = false, forceDebug = false, doProbe = false;
    bool doSweep = false, doHands = false, doAnalyze = false;
    bool doVerify = false;
    bool doBhapticsScan = false;
    bool doHmdSweep = false, doHmdTest = false;
    bool doTriggerBench = false, doPcmFormat = false, doTriggerSweep = false;
    bool doRecoilLab = false, doDeepTest = false;
    std::string benchWeapon;
    std::string gameOverride;
    // --list-tests needs an adapter to ask, and the adapter needs the config,
    // so it is recorded here and serviced once both exist.
    bool doListTests = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { PrintUsage(); return 0; }
        else if (a == "--version" || a == "-V") { PrintVersion(); return 0; }
        else if (a == "--test") {
            doTest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') testName = argv[++i];
        }
        else if (a == "--list-tests") doListTests = true;
        else if (a == "--record") {
            if (i + 1 < argc) recordPath = argv[++i];
            else { std::cerr << "--record needs a file path\n"; return 2; }
        }
        else if (a == "--replay") {
            if (i + 1 < argc) replayPath = argv[++i];
            else { std::cerr << "--replay needs a file path\n"; return 2; }
        }
        else if (a == "--profiles") {
            if (i + 1 < argc) profilePath = argv[++i];
            else { std::cerr << "--profiles needs a file path\n"; return 2; }
        }
        else if (a == "--dump-profiles") doDumpProfiles = true;
        else if (a == "--triggers") {
            doTriggerBench = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') benchWeapon = argv[++i];
        }
        else if (a == "--pcm-format") doPcmFormat = true;
        else if (a == "--trigger-sweep") doTriggerSweep = true;
        else if (a == "--recoil-lab") doRecoilLab = true;
        else if (a == "--deep-test") doDeepTest = true;
        else if (a == "--probe") doProbe = true;
        else if (a == "--analyze") doAnalyze = true;
        else if (a == "--verify") doVerify = true;
        else if (a == "--bhaptics-scan") doBhapticsScan = true;
        else if (a == "--sweep") doSweep = true;
        else if (a == "--hmd-sweep") doHmdSweep = true;
        else if (a == "--hmd-test") doHmdTest = true;
        else if (a == "--impacts") { if (i + 1 < argc) impactsPath = argv[++i]; }
        else if (a == "--hands") doHands = true;
        else if (a == "--install") doInstallOnly = true;
        else if (a == "--uninstall") doUninstall = true;
        else if (a == "--launch") doLaunch = true;
        else if (a == "--no-install") noInstall = true;
        else if (a == "--debug") forceDebug = true;
        else if (a == "--game") {
            if (i + 1 < argc) gameOverride = argv[++i];
            else {
                std::cerr << "--game needs alyx or hl2vr\n";
                return 2;
            }
        }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << "\n\n";
            PrintUsage();
            return 2;
        }
        else configPath = a;
    }

    // ---- configuration -------------------------------------------------
    Config cfg;
    if (configPath.empty()) {
        // Look next to the executable, then in the working directory. A
        // missing config is not an error: everything below is auto-detected.
        for (const std::string candidate : {ExeDir() + "\\psvr2_haptics.cfg",
                                           ExeDir() + "\\config\\psvr2_haptics.cfg",
                                           std::string("psvr2_haptics.cfg")}) {
            std::ifstream probe(candidate);
            if (probe) { configPath = candidate; break; }
        }
    }
    if (!configPath.empty()) {
        std::string err;
        if (!cfg.Load(configPath, err)) {
            std::cerr << err << "\n";
            return 2;
        }
        std::cout << "Config: " << configPath << "\n";
    } else {
        std::cout << "Config: none found, using auto-detected defaults\n";
    }
    // --game overrides the config file, so one install can drive either game
    // from two shortcuts without editing anything between runs.
    if (!gameOverride.empty()) cfg.game = gameOverride;
    if (forceDebug) cfg.debug = true;

    cfg.AutoDetect();

    // Tactile profiles. A missing file is normal - the built-ins stand.
    Profiles profiles;
    {
        std::vector<std::string> candidates;
        if (!profilePath.empty()) candidates.push_back(profilePath);
        candidates.push_back(ExeDir() + "\\haptic_profiles.cfg");
        candidates.push_back("haptic_profiles.cfg");
        std::vector<std::string> profileWarnings;
        for (const auto& c : candidates) {
            if (profiles.Load(c, profileWarnings)) {
                std::cout << "Profiles: " << c << " (" << profiles.overrideCount()
                          << " effect(s) overridden)\n";
                break;
            }
        }
        for (const auto& w : profileWarnings) std::cout << "  ! " << w << "\n";
    }

    if (doDumpProfiles) {
        const std::string out = profilePath.empty() ? "haptic_profiles.cfg" : profilePath;
        if (!profiles.Dump(out)) {
            std::cerr << "Could not write " << out << "\n";
            return 2;
        }
        std::cout << "Wrote current tactile profiles to " << out << "\n"
                     "Edit it, then use --test <name> to feel a change or --analyze to\n"
                     "measure one. No rebuild needed.\n";
        return 0;
    }

    for (const auto& w : cfg.warnings) std::cout << "  ! " << w << "\n";
    if (!cfg.obsoleteKeys.empty()) {
        std::cout << "  ! ignoring retired config keys (native injection was removed): ";
        for (size_t i = 0; i < cfg.obsoleteKeys.size(); ++i) {
            std::cout << (i ? ", " : "") << cfg.obsoleteKeys[i];
        }
        std::cout << "\n";
    }

    std::cout << "Game: "
              << (cfg.isHl2vr() ? "Half-Life 2 VR" : "Half-Life: Alyx")
              << "\n";

    // --list-tests only needs an adapter, not hardware and not the game, so
    // it is serviced here with a throwaway offline stack.
    if (doListTests) {
        Capi listCapi;
        Mixer listMixer(listCapi, false);
        TriggerManager listTriggers(listCapi, false);
        listTriggers.SetEnabled(false);
        auto listAdapter =
            MakeAdapter(cfg, listMixer, listTriggers, profiles, nullptr);
        for (const auto& t : listAdapter->SelfTestNames()) std::cout << t << "\n";
        return 0;
    }

    // Path validation is skipped for --test: hardware tests do not need the game.
    const bool doReplay = !replayPath.empty();
    if (!impactsPath.empty()) return RunImpactAnalysis(impactsPath);
    if (!doTest && !doProbe && !doSweep && !doHands && !doAnalyze && !doReplay
        && !doTriggerBench && !doPcmFormat && !doTriggerSweep && !doRecoilLab && !doDeepTest
        && !doHmdSweep && !doHmdTest && !doVerify && !doBhapticsScan) {
        std::string err;
        if (!cfg.Validate(err)) {
            // For Half-Life 2 VR, a missing game folder is only fatal when
            // something actually needs it.
            //
            // Both of its live routes are loopback SOCKETS - the game connects
            // to us - so listening needs the game RUNNING, not installed, and
            // not findable on this disk at all. Refusing to start without a
            // path made the one thing a user can test before installing
            // anything impossible to test, which is exactly backwards.
            //
            // Only the console.log fallback needs the folder, so only that is
            // lost.
            const bool needsPath = doInstallOnly || doUninstall || doLaunch;
            if (!cfg.isHl2vr() || needsPath) {
                std::cerr << "\n" << err << "\n";
                return 3;
            }
            std::cout << "Half-Life 2 VR: not found on this machine.\n"
                         "  That is fine for listening: both live routes are local"
                         " sockets\n"
                         "  the game connects to, so it only has to be RUNNING."
                         " Only the\n"
                         "  console.log fallback needs the install folder.\n";
        } else {
            std::cout << (cfg.isHl2vr() ? "Half-Life 2 VR: " : "Half-Life: Alyx: ")
                      << cfg.gamePath() << "\n";
        }
    }

    // ---- game-side deployment -------------------------------------------
    //
    // The two games install completely differently and there is no useful
    // abstraction over it: Alyx gets a folder of Lua that this executable
    // carries inside itself, Half-Life 2 VR gets a descriptor plus a compiled
    // plugin that has to be built separately against the Source SDK.
    if (cfg.isHl2vr()) {
        if (doUninstall) {
            auto r = hl2vr::UninstallPlugin(cfg.hl2vrPath);
            if (!r.ok) { std::cerr << r.error << "\n"; return 4; }
            std::cout << (r.written.empty() ? "Nothing was installed.\n"
                                            : "Removed the plugin and its descriptor.\n");
            return 0;
        }
        if (!noInstall && !doVerify && !doBhapticsScan && !doTest && !doProbe
            && !doSweep && !doHands
            && !doAnalyze
            && !doReplay && !doTriggerBench && !doPcmFormat && !doTriggerSweep
            && !doRecoilLab && !doDeepTest && !doHmdSweep && !doHmdTest) {
            auto r = hl2vr::InstallPlugin(cfg.hl2vrPath);
            if (!r.ok) {
                std::cerr << "\nPlugin install failed.\n  " << r.error << "\n";
                return 4;
            }
            if (r.written.empty()) {
                std::cout << "Plugin: up to date\n";
            } else {
                std::cout << "Plugin: installed/updated " << r.written.size()
                          << " file(s)\n";
            }
            if (r.pluginMissing) {
                // Said plainly rather than buried, because this is the one step
                // that cannot be done for the user and everything downstream
                // depends on it.
                std::cout <<
                    "! The plugin DLL has not been built, so no events will arrive.\n"
                    "  It is a 32-bit Source server plugin and needs the Source SDK\n"
                    "  2013 to compile - see src/games/hl2vr/plugin/README.md.\n"
                    "  Everything else here works without it: --test, --analyze\n"
                    "  and --replay all run the code path the game will drive.\n";
            }
        }
        if (doInstallOnly) {
            std::cout << "\nDone. Start Half-Life 2 VR, then run this again "
                      << "without --install.\n";
            return 0;
        }
    } else if (doUninstall) {
        auto r = UninstallAddon(cfg.hlaPath);
        if (!r.ok) { std::cerr << r.error << "\n"; return 4; }
        std::cout << (r.written.empty() ? "Nothing was installed.\n"
                                        : "Removed: addon folder, game script and manifest entry.\n");
        std::string e;
        std::vector<std::string> remaining;
        if (HasLegacyInstall(cfg.hlaPath, remaining)) {
            if (!RemoveLegacyInstall(cfg.hlaPath, e)) std::cout << "! " << e << "\n";
        }
        return 0;
    }

    if (!cfg.isHl2vr()
        && !noInstall && !doTest && !doProbe && !doSweep && !doHands && !doAnalyze && !doReplay
        && !doVerify && !doBhapticsScan
        && !doTriggerBench && !doPcmFormat && !doTriggerSweep && !doRecoilLab && !doDeepTest
        && !doHmdSweep && !doHmdTest) {
        auto r = InstallAddon(cfg.hlaPath);
        if (!r.ok) {
            std::cerr << "\nAddon install failed.\n  " << r.error << "\n";
            return 4;
        }
        if (r.written.empty()) {
            std::cout << "Addon: up to date\n";
        } else {
            std::cout << "Game script: installed/updated " << r.written.size() << " file(s)\n";
        }

        // Report which route will actually load, because only one of them does.
        std::vector<std::string> loaders;
        if (HasLegacyInstall(cfg.hlaPath, loaders)) {
            std::cout << "Loader: skill_manifest.cfg -> psvr2_haptics.lua  (active route)\n";
        } else {
            std::cout << "! The skill_manifest.cfg entry is missing, so the game script\n"
        "  will NOT load. Close Half-Life: Alyx and run this again.\n";
        }
    }
    if (doInstallOnly) {
        std::cout << "\nDone. Start Half-Life: Alyx, then run this again without --install.\n";
        return 0;
    }

    // ---- offline ---------------------------------------------------------
    // Needs neither hardware nor the game installed - only the game RUNNING,
    // which is a different thing and the reason it sits before every check.
    if (doBhapticsScan) return RunBhapticsScan();

    if (doVerify) {
        Capi verifyCapi; // never loaded; nothing here touches hardware
        Mixer verifyMixer(verifyCapi, false);
        verifyMixer.SetMaster(cfg.master);
        TriggerManager verifyTriggers(verifyCapi, false);
        verifyTriggers.SetEnabled(false);
        auto verifyAdapter =
            MakeAdapter(cfg, verifyMixer, verifyTriggers, profiles, nullptr);
        return RunVerify(*verifyAdapter, verifyMixer);
    }

    // ---- hardware -------------------------------------------------------
    if (doAnalyze) {
        Capi offlineCapi; // never loaded; the offline render does not touch it
        Mixer offlineMixer(offlineCapi, false);
        offlineMixer.SetMaster(cfg.master);
        TriggerManager offlineTriggers(offlineCapi, false);
        offlineTriggers.SetEnabled(false);
        auto offline = MakeAdapter(cfg, offlineMixer, offlineTriggers, profiles, nullptr);
        IGameAdapter& offlineRouter = *offline;
        // --replay --analyze renders a recorded session with no hardware at
        // all, which is what makes a recording useful to someone who cannot
        // put the headset on.
        if (doReplay) return RunReplayAnalysis(offlineRouter, offlineMixer, replayPath);
        return RunAnalyze(offlineRouter, offlineMixer);
    }

    Capi capi;
    std::string err;
    if (!capi.Load(cfg.toolkitDll, err)) {
        std::cerr << "\n" << err << "\n";
        return 5;
    }
    if (!capi.Init(err)) {
        std::cerr << "\n" << err << "\n";
        return 6;
    }
    {
        char narrow[512]{};
        WideCharToMultiByte(CP_UTF8, 0, capi.path().c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        std::cout << "Toolkit: " << narrow << "\n";
    }

    // Measure how this particular toolkit build actually behaves before any
    // audio is generated. See Capi::Calibrate for why this is not optional.
    {
        const auto caps = capi.Calibrate();
        std::cout << "PCM path: " << caps.measuredHz << " Hz"
                  << (caps.waitBlocks ? " (paced correctly)" : " (NOT pacing as expected)")
                  << ", wait-ready code=" << caps.waitReadyValue << "\n";
        if (caps.writeCodesMeaningful) {
            std::cout << "          write_pcm reports success and is being checked\n";
        } else if (caps.writeCodesVaried) {
            std::cout << "          write_pcm returned inconsistent values; not checked\n";
        } else if (caps.writeObserved >= kResultInvalidParameter &&
                   caps.writeObserved <= kResultOk) {
            std::cout << "          write_pcm constantly returns "
                      << caps.writeObserved << " (" << ResultName(caps.writeObserved)
                      << "); ambiguous, so not checked\n";
        } else {
            std::cout << "          write_pcm returns no usable result code in this "
        "toolkit build; not checked\n";
        }
        if (!caps.waitBlocks) {
            std::cout << "! The driver is not pacing PCM at "
                      << (static_cast<double>(kSampleRate) / kChunkSize)
                      << " Hz. PCM haptics may be weak or absent.\n";
        }
    }

    if (doPcmFormat) {
        const int rc = RunPcmFormat(capi);
        capi.Shutdown();
        return rc;
    }
    if (doProbe) {
        const int rc = RunProbe(capi);
        capi.Shutdown();
        return rc;
    }

    Mixer mixer(capi, cfg.debug);
    mixer.SetMaster(cfg.master);
    TriggerManager triggers(capi, cfg.debug);
    triggers.SetMaster(cfg.triggerMaster);
    triggers.SetEnabled(cfg.adaptiveTriggers);
    // Headset channel. Off unless hmd=true, and non-fatal by construction:
    // the controllers must behave identically whether or not it works.
    int exitCode = 0;
    HmdChannel hmdChannel;
    if (cfg.hmd || doHmdSweep || doHmdTest) {
        std::string why;
        if (hmdChannel.Enable(&capi, why)) {
            std::cout << "Headset rumble: ON  (needs a jailbroken headset to be felt)\n";
        } else {
            std::cout << "Headset rumble: unavailable - " << why << "\n";
        }
    }
    auto adapter = MakeAdapter(cfg, mixer, triggers, profiles,
                               hmdChannel.enabled() ? &hmdChannel : nullptr);
    IGameAdapter& router = *adapter;
    mixer.Start();

    if (doHmdSweep || doHmdTest) {
        exitCode = doHmdSweep ? RunHmdSweep(hmdChannel) : RunHmdTest(hmdChannel);
        hmdChannel.Stop();
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doSweep || doHands) {
        exitCode = doSweep ? RunSweep(mixer, triggers) : RunHands(mixer, triggers);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doDeepTest) {
        exitCode = RunDeepTest(triggers);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doRecoilLab) {
        exitCode = RunRecoilLab(triggers);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doTriggerSweep) {
        exitCode = RunTriggerSweep(triggers);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doTriggerBench) {
        exitCode = RunTriggerBench(router, triggers, benchWeapon);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doTest) {
        exitCode = RunTests(router, triggers, mixer, testName);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }
    if (doReplay) {
        exitCode = RunReplay(router, triggers, mixer, replayPath, cfg.debug);
        triggers.Reset();
        mixer.Stop();
        capi.Shutdown();
        return exitCode;
    }

    // ---- run ------------------------------------------------------------
    if (doLaunch) {
        std::string e;
        bool launched = false;
        if (cfg.isHl2vr()) {
            launched = hl2vr::LaunchGame(e);
        } else {
            // Truncated so the tailer never replays the previous session.
            ResetConsoleLog(cfg.hlaPath);
            launched = LaunchGame(e);
        }
        if (launched) {
            std::cout << "Launching " << router.gameName()
                      << " through Steam...\n";
        } else {
            std::cout << "! " << e << "\n";
        }
    }

    // Prefer the Source 2 network console: lower latency than tailing the log,
    // and it lets us push commands back into the game. Falls back silently to
    // console.log so an existing -condebug setup keeps working untouched.
    // Two candidates per game, fast one first.
    //
    // Alyx: the Source 2 network console, falling back to console.log.
    // Half-Life 2 VR: the plugin's own UDP socket, falling back to
    // console.log - the plugin prints every event as well as sending it, so
    // the slow path works even if the socket cannot be opened.
    NetConsole netcon(29000);
    UdpListener udp(29001);
    // Half-Life 2 VR announces its own haptic events to the bHaptics port, and
    // needs nothing installed to do it. Tried first for that reason: it is the
    // only route that works before the server plugin has been built.
    BhapticsListener bhaptics;
    // Empty when the game folder is unknown; LogTail then simply never
    // connects, which is the correct behaviour rather than an error.
    LogTail logtail(cfg.isHl2vr()
                        ? (cfg.hl2vrPath.empty() ? std::string()
                                                 : cfg.hl2vrConsoleLogPath())
                        : cfg.consoleLogPath());
    // Half-Life 2 VR runs BOTH of its routes at once, because neither
    // supersedes the other: the bHaptics stream knows what happened by the
    // developers' own name for it, and the plugin knows the physics behind it.
    // Picking one would throw away what the other knows.
    MultiTransport merged;
    if (cfg.isHl2vr()) {
        merged.Add(&udp, "plugin");
        merged.Add(&bhaptics, "bhaptics");
    }
    Transport& fast = cfg.isHl2vr() ? static_cast<Transport&>(merged)
                                    : static_cast<Transport&>(netcon);
    Transport* transport = nullptr;
    bool announcedTransport = false;

    std::cout << "\nWaiting for " << router.gameName()
              << "...  (Ctrl+C to stop)\n";
    for (const auto& line : router.ConnectionHelp()) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "  log path        : "
              << (cfg.isHl2vr() ? cfg.hl2vrConsoleLogPath() : cfg.consoleLogPath())
              << "\n\n";

    DeliveryMonitor delivery;
    Recorder recorder;
    if (!recordPath.empty()) {
        if (recorder.Open(recordPath)) {
            std::cout << "Recording session to " << recordPath << "\n";
        } else {
            std::cout << "! Could not open " << recordPath << " for recording.\n";
        }
    }
    bool sawScript = false;
    bool warnedNoLog = false;
    auto started = std::chrono::steady_clock::now();
    auto lastStats = started;
    auto lastNetconTry = started;

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();

        // A merged stream is offered the chance to connect EVERY iteration,
        // not only while it has nothing.
        //
        // Its sources come up at different moments - the bHaptics socket the
        // instant the game starts, the plugin only once a level has loaded -
        // and the reconnect logic below stops as soon as the transport reports
        // Connected(). With one source live that returns true, so the other
        // never got a chance and silently stayed dark for the whole session.
        // Found by running both against the middleware at once; it would have
        // shipped as "the bHaptics route just doesn't work when the plugin is
        // installed".
        //
        // Cheap: every source returns immediately once it is connected.
        if (cfg.isHl2vr()) merged.Connect();

        // Reconnect logic: try the fast path first each time we have nothing.
        if (transport == nullptr || !transport->Connected()) {
            if (fast.Connect()) {
                transport = &fast;
            } else if (logtail.Connect()) {
                transport = &logtail;
            } else {
                transport = nullptr;
            }
            if (transport != nullptr && !announcedTransport) {
                announcedTransport = true;
                std::cout << "Connected via " << transport->name() << "\n";
            }
        } else if (transport == &logtail && now - lastNetconTry > std::chrono::seconds(2)) {
            // Keep reaching for the network console even while the log tailer
            // is working.
            //
            // console.log exists from the PREVIOUS session, so the tailer
            // connects instantly - before Half-Life: Alyx has even started,
            // let alone opened its listener. The old logic only tried netcon
            // when it had nothing at all, so that early success permanently
            // locked the session onto the slow path: a recorded session showed
            // "Connected via console.log" long before the game script loaded,
            // and 56 ms of mean jitter for the next eleven minutes.
            //
            // Latency is not a detail here. Events arriving late AND bunched
            // is what turns distinct effects into overlapping mush.
            lastNetconTry = now;
            if (fast.Connect()) {
                transport = &fast;
                std::cout << "Upgraded to " << fast.name()
                          << " - much lower latency\n";
            }
        }

        for (const auto& line : transport ? transport->Poll() : std::vector<std::string>{}) {
            const auto tag = line.find("[PSVR2H]");
            if (tag == std::string::npos) continue;
            const auto restStart = line.find_first_not_of(' ', tag + 8);
            if (restStart == std::string::npos) continue;

            const std::string rest = line.substr(restStart);
            const auto colon = rest.find(':');
            const std::string event = colon == std::string::npos ? rest : rest.substr(0, colon);
            const std::string param = colon == std::string::npos ? std::string{} : rest.substr(colon + 1);

            if (event == "SCRIPT_LOADED") {
                // Alyx reloads init scripts on map change, so this arriving more
                // than once is normal. The Lua side unregisters its previous
                // listeners first, so it does not stack handlers.
                if (cfg.debug || !sawScript) std::cout << "[Alyx] script loaded (" << param << ")\n";
                sawScript = true;
                continue;
            }
            if (event == "READY") {
                std::cout << "[Alyx] connected - haptics live\n";
                router.RefreshWeaponState();
                continue;
            }
            if (event == "ERROR") {
                std::cout << "[Alyx] script error: " << param << "\n";
                continue;
            }
            if (event == "MAP") {
                std::cout << "[Alyx] map: " << param << "\n";
                continue;
            }
            if (event == "TICK") {
                const auto comma = param.find(',');
                if (comma != std::string::npos) {
                    try {
                        delivery.OnTick(std::stoll(param.substr(0, comma)),
                                        std::stod(param.substr(comma + 1)));
                    } catch (...) { /* malformed line, ignore */ }
                }
                continue;
            }
            if (cfg.debug) {
                std::cout << "[Alyx] " << event;
                if (!param.empty()) std::cout << ":" << param;
                std::cout << "\n";
            }
            // Recorded before routing, so a recording captures what the GAME
            // said rather than what this build happened to do with it. That is
            // what lets an old session still exercise new profiles.
            recorder.Write(event, param);
            router.Handle(event, param);
        }

        // A merged stream can gain or lose a source mid-session - the plugin
        // only comes up once a level has loaded, and the game may be restarted
        // while this keeps running. Say so when it changes, because "which
        // routes are live" is the first thing to check when something is
        // missing.
        if (cfg.isHl2vr() && merged.TakeChanged() && announcedTransport) {
            std::cout << "Event sources: " << merged.name() << "\n";
        }

        triggers.Tick();
        hmdChannel.Tick(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        if (transport == nullptr && !warnedNoLog &&
            now - started > std::chrono::seconds(20)) {
            warnedNoLog = true;
            std::cout << "! No connection to Half-Life: Alyx yet.\n"
        "  Add ONE of these to its Steam launch options\n"
        "  (Steam > Library > Half-Life: Alyx > Properties > Launch Options):\n"
        "    -netconport 29000   preferred, lower latency\n"
        "    -condebug           fallback, writes console.log\n";
        }
        if (cfg.debug && now - lastStats > std::chrono::seconds(30)) {
            lastStats = now;
            std::cout << "[Stats] pcm_chunks=" << mixer.chunksWritten()
                      << " pcm_errors=" << mixer.writeErrors()
                      << " trigger_cmds=" << triggers.commandsSent()
                      << " trigger_errors=" << triggers.commandErrors() << "\n";
            delivery.Report(transport ? transport->name() : "none");
        }

        // Wait ON the transport rather than sleeping blindly. The network
        // console returns the instant a byte arrives, so the poll interval
        // stops contributing to latency at all; the log tailer has nothing to
        // select() on and falls back to sleeping for the same interval.
        if (transport != nullptr) transport->WaitForData(cfg.pollMs);
        else std::this_thread::sleep_for(std::chrono::milliseconds(cfg.pollMs));
    }

    std::cout << "\nStopping...\n";
    delivery.Report(transport ? transport->name() : "none");
    if (recorder.open()) {
        std::cout << "Recorded " << recorder.count() << " events to " << recordPath << "\n"
                  << "  Replay it with:  --replay \"" << recordPath << "\"\n";
    }
    hmdChannel.Stop();   // never leave the headset buzzing
    triggers.Reset();
    mixer.Stop();
    capi.Shutdown();
    return exitCode;
}

} // namespace
} // namespace psvr2

int main(int argc, char** argv) { return psvr2::Main(argc, argv); }
