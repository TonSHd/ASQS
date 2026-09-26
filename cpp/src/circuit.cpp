#include "circuit.h"
#include "prng.h"
#include "sha256.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace asqs {

// ---------------- noise config ----------------

NoiseConfig NoiseConfig::from_json(const JValue& j) {
    NoiseConfig n;
    if (const JValue* v = j.get("p1_ppb")) n.p1_ppb = v->as_int(n.p1_ppb);
    if (const JValue* v = j.get("p2_ppb")) n.p2_ppb = v->as_int(n.p2_ppb);
    if (const JValue* v = j.get("pm_ppb")) n.pm_ppb = v->as_int(n.pm_ppb);
    if (const JValue* v = j.get("pr_ppb")) n.pr_ppb = v->as_int(n.pr_ppb);
    return n;
}

JValue NoiseConfig::to_json() const {
    JValue j = JValue::mk_obj();
    j.set("p1_ppb", JValue::mk_int(p1_ppb));
    j.set("p2_ppb", JValue::mk_int(p2_ppb));
    j.set("pm_ppb", JValue::mk_int(pm_ppb));
    j.set("pr_ppb", JValue::mk_int(pr_ppb));
    return j;
}

// ---------------- circuit config ----------------

JValue CircuitConfig::to_json() const {
    JValue j = JValue::mk_obj();
    j.set("type", JValue::mk_str(type));
    if (type == "surface_code_memory") {
        j.set("d", JValue::mk_int(d));
        j.set("rounds", JValue::mk_int(rounds));
    } else {
        j.set("qubits", JValue::mk_int(qubits));
        j.set("gates", JValue::mk_int(gates));
    }
    return j;
}

std::string CircuitConfig::sha256_hex() const {
    std::string canon = jcanonical(to_json());
    uint8_t h[32];
    sha256((const uint8_t*)canon.data(), canon.size(), h);
    return to_hex(h, 32);
}

CircuitConfig CircuitConfig::from_json(const JValue& j, std::string& err) {
    CircuitConfig c;
    c.type = j.get("type") ? j.get("type")->as_str("surface_code_memory") : "surface_code_memory";
    if (j.get("d")) c.d = int(j.get("d")->as_int(3));
    if (j.get("rounds")) c.rounds = int(j.get("rounds")->as_int(3));
    if (j.get("qubits")) c.qubits = int(j.get("qubits")->as_int(24));
    if (j.get("gates")) c.gates = int(j.get("gates")->as_int(120));
    if (j.get("noise")) c.noise = NoiseConfig::from_json(*j.get("noise"));
    if (c.type != "surface_code_memory" && c.type != "random_clifford") {
        err = "unknown circuit type: " + c.type;
        return c;
    }
    if (c.type == "surface_code_memory") {
        if (c.d != 3 && c.d != 5) { err = "surface code d must be 3 or 5"; return c; }
        if (c.rounds < 1 || c.rounds > 32) { err = "rounds must be in [1,32]"; return c; }
        if (c.d == 5 && c.rounds > 16) { err = "d=5 rounds must be <= 16"; return c; }
    } else {
        if (c.qubits < 2 || c.qubits > 64) { err = "random_clifford qubits must be in [2,64]"; return c; }
        if (c.gates < 1 || c.gates > 100000) { err = "random_clifford gates must be in [1,100000]"; return c; }
    }
    if (c.noise.p1_ppb < 0 || c.noise.p1_ppb > 1000000000 ||
        c.noise.p2_ppb < 0 || c.noise.p2_ppb > 1000000000 ||
        c.noise.pm_ppb < 0 || c.noise.pm_ppb > 1000000000 ||
        c.noise.pr_ppb < 0 || c.noise.pr_ppb > 1000000000) {
        err = "noise ppb values must be in [0, 1e9]";
    }
    return c;
}

