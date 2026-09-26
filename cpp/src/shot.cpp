#include "shot.h"
#include <cstring>
#include "prng.h"
#include "sha256.h"

namespace asqs {

namespace {

struct RngCtx {
    Prng* prng;
};

int rng_bit_cb(void* ctx) { return static_cast<RngCtx*>(ctx)->prng->bit(); }

inline double ppb_to_p(int64_t ppb) { return double(ppb) * 1e-9; }

// Apply a Pauli error (0=X, 1=Y, 2=Z) to the tableau. Y = X then Z (normative).
void apply_pauli(Tableau& t, int q, int p) {
    if (p == 0) t.px(q);
    else if (p == 1) { t.px(q); t.pz(q); }
    else t.pz(q);
}

} // namespace

std::vector<uint8_t> derive_seed(const uint8_t share_hash[32], const uint8_t header80[80]) {
    std::vector<uint8_t> buf(32 + 80);
    std::memcpy(buf.data(), share_hash, 32);
    std::memcpy(buf.data() + 32, header80, 80);
    std::vector<uint8_t> out(32);
    sha256(buf.data(), buf.size(), out.data());
    return out;
}

uint64_t derive_shot_prng_seed(const std::vector<uint8_t>& seed, uint32_t k) {
    std::vector<uint8_t> buf(seed.begin(), seed.end());
    buf.push_back(uint8_t(k & 0xff));
    buf.push_back(uint8_t((k >> 8) & 0xff));
    buf.push_back(uint8_t((k >> 16) & 0xff));
    buf.push_back(uint8_t((k >> 24) & 0xff));
    uint8_t h[32];
    sha256(buf.data(), buf.size(), h);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(h[i]) << (8 * i); // LE64
    return v;
}

void compute_detectors_observables(const Circuit& circ, const std::vector<uint8_t>& outcomes,
                                   std::vector<uint8_t>& detectors,
                                   std::vector<uint8_t>& observables) {
    detectors.clear();
    for (const auto& d : circ.detectors) {
        int p = 0;
        for (uint32_t m : d.meas) p ^= outcomes[m] & 1;
        detectors.push_back(uint8_t(p));
    }
    observables.clear();
    for (const auto& o : circ.observables) {
        int p = 0;
        for (uint32_t m : o.meas) p ^= outcomes[m] & 1;
        observables.push_back(uint8_t(p));
    }
}

ShotRecord run_shot(const Circuit& circ, const NoiseConfig& noise, uint64_t prng_seed) {
    Prng prng(prng_seed);
    RngCtx ctx{&prng};
    Tableau tab(int(circ.num_qubits));
    ShotRecord rec;
    rec.outcomes.assign(size_t(circ.num_measurements), 0);

    const double p1 = ppb_to_p(noise.p1_ppb);
    const double p2 = ppb_to_p(noise.p2_ppb);
    const double pm = ppb_to_p(noise.pm_ppb);
    const double pr = ppb_to_p(noise.pr_ppb);

    for (size_t i = 0; i < circ.instrs.size(); ++i) {
        const Instr& in = circ.instrs[i];
        switch (in.gate) {
            case Gate::H: case Gate::S: case Gate::Sdg: case Gate::X: case Gate::Z: {
                if (in.gate == Gate::H) tab.h(in.a);
                else if (in.gate == Gate::S) tab.s(in.a);
                else if (in.gate == Gate::Sdg) tab.sdg(in.a);
                else if (in.gate == Gate::X) tab.px(in.a);
                else tab.pz(in.a);
                if (prng.bernoulli(p1)) {
                    int p = int(prng.rand_below(3));
                    apply_pauli(tab, in.a, p);
                    rec.err_instr.push_back(int32_t(i));
                    rec.err_qubit.push_back(in.a);
                    rec.err_pauli.push_back(uint8_t(p));
                }
                break;
            }
            case Gate::CNOT: {
                tab.cnot(in.a, in.b);
                for (int q : {int(in.a), int(in.b)}) {
                    if (prng.bernoulli(p2)) {
                        int p = int(prng.rand_below(3));
                        apply_pauli(tab, q, p);
                        rec.err_instr.push_back(int32_t(i));
                        rec.err_qubit.push_back(uint8_t(q));
                        rec.err_pauli.push_back(uint8_t(p));
                    }
                }
                break;
            }
            case Gate::MR: {
                int out = tab.mr(in.a, rng_bit_cb, &ctx);
                if (prng.bernoulli(pm)) {
                    out ^= 1;
                    rec.err_instr.push_back(int32_t(i));
                    rec.err_qubit.push_back(in.a);
                    rec.err_pauli.push_back(3); // code 3 = measurement flip
                }
                rec.outcomes[in.meas_index] = uint8_t(out);
                break;
            }
            case Gate::MZ: {
                int out = tab.mz(in.a, rng_bit_cb, &ctx);
                if (prng.bernoulli(pm)) {
                    out ^= 1;
                    rec.err_instr.push_back(int32_t(i));
                    rec.err_qubit.push_back(in.a);
                    rec.err_pauli.push_back(3);
                }
                rec.outcomes[in.meas_index] = uint8_t(out);
                break;
            }
            case Gate::RESET: {
                tab.reset(in.a, rng_bit_cb, &ctx);
                if (prng.bernoulli(pr)) {
                    tab.px(in.a);
                    rec.err_instr.push_back(int32_t(i));
                    rec.err_qubit.push_back(in.a);
                    rec.err_pauli.push_back(0); // reset error is an X
                }
                break;
            }
        }
    }
    compute_detectors_observables(circ, rec.outcomes, rec.detectors, rec.observables);
    return rec;
}

JValue shot_to_json(const ShotRecord& rec) {
    JValue j = JValue::mk_obj();
    {
        JValue arr = JValue::mk_arr();
        for (uint8_t b : rec.outcomes) arr.push(JValue::mk_int(b));
        j.set("outcomes", std::move(arr));
    }
    {
        JValue arr = JValue::mk_arr();
        for (uint8_t b : rec.detectors) arr.push(JValue::mk_int(b));
        j.set("detectors", std::move(arr));
    }
    {
        JValue arr = JValue::mk_arr();
        for (uint8_t b : rec.observables) arr.push(JValue::mk_int(b));
        j.set("observables", std::move(arr));
    }
    {
        JValue err = JValue::mk_arr();
        for (size_t e = 0; e < rec.err_instr.size(); ++e) {
            JValue ev = JValue::mk_obj();
            ev.set("i", JValue::mk_int(rec.err_instr[e]));
            ev.set("q", JValue::mk_int(rec.err_qubit[e]));
            ev.set("p", JValue::mk_int(rec.err_pauli[e]));
            err.push(std::move(ev));
        }
        j.set("errors", std::move(err));
    }
    return j;
}

} // namespace asqs
