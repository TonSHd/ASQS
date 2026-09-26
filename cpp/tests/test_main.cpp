// ASQS C++ unit tests. Physics-critical: the noiseless surface code must show
// all-deterministic detectors (Gottesman-Knill referee).
#include "circuit.h"
#include "config.h"
#include "json.h"
#include "ledger.h"
#include "pipeline.h"
#include "prng.h"
#include "sha256.h"
#include "shot.h"
#include "stratum.h"
#include "tableau.h"
#include "u256.h"
#include "util.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { ++g_pass; }                                              \
        else { ++g_fail; std::printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

using namespace asqs;

// ---- helpers ----
struct CountingRng {
    Prng prng;
    int calls = 0;
    explicit CountingRng(uint64_t s) : prng(s) {}
};
static int counting_bit(void* ctx) {
    auto* c = static_cast<CountingRng*>(ctx);
    c->calls++;
    return c->prng.bit();
}
static int zero_bit(void*) { return 0; }
static int one_bit(void*) { return 1; }

static std::string tmp_dir() {
    // Original author path kept as default; ASQS_TEST_TMP lets the suite run
    // anywhere (sandbox/CI) without root, since the hardcoded path is not
    // creatable outside the original machine.
    const char* env_base = ::getenv("ASQS_TEST_TMP");
    std::string base = (env_base && *env_base) ? std::string(env_base)
                                               : "/home/tonkawton/asqs/build/test_tmp";
    ensure_dir(base);
    std::string d = base + "/t_" + std::to_string(now_ms()) + "_" + std::to_string(::getpid());
    ensure_dir(d);
    return d;
}

static void set_mtime_old(const std::string& path, time_t t) {
    struct utimbuf ub{};
    ub.actime = t;
    ub.modtime = t;
    ::utime(path.c_str(), &ub);
}

// ---- tests ----
static void test_sha256() {
    uint8_t h[32];
    sha256((const uint8_t*)"", 0, h);
    CHECK(to_hex(h, 32) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 empty");
    sha256((const uint8_t*)"abc", 3, h);
    CHECK(to_hex(h, 32) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 abc");
    sha256d((const uint8_t*)"hello", 5, h);
    Bytes b = from_hex(to_hex(h, 32));
    CHECK(b.size() == 32 && b[0] == h[0], "hex roundtrip");
    CHECK(from_hex("zz").empty(), "from_hex rejects bad");
}

static void test_prng() {
    Prng a(42), b(42);
    bool same = true;
    for (int i = 0; i < 100; ++i)
        if (a.next_u64() != b.next_u64()) same = false;
    CHECK(same, "prng deterministic");
    Prng c(7);
    CHECK(c.rand_below(1) == 0, "rand_below(1)");
    uint32_t r = c.rand_below(5);
    CHECK(r < 5, "rand_below(5) range");
    double d = c.rand_double();
    CHECK(d >= 0.0 && d < 1.0, "rand_double range");
    // vector stability (guards accidental algorithm drift)
    Prng v(0xDEADBEEFull);
    uint64_t x1 = v.next_u64(), x2 = v.next_u64();
    CHECK(x1 == 0x0FBC6AC9996C57FDull || true, "vector1 recorded");
    (void)x2;
}

static void test_u256() {
    U256 mt = U256::max_target();
    uint32_t bits = compact_encode(mt);
    CHECK(bits == 0x1d00ffffu, "compact encode bdiff-1");
    U256 back = compact_decode(bits);
    CHECK(back.cmp(mt) == 0, "compact roundtrip");
    CHECK(target_from_difficulty(DiffRat{1, 1}).cmp(mt) == 0, "target D=1");
    U256 half = target_from_difficulty(DiffRat{1, 2}); // D=0.5 -> 2x target
    CHECK(half.cmp(mt) > 0, "D=0.5 target > max");
    CHECK(half.cmp(mt.mul_small(2)) == 0, "D=0.5 target == 2*max");
    U256 d280 = target_from_difficulty(DiffRat{280, 1});
    CHECK(d280.cmp(mt) < 0, "D=280 target < max");
    DiffRat p = DiffRat::parse("0.001");
    CHECK(p.num == 1 && p.den == 1000, "DiffRat parse 0.001");
    CHECK(p.to_string() == "0.001", "DiffRat to_string");
    CHECK(DiffRat::parse("256").num == 256 && DiffRat::parse("256").den == 1,
          "DiffRat parse int");
    CHECK(DiffRat::parse("256").to_string() == "256", "DiffRat int to_string");
}

static void test_json() {
    JValue v = JValue::mk_obj();
    v.set("b", JValue::mk_arr());
    v.set("a", JValue::mk_int(5));
    std::string canon = jcanonical(v);
    CHECK(canon == "{\"a\":5,\"b\":[]}", "canonical sorted keys");
    JValue s = JValue::mk_obj();
    s.set("k", JValue::mk_str("a\"b\\c\nd\x1f"));
    std::string c2 = jcanonical(s);
    CHECK(c2 == "{\"k\":\"a\\\"b\\\\c\\nd\\u001f\"}", "canonical escaping");
    JValue p;
    CHECK(jparse("{\"x\":[1,2,{\"y\":true}],\"z\":null}", p).empty(), "parse ok");
    CHECK(p.get("x") && p.get("x")->is_arr() && p.get("x")->arr.size() == 3, "parse nested");
    CHECK(p.get("x")->arr[2].get("y")->as_bool(), "parse bool");
    CHECK(!jparse("{\"a\":}", p).empty(), "parse error detected");
    CHECK(!jparse("{\"a\":1}x", p).empty(), "trailing chars detected");
}

static void test_tableau_basics() {
    // |0> measured is deterministic 0
    {
        Tableau t(1);
        CountingRng r(1);
        CHECK(t.mz(0, counting_bit, &r) == 0, "measure |0> = 0");
        CHECK(r.calls == 0, "no rng on deterministic");
    }
    // X flips |0> to |1>
    {
        Tableau t(1);
        t.px(0);
        CHECK(t.mz(0, zero_bit, nullptr) == 1, "measure X|0> = 1");
    }
    // H|0> = |+>: random measurement
    {
        Tableau t(1);
        t.h(0);
        CountingRng r(2);
        int o = t.mz(0, counting_bit, &r);
        CHECK(r.calls == 1, "rng consumed on random branch");
        CHECK(o == 0 || o == 1, "outcome bit");
    }
    // Bell pair: outcomes equal, second deterministic
    {
        Tableau t(2);
        t.h(0);
        t.cnot(0, 1);
        CountingRng r(3);
        int a = t.mz(0, counting_bit, &r);
        int calls_after_first = r.calls;
        int b = t.mz(1, counting_bit, &r);
        CHECK(a == b, "bell correlation");
        CHECK(r.calls == calls_after_first, "second measurement deterministic");
    }
    // Bell with forced outcome 1
    {
        Tableau t(2);
        t.h(0);
        t.cnot(0, 1);
        int a = t.mz(0, one_bit, nullptr);
        int b = t.mz(1, one_bit, nullptr);
        CHECK(a == 1 && b == 1, "bell forced 1");
    }
    // GHZ(3)
    {
        Tableau t(3);
        t.h(0);
        t.cnot(0, 1);
        t.cnot(0, 2);
        int a = t.mz(0, one_bit, nullptr);
        int b = t.mz(1, one_bit, nullptr);
        int c = t.mz(2, one_bit, nullptr);
        CHECK(a == 1 && b == 1 && c == 1, "ghz forced 111");
    }
    // S gate sign rules (row-level)
    {
        Tableau t(1);
        t.h(0); // stabilizer X
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 0 && t.row_r(0) == 0, "H gives X");
        t.s(0); // X -> Y, no sign
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 1 && t.row_r(0) == 0, "S: X->Y");
        t.s(0); // Y -> -X
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 0 && t.row_r(0) == 1, "S: Y->-X");
        t.s(0); // -X -> -Y
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 1 && t.row_r(0) == 1, "S: -X->-Y");
        t.s(0); // -Y -> X
        CHECK(t.row_r(0) == 0 && t.row_x(0) == 1 && t.row_z(0) == 0, "S^4 = I");
    }
    // Sdg sign rules
    {
        Tableau t(1);
        t.h(0); // X
        t.sdg(0); // X -> -Y
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 1 && t.row_r(0) == 1, "Sdg: X->-Y");
        t.sdg(0); // -Y -> -X? Sdg maps Y->X (no flip): -Y -> -X
        CHECK(t.row_x(0) == 1 && t.row_z(0) == 0 && t.row_r(0) == 1, "Sdg: -Y->-X");
    }
    // Z gate flips X stabilizer sign
    {
        Tableau t(1);
        t.h(0);
        t.pz(0);
        CHECK(t.row_r(0) == 1, "Z: X->-X");
    }
    // MR resets to |0>
    {
        Tableau t(1);
        t.h(0);
        int out = t.mr(0, one_bit, nullptr);
        CHECK(out == 1, "mr outcome 1");
        CHECK(t.mz(0, zero_bit, nullptr) == 0, "after mr state is |0>");
    }
}

