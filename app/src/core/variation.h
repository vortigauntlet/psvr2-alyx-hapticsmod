// Per-instance variation, shared by every game adapter.
//
// bHaptics authors up to five hand-made variants of each event and picks one at
// random, because replaying a byte-identical waveform makes an effect read as
// canned. Synthesising means we can do it continuously instead: every instance
// is jittered slightly in pitch, length and level.
//
// The amounts are deliberately far below the ~1.5x ratio needed for two pitches
// to read as different, so a hit still lands unmistakably as glass or as stone -
// it just stops feeling like the same recording each time.

#pragma once

#include "core/haptics.h"

#include <cstdint>
#include <vector>

namespace psvr2 {

class Variation {
public:
    // Applies jitter to every voice, in submission order.
    //
    // ORDER IS LOAD-BEARING. One RNG stream runs across the whole session, so
    // the sequence of Apply() calls determines every draw. Two adapters that
    // emit the same voices in a different order produce different measurements,
    // and a test suite whose numbers move when an unrelated case is inserted is
    // not a measurement. See Reseed().
    void Apply(std::vector<Voice>& voices);

    // Returns to the fixed seed.
    //
    // Without this the self-test is not reproducible: Apply() moves length by
    // up to +/-11% and pitch by +/-5.5% from one stream, so inserting a test
    // case shifts the draw for every case after it and the entire table moves.
    // Adding two cases once changed glove-pull from 463 ms to 408 ms without a
    // single value in its profile being touched.
    //
    // That matters because the collision report compares ratios against a 1.5x
    // threshold, and an 11% swing on each of two effects is enough to move a
    // pair across that line in either direction. Pairs would appear and
    // disappear between runs for no reason connected to the design, and any
    // effort spent chasing one of those is spent chasing noise.
    //
    // The variation stays in the signal path, so what is measured is still a
    // real instance of what ships. It is just always the SAME instance.
    void Reseed() { rng_ = kSeed; }

private:
    float Jitter(float amount);

    static constexpr uint32_t kSeed = 0x2545F491u;
    uint32_t rng_ = kSeed;
};

} // namespace psvr2
