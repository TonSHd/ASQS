// ASQS - shot runner: deterministic execution of a noisy stabilizer circuit
// from a seed. RNG draw order is NORMATIVE (PROOF SPEC):
//   per instruction i, in circuit order:
//     H/S/Sdg/X/Z : apply gate; if bernoulli(p1): pauli = "XYZ"[rand_below(3)]
//                   applied after the gate (Y = px then pz).
//     CNOT        : apply gate; for target in (control, target) order:
//                   if bernoulli(p2): pauli = "XYZ"[rand_below(3)] applied.
//     MR          : measure (1 rng bit iff the tableau branch is random);
//                   then if bernoulli(pm): flip the recorded outcome.
//     MZ          : same as MR without the reset.
//     RESET       : reset (1 rng bit iff random branch); then if bernoulli(pr):
//                   apply X.
// Seed chain (normative):
//   share_hash = SHA256D(header80)
//   seed       = SHA256(share_hash || header80)          (32 bytes)
//   shot_k     = SHA256(seed || LE32(k))                 (32 bytes)
//   prng_seed_k = LE64(shot_k[0..8])
#pragma once
#include "circuit.h"
#include "tableau.h"
#include "json.h"

#include <cstdint>
#include <vector>

namespace asqs {

ShotRecord run_shot(const Circuit& circ, const NoiseConfig& noise, uint64_t prng_seed);

// Deterministic seed chain helpers (shared with the Python auditor spec).
// seed: 32 bytes from (share_hash, header80).
std::vector<uint8_t> derive_seed(const uint8_t share_hash[32], const uint8_t header80[80]);
// shot k prng seed from 32-byte seed.
uint64_t derive_shot_prng_seed(const std::vector<uint8_t>& seed, uint32_t k);

// ShotRecord -> JSON (records: outcomes, detectors, observables, errors).
JValue shot_to_json(const ShotRecord& rec);

// Compute detector/observable bits from a record's outcomes (used by runner
// and by the validator for independent recomputation).
void compute_detectors_observables(const Circuit& circ, const std::vector<uint8_t>& outcomes,
                                   std::vector<uint8_t>& detectors,
                                   std::vector<uint8_t>& observables);

} // namespace asqs
