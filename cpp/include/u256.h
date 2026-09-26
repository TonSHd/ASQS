// ASQS - 256-bit unsigned integer + Bitcoin-style difficulty/target math.
// Target formula (PROOF SPEC): target = min( floor(MAX_TARGET * den / num), 2^256 - 1 )
// where difficulty D = num/den as an exact rational (e.g. "0.001" -> 1/1000).
// MAX_TARGET is the Bitcoin mainnet difficulty-1 target:
//   0x00000000FFFF0000....0000  (~ 2^224)
#pragma once
#include <cstdint>
#include <string>
#include <array>

namespace asqs {

struct U256 {
    uint32_t limb[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // little-endian 32-bit limbs

    static U256 from_u64(uint64_t v);
    static U256 max_target();      // Bitcoin bdiff-1 target
    static U256 saturate_max();    // 2^256 - 1

    bool is_zero() const;
    int cmp(const U256& o) const;  // -1, 0, +1
    U256 mul_small(uint64_t m) const;      // result saturates at 2^256-1
    U256 div_small(uint64_t d) const;      // floor division
    std::array<uint8_t, 32> to_be_bytes() const;
    std::string to_be_hex() const;
};

// Exact rational difficulty (num/den, den > 0, both positive).
struct DiffRat {
    uint64_t num = 256;
    uint64_t den = 1;
    static DiffRat parse(const std::string& s);  // "256", "0.001"; empty/invalid -> default
    std::string to_string() const;               // canonical decimal string
    double to_double() const { return double(num) / double(den); }
};

U256 target_from_difficulty(const DiffRat& d);

// Bitcoin "compact" (nbits) encoding. decode->encode roundtrips exactly for
// canonical encodings; encode may lose precision for non-canonical targets
// (nbits is advisory in ASQS; actual share filtering uses the exact target).
uint32_t compact_encode(const U256& target);
U256 compact_decode(uint32_t bits);  // exact value of the compact form

} // namespace asqs