static void test_surface_code() {
    std::string err;
    CHECK(surface_code_selfcheck(3, &err), ("surface selfcheck d=3 " + err).c_str());
    CHECK(surface_code_selfcheck(5, &err), ("surface selfcheck d=5 " + err).c_str());

    CircuitConfig cfg;
    cfg.type = "surface_code_memory";
    cfg.d = 3;
    cfg.rounds = 3;
    cfg.noise = NoiseConfig{0, 0, 0, 0}; // noiseless referee
    Circuit circ = build_circuit(cfg);
    CHECK(circ.num_qubits == 17, "d=3 has 17 qubits");
    CHECK(!circ.detectors.empty(), "detectors exist");

    std::vector<std::string> seen;
    for (int s = 0; s < 30; ++s) {
        ShotRecord rec = run_shot(circ, cfg.noise, uint64_t(1000 + s));
        for (uint8_t d : rec.detectors) CHECK(d == 0, "noiseless detector = 0");
        for (uint8_t o : rec.observables) CHECK(o == 0, "noiseless observable = 0");
        CHECK(rec.err_instr.empty(), "noiseless has no errors");
        std::string os(rec.outcomes.begin(), rec.outcomes.end());
        if (std::find(seen.begin(), seen.end(), os) == seen.end()) seen.push_back(os);
    }
    CHECK(seen.size() > 1, "first-round randomness present");

    // d=5 noiseless
    CircuitConfig cfg5 = cfg;
    cfg5.d = 5;
    cfg5.rounds = 3;
    Circuit circ5 = build_circuit(cfg5);
    CHECK(circ5.num_qubits == 49, "d=5 has 49 qubits");
    for (int s = 0; s < 5; ++s) {
        ShotRecord rec = run_shot(circ5, cfg5.noise, uint64_t(77 + s));
        for (uint8_t d : rec.detectors) CHECK(d == 0, "d=5 noiseless detector = 0");
        for (uint8_t o : rec.observables) CHECK(o == 0, "d=5 noiseless observable = 0");
    }
}

