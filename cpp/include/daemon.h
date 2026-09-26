// ASQS - daemon: wires stratum server, shot runner, validator, ledger,
// adaptive difficulty controller, status heartbeat.
#pragma once
#include "config.h"
#include "ledger.h"
#include "pipeline.h"
#include "stratum.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace asqs {

class Daemon : public StratumCallbacks {
public:
    explicit Daemon(Config cfg);
    ~Daemon();
    int run();            // blocking; returns exit code
    int scan_once();      // validate all pending batches once, then exit

private:
    // StratumCallbacks
    void on_verified_share(const VerifiedShare& share) override;
    void on_event(const std::string& what, const std::string& detail) override;

    void shot_loop();
    void validator_loop();
    void controller_tick(int64_t now);
    void write_status();
    void log_line(const std::string& level, const std::string& msg);
    std::string seed_log_path() const { return project_root_ + "/asqs.log"; }

    // bounded share queue
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<VerifiedShare> queue_;
    static constexpr size_t kMaxQueue = 4096;

    Config cfg_;
    std::string project_root_;
    Circuit circuit_;
    Ledger ledger_;
    JobManager jobs_;
    Pipeline pipeline_;
    StratumServer server_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> scan_mode_{false};
    std::thread shot_thread_, validator_thread_;

    // logging
    std::mutex log_mu_;

    // controller state
    DiffRat current_diff_{256, 1};
    int64_t window_shares_ = 0;
    int64_t window_start_ms_ = 0;
    double hashrate_est_hps_ = 0.0;
    int64_t last_diff_change_ms_ = 0;
    int64_t total_shots_ = 0;

    // validation-batch state (every N-th processed share -> large batch)
    int64_t shares_processed_ = 0;
    int64_t validation_batches_ = 0;

    // benchmarking state
    std::atomic<bool> benchmarking_{false};
    int64_t benchmark_start_ms_ = 0;
    int64_t benchmark_shares_ = 0;
    static constexpr int64_t kBenchmarkDurationMs = 180000; // 3 minutes

    // stats snapshot for status
    int64_t started_ms_ = 0;
    std::string last_event_;
    std::string last_error_;
};

} // namespace asqs
