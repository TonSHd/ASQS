#include "daemon.h"
#include "shot.h"
#include "util.h"

#include <csignal>
#include <cstdio>
#include <fstream>

namespace asqs {

static std::atomic<bool> g_stop_flag{false};
static void handle_signal(int) { g_stop_flag = true; }

Daemon::Daemon(Config cfg)
    : cfg_(cfg), project_root_(cfg.project_root),
      ledger_(cfg.project_root + "/ledger.jsonl"), jobs_(),
      pipeline_(cfg.project_root, cfg.circuit, cfg.shots_per_share, cfg.gate, ledger_,
                cfg.local_time),
      server_(*this, jobs_),
      // The benchmark phase belongs to the adaptive controller (it is only
      // ever cleared inside controller_tick's adaptive branch); starting it
      // in fixed mode would swallow every verified share forever, since
      // nothing would ever clear the flag.
      benchmarking_(cfg_.enable_benchmark && cfg_.difficulty_mode == "adaptive") {
    current_diff_ = DiffRat::parse(cfg_.init_difficulty);
}

Daemon::~Daemon() {
    if (shot_thread_.joinable()) shot_thread_.join();
    if (validator_thread_.joinable()) validator_thread_.join();
}

void Daemon::log_line(const std::string& level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(log_mu_);
    std::ofstream f(seed_log_path(), std::ios::app);
    if (f) f << iso8601_utc(now_ms()) << " [" << level << "] " << msg << "\n";
    std::fprintf(stderr, "%s [%s] %s\n", iso8601_utc(now_ms()).c_str(), level.c_str(),
                 msg.c_str());
    if (level == "ERROR") last_error_ = msg;
}

void Daemon::on_event(const std::string& what, const std::string& detail) {
    last_event_ = what + ": " + detail;
    log_line("INFO", what + " " + detail);
}

void Daemon::on_verified_share(const VerifiedShare& share) {
    // Called from the network thread. Enqueue for the shot runner.
    // Quantum-tagged shares are, since the provenance fix, real hashcash
    // shares just like any other — no separate crypto path needed here.
    window_shares_++;

    // During benchmarking, just count shares, don't process work
    if (benchmarking_.load()) {
        benchmark_shares_++;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (queue_.size() >= kMaxQueue) {
            log_line("ERROR", "share queue overflow - dropping share (controller should raise difficulty)");
            return;
        }
        queue_.push_back(share);
    }
    qcv_.notify_one();
}

void Daemon::shot_loop() {
    log_line("INFO", "shot_loop started");
    while (!stop_.load()) {
        VerifiedShare share;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            qcv_.wait_for(lk, std::chrono::milliseconds(200),
                          [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) return;
            if (queue_.empty()) continue;
            share = queue_.front();
            queue_.pop_front();
        }

        // Every share (quantum-tagged or not) now carries a real,
        // hashcash-verified header_hex/share_hash_hex from verify_submit();
        // there is no longer a separate synthetic/dummy-header path. See
        // FORENSIC_REPORT.md for why that path was removed.
        Bytes header = from_hex(share.header_hex);
        Bytes share_hash = from_hex(share.share_hash_hex);
        std::vector<uint8_t> seed = derive_seed(share_hash.data(), header.data());

        // Validation batches: every N-th processed share derives a LARGE
        // batch (validation_shots shots, e.g. 1024+) instead of the normal
        // shots_per_share. Normal shares keep the pipeline light; validation
        // batches give the sample count needed for statistical comparison
        // against an independent reference (asqs crosscheck, PROOF_SPEC §15).
        int shot_count = cfg_.shots_per_share;
        bool is_validation = false;
        if (cfg_.validation_shots > 0) {
            ++shares_processed_;
            if (shares_processed_ % cfg_.validation_every_n_shares == 0) {
                shot_count = cfg_.validation_shots;
                is_validation = true;
                ++validation_batches_;
            }
        }

        std::vector<ShotRecord> shots;
        shots.reserve(size_t(shot_count));
        for (int k = 0; k < shot_count; ++k) {
            uint64_t ps = derive_shot_prng_seed(seed, uint32_t(k));
            shots.push_back(run_shot(circuit_, cfg_.circuit.noise, ps));
            total_shots_++;
        }
        std::string err;
        std::string fname = pipeline_.write_pending(share, seed, shots, err, is_validation);
        if (fname.empty()) {
            log_line("ERROR", "write_pending failed: " + err);
        } else {
            log_line("INFO", "batch written: " + fname + " (worker=" + share.worker +
                                 " shots=" + std::to_string(shot_count) +
                                 (is_validation ? " [VALIDATION]" : "") + ")");
        }
    }
    log_line("INFO", "shot_loop exiting");
}

void Daemon::validator_loop() {
    log_line("INFO", "validator_loop started");
    int empty_ticks = 0;
    while (!stop_.load()) {
        std::string detail;
        int r = pipeline_.validate_next(detail);
        if (r == 0) {
            if (++empty_ticks > 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        empty_ticks = 0;
        if (r == 1) log_line("INFO", detail);
        else if (r == 2) log_line("INFO", detail);
        else if (r == 3) log_line("ERROR", detail);
        else log_line("WARN", detail);
        if (scan_mode_.load()) continue; // keep draining in scan mode
    }
    log_line("INFO", "validator_loop exiting");
}

void Daemon::controller_tick(int64_t now) {
    if (cfg_.difficulty_mode != "adaptive") return;

    // Handle benchmarking phase
    if (benchmarking_.load()) {
        if (benchmark_start_ms_ == 0) {
            benchmark_start_ms_ = now;
            log_line("INFO", "benchmarking started - using initial difficulty " + current_diff_.to_string() +
                                 " for " + std::to_string(cfg_.benchmark_duration_s) + " seconds");
            return;
        }

        int64_t elapsed_ms = now - benchmark_start_ms_;
        int64_t benchmark_duration_ms = cfg_.benchmark_duration_s * 1000;

        if (elapsed_ms >= benchmark_duration_ms) {
            // Benchmark complete - calculate optimal difficulty
            double elapsed_s = double(elapsed_ms) / 1000.0;
            if (benchmark_shares_ > 0) {
                double bench_hashrate = double(benchmark_shares_) * current_diff_.to_double() * 4294967296.0 / elapsed_s;
                hashrate_est_hps_ = bench_hashrate;

                // Calculate optimal difficulty for desired share rate
                double desired_diff = bench_hashrate / (cfg_.max_shares_per_sec * 4294967296.0);

                DiffRat dmin = DiffRat::parse(cfg_.min_difficulty);
                DiffRat dmax = DiffRat::parse(cfg_.max_difficulty);

                // Clamp to valid range
                if (desired_diff < dmin.to_double()) desired_diff = dmin.to_double();
                if (desired_diff > dmax.to_double()) desired_diff = dmax.to_double();

                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", desired_diff);
                DiffRat optimal_diff = DiffRat::parse(buf);

                current_diff_ = optimal_diff;
                log_line("INFO", "benchmarking complete - measured " +
                                     std::to_string(uint64_t(bench_hashrate / 1e9)) + " GH/s, " +
                                     "set optimal difficulty to " + current_diff_.to_string());
            } else {
                log_line("WARN", "benchmarking complete but no shares received - using default difficulty");
                current_diff_ = DiffRat::parse(cfg_.init_difficulty);
            }

            benchmarking_ = false;
            benchmark_shares_ = 0;
            benchmark_start_ms_ = 0;
            server_.broadcast_difficulty(current_diff_);
            window_start_ms_ = now;
            window_shares_ = 0;
            return;
        }

        // During benchmarking, just log progress periodically
        if (elapsed_ms % 30000 < 1000) { // Every 30 seconds
            double elapsed_s = double(elapsed_ms) / 1000.0;
            double progress = elapsed_s / cfg_.benchmark_duration_s * 100.0;
            if (benchmark_shares_ > 0) {
                double current_hashrate = double(benchmark_shares_) * current_diff_.to_double() * 4294967296.0 / elapsed_s;
                log_line("INFO", "benchmarking progress: " + std::to_string(int(progress)) + "% - " +
                                     std::to_string(uint64_t(current_hashrate / 1e9)) + " GH/s (" +
                                     std::to_string(benchmark_shares_) + " shares)");
            }
        }
        return;
    }

    // Normal adaptive difficulty control
    if (window_start_ms_ == 0) { window_start_ms_ = now; return; }
    double window_s = double(now - window_start_ms_) / 1000.0;
    if (window_s < 20.0) return;

    size_t qdepth = 0;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        qdepth = queue_.size();
    }
    int pending = pipeline_.pending_count();

    DiffRat dmin = DiffRat::parse(cfg_.min_difficulty);
    DiffRat dmax = DiffRat::parse(cfg_.max_difficulty);
    auto clamp = [&](const DiffRat& d) {
        DiffRat x = d;
        if (x.to_double() < dmin.to_double()) x = dmin;
        if (x.to_double() > dmax.to_double()) x = dmax;
        return x;
    };

    // Emergency: host is behind -> raise difficulty hard.
    if (qdepth > 128 || pending > 256) {
        DiffRat nd = clamp(DiffRat{current_diff_.num * 8, current_diff_.den});
        if (nd.to_double() != current_diff_.to_double()) {
            current_diff_ = nd;
            last_diff_change_ms_ = now;
            window_shares_ = 0;
            window_start_ms_ = now;
            server_.broadcast_difficulty(current_diff_);
            log_line("WARN", "difficulty raised (backlog) to " + current_diff_.to_string());
            return;
        }
    }

    if (window_shares_ > 0) {
        // hashrate estimate from observed shares
        double inst = double(window_shares_) * current_diff_.to_double() * 4294967296.0 / window_s;
        hashrate_est_hps_ = hashrate_est_hps_ == 0 ? inst : 0.7 * hashrate_est_hps_ + 0.3 * inst;
        double desired = hashrate_est_hps_ / (cfg_.max_shares_per_sec * 4294967296.0);
        double ratio = desired / current_diff_.to_double();
        if ((ratio > 1.3 || ratio < 0.77) && now - last_diff_change_ms_ > 30000) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.9g", desired);
            DiffRat nd = clamp(DiffRat::parse(buf));
            if (nd.to_double() != current_diff_.to_double()) {
                current_diff_ = nd;
                last_diff_change_ms_ = now;
                server_.broadcast_difficulty(current_diff_);
                log_line("INFO", "difficulty adapted to " + current_diff_.to_string() +
                                     " (est " + std::to_string(uint64_t(hashrate_est_hps_ / 1e9)) +
                                     " GH/s)");
            }
        }
    }
    window_shares_ = 0;
    window_start_ms_ = now;
}

void Daemon::write_status() {
    JValue s = JValue::mk_obj();
    s.set("now_utc", JValue::mk_str(iso8601_utc(now_ms())));
    s.set("started_ms", JValue::mk_int(started_ms_));
    if (started_ms_ > 0)
        s.set("uptime_s", JValue::mk_int((now_ms() - started_ms_) / 1000));
    s.set("version", JValue::mk_str("0.1.0"));
    s.set("miners_connected", JValue::mk_int(server_.connected_miners()));
    {
        std::lock_guard<std::mutex> lk(qmu_);
        s.set("queue_depth", JValue::mk_int(int64_t(queue_.size())));
    }
    s.set("pending_batches", JValue::mk_int(pipeline_.pending_count()));
    s.set("difficulty", JValue::mk_str(current_diff_.to_string()));
    s.set("difficulty_mode", JValue::mk_str(cfg_.difficulty_mode));
    s.set("hashrate_est_hps", JValue::mk_int(int64_t(hashrate_est_hps_)));
    s.set("shares_received", JValue::mk_int(int64_t(server_.shares_received())));
    s.set("shares_verified", JValue::mk_int(int64_t(server_.shares_verified())));
    s.set("shares_rejected", JValue::mk_int(int64_t(server_.shares_rejected())));
    s.set("batches_written", JValue::mk_int(pipeline_.stats().batches_written));
    s.set("batches_validated", JValue::mk_int(pipeline_.stats().validated));
    s.set("batches_useful", JValue::mk_int(pipeline_.stats().useful));
    s.set("batches_useless_deleted", JValue::mk_int(pipeline_.stats().useless_deleted));
    s.set("batches_invalid_deleted", JValue::mk_int(pipeline_.stats().invalid_deleted));
    s.set("shots_total", JValue::mk_int(total_shots_));
    s.set("circuit", cfg_.circuit.to_json());
    s.set("circuit_sha256", JValue::mk_str(cfg_.circuit.sha256_hex()));
    s.set("noise", cfg_.circuit.noise.to_json());
    s.set("shots_per_share", JValue::mk_int(cfg_.shots_per_share));
    s.set("validation_shots", JValue::mk_int(cfg_.validation_shots));
    s.set("validation_every_n_shares", JValue::mk_int(cfg_.validation_every_n_shares));
    s.set("validation_batches_total", JValue::mk_int(validation_batches_));
    s.set("last_event", JValue::mk_str(last_event_));
    s.set("last_error", JValue::mk_str(last_error_));
    s.set("benchmarking", JValue::mk_bool(benchmarking_.load()));
    if (benchmarking_.load()) {
        s.set("benchmark_shares", JValue::mk_int(benchmark_shares_));
        if (benchmark_start_ms_ > 0) {
            int64_t elapsed = now_ms() - benchmark_start_ms_;
            s.set("benchmark_elapsed_s", JValue::mk_int(elapsed / 1000));
        }
    }
    write_file_atomic(project_root_ + "/status.json", jserialize(s, true));
}

int Daemon::scan_once() {
    scan_mode_ = true;
    ensure_dir(project_root_);
    std::string lerr;
    if (!ledger_.init(lerr)) {
        log_line("ERROR", "ledger init: " + lerr);
        return 1;
    }
    pipeline_.cleanup_tmp_files();
    int n = 0;
    for (;;) {
        std::string detail;
        int r = pipeline_.validate_next(detail);
        if (r == 0) break;
        if (r == 1 || r == 2) log_line("INFO", detail);
        else if (r == 3) log_line("ERROR", detail);
        else log_line("WARN", detail);
        if (++n > 100000) break;
    }
    log_line("INFO", "scan complete: " + std::to_string(n) + " batches processed");
    return 0;
}

int Daemon::run() {
    try {
        log_line("INFO", "daemon startup started");
        started_ms_ = now_ms();
        log_line("INFO", "setting started_ms");
        ensure_dir(project_root_);
        log_line("INFO", "ensured project root directory");
        std::string lerr;
        if (!ledger_.init(lerr)) {
            log_line("ERROR", "ledger init: " + lerr);
            return 1;
        }
        log_line("INFO", "ledger initialized");
        pipeline_.cleanup_tmp_files();
        log_line("INFO", "cleaned tmp files");

        // Build + self-validate circuit.
        log_line("INFO", "building circuit");
        try {
            circuit_ = build_circuit(cfg_.circuit);
        } catch (const std::exception& e) {
            log_line("ERROR", std::string("circuit build failed: ") + e.what());
            return 1;
        }
        log_line("INFO", "circuit built successfully");
        jobs_.set_circuit_tag(cfg_.circuit.sha256_hex().substr(0, 16));
        log_line("INFO", "circuit tag set");
        log_line("INFO", "circuit " + cfg_.circuit.type + " sha256=" + cfg_.circuit.sha256_hex() +
                             " qubits=" + std::to_string(circuit_.num_qubits) + " meas=" +
                             std::to_string(circuit_.num_measurements) + " detectors=" +
                             std::to_string(circuit_.detectors.size()));

        // Server
        log_line("INFO", "starting server");
        std::string serr;
        if (!server_.start(cfg_.ip, cfg_.port, serr)) {
            log_line("ERROR", "server start: " + serr);
            return 1;
        }
        log_line("INFO", "server started");
        server_.set_job_interval(cfg_.job_interval_s);
        log_line("INFO", "job interval set");

        // Start with quantum computation jobs instead of hashcash
        log_line("INFO", "creating quantum job");
        Job quantum_job = server_.job_manager().make_quantum_job(
            cfg_.circuit.type,
            cfg_.circuit.d,
            cfg_.circuit.rounds,
            cfg_.shots_per_share,
            current_diff_
        );
        log_line("INFO", "quantum job created");
        server_.broadcast_quantum_job(quantum_job);
        log_line("INFO", "quantum job broadcasted");
        log_line("INFO", "listening on " + cfg_.ip + ":" + std::to_string(cfg_.port) +
                             " (quantum mode: " + cfg_.circuit.type + " d=" +
                             std::to_string(cfg_.circuit.d) + " rounds=" +
                             std::to_string(cfg_.circuit.rounds) + ")");

        ::signal(SIGINT, handle_signal);
        ::signal(SIGTERM, handle_signal);
        ::signal(SIGPIPE, SIG_IGN);
        log_line("INFO", "signals installed");

        log_line("INFO", "starting threads");
        shot_thread_ = std::thread([this] { shot_loop(); });
        validator_thread_ = std::thread([this] { validator_loop(); });
        log_line("INFO", "threads started");

        int64_t last_status = 0, last_tick = 0;
        while (!g_stop_flag) {
            // service network for up to ~200ms
            server_.run_once(200);
            int64_t now = now_ms();
            if (now - last_tick >= 1000) {
                last_tick = now;
                controller_tick(now);
            }
            if (now - last_status >= 1000) {
                last_status = now;
                write_status();
            }
        }
        log_line("INFO", "shutting down");
        stop_ = true;
        qcv_.notify_all();
        server_.stop();
        if (shot_thread_.joinable()) shot_thread_.join();
        if (validator_thread_.joinable()) validator_thread_.join();
        write_status();
        log_line("INFO", "bye");
        return 0;
    } catch (const std::exception& e) {
        log_line("ERROR", std::string("daemon exception: ") + e.what());
        return 1;
    } catch (...) {
        log_line("ERROR", "daemon unknown exception");
        return 1;
    }
}

} // namespace asqs