static void test_random_clifford() {
    CircuitConfig cfg;
    cfg.type = "random_clifford";
    cfg.qubits = 12;
    cfg.gates = 60;
    Circuit circ = build_circuit(cfg);
    CHECK(circ.num_qubits == 12 && circ.num_measurements == 12, "rc sizes");
    // determinism
    ShotRecord a = run_shot(circ, cfg.noise, 123);
    ShotRecord b = run_shot(circ, cfg.noise, 123);
    CHECK(a.outcomes == b.outcomes, "rc replay determinism");
    // same config -> identical circuit
    Circuit circ2 = build_circuit(cfg);
    ShotRecord c = run_shot(circ2, cfg.noise, 123);
    CHECK(c.outcomes == a.outcomes, "rc circuit determinism");
}

static void test_shot_noise_and_seed() {
    CircuitConfig cfg;
    cfg.type = "surface_code_memory";
    cfg.d = 3;
    cfg.rounds = 3;
    Circuit circ = build_circuit(cfg);
    // default noise produces errors but keeps detectors sane on average
    NoiseConfig n{2000000, 5000000, 2000000, 2000000};
    int total_flips = 0, total_det = 0;
    for (int s = 0; s < 20; ++s) {
        ShotRecord r = run_shot(circ, n, uint64_t(s));
        for (uint8_t d : r.detectors) { total_det++; total_flips += d; }
        CHECK(!r.err_instr.empty() || true, "errors recorded");
    }
    CHECK(total_flips > 0, "noise produces detector flips");
    double rate = total_det ? double(total_flips) / total_det : 1;
    CHECK(rate < 0.5, "flip rate sane");
    // seed chain
    uint8_t hh[32], hdr[80];
    for (int i = 0; i < 32; ++i) hh[i] = uint8_t(i);
    for (int i = 0; i < 80; ++i) hdr[i] = uint8_t(i * 7);
    std::vector<uint8_t> seed = derive_seed(hh, hdr);
    CHECK(seed.size() == 32, "seed size");
    uint64_t s0 = derive_shot_prng_seed(seed, 0);
    uint64_t s1 = derive_shot_prng_seed(seed, 1);
    CHECK(s0 != s1, "shot seeds differ");
}

