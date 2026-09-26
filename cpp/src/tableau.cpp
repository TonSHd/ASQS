#include "tableau.h"

namespace asqs {

static inline int bit(uint64_t m, int i) { return int((m >> i) & 1ull); }

void Tableau::h(int a) {
    for (auto& row : rows_) {
        uint64_t xa = (row.x >> a) & 1ull;
        uint64_t za = (row.z >> a) & 1ull;
        row.r ^= int(xa & za);   // Y on a -> -Y under H
        row.x = (row.x & ~(1ull << a)) | (za << a);
        row.z = (row.z & ~(1ull << a)) | (xa << a);
    }
}

void Tableau::s(int a) {
    for (auto& row : rows_) {
        uint64_t xa = (row.x >> a) & 1ull;
        uint64_t za = (row.z >> a) & 1ull;
        row.r ^= int(xa & za);   // Y on a -> -X
        row.z ^= (xa << a);      // z_a ^= x_a  (X -> Y, Y -> X)
    }
}

void Tableau::sdg(int a) {
    for (auto& row : rows_) {
        uint64_t xa = (row.x >> a) & 1ull;
        uint64_t za = (row.z >> a) & 1ull;
        row.r ^= int(xa & (za ^ 1ull)); // X on a -> -Y
        row.z ^= (xa << a);             // X -> Y, Y -> X
    }
}

void Tableau::px(int a) {
    for (auto& row : rows_) row.r ^= bit(row.z, a); // Z components flip sign
}

void Tableau::pz(int a) {
    for (auto& row : rows_) row.r ^= bit(row.x, a); // X components flip sign
}

void Tableau::cnot(int c, int t) {
    for (auto& row : rows_) {
        uint64_t xc = (row.x >> c) & 1ull;
        uint64_t zt = (row.z >> t) & 1ull;
        row.x ^= (xc << t);   // x_t ^= x_c
        row.z ^= (zt << c);   // z_c ^= z_t
    }
}

bool Tableau::has_random_bit(int a) const {
    for (const auto& row : rows_) if (bit(row.x, a)) return true;
    return false;
}

int Tableau::mz(int a, int (*rng_bit)(void*), void* ctx) {
    int p = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (bit(rows_[i].x, a)) { p = int(i); break; }
    }
    if (p >= 0) {
        // Random branch.
        int outcome = rng_bit(ctx);
        const Row src = rows_[size_t(p)];
        for (auto& row : rows_) {
            if (bit(row.x, a) && (&row != &rows_[size_t(p)])) {
                row.x ^= src.x;
                row.z ^= src.z;
                row.r ^= src.r;
            }
        }
        rows_[size_t(p)] = Row{0, 1ull << a, outcome};
        return outcome;
    }
    // Deterministic branch: unique subset product equals Z_a.
    return solve_deterministic(a);
}

int Tableau::mr(int a, int (*rng_bit)(void*), void* ctx) {
    int outcome = mz(a, rng_bit, ctx);
    if (outcome) px(a);
    return outcome;
}

void Tableau::reset(int a, int (*rng_bit)(void*), void* ctx) {
    int outcome = mz(a, rng_bit, ctx);
    if (outcome) px(a);
}

int Tableau::z_value_if_deterministic(int a) const {
    if (has_random_bit(a)) return -1;
    return solve_deterministic(a);
}

int Tableau::solve_deterministic(int a) const {
    // Find the unique subset of rows whose FULL Pauli vectors (x,z) XOR to
    // (0, e_a); return the XOR of their sign bits (= eigenvalue bit of Z_a).
    // NOTE: constraining only z-masks is WRONG (two rows may share a z-pattern
    // while differing in x; then the z-only subset is not unique).
    //
    // Method: phase 1 RREF over all 2n columns (column c<n = x bit c,
    // c>=n = z bit c-n), one pivot per row, pivot column cleared from all
    // other rows. Phase 2: eliminate the target (0, e_a) against pivot rows
    // in ascending column order; pivot rows have 0 at all other pivot
    // columns, so earlier pivot bits are never reintroduced; free columns
    // self-cancel iff the target is in the span. The subset (hence sign) is
    // unique, so any pivot order gives the same result.
    const int n = n_;
    if (a < 0 || a >= n) return 0;
    uint64_t wx[64], wz[64];
    int wr[64];
    for (int i = 0; i < n; ++i) {
        wx[i] = rows_[size_t(i)].x;
        wz[i] = rows_[size_t(i)].z;
        wr[i] = rows_[size_t(i)].r;
    }
    auto has_bit = [&](int i, int col) -> bool {
        return (col < n) ? ((wx[i] >> col) & 1ull) != 0
                         : ((wz[i] >> (col - n)) & 1ull) != 0;
    };
    int pivot_row_of[128];
    for (int c = 0; c < 2 * n; ++c) pivot_row_of[c] = -1;
    uint64_t used = 0;
    for (int c = 0; c < 2 * n; ++c) {
        int piv = -1;
        for (int i = 0; i < n; ++i) {
            if (!((used >> i) & 1ull) && has_bit(i, c)) { piv = i; break; }
        }
        if (piv < 0) continue;
        used |= 1ull << piv;
        pivot_row_of[c] = piv;
        for (int i = 0; i < n; ++i) {
            if (i != piv && has_bit(i, c)) {
                wx[i] ^= wx[piv];
                wz[i] ^= wz[piv];
                wr[i] ^= wr[piv];
            }
        }
    }
    uint64_t tx = 0, tz = 1ull << a;
    int tr = 0;
    auto t_has = [&](int col) -> bool {
        return (col < n) ? ((tx >> col) & 1ull) != 0
                         : ((tz >> (col - n)) & 1ull) != 0;
    };
    for (int c = 0; c < 2 * n; ++c) {
        if (t_has(c) && pivot_row_of[c] >= 0) {
            int p = pivot_row_of[c];
            tx ^= wx[p];
            tz ^= wz[p];
            tr ^= wr[p];
        }
    }
    if (tx != 0 || tz != 0) return 0; // Z_a not in group: invalid state (defensive)
    return tr;
}

} // namespace asqs
