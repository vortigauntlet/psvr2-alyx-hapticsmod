#include "profiles.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace psvr2 {

const char* const kProfileNames[] = {
    "PISTOL_FIRE", "SHOTGUN_FIRE", "SMG_FIRE", "GRENADE_FIRE",
    "MELEE_FIRE", "DEFAULT_FIRE",
    "GLOVE_LOCK", "GLOVE_PULL", "GLOVE_CATCH",
    "HURT",
};
const int kProfileCount = static_cast<int>(sizeof(kProfileNames) / sizeof(kProfileNames[0]));

namespace {

// One record per voice, so the built-ins can be printed back out in the same
// format a user would type. Storing them as Voices directly would lose which
// builder made them and with what arguments.
struct Spec {
    const char* kind;
    float a, b, c, d, e;
    float amDepth, amFreq, fmDepth, fmFreq, delayMs;
};

struct Builtin {
    const char* name;
    const char* note;
    std::vector<Spec> voices;
};

// The built-in table. These are the values the project ships with, and the
// same numbers that were arrived at by measurement with --analyze.
const std::vector<Builtin>& BuiltinTable() {
    static const std::vector<Builtin> table = {
        {"PISTOL_FIRE", "A single meaty crack. The middle rung of the length ladder.",
         // Weapons are now separated primarily by DURATION, not pitch.
         //
         // They used to sit at 131/176/542 ms and 335/284/94 Hz. The pistol and
         // SMG were 1.34x apart in length and 1.18x in pitch - both below the
         // ~1.5x ratio skin needs before two vibrations read as different at
         // all. They were not similar; perceptually they were the SAME effect,
         // which is exactly what was reported on hardware.
         //
         // The ladder is now roughly 85 / 175 / 540 ms - each step about 2-3x
         // the last, comfortably past the threshold in both directions.
         {{"transient", 470, 0.34f, 10, 6, 0, 0, 0, 0, 0, 0},
          {"body", 300, 265, 0.86f, 175, 78, 0, 0, 0, 0, 0}}},

        {"SHOTGUN_FIRE", "Heaviest thing in the game: a long fall into the low lobe.",
         {{"transient", 200, 0.34f, 16, 10, 0, 0, 0, 0, 0, 0},
          // Brought down from 0.89. At that level the shotgun drove its own
          // limiter to 0.74 and the empty-chamber variant to 0.67, and what a
          // limiter squashes first is the sharp transient - so the heaviest
          // weapon in the game was paying for its weight by losing its edge,
          // and being flattened toward everything else in the process.
          //
          // This weapon is still far and away the largest thing in the suite
          // on the measure that matters: it carries roughly 2.5x the rms of
          // the pistol over 3x the duration. Weight comes from pitch and
          // length, not from the last notch of level - the same conclusion the
          // trigger work reached independently when 8/8 turned out to push
          // back less than 7.
          {"body", 160, 45, 0.76f, 520, 300, 0, 0, 0, 0, 0}}},

        {"SMG_FIRE", "A short hard spit. The SHORTEST thing in the game.",
         // The bottom rung of the length ladder, at roughly half the pistol.
         //
         // An SMG fires many times a second, so its character comes from the
         // RATE of the shots, not from any one of them - each individual round
         // should be over almost before it registers, so a burst reads as a
         // burst rather than as a smear. The choke group stops consecutive
         // shots summing, which is what makes a short one viable.
         //
         // Full amplitude despite the short length: brevity is the signature,
         // weakness is not. Short-and-quiet is what made the old clicks
         // imperceptible; short-and-hard is a crack.
         {{"transient", 450, 0.38f, 8, 5, 0, 0, 0, 0, 0, 0},
          {"body", 330, 300, 0.92f, 85, 34, 0, 0, 0, 0, 0},
          {"body", 180, 155, 0.34f, 60, 26, 0, 0, 0, 0, 0}}},

        {"GRENADE_FIRE", "Rising rather than falling - a throw, not an impact.",
         {{"body", 150, 350, 0.70f, 300, 160, 0.25f, 9.0f, 0, 0, 0}}},

        {"MELEE_FIRE", "Low and heavy with no metallic edge.",
         {{"transient", 190, 0.32f, 16, 10, 0, 0, 0, 0, 0, 0},
          {"body", 135, 100, 1.02f, 340, 175, 0.30f, 5.5f, 0, 0, 0}}},

        {"DEFAULT_FIRE", "Anything without its own profile.",
         {{"transient", 320, 0.36f, 12, 8, 0, 0, 0, 0, 0, 0},
          {"body", 260, 230, 0.90f, 160, 70, 0, 0, 0, 0, 0}}},

        {"GLOVE_LOCK", "Acquisition: a two-part latch - tick, then it catches.",
         // Reported too weak. Every layer sat at 460-470 Hz, the weakest part
         // of the band, so a cue meant to be small and precise came out as
         // barely there. Now the accents stay bright but the body sits at
         // 300 Hz, the measured peak of the actuator's response.
         //
         // The second tick is the real upgrade: skin resolves TIMING far
         // better than pitch, so two ticks 36 ms apart read unmistakably as
         // "click - locked" where one tone read as a faint buzz.
         //
         // Shortened hard, because it was colliding with the CATCH - the two
         // ends of the game's signature interaction measured 141 ms/289 Hz and
         // 171 ms/268 Hz, inside the ~1.5x ratio skin needs on either axis.
         // They were perceptually the same event. A lock is now a brief high
         // tick-tick; a catch is a long falling snap. Nothing else about the
         // gesture had to change.
         //
         // Then overshot the other way: at a 46 ms body it stopped registering
         // at all on hardware. This is the acquisition cue - the moment an
         // object highlights and becomes grabbable - and it fires constantly,
         // so it must stay small. But small is not the same as absent.
         //
         // Roughly doubled and brought down off the rolloff, which buys real
         // force without much length. It still measures about half the catch,
         // so the two remain clearly separate events.
         //
         // Then overshot the other way twice over. At a 46 ms body it stopped
         // registering at all; doubling it put the QUIETEST cue in the game
         // into the middle of the loudness order, above carrying weight and
         // above a rubber impact.
         //
         // This is the acquisition cue - an object highlighting as grabbable -
         // and it fires constantly while you sweep the room. It belongs at the
         // very bottom of the dynamic range: felt, and nothing more.
         //
         // What rescued it from imperceptibility was PITCH, not level. The
         // dead version sat at 372 Hz, up where the actuator barely displaces.
         // Holding it at ~315 Hz with the double tick intact buys enough force
         // to register at roughly half the amplitude, which is how it can be
         // both the faintest thing here and still there.
         // The double tick was never actually double. Spec carries eleven
         // fields and this row was written with ten, so the 32 ms that was
         // meant to be `delayMs` landed in `fmFreq` - an FM rate with zero
         // depth, which does nothing - and the delay defaulted to 0. Both
         // ticks fired on the same sample and summed into one.
         //
         // That is why this cue kept reading as "a faint buzz" through several
         // rounds of retuning: every round adjusted pitch and level, and the
         // separation that was supposed to be carrying the character was not
         // there to adjust. --dump-profiles showed it plainly once looked at -
         // no `delay:` modifier on the second tick.
         {{"transient", 420, 0.19f, 9, 5, 0, 0, 0, 0, 0, 0},
          {"transient", 390, 0.15f, 8, 5, 0, 0, 0, 0, 0, 32},
          {"body", 330, 300, 0.29f, 88, 42, 0, 0, 0, 0, 0}}},

        {"GLOVE_PULL", "A long RISING sweep. Nothing else in the game rises like this.",
         // The sweep is the CHARACTER of the pull and it is the right shape -
         // but it ends at 430 Hz, and the measured response says force dies as
         // the band climbs. So the gesture was arriving exactly backwards: the
         // climax of the pull, the moment the object is nearly in your hand,
         // was landing in the weakest part of the actuator's range.
         //
         // The project's own rule covers this - character in the accent, FORCE
         // at 150-300 Hz - and the sweep alone had nowhere to put the force.
         //
         // So the sweep keeps rising and a second layer swells underneath it,
         // inside the force band, with a long attack so the tension BUILDS
         // rather than announcing itself. The finger feels the line come taut
         // while the palm hears the pitch climb.
         //
         // The sweep gives up level to pay for it: at 0.56 plus a second voice
         // this limited to 0.71, which squashes the very climb it exists for.
         {{"body", 85, 430, 0.44f, 440, 260, 0.28f, 11.0f, 12.0f, 26.0f, 0},
          {"tone", 175, 265, 0.34f, 430, 210, 0, 0, 0, 0, 0}}},

        {"GLOVE_CATCH", "The capture only - bright and short. Weight arrives separately.",
         // A light catch was reported as borderline. The gesture is right - a
         // bright arrival that falls - but it STARTED at 470 Hz, so the first
         // third of the sweep was spent in the band the hardware cannot drive.
         // Beginning the fall at 330 Hz keeps the same shape and lands all of
         // it inside the usable range. A light catch has almost no mass layer
         // under it, so this sweep is nearly all of what it feels like.
         //
         // Amplitude comes DOWN as the pitch does, which is the whole point:
         // in an efficient part of the band you need less drive, not the same.
         // Holding 0.80 while moving to 330 Hz drove a heavy catch to 0.74 on
         // the limiter, squashing the mass layer underneath it.
         // Shortened, because WEIGHT is what should extend a catch.
         //
         // At 190 ms the bare snap measured 171 ms / 268 Hz and a light catch
         // measured 189 ms / 248 Hz - inside the 1.5x ratio on both axes, so
         // skin reads them as the same event. The mass layer was doing nothing
         // at the light end, which is precisely where most of the game lives.
         //
         // The fix is not to inflate a light catch. It is to make the bare
         // snap honest: when no object resolves, nothing arrived in the hand,
         // so the capture should land and STOP. Every resolved catch then adds
         // its settle underneath and is longer by definition - a ladder that
         // starts at the snap instead of one where two rungs sit on top of
         // each other.
         //
         // Shortening it also buys back headroom the mass layer was fighting
         // for: a heavy catch was limiting to 0.68.
         //
         // Cut to 115 ms first, which overshot: it cleared the light catch and
         // landed on top of GLOVE_LOCK instead (116 ms/267 Hz against
         // 88 ms/316 Hz). The lock is pinned short and faint on purpose, so
         // the snap is what has to move. 165 ms sits nearly two lock-lengths
         // clear while still reading as a capture that stops.
         {{"transient", 470, 0.36f, 11, 6, 0, 0, 0, 0, 0, 0},
          {"body", 330, 215, 0.66f, 165, 78, 0, 0, 0, 0, 0}}},

        {"HURT", "Scaled by how hard you were hit; these are the full-strength values.",
         {{"transient", 240, 0.30f, 18, 12, 0, 0, 0, 0, 0, 0},
          {"body", 130, 95, 0.79f, 190, 110, 0, 0, 0, 0, 0},
          {"texture", 300, 1.1f, 0.14f, 70, 50, 0, 0, 0, 0, 0}}},
    };
    return table;
}

Voice FromSpec(const Spec& s) {
    Voice v;
    const std::string kind = s.kind;
    if (kind == "transient")    v = Transient(s.a, s.b, s.c, s.d);
    else if (kind == "texture") v = Texture(s.a, s.b, s.c, s.d, s.e);
    else if (kind == "tone")    v = Tone(s.a, s.b, s.c, s.d, static_cast<int>(s.e));
    else                        v = Body(s.a, s.b, s.c, s.d, s.e);
    v.amDepth = s.amDepth;
    v.amFreq = s.amFreq;
    v.fmDepth = s.fmDepth;
    v.fmFreq = s.fmFreq;
    if (s.delayMs > 0.0f) v.delay = static_cast<int>(kSampleRate * s.delayMs / 1000.0f);
    return v;
}

std::string Trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool KnownName(const std::string& n) {
    for (int i = 0; i < kProfileCount; ++i) {
        if (n == kProfileNames[i]) return true;
    }
    return false;
}

// "am:0.4,12" -> depth 0.4, freq 12
bool ParsePair(const std::string& body, float& x, float& y) {
    const auto comma = body.find(',');
    if (comma == std::string::npos) return false;
    try {
        x = std::stof(body.substr(0, comma));
        y = std::stof(body.substr(comma + 1));
    } catch (...) { return false; }
    return true;
}

} // namespace