// ---------------- GF(2) helpers ----------------
namespace {

// rank of a set of 64-bit row masks
int gf2_rank(std::vector<uint64_t> rows) {
    int rank = 0;
    for (int bit = 63; bit >= 0 && rank < (int)rows.size(); --bit) {
        int piv = -1;
        for (size_t i = rank; i < rows.size(); ++i)
            if ((rows[i] >> bit) & 1ull) { piv = int(i); break; }
        if (piv < 0) continue;
        std::swap(rows[size_t(rank)], rows[size_t(piv)]);
        for (size_t i = 0; i < rows.size(); ++i)
            if (i != size_t(rank) && ((rows[i] >> bit) & 1ull)) rows[i] ^= rows[size_t(rank)];
        ++rank;
    }
    return rank;
}

// is target in the span of rows?
bool gf2_in_span(const std::vector<uint64_t>& rows, uint64_t target) {
    if (target == 0) return true;
    std::vector<uint64_t> r = rows;
    r.push_back(target);
    int rank_with = gf2_rank(r);
    int rank_without = gf2_rank(rows);
    return rank_with == rank_without;
}

struct Check {
    char type; // 'X' or 'Z'
    std::vector<int> qubits; // data qubit indices (r*d+c)
};

// Build the surface code check sets per the normative construction.
struct SurfaceChecks {
    int d;
    std::vector<Check> xchecks, zchecks;
    std::vector<int> logical_row0; // data qubits of row 0
};

SurfaceChecks build_checks(int d) {
    SurfaceChecks sc;
    sc.d = d;
    auto data_id = [d](int r, int c) { return r * d + c; };
    // Inner faces, row-major: X if a+b even, Z if odd.
    for (int a = 0; a <= d - 2; ++a) {
        for (int b = 0; b <= d - 2; ++b) {
            Check ck;
            ck.type = ((a + b) % 2 == 0) ? 'X' : 'Z';
            ck.qubits = {data_id(a, b), data_id(a + 1, b), data_id(a, b + 1), data_id(a + 1, b + 1)};
            (ck.type == 'X' ? sc.xchecks : sc.zchecks).push_back(std::move(ck));
        }
    }
    // Top X-stubs (c odd), bottom X-stubs (c even).
    for (int c = 0; c <= d - 2; ++c) {
        if (c % 2 == 1) {
            Check ck; ck.type = 'X';
            ck.qubits = {data_id(0, c), data_id(0, c + 1)};
            sc.xchecks.push_back(std::move(ck));
        }
        if (c % 2 == 0) {
            Check ck; ck.type = 'X';
            ck.qubits = {data_id(d - 1, c), data_id(d - 1, c + 1)};
            sc.xchecks.push_back(std::move(ck));
        }
    }
    // Left Z-stubs (r even), right Z-stubs (r odd).
    for (int r = 0; r <= d - 2; ++r) {
        if (r % 2 == 0) {
            Check ck; ck.type = 'Z';
            ck.qubits = {data_id(r, 0), data_id(r + 1, 0)};
            sc.zchecks.push_back(std::move(ck));
        }
        if (r % 2 == 1) {
            Check ck; ck.type = 'Z';
            ck.qubits = {data_id(r, d - 1), data_id(r + 1, d - 1)};
            sc.zchecks.push_back(std::move(ck));
        }
    }
    for (int c = 0; c < d; ++c) sc.logical_row0.push_back(data_id(0, c));
    return sc;
}

// Mask over data qubits (d*d <= 25 bits for d=5).
uint64_t check_mask(const Check& ck) {
    uint64_t m = 0;
    for (int q : ck.qubits) m |= 1ull << q;
    return m;
}

} // namespace