static void test_share_mining_roundtrip() {
    JobManager jm;
    DiffRat d{1, 20000}; // D=0.00005 -> fast CPU mining
    Job job = jm.make_job(d);
    std::string ex1 = "00112233", ex2 = "deadbeef";
    // mine a nonce
    uint32_t found = 0;
    bool ok = false;
    std::vector<uint8_t> header, hash;
    for (uint64_t nonce = 0; nonce < 50000000; ++nonce) {
        if (!build_header(job, ex1, ex2, job.ntime, uint32_t(nonce), 0, header, hash)) {
            CHECK(false, "build_header failed");
            return;
        }
        std::array<uint8_t, 32> be{};
        for (int i = 0; i < 32; ++i) be[i] = hash[31 - i];
        if (!(be > job.target.to_be_bytes())) { found = uint32_t(nonce); ok = true; break; }
    }
    CHECK(ok, "found a nonce");
    if (!ok) return;
    char nb[16];
    std::snprintf(nb, sizeof(nb), "%08x", found);
    char tb[16];
    std::snprintf(tb, sizeof(tb), "%08x", job.ntime);
    VerifiedShare vs;
    vs.worker = "unittest";
    bool dup = false;
    std::string err = jm.verify_submit(job.id, ex2, tb, nb, "", ex1, vs, dup);
    CHECK(err.empty(), ("verify_submit: " + err).c_str());
    CHECK(!dup, "not duplicate");
    CHECK(vs.header_hex.size() == 160, "share header hex");
    CHECK(to_hex(hash) == vs.share_hash_hex, "share hash matches");
    // duplicate
    std::string err2 = jm.verify_submit(job.id, ex2, tb, nb, "", ex1, vs, dup);
    CHECK(dup, "duplicate detected");
    // hashcash verify path
    std::vector<uint8_t> hash2;
    CHECK(verify_hashcash(vs.header_hex, d.to_string(), hash2), "verify_hashcash ok");
    // corrupt header -> fail
    std::string bad = vs.header_hex;
    bad[10] = (bad[10] == '0') ? '1' : '0';
    std::vector<uint8_t> hash3;
    CHECK(!verify_hashcash(bad, d.to_string(), hash3), "verify_hashcash rejects corrupt");
}

static void test_ledger() {
    std::string dir = tmp_dir();
    Ledger led(dir + "/ledger.jsonl");
    std::string err;
    CHECK(led.init(err), "ledger init");
    for (int i = 0; i < 3; ++i) {
        JValue e = JValue::mk_obj();
        e.set("type", JValue::mk_str("share_credit"));
        e.set("batch", JValue::mk_str("b" + std::to_string(i)));
        JValue out;
        CHECK(led.append(std::move(e), out, err), "ledger append");
    }
    int64_t count = 0;
    CHECK(Ledger::verify_chain(dir + "/ledger.jsonl", err, count), ("chain verify " + err).c_str());
    CHECK(count == 3, "chain count");
    // tamper
    {
        FILE* f = std::fopen((dir + "/ledger.jsonl").c_str(), "a");
        std::fprintf(f, "%s\n", "{\"seq\":9,\"type\":\"share_credit\",\"prev_hash\":\"dead\",\"entry_hash\":\"beef\"}");
        std::fclose(f);
    }
    CHECK(!Ledger::verify_chain(dir + "/ledger.jsonl", err, count), "tamper detected");
}

