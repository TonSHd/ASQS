#include "pipeline.h"
#include "sha256.h"
#include "util.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <sys/stat.h>

namespace asqs {

Pipeline::Pipeline(std::string root, CircuitConfig cfg, int shots_per_share, GatePolicy gate,
                   Ledger& ledger, bool local_time)
    : root_(std::move(root)), cfg_(std::move(cfg)), shots_per_share_(shots_per_share),
      gate_(gate), ledger_(ledger), local_time_(local_time) {
    ensure_dir(notvalidated_dir());
    ensure_dir(outputs_dir());
    fsync_dir(root_);
}

void Pipeline::cleanup_tmp_files() {
    // write_file_atomic() stages as "<final-path>.json.tmp.<pid>" — i.e. the
    // ".tmp." marker sits at the END of the name, not right after the
    // prefix. The old prefix match ("asqs_notvalidated_.tmp.") could never
    // match a real staged file, so crash leftovers were never swept.
    for (const auto& name : list_dir(notvalidated_dir())) {
        if (name.rfind("asqs_notvalidated_", 0) == 0 && name.find(".tmp.") != std::string::npos)
            delete_file(notvalidated_dir() + "/" + name);
    }
    for (const auto& name : list_dir(outputs_dir())) {
        if (name.rfind("asqs_out_", 0) == 0 && name.find(".tmp.") != std::string::npos)
            delete_file(outputs_dir() + "/" + name);
    }
}

std::string Pipeline::unique_path(const std::string& dir, const std::string& prefix_ts) {
    // prefix_ts already includes the asqs_notvalidated_/asqs_out_ prefix +
    // timestamp + batch-id tail, so collisions are not expected (the tail is
    // 128 bits of sha256(header_hex||nonce_hex)). The _N suffix below stays
    // as a last-resort guard (e.g. the same share written twice on purpose).
    std::string base = dir + "/" + prefix_ts;
    std::string path = base + ".json";
    int n = 2;
    while (file_exists(path)) {
        path = base + "_" + std::to_string(n) + ".json";
        ++n;
    }
    return path;
}

std::string Pipeline::write_pending(const VerifiedShare& share, const std::vector<uint8_t>& seed,
                                    const std::vector<ShotRecord>& shots, std::string& err,
                                    bool validation_batch) {
    if (seed.size() != 32) { err = "bad seed"; return ""; }
    int64_t now = now_ms();
    JValue batch = JValue::mk_obj();
    // v2 = v1 + circuit_program (full instruction list + provenance),
    // circuit_program_sha256, noise_model (machine-readable semantics) and
    // an optional validation_batch marker. Both versions replay identically.
    batch.set("schema", JValue::mk_str("asqs.batch/2"));
    if (validation_batch)
        batch.set("validation_batch", JValue::mk_bool(true));

    uint8_t bid[32];
    std::string key = share.header_hex + share.nonce_hex;
    sha256((const uint8_t*)key.data(), key.size(), bid);
    batch.set("batch_id", JValue::mk_str(to_hex(bid, 16)));

    batch.set("created_ms", JValue::mk_int(now));
    batch.set("created_utc", JValue::mk_str(iso8601_utc(now)));
    batch.set("circuit", cfg_.to_json());
    batch.set("circuit_sha256", JValue::mk_str(cfg_.sha256_hex()));
    batch.set("noise", cfg_.noise.to_json());
    // Machine-readable simulation semantics: the ppb values PLUS the exact
    // rules for how they are applied (PROOF_SPEC §14). Without this, two
    // simulators could interpret the same numbers differently.
    batch.set("noise_model", noise_model_json(cfg_.noise));
    batch.set("shots_per_share", JValue::mk_int(int64_t(shots.size())));
    // The full circuit program: exact instruction list, detectors,
    // observables and generation provenance (PROOF_SPEC §13). The hash pins
    // the program; an independent verifier can reconstruct the circuit from
    // this without re-implementing the builder.
    {
        Circuit circ = build_circuit(cfg_);
        JValue program = circuit_program_json(cfg_, circ);
        batch.set("circuit_program", program);
        batch.set("circuit_program_sha256", JValue::mk_str(circuit_program_sha256_hex(program)));
    }

    JValue sh = JValue::mk_obj();
    sh.set("worker", JValue::mk_str(share.worker));
    sh.set("job_id", JValue::mk_str(share.job_id));
    sh.set("extranonce1", JValue::mk_str(share.extranonce1_hex));
    sh.set("extranonce2", JValue::mk_str(share.extranonce2_hex));
    sh.set("ntime", JValue::mk_str(share.ntime_hex));
    sh.set("nonce", JValue::mk_str(share.nonce_hex));
    sh.set("difficulty", JValue::mk_str(share.difficulty_str));
    sh.set("header_hex", JValue::mk_str(share.header_hex));
    sh.set("share_hash_hex", JValue::mk_str(share.share_hash_hex));
    sh.set("received_ms", JValue::mk_int(share.received_ms));
    batch.set("share", std::move(sh));

    JValue sd = JValue::mk_obj();
    sd.set("derivation", JValue::mk_str("sha256(sha256d(header80) || header80)"));
    sd.set("shot_seed", JValue::mk_str("sha256(seed || LE32(k))"));
    sd.set("prng_seed", JValue::mk_str("LE64(shot_seed[0..8])"));
    sd.set("seed_hex", JValue::mk_str(to_hex(seed)));
    batch.set("seed", std::move(sd));

    JValue shots_arr = JValue::mk_arr();
    for (size_t k = 0; k < shots.size(); ++k) {
        JValue sj = shot_to_json(shots[k]);
        sj.set("index", JValue::mk_int(int64_t(k)));
        shots_arr.push(std::move(sj));
    }
    batch.set("shots", std::move(shots_arr));

    std::string ts = asqs_timestamp(local_time_, now);
    // Batch-id tail: the same 32 hex chars as the "batch_id" field above.
    // Two batches in the same second (or even the same millisecond) now get
    // distinct names by construction — different nonces give different ids —
    // instead of colliding into _2/_3 suffixes, and the log's "batch written"
    // lines become individually identifiable.
    std::string fname = "asqs_notvalidated_" + ts + "_" + to_hex(bid, 16);
    std::string path = unique_path(notvalidated_dir(), fname);

    std::string data = jserialize(batch, true);
    if (data.empty()) { err = "serialize failed"; return ""; }
    if (!write_file_atomic(path, data)) { err = "write failed: " + path; return ""; }
    fsync_dir(notvalidated_dir());
    stats_.batches_written++;
    // Return just the filename (caller may log).
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

int Pipeline::pending_count() {
    int n = 0;
    for (const auto& name : list_dir(notvalidated_dir()))
        if (name.rfind("asqs_notvalidated_", 0) == 0 && name.find(".tmp.") == std::string::npos)
            ++n;
    return n;
}

int Pipeline::validate_next(std::string& detail) {
    // FIFO: oldest mtime first (never cut in line).
    std::vector<std::pair<int64_t, std::string>> files;
    for (const auto& name : list_dir(notvalidated_dir())) {
        if (name.rfind("asqs_notvalidated_", 0) != 0) continue;
        if (name.find(".tmp.") != std::string::npos) continue;
        struct stat st;
        std::string p = notvalidated_dir() + "/" + name;
        if (::stat(p.c_str(), &st) != 0) continue;
        files.push_back({int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000, name});
    }
    if (files.empty()) return 0;
    std::sort(files.begin(), files.end());
    return validate_file(notvalidated_dir() + "/" + files.front().second, files.front().second,
                         detail);
}

GateStats evaluate_gate(const CircuitConfig& cfg, const GatePolicy& policy,
                        const std::vector<ShotRecord>& shots) {
    GateStats g;
    g.distinct_outcomes = 0;
    std::set<std::string> outcome_strings;
    std::set<std::string> syndrome_strings;
    for (const auto& s : shots) {
        for (uint8_t d : s.detectors) {
            g.detector_total++;
            g.detector_flips += d;
        }
        {
            std::string syn;
            bool any = false;
            for (uint8_t d : s.detectors) {
                syn.push_back(char('0' + (d & 1)));
                if (d) any = true;
            }
            if (any) syndrome_strings.insert(syn);
        }
        {
            std::string ob;
            for (uint8_t o : s.outcomes) ob.push_back(char('0' + (o & 1)));
            outcome_strings.insert(ob);
        }
    }
    g.distinct_syndromes = int64_t(syndrome_strings.size());
    g.distinct_outcomes = int64_t(outcome_strings.size());
    if (g.detector_total > 0)
        g.flip_rate_ppm = g.detector_flips * 1000000 / g.detector_total;
    else
        g.flip_rate_ppm = 0;

    if (cfg.type == "random_clifford") {
        if (g.distinct_outcomes < policy.min_distinct_outcomes) {
            g.reason = "degenerate outcome distribution";
            return g;
        }
        g.useful = true;
        g.reason = "non-degenerate outcomes";
        return g;
    }

    // surface_code_memory
    if (!cfg.noise.any()) {
        g.useful = true;
        g.reason = "noiseless mode: validity only";
        return g;
    }
    double rate = g.detector_total ? double(g.detector_flips) / double(g.detector_total) : 0.0;
    if (g.detector_flips < policy.min_detector_flips) {
        g.reason = "no detector activity (noise not observable)";
        return g;
    }
    if (rate > policy.max_detector_flip_rate) {
        g.reason = "detector flip rate above sanity ceiling";
        return g;
    }
    if (g.distinct_syndromes < policy.min_distinct_syndromes) {
        g.reason = "syndrome diversity too low";
        return g;
    }
    g.useful = true;
    g.reason = "statistics within gate policy";
    return g;
}

int Pipeline::validate_file(const std::string& path, const std::string& name, std::string& detail) {
    try {
        auto pd = read_file(path);
        if (!pd) { detail = "unreadable: " + name; delete_file(path); stats_.invalid_deleted++; return 3; }
        JValue batch;
        std::string err = jparse(*pd, batch);
        if (!err.empty()) { detail = "bad json (" + name + "): " + err; delete_file(path); stats_.invalid_deleted++; return 3; }

        auto invalid = [&](const std::string& why) {
            detail = "INVALID (" + name + "): " + why;
            delete_file(path);
            stats_.invalid_deleted++;
            return 3;
        };

        const JValue* schema_j = batch.get("schema");
        std::string schema = schema_j ? schema_j->as_str() : "";
        bool is_v2 = schema == "asqs.batch/2";
        if (!is_v2 && schema != "asqs.batch/1") return invalid("bad schema");
        const JValue* sh = batch.get("share");
        const JValue* sd = batch.get("seed");
        const JValue* shots_j = batch.get("shots");
        const JValue* circ_j = batch.get("circuit");
        if (!sh || !sd || !shots_j || !circ_j || !shots_j->is_arr())
            return invalid("missing fields");
        std::string header_hex = sh->get("header_hex") ? sh->get("header_hex")->as_str() : "";
        std::string difficulty = sh->get("difficulty") ? sh->get("difficulty")->as_str() : "";
        std::string share_hash_hex =
            sh->get("share_hash_hex") ? sh->get("share_hash_hex")->as_str() : "";
        std::string seed_hex = sd->get("seed_hex") ? sd->get("seed_hex")->as_str() : "";
        if (header_hex.size() != 160 || share_hash_hex.size() != 64 || seed_hex.size() != 64)
            return invalid("bad hex field lengths");

        // 1) hashcash re-verification — always, no exceptions. Quantum jobs
        // are real hashcash jobs now (see make_quantum_job / FORENSIC_REPORT.md);
        // there is no longer a "difficulty==1 means trust it" carve-out.
        std::vector<uint8_t> hash;
        if (!verify_hashcash(header_hex, difficulty, hash))
            return invalid("hashcash failed");
        if (to_hex(hash) != share_hash_hex) return invalid("share hash mismatch");

        // 2) seed re-derivation
        Bytes header = from_hex(header_hex);
        std::vector<uint8_t> seed = derive_seed(hash.data(), header.data());
        if (to_hex(seed) != seed_hex) return invalid("seed derivation mismatch");

        // 3) circuit rebuild + identity check
        std::string cerr2;
        CircuitConfig cfg = CircuitConfig::from_json(*circ_j, cerr2);
        if (!cerr2.empty()) return invalid("circuit config: " + cerr2);
        // Noise is stored separately in the batch (not part of circuit identity)
        // and MUST be loaded for the replay.
        if (const JValue* nj = batch.get("noise")) cfg.noise = NoiseConfig::from_json(*nj);
        else return invalid("missing noise config");
        Circuit circ;
        try {
            circ = build_circuit(cfg);
        } catch (const std::exception& e) {
            return invalid(std::string("circuit build: ") + e.what());
        }
        if (cfg.sha256_hex() != cfg_.sha256_hex())
            return invalid("circuit identity mismatch with daemon config");
        if (const JValue* ch = batch.get("circuit_sha256"))
            if (ch->as_str() != cfg.sha256_hex()) return invalid("circuit hash mismatch");

        // 3b) v2: circuit program verification — rebuild the program from
        // the embedded identity and compare hashes. A tampered/absent program
        // in a v2 record is INVALID (the record's core promise is broken).
        if (is_v2) {
            const JValue* prog = batch.get("circuit_program");
            const JValue* prog_hash = batch.get("circuit_program_sha256");
            if (!prog || !prog_hash) return invalid("v2 missing circuit program");
            if (circuit_program_sha256_hex(*prog) != prog_hash->as_str())
                return invalid("circuit program hash mismatch");
            JValue expect = circuit_program_json(cfg, circ);
            if (circuit_program_sha256_hex(expect) != prog_hash->as_str())
                return invalid("circuit program does not match its identity");
        }
        // v2 noise_model semantics must equal the normative block for the
        // embedded ppb values (a wrong block means the numbers could be
        // interpreted differently than the replay applies them).
        if (is_v2) {
            const JValue* nm = batch.get("noise_model");
            if (!nm) return invalid("v2 missing noise_model");
            if (jcanonical(*nm) != jcanonical(noise_model_json(cfg.noise)))
                return invalid("noise_model semantics mismatch");
        }

        // 4) full deterministic replay
        int64_t k_count = int64_t(shots_j->arr.size());
        std::vector<ShotRecord> records;
        for (int64_t k = 0; k < k_count; ++k) {
            uint64_t ps = derive_shot_prng_seed(seed, uint32_t(k));
            records.push_back(run_shot(circ, cfg.noise, ps));
            // Compare against the embedded shot, ignoring presentation-only keys.
            JValue theirs_clean = JValue::mk_obj();
            if (shots_j->arr[size_t(k)].is_obj()) {
                for (auto& kv : shots_j->arr[size_t(k)].obj)
                    if (kv.first != "index") theirs_clean.set(kv.first, kv.second);
            }
            std::string mine = jcanonical(shot_to_json(records[size_t(k)]));
            std::string theirs = jcanonical(theirs_clean);
            if (mine != theirs) return invalid("replay mismatch at shot " + std::to_string(k));
        }

        // 5) usefulness gate
        GateStats g = evaluate_gate(cfg, gate_, records);
        if (!g.useful) {
            detail = "USELESS (" + name + "): " + g.reason;
            delete_file(path);
            stats_.useless_deleted++;
            return 2;
        }

        // 6) ledger entry (proof anchor)
        JValue entry = JValue::mk_obj();
        entry.set("type", JValue::mk_str("share_credit"));
        entry.set("ts_ms", JValue::mk_int(now_ms()));
        entry.set("batch", JValue::mk_str(name));
        entry.set("worker", JValue::mk_str(sh->get("worker") ? sh->get("worker")->as_str() : ""));
        entry.set("share_hash", JValue::mk_str(share_hash_hex));
        entry.set("difficulty", JValue::mk_str(difficulty));
        entry.set("circuit", JValue::mk_str(cfg.type));
        entry.set("circuit_sha256", JValue::mk_str(cfg.sha256_hex()));
        entry.set("shots", JValue::mk_int(k_count));
        entry.set("detector_flips", JValue::mk_int(g.detector_flips));
        entry.set("detector_total", JValue::mk_int(g.detector_total));
        entry.set("flip_rate_ppm", JValue::mk_int(g.flip_rate_ppm));
        // proof core hash (proof without ledger fields)
        JValue proof_core = JValue::mk_obj();
        {
            JValue shots_arr = JValue::mk_arr();
            for (const auto& r : records) shots_arr.push(shot_to_json(r));
            std::string canon = jcanonical(shots_arr);
            uint8_t h[32];
            sha256((const uint8_t*)canon.data(), canon.size(), h);
            proof_core.set("replay_sha256", JValue::mk_str(to_hex(h, 32)));
            proof_core.set("verdict", JValue::mk_str("useful"));
            proof_core.set("reason", JValue::mk_str(g.reason));
        }
        entry.set("proof_sha256", JValue::mk_str(proof_core.get("replay_sha256")->as_str()));
        JValue ledger_entry;
        std::string lerr;
        if (!ledger_.append(std::move(entry), ledger_entry, lerr)) {
            detail = "LEDGER ERROR (" + name + "): " + lerr;
            // Keep the pending file; retry on next pass.
            return 4;
        }

        // 7) write proof into the batch and promote to outputs/
        JValue proof = proof_core;
        int64_t vnow = now_ms();
        proof.set("validated_ms", JValue::mk_int(vnow));
        proof.set("validated_utc", JValue::mk_str(iso8601_utc(vnow)));
        proof.set("replay", JValue::mk_str("match"));
        {
            JValue gs = JValue::mk_obj();
            gs.set("detector_flips", JValue::mk_int(g.detector_flips));
            gs.set("detector_total", JValue::mk_int(g.detector_total));
            gs.set("flip_rate_ppm", JValue::mk_int(g.flip_rate_ppm));
            gs.set("distinct_syndromes", JValue::mk_int(g.distinct_syndromes));
            gs.set("distinct_outcomes", JValue::mk_int(g.distinct_outcomes));
            proof.set("gate", std::move(gs));
        }
        proof.set("ledger_seq", JValue::mk_int(ledger_entry.get("seq")->as_int()));
        proof.set("ledger_entry_hash", JValue::mk_str(ledger_entry.get("entry_hash")->as_str()));
        batch.set("proof", std::move(proof));

        // Output name: asqs_out_<same ts + batch-id tail>
        std::string ts_part = name.substr(strlen("asqs_notvalidated_"));
        if (ts_part.size() > 5 && ts_part.compare(ts_part.size() - 5, 5, ".json") == 0)
            ts_part = ts_part.substr(0, ts_part.size() - 5);
        std::string out_path = unique_path(outputs_dir(), "asqs_out_" + ts_part);

        std::string data = jserialize(batch, true);
        if (data.empty() || !write_file_atomic(out_path, data)) {
            detail = "OUTPUT WRITE FAILED (" + name + ")";
            delete_file(path); // ledger already credited; keep audit trail in log
            stats_.invalid_deleted++;
            return 3;
        }
        fsync_dir(outputs_dir());
        if (!delete_file(path)) {
            detail = "promoted but source unlink failed: " + name;
        }
        stats_.validated++;
        stats_.useful++;
        detail = "PROMOTED " + name + " -> " +
                 (out_path.find_last_of('/') == std::string::npos
                      ? out_path
                      : out_path.substr(out_path.find_last_of('/') + 1)) +
                 " (" + g.reason + ")";
        return 1;
    } catch (const std::exception& e) {
        detail = "EXCEPTION (" + name + "): " + std::string(e.what());
        delete_file(path);
        stats_.invalid_deleted++;
        return 3;
    } catch (...) {
        detail = "UNKNOWN EXCEPTION (" + name + ")";
        delete_file(path);
        stats_.invalid_deleted++;
        return 3;
    }
}

} // namespace asqs