bool surface_code_selfcheck(int d, std::string* err_out) {
    auto fail = [&](const std::string& m) {
        if (err_out) *err_out = m;
        return false;
    };
    if (d != 3 && d != 5) return fail("d must be 3 or 5");
    SurfaceChecks sc = build_checks(d);
    size_t want = size_t((d * d - 1) / 2);
    if (sc.xchecks.size() != want || sc.zchecks.size() != want)
        return fail("check count mismatch");
    // Commutation: every X check overlaps every Z check on an even number of
    // qubits.
    for (auto& xc : sc.xchecks) {
        for (auto& zc : sc.zchecks) {
            uint64_t m = check_mask(xc) & check_mask(zc);
            if (__builtin_parityll(m) != 0)
                return fail("X/Z check pair anticommutes");
        }
    }
    // Independence: rank == count for each set.
    std::vector<uint64_t> xm, zm;
    for (auto& c : sc.xchecks) xm.push_back(check_mask(c));
    for (auto& c : sc.zchecks) zm.push_back(check_mask(c));
    if (gf2_rank(xm) != (int)xm.size()) return fail("X checks dependent");
    if (gf2_rank(zm) != (int)zm.size()) return fail("Z checks dependent");
    // Logical row 0: commutes with all X checks (even overlap)...
    uint64_t lz = 0;
    for (int q : sc.logical_row0) lz |= 1ull << q;
    for (auto& c : sc.xchecks) {
        if (__builtin_parityll(check_mask(c) & lz) != 0)
            return fail("logical Z anticommutes with an X check");
    }
    // ...and is NOT in the Z-check span (it is a nontrivial logical).
    if (gf2_in_span(zm, lz)) return fail("logical Z is in the Z-check span");
    return true;
}

