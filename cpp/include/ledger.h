// ASQS - hash-chained append-only JSONL share ledger.
// entry_hash = SHA256(canonical(entry without entry_hash)); each entry's
// prev_hash = previous entry's entry_hash (genesis: 64*'0').
#pragma once
#include "json.h"
#include "sha256.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace asqs {

class Ledger {
public:
    explicit Ledger(std::string path) : path_(std::move(path)) {}

    // Load existing chain tail (call once at startup before append).
    bool init(std::string& err) {
        std::ifstream f(path_);
        if (!f) return true; // fresh ledger
        std::string line;
        int64_t n = 0;
        std::string last;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            JValue v;
            if (!jparse(line, v).empty()) { err = "corrupt ledger line"; return false; }
            if (const JValue* h = v.get("entry_hash")) last = h->as_str();
            if (const JValue* s = v.get("seq")) n = s->as_int();
        }
        next_seq_ = n + 1;
        last_hash_ = last.empty() ? std::string(64, '0') : last;
        return true;
    }

    // Appends an entry; fills seq/prev_hash/entry_hash; returns final entry.
    bool append(JValue entry, JValue& out, std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        entry.set("seq", JValue::mk_int(next_seq_));
        entry.set("prev_hash", JValue::mk_str(last_hash_));
        std::string canon = jcanonical(entry);
        if (canon.empty()) { err = "canonical serialization failed (float present?)"; return false; }
        uint8_t h[32];
        sha256((const uint8_t*)canon.data(), canon.size(), h);
        std::string eh = to_hex(h, 32);
        entry.set("entry_hash", JValue::mk_str(eh));
        std::ofstream f(path_, std::ios::app);
        if (!f) { err = "cannot open ledger"; return false; }
        std::string line = jserialize(entry, false);
        f << line << "\n";
        f.flush();
        if (!f.good()) { err = "ledger write failed"; return false; }
        ++next_seq_;
        last_hash_ = eh;
        out = entry;
        return true;
    }

    // Full chain verification (used by CLI / auditor).
    static bool verify_chain(const std::string& path, std::string& err, int64_t& count) {
        std::ifstream f(path);
        if (!f) { err = "no ledger at " + path; return false; }
        std::string line, prev = std::string(64, '0');
        int64_t expect_seq = 1, n = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            JValue v;
            if (!jparse(line, v).empty()) { err = "corrupt line " + std::to_string(n + 1); return false; }
            const JValue* seq = v.get("seq");
            const JValue* ph = v.get("prev_hash");
            const JValue* eh = v.get("entry_hash");
            if (!seq || !ph || !eh) { err = "missing chain fields"; return false; }
            if (seq->as_int() != expect_seq) { err = "seq gap"; return false; }
            if (ph->as_str() != prev) { err = "prev_hash mismatch at seq " + std::to_string(expect_seq); return false; }
            std::string expect_hash = eh->as_str();
            JValue copy = v;
            // recompute
            JValue recalc;
            {
                // rebuild entry without entry_hash
                JValue e2 = JValue::mk_obj();
                for (auto& kv : v.obj)
                    if (kv.first != "entry_hash") e2.set(kv.first, kv.second);
                std::string canon = jcanonical(e2);
                uint8_t h[32];
                sha256((const uint8_t*)canon.data(), canon.size(), h);
                std::string hh = to_hex(h, 32);
                if (hh != expect_hash) { err = "entry_hash mismatch at seq " + std::to_string(expect_seq); return false; }
            }
            (void)recalc;
            prev = expect_hash;
            ++expect_seq;
            ++n;
        }
        count = n;
        return true;
    }

private:
    std::string path_;
    int64_t next_seq_ = 1;
    std::string last_hash_ = std::string(64, '0');
    std::mutex mu_;
};

} // namespace asqs