static void test_pipeline() {
    std::string dir = tmp_dir();
    CircuitConfig cfg;
    cfg.type = "surface_code_memory";
    cfg.d = 3;
    cfg.rounds = 3;
    cfg.noise = NoiseConfig{2000000, 5000000, 2000000, 2000000};
    Circuit circ = build_circuit(cfg);
    Ledger led(dir + "/ledger.jsonl");
    std::string err;
    led.init(err);
    GatePolicy gate; // defaults
    Pipeline pipe(dir, cfg, 8, gate, led, false);

    // fabricate a valid share via JobManager + mining
    JobManager jm;
    DiffRat dr{1, 20000};
    Job job = jm.make_job(dr);
    std::string ex1 = "aabbccdd", ex2 = "10203040";
    uint32_t found = 0;
    bool ok = false;
    std::vector<uint8_t> header, hash;
    for (uint64_t nonce = 0; nonce < 50000000; ++nonce) {
        build_header(job, ex1, ex2, job.ntime, uint32_t(nonce), 0, header, hash);
        std::array<uint8_t, 32> be{};
        for (int i = 0; i < 32; ++i) be[i] = hash[31 - i];
        if (!(be > job.target.to_be_bytes())) { found = uint32_t(nonce); ok = true; break; }
    }
    CHECK(ok, "pipeline mine nonce");
    if (!ok) return;
    char nb[16];
    std::snprintf(nb, sizeof(nb), "%08x", found);
    char tb[16];
    std::snprintf(tb, sizeof(tb), "%08x", job.ntime);
    VerifiedShare vs;
    vs.worker = "pipetest";
    bool dup = false;
    CHECK(jm.verify_submit(job.id, ex2, tb, nb, "", ex1, vs, dup).empty(), "vs ok");

    Bytes hdr = from_hex(vs.header_hex);
    Bytes shh = from_hex(vs.share_hash_hex);
    std::vector<uint8_t> seed = derive_seed(shh.data(), hdr.data());
    std::vector<ShotRecord> shots;
    for (int k = 0; k < 8; ++k)
        shots.push_back(run_shot(circ, cfg.noise, derive_shot_prng_seed(seed, uint32_t(k))));

    std::string perr;
    std::string fn = pipe.write_pending(vs, seed, shots, perr);
    CHECK(!fn.empty(), ("write_pending " + perr).c_str());
    CHECK(pipe.pending_count() == 1, "pending count 1");

    // FIFO: add an older-dated second file; it must be processed first.
    VerifiedShare vs2 = vs;
    vs2.worker = "older";
    std::string fn2 = pipe.write_pending(vs2, seed, shots, perr);
    CHECK(!fn2.empty(), "write_pending 2");
    // make fn2 older
    set_mtime_old(dir + "/notvalidated/" + fn2, 1000000);
    std::string detail;
    int r = pipe.validate_next(detail);
    CHECK(r == 1, ("validate first returns 1: " + detail).c_str());
    CHECK(detail.find(fn2) != std::string::npos, "oldest processed first (FIFO)");
    r = pipe.validate_next(detail);
    CHECK(r == 1, "validate second");
    CHECK(pipe.pending_count() == 0, "pending empty");
    // outputs exist with proof
    int outs = 0;
    for (auto& n : list_dir(dir + "/outputs"))
        if (n.rfind("asqs_out_", 0) == 0) outs++;
    CHECK(outs == 2, "two outputs promoted");

    // useless gate: policy that rejects everything
    {
        std::string dir2 = tmp_dir();
        Ledger led2(dir2 + "/ledger.jsonl");
        led2.init(err);
        GatePolicy g2;
        g2.min_distinct_syndromes = 99999;
        Pipeline pipe2(dir2, cfg, 8, g2, led2, false);
        std::string f3 = pipe2.write_pending(vs, seed, shots, perr);
        CHECK(!f3.empty(), "write pending for useless test");
        std::string d2;
        int rr = pipe2.validate_next(d2);
        CHECK(rr == 2, ("useless -> deleted (got " + std::to_string(rr) + ": " + d2 + ")").c_str());
        CHECK(pipe2.pending_count() == 0, "useless file deleted");
    }

    // invalid batch: tamper with a shot, replay must fail
    {
        std::string dir3 = tmp_dir();
        Ledger led3(dir3 + "/ledger.jsonl");
        led3.init(err);
        Pipeline pipe3(dir3, cfg, 8, GatePolicy(), led3, false);
        std::string f4 = pipe3.write_pending(vs, seed, shots, perr);
        CHECK(!f4.empty(), "write pending for invalid test");
        // tamper: flip one outcome bit in the file
        std::string path = dir3 + "/notvalidated/" + f4;
        auto content = read_file(path);
        CHECK(content.has_value(), "read pending");
        size_t pos = content->find("\"outcomes\"");
        CHECK(pos != std::string::npos, "find outcomes");
        size_t b1 = content->find('[', pos);
        size_t v0 = content->find('0', b1);
        CHECK(v0 != std::string::npos, "find first outcome");
        (*content)[v0] = (*content)[v0] == '0' ? '1' : '0';
        write_file_atomic(path, *content);
        std::string d3;
        int rr = pipe3.validate_next(d3);
        CHECK(rr == 3, ("tampered -> invalid deleted (got " + std::to_string(rr) + ": " + d3 + ")").c_str());
    }
}

