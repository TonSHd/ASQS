// ASQS - pipeline: pending batch writing (notvalidated/), FIFO validation
// (oldest first), promotion to outputs/ with embedded proof, usefulness gate.
#pragma once
#include "circuit.h"
#include "json.h"
#include "ledger.h"
#include "shot.h"
#include "stratum.h"

#include <string>
#include <vector>

namespace asqs {

struct GatePolicy {
    int64_t min_detector_flips = 1;       // noise>0: batch must show >= this flips
    double max_detector_flip_rate = 0.25; // sanity ceiling
    int64_t min_distinct_syndromes = 2;   // distinct nonzero detector patterns
    int64_t min_distinct_outcomes = 2;    // random_clifford: distinct outcome strings
};

struct PipelineStats {
    int64_t batches_written = 0;
    int64_t validated = 0;
    int64_t useful = 0;
    int64_t useless_deleted = 0;
    int64_t invalid_deleted = 0;
};

class Pipeline {
public:
    Pipeline(std::string root, CircuitConfig cfg, int shots_per_share, GatePolicy gate,
             Ledger& ledger, bool local_time);

    // Build + write a pending batch file. Returns final filename or empty + err.
    // validation_batch=true marks the record as a large statistical validation
    // batch (see Daemon::shot_loop / README "validation batches").
    std::string write_pending(const VerifiedShare& share, const std::vector<uint8_t>& seed,
                              const std::vector<ShotRecord>& shots, std::string& err,
                              bool validation_batch = false);

    // Validate the oldest pending file (FIFO by mtime).
    // Returns: 0 = nothing pending, 1 = promoted, 2 = deleted (useless),
    //          3 = deleted (invalid), 4 = skipped (retry later)
    int validate_next(std::string& detail);

    int pending_count();
    void cleanup_tmp_files();
    const PipelineStats& stats() const { return stats_; }

private:
    std::string notvalidated_dir() const { return root_ + "/notvalidated"; }
    std::string outputs_dir() const { return root_ + "/outputs"; }
    std::string unique_path(const std::string& dir, const std::string& prefix_ts);
    int validate_file(const std::string& path, const std::string& name, std::string& detail);

    std::string root_;
    CircuitConfig cfg_;
    int shots_per_share_;
    GatePolicy gate_;
    Ledger& ledger_;
    bool local_time_;
    PipelineStats stats_;
};

// Compute usefulness stats from a batch's shot records.
struct GateStats {
    int64_t detector_flips = 0;
    int64_t detector_total = 0;
    int64_t flip_rate_ppm = 0;
    int64_t distinct_syndromes = 0;
    int64_t distinct_outcomes = 0;
    bool useful = false;
    std::string reason;
};
GateStats evaluate_gate(const CircuitConfig& cfg, const GatePolicy& policy,
                        const std::vector<ShotRecord>& shots);

} // namespace asqs
