#include "variation.h"

#include <algorithm>

namespace psvr2 {

float Variation::Jitter(float amount) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float u = static_cast<float>(rng_ & 0xFFFFFF) / 16777215.0f; // 0..1
    return 1.0f + (u * 2.0f - 1.0f) * amount;
}

void Variation::Apply(std::vector<Voice>& voices) {
    for (auto& v : voices) {
        const float pitch = Jitter(0.055f);
        v.f0 *= pitch;
        v.f1 *= pitch;
        v.amp *= Jitter(0.08f);
        const float stretch = Jitter(0.11f);
        v.length = std::max(1, static_cast<int>(v.length * stretch));
        v.decayTau *= stretch;
        if (v.hold > 0.0f) v.hold *= stretch;
    }
}

} // namespace psvr2
