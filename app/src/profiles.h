// Editable tactile profiles.
//
// Every waveform in this project used to be a compile-time constant, which put
// a rebuild between "that feels wrong" and trying something else. That is the
// wrong way round: the person who can feel the hardware should be the one
// changing the numbers, and they should hear the result immediately.
//
// So the fixed-shape effects are defined as data. The built-in table below is
// still the default and the program works with no file at all, but a profile
// file can override any named effect, and --dump-profiles writes the current
// built-ins out in exactly that format so there is always a correct starting
// point rather than a blank page.
//
// Effects whose shape is computed from live gameplay values - impact energy,
// caught mass, door weight - stay in code, because there is nothing static to
// put in a file. Their MATERIAL character is tunable here instead.
//
// File format, one effect per section:
//
//     [SHOTGUN_FIRE]
//     transient 200 0.34 16 10
//     body 160 45 0.89 520 300 am:0.3,8 delay:12
//
//   transient <freq> <amp> <ms> <decayMs>
//   body      <f0> <f1> <amp> <ms> <decayMs>
//   texture   <centreHz> <q> <amp> <ms> <decayMs>
//   tone      <f0> <f1> <amp> <ms> <attackMs>
//
// Optional trailing modifiers, any order:
//   am:<depth>,<hz>     tremolo - the strongest differentiator on this hardware
//   fm:<depth>,<hz>     roughness
//   delay:<ms>          offset from the start of the effect

#pragma once

#include "haptics.h"

#include <map>
#include <string>
#include <vector>

namespace psvr2 {

// Names of the overridable effects. Kept as constants because a typo in a
// profile file should be reported, not silently ignored.
extern const char* const kProfileNames[];
extern const int kProfileCount;

class Profiles {
public:
    // Applies overrides from `path` on top of the built-ins. A missing file is
    // not an error - it just means the built-ins stand. Parse problems are
    // appended to `warnings` and never fatal, because a typo in one line
    // should not cost the user every other effect in the file.
    bool Load(const std::string& path, std::vector<std::string>& warnings);

    // Built-in values unless a file overrode them.
    std::vector<Voice> Build(const std::string& name) const;

    // True when `name` came from a file rather than the built-in table.
    bool Overridden(const std::string& name) const;

    // Writes every effect in the current table in the file format above, with
    // explanatory comments. Generated from the live values, so it cannot drift
    // out of step with the code the way a hand-written example would.
    bool Dump(const std::string& path) const;

    int overrideCount() const { return static_cast<int>(overrides_.size()); }

private:
    std::map<std::string, std::vector<Voice>> overrides_;
};

} // namespace psvr2