bool Profiles::Load(const std::string& path, std::vector<std::string>& warnings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Same BOM tolerance as the main config: PowerShell writes one by default
    // and it would otherwise break the very first section header.
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    std::istringstream stream(content);
    std::string line, section;
    int lineNo = 0;
    std::map<std::string, std::vector<Voice>> parsed;

    while (std::getline(stream, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string::npos) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unterminated section header");
                continue;
            }
            section = Trim(line.substr(1, close - 1));
            if (!KnownName(section)) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unknown effect '" + section + "', ignored");
                section.clear();
            }
            continue;
        }
        if (section.empty()) continue;

        std::istringstream ls(line);
        Spec s{};
        std::string kind;
        ls >> kind;
        s.kind = nullptr;
        static const char* kKinds[] = {"transient", "body", "texture", "tone"};
        for (const char* k : kKinds) {
            if (kind == k) { s.kind = k; break; }
        }
        if (s.kind == nullptr) {
            warnings.push_back("profiles line " + std::to_string(lineNo) +
                               ": unknown voice type '" + kind + "'");
            continue;
        }

        // transient takes four numbers, everything else takes five.
        const int wanted = (kind == "transient") ? 4 : 5;
        float nums[5]{};
        int got = 0;
        while (got < wanted && (ls >> nums[got])) ++got;
        if (got < wanted) {
            warnings.push_back("profiles line " + std::to_string(lineNo) + ": " + kind +
                               " needs " + std::to_string(wanted) + " numbers, got " +
                               std::to_string(got));
            continue;
        }
        if (kind == "transient") { s.a = nums[0]; s.b = nums[1]; s.c = nums[2]; s.d = nums[3]; }
        else { s.a = nums[0]; s.b = nums[1]; s.c = nums[2]; s.d = nums[3]; s.e = nums[4]; }

        std::string mod;
        while (ls >> mod) {
            const auto colon = mod.find(':');
            if (colon == std::string::npos) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": bad modifier '" + mod + "'");
                continue;
            }
            const std::string key = mod.substr(0, colon);
            const std::string val = mod.substr(colon + 1);
            if (key == "am") {
                if (!ParsePair(val, s.amDepth, s.amFreq)) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": am needs depth,hz");
                }
            } else if (key == "fm") {
                if (!ParsePair(val, s.fmDepth, s.fmFreq)) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": fm needs depth,hz");
                }
            } else if (key == "delay") {
                try { s.delayMs = std::stof(val); }
                catch (...) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": delay needs a number");
                }
            } else {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unknown modifier '" + key + "'");
            }
        }
        parsed[section].push_back(FromSpec(s));
    }

    // A section that appeared but produced no usable voice would silently
    // mute that effect, which is a far worse outcome than ignoring the
    // section. Only non-empty overrides are taken.
    for (auto& [name, voices] : parsed) {
        if (voices.empty()) {
            warnings.push_back("profiles: [" + name +
                               "] had no valid voices, keeping the built-in");
            continue;
        }
        overrides_[name] = std::move(voices);
    }
    return true;
}

