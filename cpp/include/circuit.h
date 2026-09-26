// ASQS - circuit library: rotated surface code memory experiments (d=3,5)
// and seeded random Clifford benchmark circuits.
//
// Surface code construction (normative, PROOF SPEC):
//  - Data qubits: (r,c) for 0<=r,c<d, index r*d+c.
//  - Inner face F(a,b), 0<=a,b<=d-2 (unit square with corners (a,b),(a+1,b),
//    (a,b+1),(a+1,b+1)):
//      X-check if a+b even, Z-check if a+b odd.
//  - Boundary stubs (weight 2):
//      TOP    X-stub: {(0,c),(0,c+1)}     for odd  c
//      BOTTOM X-stub: {(d-1,c),(d-1,c+1)} for even c
//      LEFT   Z-stub: {(r,0),(r+1,0)}     for even r
//      RIGHT  Z-stub: {(r,d-1),(r+1,d-1)} for odd  r
//  - Qubit ids: data 0..d^2-1; X-ancillas next (in check enumeration order:
//    inner faces row-major, then top stubs c asc, then bottom stubs c asc);
//    Z-ancillas after (inner faces row-major, then left stubs r asc, then
//    right stubs r asc).
//  - Per round: for each X-ancilla (order above): H(a); CNOT(a,t) for each
//    t in support (sorted by data qubit index); H(a); MR(a).
//    Then for each Z-ancilla (order above): CNOT(t,a) for each t in support
//    (sorted); MR(a).
//  - After R rounds: MZ on every data qubit in index order.
//  - Detectors: (i) each ancilla round-overlap parity (rounds >= 2);
//    (ii) each Z-ancilla first-round value; (iii) each Z-ancilla last round
//    XOR final data parity over its support. Observable: final data parity
//    of row 0 (logical Z).
// The builder SELF-VALIDATES (commutation, independence, logical independence)
// and throws on any failure.
#pragma once
#include "tableau.h"
#include "json.h"

#include <stdexcept>
#include <string>

namespace asqs {

struct NoiseConfig {
    // Parts per billion (integer, exact rational semantics: p = ppb * 1e-9).
    // Defaults: p1=5e-4, p2=1e-3, pm=5e-4, pr=5e-4 (typical below-threshold regime).
    int64_t p1_ppb = 500000;
    int64_t p2_ppb = 1000000;
    int64_t pm_ppb = 500000;
    int64_t pr_ppb = 500000;
    bool any() const { return p1_ppb || p2_ppb || pm_ppb || pr_ppb; }
    static NoiseConfig from_json(const JValue& j);
    JValue to_json() const;
};

struct CircuitConfig {
    std::string type = "surface_code_memory"; // or "random_clifford"
    int d = 3;            // surface code distance (odd, 3 or 5)
    int rounds = 3;       // surface code rounds
    int qubits = 24;      // random_clifford
    int gates = 120;      // random_clifford
    NoiseConfig noise;

    static CircuitConfig from_json(const JValue& j, std::string& err);
    JValue to_json() const; // circuit identity only (no noise)
    std::string sha256_hex() const; // sha256 of canonical identity json
};

// Build a circuit from config. Throws std::runtime_error on invalid config
// or if the self-validation fails.
Circuit build_circuit(const CircuitConfig& cfg);

// Surface-code self-check (exposed for tests): true iff X/Z check sets
// commute pairwise, are each independent, and logical Z (row 0) commutes
// with all X checks and is outside the Z-check span.
bool surface_code_selfcheck(int d, std::string* err_out = nullptr);

// Full circuit program serialization (normative encoding, PROOF_SPEC §13).
// Emits the exact instruction list so an independent verifier can
// reconstruct the circuit without re-implementing the builder:
//   { encoding:"asqs.program/1", num_qubits, num_measurements,
//     operations:[{"gate":"H","qubit":3},
//                 {"gate":"CNOT","control":3,"target":7},
//                 {"gate":"MZ","qubit":0,"meas_index":0}, ...],
//     detectors:[{name,meas[]}...], observables:[{name,meas[]}],
//     provenance:{generator, circuit_sha256, construction, seed derivation} }
// The hash of the canonical serialization pins the program itself.
JValue circuit_program_json(const CircuitConfig& cfg, const Circuit& circ);
std::string circuit_program_sha256_hex(const JValue& program);

// Machine-readable noise-model semantics (normative, PROOF_SPEC §14):
// the ppb values PLUS the exact rules for how they are applied. Two
// simulators reading the same record must interpret the numbers the same
// way; this block makes that interpretation self-describing.
JValue noise_model_json(const NoiseConfig& n);

// Gate name table (normative): gate enum -> canonical string.
const char* gate_name(Gate g);

} // namespace asqs
