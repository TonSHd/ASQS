// ASQS - bundled SHA-256 (FIPS 180-4), public-domain style implementation.
// No external crypto dependency; verified against FIPS test vectors in unit tests.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace asqs {

using Bytes = std::vector<uint8_t>;

// One-shot SHA-256.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// Double SHA-256 (Bitcoin-style hash).
void sha256d(const uint8_t* data, size_t len, uint8_t out[32]);

Bytes sha256_vec(const Bytes& data);
Bytes sha256d_vec(const Bytes& data);

// Lowercase hex encoding/decoding. from_hex returns empty Bytes on invalid input.
std::string to_hex(const uint8_t* p, size_t n);
std::string to_hex(const Bytes& b);
Bytes from_hex(const std::string& s);

} // namespace asqs
