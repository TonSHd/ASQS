// ASQS - Stratum v1 server (pool side) + synthetic job fabrication +
// share verification. Compatible with ESP-Miner / Bitaxe (public-pool-style
// handshake; extranonce sizes are not assumed on verify - any even-length
// extranonce2 submitted by the miner is accepted).
//
// Byte-order conventions (normative, PROOF SPEC §5; verified byte-exact
// against ESP-Miner v2.15.3 — the firmware running on the Bitaxe Gamma 601
// / BM1370 — components/stratum/mining.c construct_bm_job() +
// test_nonce_value()):
//   header80 = LE32(version) || prevhash_field(32) || SHA256D(coinbase)
//              || LE32(ntime) || LE32(nbits) || LE32(nonce)
//   coinbase = cb1 || ex1 || ex2 || cb2
//   notify prevhash hex = hex(bswap32-per-word(prevhash_field))
//              (real-world stratum convention: pools send
//              word_reverse(display prevhash); the firmware applies
//              bswap32-per-word() to the wire value before inserting it
//              into the header — mining.c:78-80)
//   notify version/nbits/ntime hex, submit nonce/ntime hex = BE hex of u32
//   share valid iff LE-value(SHA256D(header80)) <= target (BE compare)
#pragma once
#include "json.h"
#include "u256.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace asqs {

struct VerifiedShare {
    std::string worker;
    std::string job_id;
    std::string extranonce1_hex;
    std::string extranonce2_hex;
    std::string ntime_hex;
    std::string nonce_hex;
    std::string header_hex;    // 80-byte header (hex)
    std::string share_hash_hex;// raw SHA256D(header80) digest (hex)
    std::string difficulty_str;// canonical difficulty string
    int64_t received_ms = 0;

    // Quantum computation results
    bool quantum_mode = false;
    std::string quantum_results_hex; // encoded quantum simulation results
    int shots_completed = 0;
};

struct StratumCallbacks {
    virtual void on_verified_share(const VerifiedShare& share) = 0;
    virtual void on_event(const std::string& what, const std::string& detail) = 0;
    virtual ~StratumCallbacks() = default;
};

struct Job {
    std::string id;            // hex job id
    // Bitcoin mining fields (kept for stratum compatibility)
    std::vector<uint8_t> prevhash_field; // 32 bytes as serialized in header
    std::vector<uint8_t> coinbase1, coinbase2;
    uint32_t version = 0x20000000;
    uint32_t ntime = 0;
    uint32_t nbits = 0;
    DiffRat difficulty;
    U256 target;
    int64_t created_ms = 0;

    // Quantum computation fields
    std::string circuit_type;      // "surface_code_memory", "random_clifford", etc.
    int circuit_d = 3;             // code distance
    int circuit_rounds = 3;       // number of rounds
    int shots_per_job = 16;        // number of quantum shots to perform
    std::string circuit_seed;      // seed for deterministic circuit generation
    bool quantum_mode = false;     // true if this is a quantum computation job
};

class JobManager {
public:
    void set_circuit_tag(const std::string& tag_hex); // embedded into coinbase1
    Job make_job(const DiffRat& difficulty);
    Job make_quantum_job(const std::string& circuit_type, int d, int rounds, int shots,
                         const DiffRat& difficulty);
    // Returns empty id string on failure + reason.
    std::string verify_submit(const std::string& job_id, const std::string& ex2_hex,
                              const std::string& ntime_hex, const std::string& nonce_hex,
                              const std::string& version_bits_hex,
                              const std::string& ex1_hex, VerifiedShare& out_share,
                              bool& duplicate);
private:
    std::mutex mu_;
    std::map<std::string, Job> jobs_; // recent jobs
    uint64_t job_counter_ = 0;
    std::string circuit_tag_ = "0000000000000000";
    uint64_t rng_state_ = 0x51ce5eedULL;
    std::map<std::string, int64_t> seen_; // dedupe: key -> ms
};

class StratumServer {
public:
    StratumServer(StratumCallbacks& sink, JobManager& jobs);
    ~StratumServer();

    bool start(const std::string& ip, uint16_t port, std::string& err);
    void run_once(int timeout_ms);                   // single epoll service pass
    void stop();
    void broadcast_difficulty(const DiffRat& d);    // set_difficulty + clean job
    void broadcast_quantum_job(const Job& job);      // send quantum computation job
    void set_job_interval(int seconds) { job_interval_s_ = seconds; }
    JobManager& job_manager() { return jobs_; }      // access to job manager

    // stats
    int connected_miners() const;
    uint64_t shares_received() const { return shares_received_.load(); }
    uint64_t shares_verified() const { return shares_verified_.load(); }
    uint64_t shares_rejected() const { return shares_rejected_.load(); }

private:
    struct Conn {
        int fd = -1;
        std::string inbuf, outbuf;
        bool subscribed = false;
        bool authorized = false;
        std::string worker;
        std::string extranonce1_hex;
        std::string session_id;
        int64_t connected_ms = 0;
        int64_t last_rx_ms = 0;
        uint64_t submits_window = 0;
        int64_t window_start_ms = 0;
        bool throttled = false;
    };

    void accept_new();
    void on_readable(Conn& c);
    void on_writable(Conn& c);
    void close_conn(int fd);
    void handle_line(Conn& c, const std::string& line);
    void send_json(Conn& c, const JValue& v);
    void send_request(Conn& c, const std::string& method, const JValue& params);
    void send_response(Conn& c, const JValue& id, const JValue& result, const JValue& error);
    void push_job(Conn& c, bool clean);
    void push_quantum_job(Conn& c, const Job& job);
    void send_difficulty(Conn& c, const DiffRat& d);
    void housekeeping(int64_t now);

    StratumCallbacks& sink_;
    JobManager& jobs_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    std::map<int, Conn> conns_;
    DiffRat current_diff_{256, 1};
    Job last_job_;
    bool has_job_ = false;
    int job_interval_s_ = 30;
    std::atomic<uint64_t> shares_received_{0};
    std::atomic<uint64_t> shares_verified_{0};
    std::atomic<uint64_t> shares_rejected_{0};
};

// Utility: reconstruct header from parts (also used by validator).
// Returns false on malformed input.
bool build_header(const Job& job, const std::string& ex1_hex, const std::string& ex2_hex,
                  uint32_t ntime, uint32_t nonce, uint32_t version_bits,
                  std::vector<uint8_t>& header80_out,
                  std::vector<uint8_t>& share_hash_out);

// Verify a share's hashcash directly from stored fields (validator path).
// header_hex: 160 hex chars. Returns true iff LE(hash) <= target.
bool verify_hashcash(const std::string& header_hex, const std::string& difficulty_str,
                     std::vector<uint8_t>& share_hash_out);

} // namespace asqs