bool Profiles::Overridden(const std::string& name) const {
    return overrides_.find(name) != overrides_.end();
}

std::vector<Voice> Profiles::Build(const std::string& name) const {
    const auto it = overrides_.find(name);
    if (it != overrides_.end()) return it->second;
    for (const auto& b : BuiltinTable()) {
        if (name == b.name) {
            std::vector<Voice> out;
            out.reserve(b.voices.size());
            for (const auto& s : b.voices) out.push_back(FromSpec(s));
            return out;
        }
    }
    return {};
}

bool Profiles::Dump(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;

    f << "# PSVR2 Alyx Haptics - tactile profiles\n"
         "#\n"
         "# These are the values the program currently uses. Edit them, then run\n"
         "#   psvr2_alyx_haptics.exe --test <name>\n"
         "# to feel the change, or --analyze to measure it. No rebuild needed.\n"
         "#\n"
         "#   transient <freq> <amp> <ms> <decayMs>\n"
         "#   body      <f0> <f1> <amp> <ms> <decayMs>\n"
         "#   texture   <centreHz> <q> <amp> <ms> <decayMs>\n"
         "#   tone      <f0> <f1> <amp> <ms> <attackMs>\n"
         "#\n"
         "# Optional trailing modifiers: am:<depth>,<hz>  fm:<depth>,<hz>  delay:<ms>\n"
         "#\n"
         "# Things worth knowing before you turn a dial:\n"
         "#\n"
         "#   * The actuator produces NOTHING above about 520 Hz. A frequency above\n"
         "#     that is not a bright effect, it is a silent one.\n"
         "#   * There is a real dip at 80-110 Hz and a peak at 180-300 Hz.\n"
         "#   * Pitch alone is coarse: two frequencies need roughly a 1.5x ratio\n"
         "#     before they read as different. Length and tremolo (am:) separate\n"
         "#     effects far better than pitch does.\n"
         "#   * Effects shorter than ~100 ms cannot carry a pitch at all; they all\n"
         "#     read as 'a tap'.\n"
         "#   * If --analyze shows the limiter well below 1.0, the effect is being\n"
         "#     squashed and will feel more like everything else, not less.\n"
         "\n";

    for (const auto& b : BuiltinTable()) {
        f << "# " << b.note << "\n[" << b.name << "]\n";
        const auto it = overrides_.find(b.name);
        const bool custom = it != overrides_.end();
        if (custom) {
            // Dump what is actually in force, not the built-in it replaced.
            for (const auto& v : it->second) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "body %.1f %.1f %.3f %.1f %.1f",
                              v.f0, v.f1, v.amp,
                              v.length * 1000.0f / kSampleRate,
                              v.decayTau * 1000.0f / kSampleRate);
                f << buf;
                if (v.amDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " am:%.2f,%.1f", v.amDepth, v.amFreq);
                    f << buf;
                }
                if (v.fmDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " fm:%.1f,%.1f", v.fmDepth, v.fmFreq);
                    f << buf;
                }
                f << "\n";
            }
        } else {
            for (const auto& s : b.voices) {
                char buf[256];
                if (std::string(s.kind) == "transient") {
                    std::snprintf(buf, sizeof(buf), "transient %.1f %.3f %.1f %.1f",
                                  s.a, s.b, s.c, s.d);
                } else {
                    std::snprintf(buf, sizeof(buf), "%s %.1f %.1f %.3f %.1f %.1f",
                                  s.kind, s.a, s.b, s.c, s.d, s.e);
                }
                f << buf;
                if (s.amDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " am:%.2f,%.1f", s.amDepth, s.amFreq);
                    f << buf;
                }
                if (s.fmDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " fm:%.1f,%.1f", s.fmDepth, s.fmFreq);
                    f << buf;
                }
                if (s.delayMs > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " delay:%.1f", s.delayMs);
                    f << buf;
                }
                f << "\n";
            }
        }
        f << "\n";
    }
    return f.good();
}

} // namespace psvr2
