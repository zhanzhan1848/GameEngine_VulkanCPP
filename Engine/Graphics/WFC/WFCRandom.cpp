// Engine/Graphics/WFC/WFCRandom.cpp
//
// Task 1 (Phase A.2): xorshift32* implementation.
#include "WFCRandom.h"

namespace primal::graphics::wfc {

u32 WFCRandom::NextU32() {
    // xorshift32* (Marsaglia): one shift triple, then multiply by constant.
    u32 x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return x * 0x9E3779B9u;  // golden ratio constant for mixing
}

u32 WFCRandom::NextRange(u32 n) {
    if (n == 0) return 0;
    // Lemire's debias method: u32 range without modulo bias.
    u64 m = static_cast<u64>(NextU32()) * n;
    u32 l = static_cast<u32>(m);
    if (l < n) {
        u32 t = (0u - n) % n;
        while (l < t) {
            m = static_cast<u64>(NextU32()) * n;
            l = static_cast<u32>(m);
        }
    }
    return static_cast<u32>(m >> 32);
}

f32 WFCRandom::NextF32() {
    // Top 24 bits for mantissa - gives [0, 1).
    return static_cast<f32>(NextU32() >> 8) / static_cast<f32>(1u << 24);
}

bool WFCRandom::NextBool(f32 p_true) {
    return NextF32() < p_true;
}

} // namespace primal::graphics::wfc
