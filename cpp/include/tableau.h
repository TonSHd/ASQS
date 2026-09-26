// ASQS - Stabilizer tableau engine, qubits n <= 64.
//
// Design (PROOF SPEC normative):
//  - We track ONLY the n stabilizer rows: each row = Pauli product over n
//    qubits packed as two 64-bit masks (x, z) + one sign bit r. Bit i of a
//    mask = qubit i. The state is the simultaneous +1/-1 eigenstate of the
//    n rows (with signs given by r).
//  - Gates are exact Pauli-conjugation row updates (H, S, Sdg, X, Z, CNOT).
//  - Z-measurement of qubit a:
//      * If some stabilizer row has x bit a set -> the state is not a Z_a
//        eigenstate: outcome is RANDOM (one RNG bit). Update: XOR row p into
//        every other row with x bit a set; replace row p by Z_a with r =
//        outcome. (Standard stabilizer-group update; group-correct.)
//      * Else the outcome is DETERMINISTIC: Z_a = (sign) * product of a
//        unique subset of the rows (unique because the rows are independent
//        generators). We find the subset by GF(2) Gaussian elimination over
//        the z-masks and output the XOR of the subset's sign bits. The
//        subset is unique, hence the result is independent of the
//        elimination order (auditors may use any pivot order).
//
// The Python audit tool mirrors this engine exactly (bit-for-bit outputs).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace asqs {

enum class Gate : uint8_t {
    H, S, Sdg, X, Z, CNOT, MR, MZ, RESET
};

struct Instr {
    Gate gate;
    uint8_t a = 0;           // target / control
    uint8_t b = 0;           // second target (CNOT: control=a, target=b)
    uint8_t pad = 0;
    uint32_t meas_index = 0; // assigned during circuit build
};

struct Detector {
    std::vector<uint32_t> meas; // measurement indices with deterministic parity
    std::string name;
};

struct Observable {
    std::vector<uint32_t> meas;
    std::string name;
};

struct Circuit {
    uint32_t num_qubits = 0;
    uint32_t num_measurements = 0;
    std::vector<Instr> instrs;
    std::vector<Detector> detectors;
    std::vector<Observable> observables;

    // Noise attach points (indices into instrs) are implicit: noise is
    // applied per-instruction by the shot runner (see shot.h).
};

// Result of one full circuit execution.
struct ShotRecord {
    std::vector<uint8_t> outcomes;     // num_measurements bits (circuit order)
    std::vector<uint8_t> detectors;    // one bit per detector (parity of its meas)
    std::vector<uint8_t> observables;  // one bit per observable
    std::vector<int32_t> err_instr;    // error realizations (ground truth):
    std::vector<uint8_t> err_qubit;    //   instruction index, qubit, pauli
    std::vector<uint8_t> err_pauli;    //   0=X, 1=Y, 2=Z
};

class Tableau {
public:
    explicit Tableau(int nq) : n_(nq) {
        rows_.assign(size_t(nq), Row{0, 0, 0});
        for (int i = 0; i < nq; ++i) rows_[size_t(i)].z = 1ull << i; // |0...0>
    }

    int n() const { return n_; }

    // Gates (O(n) row ops, no RNG).
    void h(int a);
    void s(int a);
    void sdg(int a);
    void px(int a);
    void pz(int a);
    void cnot(int c, int t);

    // Z-measurement. rng_bit(ctx) is called ONLY on the random branch.
    int mz(int a, int (*rng_bit)(void* ctx), void* ctx);

    // Measure + reset to |0>: returns the pre-reset outcome.
    int mr(int a, int (*rng_bit)(void* ctx), void* ctx);

    // Reset to |0>, discarding the outcome (still consumes an RNG bit on the
    // random branch).
    void reset(int a, int (*rng_bit)(void* ctx), void* ctx);

    // Deterministic Z_a value if no stabilizer anticommutes (else -1).
    // Exposed for tests.
    int z_value_if_deterministic(int a) const;
    bool has_random_bit(int a) const;

    // Stabilizer row accessors (tests/diagnostics).
    uint64_t row_x(int i) const { return rows_[size_t(i)].x; }
    uint64_t row_z(int i) const { return rows_[size_t(i)].z; }
    int row_r(int i) const { return rows_[size_t(i)].r; }

private:
    struct Row {
        uint64_t x;
        uint64_t z;
        int r;
    };
    int n_;
    std::vector<Row> rows_;

    int solve_deterministic(int a) const; // GF(2) elimination; assumes solvable
};

} // namespace asqs