Circuit build_circuit(const CircuitConfig& cfg) {
    if (cfg.type == "surface_code_memory") {
        std::string err;
        if (!surface_code_selfcheck(cfg.d, &err))
            throw std::runtime_error("surface code self-check failed: " + err);
        SurfaceChecks sc = build_checks(cfg.d);
        int d = cfg.d;
        int ndata = d * d;
        int nx = int(sc.xchecks.size());
        int nz = int(sc.zchecks.size());
        Circuit circ;
        circ.num_qubits = uint32_t(ndata + nx + nz);

        // Measurement bookkeeping: meas[anc][round], final data meas.
        // anc index space: X ancillas 0..nx-1, Z ancillas nx..nx+nz-1.
        auto x_anc = [ndata](int i) { return ndata + i; };
        auto z_anc = [ndata, nx](int i) { return ndata + nx + i; };
        std::vector<std::vector<uint32_t>> anc_meas(size_t(nx + nz));
        std::vector<uint32_t> data_meas(size_t(ndata), 0);
        uint32_t midx = 0;

        auto emit_mr = [&](uint8_t q) {
            Instr in; in.gate = Gate::MR; in.a = q; in.meas_index = midx++;
            circ.instrs.push_back(in);
        };
        auto emit_mz = [&](uint8_t q) {
            Instr in; in.gate = Gate::MZ; in.a = q; in.meas_index = midx++;
            circ.instrs.push_back(in);
        };
        auto emit1 = [&](Gate g, uint8_t q) {
            Instr in; in.gate = g; in.a = q; in.meas_index = 0;
            circ.instrs.push_back(in);
        };
        auto emit_cnot = [&](uint8_t c, uint8_t t) {
            Instr in; in.gate = Gate::CNOT; in.a = c; in.b = t; in.meas_index = 0;
            circ.instrs.push_back(in);
        };

        for (int round = 1; round <= cfg.rounds; ++round) {
            // X ancillas: H; CNOT(anc -> data); H; MR
            for (int i = 0; i < nx; ++i) {
                uint8_t a = uint8_t(x_anc(i));
                emit1(Gate::H, a);
                std::vector<int> sup = sc.xchecks[size_t(i)].qubits;
                std::sort(sup.begin(), sup.end());
                for (int t : sup) emit_cnot(a, uint8_t(t));
                emit1(Gate::H, a);
                emit_mr(a);
                anc_meas[size_t(i)].push_back(midx - 1);
            }
            // Z ancillas: CNOT(data -> anc); MR
            for (int i = 0; i < nz; ++i) {
                uint8_t a = uint8_t(z_anc(i));
                std::vector<int> sup = sc.zchecks[size_t(i)].qubits;
                std::sort(sup.begin(), sup.end());
                for (int t : sup) emit_cnot(uint8_t(t), a);
                emit_mr(a);
                anc_meas[size_t(nx + i)].push_back(midx - 1);
            }
        }
        // Final data measurement (Z basis), index order.
        for (int q = 0; q < ndata; ++q) {
            emit_mz(uint8_t(q));
            data_meas[size_t(q)] = midx - 1;
        }
        circ.num_measurements = midx;

        // Detectors.
        for (int i = 0; i < nx; ++i) {
            for (size_t r = 1; r < anc_meas[size_t(i)].size(); ++r) {
                Detector det;
                det.meas = {anc_meas[size_t(i)][r - 1], anc_meas[size_t(i)][r]};
                det.name = "X" + std::to_string(i) + "r" + std::to_string(r + 1);
                circ.detectors.push_back(std::move(det));
            }
        }
        for (int i = 0; i < nz; ++i) {
            const auto& m = anc_meas[size_t(nx + i)];
            // First-round value is deterministic 0 (|0..0> is a +1 eigenstate).
            if (!m.empty()) {
                Detector det;
                det.meas = {m[0]};
                det.name = "Z" + std::to_string(i) + "r1";
                circ.detectors.push_back(std::move(det));
            }
            for (size_t r = 1; r < m.size(); ++r) {
                Detector det;
                det.meas = {m[r - 1], m[r]};
                det.name = "Z" + std::to_string(i) + "r" + std::to_string(r + 1);
                circ.detectors.push_back(std::move(det));
            }
            // Last round vs final data parity over support.
            if (!m.empty()) {
                Detector det;
                det.meas = {m.back()};
                for (int q : sc.zchecks[size_t(i)].qubits) det.meas.push_back(data_meas[size_t(q)]);
                det.name = "Z" + std::to_string(i) + "final";
                circ.detectors.push_back(std::move(det));
            }
        }
        // Observable: logical Z = row-0 data parity.
        {
            Observable ob;
            for (int q : sc.logical_row0) ob.meas.push_back(data_meas[size_t(q)]);
            ob.name = "ZL_row0";
            circ.observables.push_back(std::move(ob));
        }
        return circ;
    }

    if (cfg.type == "random_clifford") {
        // Deterministic circuit from config hash.
        std::string canon = jcanonical(cfg.to_json());
        std::string dom = "asqs-circuit|" + canon;
        uint8_t h[32];
        sha256((const uint8_t*)dom.data(), dom.size(), h);
        uint64_t seed = 0;
        for (int i = 0; i < 8; ++i) seed = (seed << 8) | h[i];
        Prng prng(seed);

        Circuit circ;
        circ.num_qubits = uint32_t(cfg.qubits);
        uint32_t midx = 0;
        for (int g = 0; g < cfg.gates; ++g) {
            uint32_t kind = prng.rand_below(100);
            Instr in;
            if (kind < 30) {
                in.gate = Gate::H;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
            } else if (kind < 45) {
                in.gate = Gate::S;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
            } else if (kind < 60) {
                in.gate = Gate::Sdg;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
            } else if (kind < 65) {
                in.gate = Gate::X;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
            } else if (kind < 70) {
                in.gate = Gate::Z;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
            } else {
                in.gate = Gate::CNOT;
                in.a = uint8_t(prng.rand_below(uint32_t(cfg.qubits)));
                uint32_t t = prng.rand_below(uint32_t(cfg.qubits - 1));
                if (int(t) >= in.a) ++t;
                in.b = uint8_t(t);
            }
            circ.instrs.push_back(in);
        }
        for (int q = 0; q < cfg.qubits; ++q) {
            Instr in; in.gate = Gate::MZ; in.a = uint8_t(q); in.meas_index = midx++;
            circ.instrs.push_back(in);
        }
        circ.num_measurements = midx;
        Observable ob;
        for (uint32_t q = 0; q < circ.num_measurements; ++q) ob.meas.push_back(q);
        ob.name = "parity_all";
        circ.observables.push_back(std::move(ob));
        return circ;
    }

    throw std::runtime_error("unknown circuit type: " + cfg.type);
}

// ---------------- circuit program serialization (PROOF_SPEC §13) ----------------