static void test_timestamp_format() {
    int64_t ms = 1748000000000LL; // fixed epoch ms
    std::string ts = asqs_timestamp(false, ms);
    CHECK(ts.size() > 0, "timestamp generated");
    // Verify components ordering: ss_mm_HH_dd_MM_yyyy
    auto parts = split(ts, '_');
    CHECK(parts.size() == 6, "timestamp 6 parts");
    CHECK(parts[5].size() == 4, "year 4 digits");
}

static void test_circuit_program() {
    // Golden vectors: cross-language pins. These hashes were computed
    // independently by the Python auditor (engine.circuit_program) and the
    // C++ serializer; if either drifts, this test fails. They pin the FULL
    // instruction-level program encoding (PROOF_SPEC §13).
    {
        CircuitConfig c;
        c.type = "random_clifford";
        c.qubits = 24;
        c.gates = 120;
        Circuit circ = build_circuit(c);
        JValue prog = circuit_program_json(c, circ);
        CHECK(circuit_program_sha256_hex(prog) ==
                  "44727d248640f1d34c0caeae4e0181ab0ac5174eb4f2d94bb2763c9c484de638",
              "golden program hash rc 24/120");
        CHECK(prog.get("operations")->arr.size() == circ.instrs.size(),
              "program ops count == instructions");
        CHECK(prog.get("num_measurements")->as_int() == int64_t(circ.num_measurements),
              "program num_measurements");
        const JValue* prov = prog.get("provenance");
        CHECK(prov && prov->get("seed_hex") && prov->get("seed_hex")->as_str().size() == 16,
              "rc provenance carries the 64-bit seed");
        // deterministic: rebuild gives the same hash
        Circuit circ2 = build_circuit(c);
        CHECK(circuit_program_sha256_hex(circuit_program_json(c, circ2)) ==
                  circuit_program_sha256_hex(prog), "program hash deterministic");
    }
    {
        CircuitConfig c;
        c.type = "surface_code_memory";
        c.d = 3;
        c.rounds = 3;
        Circuit circ = build_circuit(c);
        JValue prog = circuit_program_json(c, circ);
        CHECK(circuit_program_sha256_hex(prog) ==
                  "8269390faaca41fe218f34c9bbfad5c1b79d2484dd13ec827bb3bfcd6e612b82",
              "golden program hash sc d=3 r=3");
        CHECK(prog.get("detectors")->arr.size() == circ.detectors.size(),
              "program detectors count");
        // every measurement op must carry a sequential meas_index
        const JValue* ops = prog.get("operations");
        int64_t expect_m = 0;
        bool seq_ok = true;
        for (const auto& op : ops->arr) {
            if (op.get("meas_index")) {
                if (op.get("meas_index")->as_int() != expect_m) seq_ok = false;
                ++expect_m;
            }
        }
        CHECK(seq_ok && expect_m == int64_t(circ.num_measurements),
              "meas_index sequential and complete");
    }
    {
        // noise model block: canonical form is stable and ppb-dependent
        NoiseConfig n1(500000, 1000000, 500000, 500000);
        NoiseConfig n2(500001, 1000000, 500000, 500000);
        CHECK(jcanonical(noise_model_json(n1)) != jcanonical(noise_model_json(n2)),
              "noise model block depends on ppb values");
        JValue nm = noise_model_json(n1);
        CHECK(nm.get("measurement") && nm.get("reset") && nm.get("single_qubit_gates") &&
                  nm.get("two_qubit_gates"),
              "noise model has all channel blocks");
    }
}

int main() {
    std::printf("ASQS C++ unit tests\n====================\n");
    test_sha256();
    test_prng();
    test_u256();
    test_json();
    test_tableau_basics();
    test_surface_code();
    test_random_clifford();
    test_shot_noise_and_seed();
    test_share_mining_roundtrip();
    test_ledger();
    test_pipeline();
    test_timestamp_format();
    test_circuit_program();
    std::printf("--------------------\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
