// ASQS - daemon configuration (config file + CLI overrides merged by main).
#pragma once
#include "circuit.h"
#include "pipeline.h"
#include "u256.h"
#include "util.h"

#include <cstdint>
#include <string>

namespace asqs {

struct Config {
    std::string ip = "0.0.0.0";
    uint16_t port = 3333;
    std::string project_root = ".";
    CircuitConfig circuit;          // defaults: surface_code_memory d=3 rounds=3
    int shots_per_share = 16;
    double max_shares_per_sec = 1.0;
    std::string difficulty_mode = "adaptive"; // "adaptive" | "fixed"
    std::string init_difficulty = "256";
    std::string min_difficulty = "0.001";
    std::string max_difficulty = "1099511627776";
    int job_interval_s = 30;
    bool local_time = false;
    bool enable_benchmark = true;    // enable 3-minute hardware benchmark on startup
    int benchmark_duration_s = 180; // benchmark duration in seconds
    GatePolicy gate;
    // Statistical validation batches (README "Validation batches"): every
    // N-th processed share derives a LARGE batch (validation_shots shots,
    // e.g. 1024-65536) instead of the normal shots_per_share. 0 disables.
    int validation_shots = 0;            // e.g. 1024; 0 = off
    int validation_every_n_shares = 100; // every N-th processed share

    static Config from_json_file(const std::string& path, std::string& err) {
        Config c;
        auto txt = read_file(path);
        if (!txt) { err = "cannot read config: " + path; return c; }
        JValue j;
        std::string perr = jparse(*txt, j);
        if (!perr.empty()) { err = "config parse: " + perr; return c; }
        if (const JValue* v = j.get("ip")) c.ip = v->as_str(c.ip);
        if (const JValue* v = j.get("port")) c.port = uint16_t(v->as_int(c.port));
        if (const JValue* v = j.get("project_root")) c.project_root = v->as_str(c.project_root);
        if (const JValue* v = j.get("shots_per_share")) c.shots_per_share = int(v->as_int(16));
        if (const JValue* v = j.get("max_shares_per_sec")) c.max_shares_per_sec = v->as_double(1.0);
        if (const JValue* v = j.get("difficulty_mode")) c.difficulty_mode = v->as_str(c.difficulty_mode);
        if (const JValue* v = j.get("init_difficulty")) c.init_difficulty = v->as_str(c.init_difficulty);
        if (const JValue* v = j.get("min_difficulty")) c.min_difficulty = v->as_str(c.min_difficulty);
        if (const JValue* v = j.get("max_difficulty")) c.max_difficulty = v->as_str(c.max_difficulty);
        if (const JValue* v = j.get("job_interval_s")) c.job_interval_s = int(v->as_int(30));
        if (const JValue* v = j.get("local_time")) c.local_time = v->as_bool(false);
        if (const JValue* v = j.get("enable_benchmark")) c.enable_benchmark = v->as_bool(true);
        if (const JValue* v = j.get("benchmark_duration_s")) c.benchmark_duration_s = int(v->as_int(180));
        if (const JValue* v = j.get("validation_shots")) c.validation_shots = int(v->as_int(0));
        if (const JValue* v = j.get("validation_every_n_shares")) c.validation_every_n_shares = int(v->as_int(100));
        if (const JValue* v = j.get("circuit")) {
            std::string cerr2;
            c.circuit = CircuitConfig::from_json(*v, cerr2);
            if (!cerr2.empty()) err = "circuit config: " + cerr2;
        }
        if (const JValue* v = j.get("gate")) {
            if (const JValue* g = v->get("min_detector_flips"))
                c.gate.min_detector_flips = g->as_int(1);
            if (const JValue* g = v->get("max_detector_flip_rate"))
                c.gate.max_detector_flip_rate = g->as_double(0.25);
            if (const JValue* g = v->get("min_distinct_syndromes"))
                c.gate.min_distinct_syndromes = g->as_int(2);
            if (const JValue* g = v->get("min_distinct_outcomes"))
                c.gate.min_distinct_outcomes = g->as_int(2);
        }
        validate(c, err);
        return c;
    }

    static void validate(const Config& c, std::string& err) {
        if (c.difficulty_mode != "adaptive" && c.difficulty_mode != "fixed")
            err = "difficulty_mode must be adaptive|fixed";
        if (c.shots_per_share < 1 || c.shots_per_share > 4096)
            err = "shots_per_share must be in [1,4096]";
        if (c.max_shares_per_sec <= 0 || c.max_shares_per_sec > 1000)
            err = "max_shares_per_sec must be in (0,1000]";
        if (c.job_interval_s < 5 || c.job_interval_s > 3600)
            err = "job_interval_s must be in [5,3600]";
        if (DiffRat::parse(c.min_difficulty).num == 0) err = "min_difficulty invalid";
        if (c.benchmark_duration_s < 30 || c.benchmark_duration_s > 3600)
            err = "benchmark_duration_s must be in [30,3600]";
        if (c.validation_shots < 0 || c.validation_shots > 65536)
            err = "validation_shots must be in [0,65536]";
        if (c.validation_shots > 0 && c.validation_shots < 16)
            err = "validation_shots must be 0 or >= 16 (a validation batch must be statistically meaningful)";
        if (c.validation_every_n_shares < 1 || c.validation_every_n_shares > 1000000000)
            err = "validation_every_n_shares must be in [1,1e9]";
    }
};

} // namespace asqs