const char* gate_name(Gate g) {
    switch (g) {
        case Gate::H:     return "H";
        case Gate::S:     return "S";
        case Gate::Sdg:   return "SDG";
        case Gate::X:     return "X";
        case Gate::Z:     return "Z";
        case Gate::CNOT:  return "CNOT";
        case Gate::MR:    return "MR";
        case Gate::MZ:    return "MZ";
        case Gate::RESET: return "RESET";
    }
    return "?";
}

JValue circuit_program_json(const CircuitConfig& cfg, const Circuit& circ) {
    JValue j = JValue::mk_obj();
    j.set("encoding", JValue::mk_str("asqs.program/1"));
    j.set("num_qubits", JValue::mk_int(int64_t(circ.num_qubits)));
    j.set("num_measurements", JValue::mk_int(int64_t(circ.num_measurements)));

    {
        JValue ops = JValue::mk_arr();
        for (const Instr& in : circ.instrs) {
            JValue op = JValue::mk_obj();
            op.set("gate", JValue::mk_str(gate_name(in.gate)));
            if (in.gate == Gate::CNOT) {
                op.set("control", JValue::mk_int(int64_t(in.a)));
                op.set("target", JValue::mk_int(int64_t(in.b)));
            } else if (in.gate == Gate::RESET) {
                op.set("qubit", JValue::mk_int(int64_t(in.a)));
            } else {
                op.set("qubit", JValue::mk_int(int64_t(in.a)));
                if (in.gate == Gate::MR || in.gate == Gate::MZ)
                    op.set("meas_index", JValue::mk_int(int64_t(in.meas_index)));
            }
            ops.push(std::move(op));
        }
        j.set("operations", std::move(ops));
    }
    {
        JValue dets = JValue::mk_arr();
        for (const Detector& d : circ.detectors) {
            JValue dj = JValue::mk_obj();
            dj.set("name", JValue::mk_str(d.name));
            JValue m = JValue::mk_arr();
            for (uint32_t mi : d.meas) m.push(JValue::mk_int(int64_t(mi)));
            dj.set("meas", std::move(m));
            dets.push(std::move(dj));
        }
        j.set("detectors", std::move(dets));
    }
    {
        JValue obs = JValue::mk_arr();
        for (const Observable& o : circ.observables) {
            JValue oj = JValue::mk_obj();
            oj.set("name", JValue::mk_str(o.name));
            JValue m = JValue::mk_arr();
            for (uint32_t mi : o.meas) m.push(JValue::mk_int(int64_t(mi)));
            oj.set("meas", std::move(m));
            obs.push(std::move(oj));
        }
        j.set("observables", std::move(obs));
    }

    // Provenance: exactly how this circuit was generated (PROOF_SPEC §8).
    JValue prov = JValue::mk_obj();
    prov.set("generator", JValue::mk_str("asqs-circuit/1"));
    prov.set("circuit_sha256", JValue::mk_str(cfg.sha256_hex()));
    if (cfg.type == "random_clifford") {
        std::string canon = jcanonical(cfg.to_json());
        std::string dom = "asqs-circuit|" + canon;
        uint8_t h[32];
        sha256((const uint8_t*)dom.data(), dom.size(), h);
        uint64_t seed = 0;
        for (int i = 0; i < 8; ++i) seed = (seed << 8) | h[i];
        char seed_hex[17];
        std::snprintf(seed_hex, sizeof(seed_hex), "%016llx", (unsigned long long)seed);
        prov.set("construction", JValue::mk_str(
            "PRNG-driven generation from the circuit identity (PROOF_SPEC §8.2)"));
        prov.set("seed_derivation", JValue::mk_str(
            "seed = BE64(SHA256(\"asqs-circuit|\" || canonical(circuit identity))[0..8])"));
        prov.set("seed_hex", JValue::mk_str(seed_hex));
        prov.set("prng", JValue::mk_str(
            "splitmix64 (2 draws) seeding + xoroshiro128** stream (PROOF_SPEC §3)"));
        prov.set("domain", JValue::mk_str(dom));
        prov.set("gate_distribution", JValue::mk_str(
            "per gate: kind = rand_below(100); [0,30) H, [30,45) S, [45,60) Sdg, "
            "[60,65) X, [65,70) Z, [70,100) CNOT(a,b) with b != a drawn via "
            "rand_below(Q-1) + (t>=a)"));
    } else {
        prov.set("construction", JValue::mk_str(
            "normative rotated-surface-code memory experiment from (d, rounds) "
            "(PROOF_SPEC §8.1); self-validating: X/Z commutation, generator "
            "independence, logical independence (GF(2))"));
        prov.set("seed_derivation", JValue::mk_str(
            "deterministic construction from (d, rounds); no PRNG"));
    }
    j.set("provenance", std::move(prov));
    return j;
}

