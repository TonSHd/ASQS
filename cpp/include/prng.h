// ASQS - deterministic PRNG: SplitMix64 seeding + xoroshiro128** stream.
// IMPORTANT: This exact algorithm (constants, draw order) is part of the
// PROOF SPEC (see PROOF_SPEC.md). The Python audit tool mirrors it bit-for-bit.
#pragma once
#include <cstdint>
#include <string>

namespace asqs {

class Prng {
public:
    explicit Prng(uint64_t seed) { reseed(seed); }

    void reseed(uint64_t seed) {
        sm_state_ = seed;
        s_[0] = splitmix64_next(sm_state_);
        s_[1] = splitmix64_next(sm_state_);
        if (s_[0] == 0 && s_[1] == 0) s_[0] = 0x9E3779B97F4A7C15ull;
    }

    // xoroshiro128** next
    uint64_t next_u64() {
        const uint64_t s0 = s_[0];
        uint64_t s1 = s_[1];
        const uint64_t result = rotl(s0 * 5, 7) * 9;
        s1 ^= s0;
        s_[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16);
        s_[1] = rotl(s1, 37);
        return result;
    }

    // Spec: single bit = next_u64() & 1
    int bit() { return int(next_u64() & 1ull); }

    // Spec: uniform choice in [0,k) = (next_u64() >> 32) % k   (k >= 1)
    uint32_t rand_below(uint32_t k) {
        if (k <= 1) return 0;
        return uint32_t((next_u64() >> 32) % k);
    }

    // Spec: double in [0,1) = (next_u64() >> 11) * 2^-53
    double rand_double() {
        return double(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    // Bernoulli(p) draw: one rand_double() compared to p.
    bool bernoulli(double p) { return rand_double() < p; }

private:
    static uint64_t splitmix64_next(uint64_t& state) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    uint64_t sm_state_ = 0;
    uint64_t s_[2] = {0, 0};
};

} // namespace asqs
