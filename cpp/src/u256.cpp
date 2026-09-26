#include "u256.h"
#include "sha256.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace asqs {

U256 U256::from_u64(uint64_t v) {
    U256 r;
    r.limb[0] = uint32_t(v);
    r.limb[1] = uint32_t(v >> 32);
    return r;
}

U256 U256::max_target() {
    // 0x00000000FFFF0000...0000 -> limbs: limb[0..] little endian 32-bit
    // value = 0xFFFF * 2^208
    U256 r;
    r.limb[6] = 0x0000FFFFu; // 0xFFFF << 32*6 = bits 192..223; plus zero bit 224+
    // Actually: 0xFFFF0000... in BE = 0xFFFF * 2^208.
    // 2^208 = limb index 6 (192..223) bit 208 -> within limb 6 (32*6=192..223).
    // 0xFFFF * 2^208: 0xFFFF occupies bits 208..223 => limb[6] = 0xFFFF << 16 = 0xFFFF0000.
    r.limb[6] = 0xFFFF0000u;
    r.limb[7] = 0;
    return r;
}

U256 U256::saturate_max() {
    U256 r;
    for (auto& l : r.limb) l = 0xFFFFFFFFu;
    return r;
}

bool U256::is_zero() const {
    for (auto l : limb) if (l) return false;
    return true;
}

int U256::cmp(const U256& o) const {
    for (int i = 7; i >= 0; --i) {
        if (limb[i] < o.limb[i]) return -1;
        if (limb[i] > o.limb[i]) return 1;
    }
    return 0;
}

U256 U256::mul_small(uint64_t m) const {
    U256 r;
    uint64_t carry = 0;
    for (int i = 0; i < 8; ++i) {
        uint64_t prod = uint64_t(limb[i]) * m + carry;
        r.limb[i] = uint32_t(prod);
        carry = prod >> 32;
    }
    if (carry != 0) return U256::saturate_max(); // overflow -> saturate
    return r;
}

U256 U256::div_small(uint64_t d) const {
    U256 r;
    if (d == 0) return U256::saturate_max();
    uint64_t rem = 0;
    for (int i = 7; i >= 0; --i) {
        uint64_t cur = (rem << 32) | limb[i];
        r.limb[i] = uint32_t(cur / d);
        rem = cur % d;
    }
    return r;
}

std::array<uint8_t, 32> U256::to_be_bytes() const {
    // limb[0] = least significant 32 bits -> occupies bytes b[28..31] (b[31] = LSB).
    std::array<uint8_t, 32> b{};
    for (int i = 0; i < 8; ++i) {
        b[31 - i * 4] = uint8_t(limb[i]);         // low byte
        b[30 - i * 4] = uint8_t(limb[i] >> 8);
        b[29 - i * 4] = uint8_t(limb[i] >> 16);
        b[28 - i * 4] = uint8_t(limb[i] >> 24);   // MSB of limb
    }
    return b;
}

std::string U256::to_be_hex() const {
    auto b = to_be_bytes();
    return to_hex(b.data(), 32);
}

DiffRat DiffRat::parse(const std::string& s0) {
    std::string s = s0;
    // strip spaces
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    DiffRat d;
    if (s.empty()) return d;
    size_t dot = s.find('.');
    std::string ip = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string fp = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    if (fp.size() > 9) fp = fp.substr(0, 9); // cap 9 decimals
    auto digits_ok = [](const std::string& x) {
        if (x.empty()) return true;
        for (char c : x) if (c < '0' || c > '9') return false;
        return true;
    };
    if (!digits_ok(ip) || !digits_ok(fp)) return d;
    if (ip.empty() && fp.empty()) return d;
    std::string numstr = ip + fp;
    if (numstr.empty()) numstr = "0";
    // strip leading zeros
    size_t nz = numstr.find_first_not_of('0');
    numstr = (nz == std::string::npos) ? "0" : numstr.substr(nz);
    uint64_t num = 0;
    try { num = std::stoull(numstr); } catch (...) { return d; }
    if (num == 0) { d.num = 0; d.den = 1; return d; }
    uint64_t den = 1;
    for (size_t i = 0; i < fp.size(); ++i) {
        if (den > (1ull << 62) / 10) break;
        den *= 10;
    }
    d.num = num;
    d.den = den;
    return d;
}

std::string DiffRat::to_string() const {
    // canonical decimal (no trailing zeros)
    if (num == 0) return "0";
    if (den == 1) return std::to_string(num);
    // compute decimal expansion via long division
    std::string ip = std::to_string(num / den);
    uint64_t rem = num % den;
    if (rem == 0) return ip;
    std::string fp;
    while (rem != 0 && fp.size() < 18) {
        rem *= 10;
        fp.push_back(char('0' + rem / den));
        rem %= den;
    }
    while (!fp.empty() && fp.back() == '0') fp.pop_back();
    if (fp.empty()) return ip;
    return ip + "." + fp;
}

U256 target_from_difficulty(const DiffRat& d) {
    // target = min( floor(MAX_TARGET * den / num), 2^256 - 1 )
    // den is capped <= 10^9 by DiffRat::parse, so MAX_TARGET*den < 2^254 (no overflow).
    if (d.num == 0 || d.den == 0) return U256::saturate_max();
    if (d.den >= (1ull << 32)) return U256::saturate_max(); // safety cap (unreachable)
    U256 scaled = U256::max_target().mul_small(d.den);
    return scaled.div_small(d.num);
}

uint32_t compact_encode(const U256& target) {
    auto b = target.to_be_bytes();
    int first = 0;
    while (first < 32 && b[first] == 0) ++first;
    int size = 32 - first; // significant bytes
    if (size == 0) return 0; // zero target
    uint32_t exponent, mantissa;
    if (size > 3) {
        mantissa = (uint32_t(b[first]) << 16) | (uint32_t(b[first + 1]) << 8) |
                   uint32_t(b[first + 2]);
        exponent = uint32_t(size);
        if ((mantissa & 0x800000u) != 0) { // would collide with sign bit
            mantissa >>= 8;
            exponent += 1;
        }
    } else {
        mantissa = 0;
        for (int i = 0; i < size; ++i) mantissa = (mantissa << 8) | b[first + i];
        mantissa <<= 8 * (3 - size);
        exponent = uint32_t(size);
    }
    if (exponent > 255) return 0;
    return (exponent << 24) | mantissa;
}

U256 compact_decode(uint32_t bits) {
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x007fffffu;
    if (exponent == 0 || mantissa == 0) return U256();
    U256 r = U256::from_u64(mantissa);
    // value = mantissa * 256^(exponent-3)
    for (uint32_t i = 3; i < exponent; ++i) {
        U256 s = r.mul_small(256);
        if (s.cmp(r) < 0) return U256::saturate_max(); // overflow
        r = s;
    }
    return r;
}

} // namespace asqs