std::string circuit_program_sha256_hex(const JValue& program) {
    std::string canon = jcanonical(program);
    uint8_t h[32];
    sha256((const uint8_t*)canon.data(), canon.size(), h);
    return to_hex(h, 32);
}

// ---------------- noise model semantics (PROOF_SPEC §14) ----------------

JValue noise_model_json(const NoiseConfig& n) {
    JValue j = n.to_json(); // p1_ppb, p2_ppb, pm_ppb, pr_ppb
    j.set("probability_semantics", JValue::mk_str(
        "p = ppb * 1e-9 (exact rational). Each bernoulli draw consumes one "
        "u64: (next_u64 >> 11) * 2^-53 < p"));
    {
        JValue s = JValue::mk_obj();
        JValue applies = JValue::mk_arr();
        for (const char* g : {"H", "S", "SDG", "X", "Z"}) applies.push(JValue::mk_str(g));
        s.set("applies_to", std::move(applies));
        s.set("after_gate", JValue::mk_bool(true));
        s.set("channel", JValue::mk_str(
            "with probability p1, a uniformly random Pauli X/Y/Z (each p1/3) is "
            "applied to the gate's qubit after the gate"));
        j.set("single_qubit_gates", std::move(s));
    }
    {
        JValue s = JValue::mk_obj();
        JValue applies = JValue::mk_arr();
        applies.push(JValue::mk_str("CNOT"));
        s.set("applies_to", std::move(applies));
        s.set("after_gate", JValue::mk_bool(true));
        s.set("qubit_order", JValue::mk_str("control first, then target"));
        s.set("channel", JValue::mk_str(
            "for each of (control, target) in order, independently with "
            "probability p2, a uniformly random Pauli X/Y/Z (each p2/3) is "
            "applied to that qubit after the gate"));
        j.set("two_qubit_gates", std::move(s));
    }
    {
        JValue s = JValue::mk_obj();
        JValue applies = JValue::mk_arr();
        applies.push(JValue::mk_str("MR"));
        applies.push(JValue::mk_str("MZ"));
        s.set("applies_to", std::move(applies));
        s.set("after_measurement", JValue::mk_bool(true));
        s.set("channel", JValue::mk_str(
            "with probability pm, the RECORDED outcome bit is flipped "
            "(classical bit flip; the post-measurement quantum state is "
            "unchanged). Equivalent to an X_ERROR(pm) immediately before the "
            "measurement for terminal MZ and for MR in general"));
        j.set("measurement", std::move(s));
    }
    {
        JValue s = JValue::mk_obj();
        JValue applies = JValue::mk_arr();
        applies.push(JValue::mk_str("RESET"));
        s.set("applies_to", std::move(applies));
        s.set("after_reset", JValue::mk_bool(true));
        s.set("channel", JValue::mk_str(
            "with probability pr, X is applied to the qubit after the reset "
            "to |0> (state becomes |1>)"));
        j.set("reset", std::move(s));
    }
    j.set("y_convention", JValue::mk_str("Y is applied as X then Z"));
    j.set("measurement_rng", JValue::mk_str(
        "one u64 bit is consumed iff the tableau measurement branch is "
        "random; the deterministic branch consumes none (PROOF_SPEC §7)"));
    j.set("application_order", JValue::mk_str(
        "instructions in program order; within an instruction: gate first, "
        "then the noise draws in the stated order"));
    return j;
}

} // namespace asqs
