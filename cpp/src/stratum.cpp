#include "stratum.h"
#include "sha256.h"
#include "util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <random>
#include <cstdio>

namespace asqs {

// ------------------------------------------------------------------
// JobManager
// ------------------------------------------------------------------

void JobManager::set_circuit_tag(const std::string& tag_hex) {
    std::lock_guard<std::mutex> lk(mu_);
    circuit_tag_ = tag_hex;
}

Job JobManager::make_job(const DiffRat& difficulty) {
    std::lock_guard<std::mutex> lk(mu_);
    Job j;
    ++job_counter_;
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%08llx", (unsigned long long)job_counter_);
    j.id = idbuf;
    // prevhash field: 32 pseudorandom bytes (unique per job)
    j.prevhash_field.resize(32);
    for (int i = 0; i < 32; i += 8) {
        rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t v = rng_state_ ^ (uint64_t)now_ms();
        std::memcpy(j.prevhash_field.data() + i, &v, (i + 8 <= 32) ? 8 : (32 - i));
    }
    // coinbase1: "ASQS" magic + LE32(job counter) + circuit tag (8 bytes)
    j.coinbase1 = {'A', 'S', 'Q', 'S'};
    for (int i = 0; i < 4; ++i)
        j.coinbase1.push_back(uint8_t((job_counter_ >> (8 * i)) & 0xff));
    Bytes tag = from_hex(circuit_tag_);
    if (tag.size() != 8) tag.assign(8, 0);
    j.coinbase1.insert(j.coinbase1.end(), tag.begin(), tag.end());
    j.coinbase2 = {'A', 'S', 'Q', 'S', '-', 'T', 'A', 'I', 'L'};
    j.ntime = uint32_t(now_sec());
    j.difficulty = difficulty;
    j.target = target_from_difficulty(difficulty);
    j.nbits = compact_encode(j.target);
    j.created_ms = now_ms();
    j.quantum_mode = false;
    jobs_[j.id] = j;
    // Prune stale jobs (keep 10 minutes).
    int64_t cutoff = now_ms() - 600000;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->second.created_ms < cutoff) it = jobs_.erase(it);
        else ++it;
    }
    // Prune seen-map.
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (it->second < cutoff) it = seen_.erase(it);
        else ++it;
    }
    return j;
}

Job JobManager::make_quantum_job(const std::string& circuit_type, int d, int rounds, int shots,
                                  const DiffRat& difficulty) {
    std::lock_guard<std::mutex> lk(mu_);
    Job j;
    ++job_counter_;
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%08llx", (unsigned long long)job_counter_);
    j.id = idbuf;

    // Generate deterministic seed for circuit
    uint64_t seed = job_counter_ ^ (uint64_t)now_ms();
    char seedbuf[32];
    std::snprintf(seedbuf, sizeof(seedbuf), "%016llx", (unsigned long long)seed);
    j.circuit_seed = seedbuf;

    // Quantum computation parameters (bookkeeping only — the header/hashcash
    // path below is identical to make_job(); a quantum job is a real,
    // ASIC-minable hashcash job that also happens to be tagged with which
    // circuit the resulting share_hash will seed).
    j.circuit_type = circuit_type;
    j.circuit_d = d;
    j.circuit_rounds = rounds;
    j.shots_per_job = shots;
    j.quantum_mode = true;

    // Same real Bitcoin-header-shaped fields as make_job(): a pseudorandom,
    // per-job prevhash field and a genuine difficulty target. No shortcuts —
    // real proof-of-work is required to redeem this job, same as any other.
    j.prevhash_field.resize(32);
    for (int i = 0; i < 32; i += 8) {
        rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t v = rng_state_ ^ (uint64_t)now_ms();
        std::memcpy(j.prevhash_field.data() + i, &v, (i + 8 <= 32) ? 8 : (32 - i));
    }
    j.coinbase1 = {'A', 'S', 'Q', 'S', 'Q'};
    j.coinbase2 = {'Q', 'U', 'A', 'N', 'T'};
    j.version = 0x20000000;
    j.ntime = uint32_t(now_sec());
    j.difficulty = difficulty;
    j.target = target_from_difficulty(difficulty);
    j.nbits = compact_encode(j.target);
    j.created_ms = now_ms();

    jobs_[j.id] = j;

    // Prune stale jobs + seen-map (same as make_job).
    int64_t cutoff = now_ms() - 600000;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->second.created_ms < cutoff) it = jobs_.erase(it);
        else ++it;
    }
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (it->second < cutoff) it = seen_.erase(it);
        else ++it;
    }

    return j;
}

std::string JobManager::verify_submit(const std::string& job_id, const std::string& ex2_hex,
                                      const std::string& ntime_hex, const std::string& nonce_hex,
                                      const std::string& version_bits_hex,
                                      const std::string& ex1_hex, VerifiedShare& out_share,
                                      bool& duplicate) {
    duplicate = false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return "unknown or stale job id";
    const Job& job = it->second;

    if (ex2_hex.empty() || ex2_hex.size() > 128 || ex2_hex.size() % 2 != 0)
        return "bad extranonce2";
    if (ntime_hex.size() != 8 || nonce_hex.size() != 8) return "bad ntime/nonce";
    uint32_t ntime = 0, nonce = 0, version_bits = 0;
    try {
        ntime = uint32_t(std::stoul(ntime_hex, nullptr, 16));
        nonce = uint32_t(std::stoul(nonce_hex, nullptr, 16));
        if (!version_bits_hex.empty()) {
            if (version_bits_hex.size() != 8) return "bad version bits";
            version_bits = uint32_t(std::stoul(version_bits_hex, nullptr, 16));
        }
    } catch (...) {
        return "bad ntime/nonce";
    }
    // ntime rolling tolerance: [job.ntime, job.ntime + 2h]
    if (ntime < job.ntime || ntime > job.ntime + 7200) return "ntime out of range";

    std::vector<uint8_t> header, hash;
    if (!build_header(job, ex1_hex, ex2_hex, ntime, nonce, version_bits, header, hash))
        return "header build failed";

    // hashcash: LE(hash) <= target  <=>  reverse(hash) <= target (BE compare)
    std::array<uint8_t, 32> be{};
    for (int i = 0; i < 32; ++i) be[i] = hash[31 - i];
    auto target_be = job.target.to_be_bytes();

    if (be > target_be) return "hash above target (low difficulty)";

    std::string dedup_key = job_id + "|" + ex2_hex + "|" + ntime_hex + "|" + nonce_hex + "|" + version_bits_hex;
    auto sit = seen_.find(dedup_key);
    if (sit != seen_.end()) { duplicate = true; return "duplicate share"; }
    seen_[dedup_key] = now_ms();

    out_share.job_id = job_id;
    out_share.extranonce1_hex = ex1_hex;
    out_share.extranonce2_hex = ex2_hex;
    out_share.ntime_hex = ntime_hex;
    out_share.nonce_hex = nonce_hex;
    out_share.header_hex = to_hex(header);
    out_share.share_hash_hex = to_hex(hash);
    out_share.difficulty_str = job.difficulty.to_string();
    out_share.received_ms = now_ms();
    // Bookkeeping only (which circuit this job was tagged for) — does not
    // change how the share was verified above; real hashcash either way.
    out_share.quantum_mode = job.quantum_mode;
    return "";
}

// ------------------------------------------------------------------
// header building / verification
// ------------------------------------------------------------------

// Conventional AsicBoost/BIP310 version-rolling mask granted in
// mining.configure. Used both to answer mining.configure and to
// reconstruct the rolled version in build_header().
static constexpr uint32_t kVersionRollMask = 0x1fffe000u;

// Real-world stratum v1 wire convention, byte-exact against ESP-Miner
// v2.15.3 (the firmware on the Bitaxe Gamma 601 / BM1370 — see
// components/stratum/mining.c construct_bm_job()/test_nonce_value() and
// utils.c reverse_endianness_per_word()): the miner places
// bswap32-per-word(hex2bin(notify.prevhash)) into the 80-byte header. The
// wire value real pools exchange is therefore the per-word-swapped form
// of the raw header field (equivalently: word_reverse of the
// display-order prevhash). build_header() uses job.prevhash_field
// verbatim, so the wire must carry this swapped form. See
// FORENSIC_REPORT.md §10 for the derivation against real hardware.
static std::vector<uint8_t> wire_prevhash(const std::vector<uint8_t>& prevhash_field) {
    std::vector<uint8_t> w = prevhash_field;
    for (size_t i = 0; i + 4 <= w.size(); i += 4) {
        std::swap(w[i], w[i + 3]);
        std::swap(w[i + 1], w[i + 2]);
    }
    return w;
}

static void put_le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff));
}

bool build_header(const Job& job, const std::string& ex1_hex, const std::string& ex2_hex,
                  uint32_t ntime, uint32_t nonce, uint32_t version_bits,
                  std::vector<uint8_t>& header80_out,
                  std::vector<uint8_t>& share_hash_out) {
    Bytes ex1 = from_hex(ex1_hex);
    Bytes ex2 = from_hex(ex2_hex);
    if (ex1.empty() && !ex1_hex.empty()) return false;
    if (ex2.empty() && !ex2_hex.empty()) return false;
    // coinbase = cb1 || ex1 || ex2 || cb2
    std::vector<uint8_t> cb = job.coinbase1;
    cb.insert(cb.end(), ex1.begin(), ex1.end());
    cb.insert(cb.end(), ex2.begin(), ex2.end());
    cb.insert(cb.end(), job.coinbase2.begin(), job.coinbase2.end());
    uint8_t m[32];
    sha256d(cb.data(), cb.size(), m);
    header80_out.clear();
    // version-rolling (BIP310): the miner submits the rolled nVersion bits
    // as the 6th mining.submit parameter; the actual header version is
    // (base & ~mask) | (bits & mask), mirroring what the ASIC hashed
    // (ESP-Miner asic_result_task.c:78 submits rolled ^ base, and
    // increment_bitmask() in mining.c only ever moves bits inside the
    // granted mask).
    put_le32(header80_out, (job.version & ~kVersionRollMask) |
                               (version_bits & kVersionRollMask));
    header80_out.insert(header80_out.end(), job.prevhash_field.begin(),
                        job.prevhash_field.end());
    // Merkle root enters the header RAW: the firmware inserts the
    // sha256d(coinbase) digest bytes verbatim (mining.c:88 midstate_data /
    // mining.c:155 test_nonce_value), with no reversal.
    header80_out.insert(header80_out.end(), m, m + 32);
    put_le32(header80_out, ntime);
    put_le32(header80_out, job.nbits);
    put_le32(header80_out, nonce);
    if (header80_out.size() != 80) return false;
    share_hash_out.resize(32);
    sha256d(header80_out.data(), header80_out.size(), share_hash_out.data());
    return true;
}

bool verify_hashcash(const std::string& header_hex, const std::string& difficulty_str,
                     std::vector<uint8_t>& share_hash_out) {
    Bytes header = from_hex(header_hex);
    if (header.size() != 80) return false;
    share_hash_out.resize(32);
    sha256d(header.data(), header.size(), share_hash_out.data());
    DiffRat d = DiffRat::parse(difficulty_str);
    U256 target = target_from_difficulty(d);
    std::array<uint8_t, 32> be{};
    for (int i = 0; i < 32; ++i) be[i] = share_hash_out[31 - i];
    return !(be > target.to_be_bytes());
}

// ------------------------------------------------------------------
// StratumServer
// ------------------------------------------------------------------

StratumServer::StratumServer(StratumCallbacks& sink, JobManager& jobs)
    : sink_(sink), jobs_(jobs) {}

StratumServer::~StratumServer() { stop(); }

bool StratumServer::start(const std::string& ip, uint16_t port, std::string& err) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) { err = "socket() failed"; return false; }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") addr.sin_addr.s_addr = INADDR_ANY;
    else if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        err = "invalid bind ip";
        return false;
    }
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        err = std::string("bind() failed: ") + std::strerror(errno);
        return false;
    }
    if (listen(listen_fd_, 64) != 0) { err = "listen() failed"; return false; }
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) { err = "epoll_create1 failed"; return false; }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
    return true;
}

void StratumServer::stop() {
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    for (auto& [fd, c] : conns_) { (void)c; ::close(fd); }
    conns_.clear();
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
}

int StratumServer::connected_miners() const { return (int)conns_.size(); }

void StratumServer::accept_new() {
    for (;;) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0) return;
        if ((int)conns_.size() >= 128) { ::close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Conn& c = conns_[fd];
        c.fd = fd;
        c.connected_ms = now_ms();
        c.last_rx_ms = c.connected_ms;
        c.session_id = to_hex(sha256_vec(Bytes{(uint8_t)(fd & 0xff), (uint8_t)now_sec()}))
                           .substr(0, 16);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        sink_.on_event("miner_connect", "fd=" + std::to_string(fd));
    }
}

void StratumServer::close_conn(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns_.erase(fd);
}

void StratumServer::send_json(Conn& c, const JValue& v) {
    std::string line = jserialize(v, false);
    if (line.empty()) return;
    line.push_back('\n');
    c.outbuf += line;
    // Try immediate write; enable EPOLLOUT if pending.
    // (Simplified: rely on level-triggered epoll + writable check.)
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    ev.data.fd = c.fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev);
}

void StratumServer::send_response(Conn& c, const JValue& id, const JValue& result,
                                  const JValue& error) {
    JValue v = JValue::mk_obj();
    v.set("id", id);
    v.set("result", result);
    v.set("error", error);
    send_json(c, v);
}

void StratumServer::send_request(Conn& c, const std::string& method, const JValue& params) {
    JValue v = JValue::mk_obj();
    v.set("id", JValue::mk_null());
    v.set("method", JValue::mk_str(method));
    v.set("params", params);
    send_json(c, v);
}

void StratumServer::send_difficulty(Conn& c, const DiffRat& d) {
    JValue params = JValue::mk_arr();
    if (d.den == 1) params.push(JValue::mk_int(int64_t(d.num)));
    else params.push(JValue::mk_dbl(d.to_double()));
    send_request(c, "mining.set_difficulty", params);
}

void StratumServer::push_job(Conn& c, bool clean) {
    if (!has_job_) return;
    const Job& j = last_job_;
    JValue params = JValue::mk_arr();
    params.push(JValue::mk_str(j.id));
    // Wire prevhash: per-word byte-swapped relative to the raw header
    // field — the real-world stratum convention. Firmware applies
    // bswap32-per-word() to the wire value before inserting it into the
    // 80-byte header (ESP-Miner mining.c:78-80, confirmed against real
    // BM1370 hardware), so a pool must send the swapped form for real
    // miners to hash the header the pool will reconstruct. See
    // wire_prevhash() and FORENSIC_REPORT.md §10.
    params.push(JValue::mk_str(to_hex(wire_prevhash(j.prevhash_field))));
    params.push(JValue::mk_str(to_hex(j.coinbase1)));
    params.push(JValue::mk_str(to_hex(j.coinbase2)));
    params.push(JValue::mk_arr()); // merkle branches
    char vb[16], nb[16], tb[16];
    std::snprintf(vb, sizeof(vb), "%08x", j.version);
    std::snprintf(nb, sizeof(nb), "%08x", j.nbits);
    std::snprintf(tb, sizeof(tb), "%08x", j.ntime);
    params.push(JValue::mk_str(vb));
    params.push(JValue::mk_str(nb));
    params.push(JValue::mk_str(tb));
    params.push(JValue::mk_bool(clean));
    send_request(c, "mining.notify", params);
}

void StratumServer::push_quantum_job(Conn& c, const Job& job) {
    // A quantum job is a real hashcash job underneath (see make_quantum_job);
    // the miner must be told its real difficulty target, and the notified
    // prevhash follows the real-world wire convention (per-word swap of the
    // raw header field — see push_job()/wire_prevhash()).
    send_difficulty(c, job.difficulty);

    // Send standard mining.notify for compatibility
    JValue params = JValue::mk_arr();
    params.push(JValue::mk_str(job.id));
    // Wire prevhash in the real-world stratum convention (per-word swap of
    // the raw header field) — see push_job()/wire_prevhash() and
    // FORENSIC_REPORT.md §10.
    params.push(JValue::mk_str(to_hex(wire_prevhash(job.prevhash_field))));
    params.push(JValue::mk_str(to_hex(job.coinbase1)));
    params.push(JValue::mk_str(to_hex(job.coinbase2)));
    params.push(JValue::mk_arr()); // merkle branches
    char vb[16], nb[16], tb[16];
    std::snprintf(vb, sizeof(vb), "%08x", job.version);
    std::snprintf(nb, sizeof(nb), "%08x", job.nbits);
    std::snprintf(tb, sizeof(tb), "%08x", job.ntime);
    params.push(JValue::mk_str(vb));
    params.push(JValue::mk_str(nb));
    params.push(JValue::mk_str(tb));
    params.push(JValue::mk_bool(true)); // clean jobs
    send_request(c, "mining.notify", params);

    // Send quantum computation parameters via custom method
    JValue qparams = JValue::mk_arr();
    qparams.push(JValue::mk_str(job.id));
    qparams.push(JValue::mk_str(job.circuit_type));
    qparams.push(JValue::mk_int(job.circuit_d));
    qparams.push(JValue::mk_int(job.circuit_rounds));
    qparams.push(JValue::mk_int(job.shots_per_job));
    qparams.push(JValue::mk_str(job.circuit_seed));
    send_request(c, "mining.quantum_job", qparams);
}

void StratumServer::broadcast_difficulty(const DiffRat& d) {
    current_diff_ = d;
    last_job_ = jobs_.make_job(d);
    has_job_ = true;
    for (auto& [fd, c] : conns_) {
        (void)fd;
        send_difficulty(c, d);
        if (c.subscribed) push_job(c, true);
    }
}

void StratumServer::broadcast_quantum_job(const Job& job) {
    last_job_ = job;
    has_job_ = true;
    current_diff_ = job.difficulty; // quantum jobs use difficulty 1

    for (auto& [fd, c] : conns_) {
        (void)fd;
        send_difficulty(c, job.difficulty);
        if (c.subscribed) push_quantum_job(c, job);
    }
}

void StratumServer::on_writable(Conn& c) {
    if (c.outbuf.empty()) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = c.fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev);
        return;
    }
    ssize_t n = ::send(c.fd, c.outbuf.data(), c.outbuf.size(), MSG_NOSIGNAL);
    if (n > 0) {
        c.outbuf.erase(0, size_t(n));
        if (c.outbuf.empty()) {
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = c.fd;
            epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, c.fd, &ev);
        }
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // keep EPOLLOUT
    } else {
        close_conn(c.fd);
    }
}

void StratumServer::on_readable(Conn& c) {
    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.inbuf.append(buf, size_t(n));
            c.last_rx_ms = now_ms();
            if (c.inbuf.size() > 1024 * 1024) { close_conn(c.fd); return; }
            continue;
        }
        if (n == 0) { close_conn(c.fd); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(c.fd);
        return;
    }
    // Extract lines.
    size_t pos;
    while ((pos = c.inbuf.find('\n')) != std::string::npos) {
        std::string line = c.inbuf.substr(0, pos);
        c.inbuf.erase(0, pos + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 16384) { close_conn(c.fd); return; }
        if (!line.empty()) handle_line(c, line);
        if (conns_.find(c.fd) == conns_.end()) return; // closed in handler
    }
}

void StratumServer::handle_line(Conn& c, const std::string& line) {
    JValue msg;
    std::string err = jparse(line, msg);
    if (!err.empty()) {
        sink_.on_event("bad_json", err);
        return;
    }
    std::string method = msg.get("method") ? msg.get("method")->as_str() : "";
    const JValue* idp = msg.get("id");

    if (method == "mining.subscribe") {
        std::string ua = "unknown";
        if (const JValue* p = msg.get("params"); p && p->is_arr() && !p->arr.empty() &&
                                              p->arr[0].is_str())
            ua = p->arr[0].as_str();
        // extranonce1: 4 bytes hex, per connection
        std::random_device rd;
        uint32_t r = rd();
        char eb[16];
        std::snprintf(eb, sizeof(eb), "%08x", r);
        c.extranonce1_hex = eb;
        c.subscribed = true;
        // result = [ [[method, session_id], ...], extranonce1, extranonce2_size ]
        JValue subs = JValue::mk_arr();
        JValue pair = JValue::mk_arr();
        pair.push(JValue::mk_str("mining.set_difficulty"));
        pair.push(JValue::mk_str(c.session_id));
        subs.push(std::move(pair));
        JValue result = JValue::mk_arr();
        result.push(std::move(subs));
        result.push(JValue::mk_str(c.extranonce1_hex));
        result.push(JValue::mk_int(4)); // extranonce2 size hint
        send_response(c, idp ? *idp : JValue::mk_int(0), result, JValue::mk_null());
        sink_.on_event("subscribe", ua + " fd=" + std::to_string(c.fd));
        // Always send current difficulty and job to subscribed miners
        send_difficulty(c, current_diff_);
        if (has_job_) push_job(c, true);
        return;
    }

    if (method == "mining.authorize") {
        std::string worker = "worker";
        if (const JValue* p = msg.get("params"); p && p->is_arr() && !p->arr.empty() &&
                                              p->arr[0].is_str())
            worker = p->arr[0].as_str();
        c.worker = worker;
        c.authorized = true;
        send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_bool(true),
                      JValue::mk_null());
        sink_.on_event("authorize", worker + " fd=" + std::to_string(c.fd));
        // Always send current difficulty and job when authorized
        send_difficulty(c, current_diff_);
        if (has_job_) push_job(c, true);
        return;
    }

    if (method == "mining.configure") {
        // BIP310. Real ASIC firmware (e.g. ESP-Miner on Bitaxe/BM1370) sends
        // this right after subscribe. Confirmed directly against real
        // hardware: declining the extension here does NOT stop the firmware
        // from rolling nVersion bits anyway — it keeps sending a 6th
        // "version bits" parameter in mining.submit regardless. So rather
        // than fight that, grant the extension honestly (matching what the
        // hardware actually does) and read+apply that 6th parameter in
        // mining.submit/verify_submit — see FORENSIC_REPORT.md §9.
        static const char* kVersionMask = "1fffe000"; // conventional AsicBoost mask (== kVersionRollMask)
        JValue result = JValue::mk_obj();
        result.set("version-rolling", JValue::mk_bool(true));
        result.set("version-rolling.mask", JValue::mk_str(kVersionMask));
        send_response(c, idp ? *idp : JValue::mk_int(0), result, JValue::mk_null());
        sink_.on_event("configure", "version-rolling granted mask=" + std::string(kVersionMask) +
                                        " fd=" + std::to_string(c.fd));
        return;
    }

    if (method == "mining.suggest_difficulty") {
        send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_bool(true),
                      JValue::mk_null());
        return;
    }

    if (method == "mining.get_transactions") {
        send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_arr(), JValue::mk_null());
        return;
    }

    if (method == "mining.ping") {
        send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_str("pong"),
                      JValue::mk_null());
        return;
    }

    if (method == "mining.submit") {
        shares_received_++;
        // flood guard: > 200 submits in the last second -> throttle-drop
        int64_t now = now_ms();
        if (now - c.window_start_ms > 1000) {
            c.window_start_ms = now;
            c.submits_window = 0;
        }
        if (++c.submits_window > 200) {
            sink_.on_event("throttled", "fd=" + std::to_string(c.fd));
            close_conn(c.fd);
            return;
        }
        JValue errv = JValue::mk_arr();
        const JValue* params = msg.get("params");
        VerifiedShare vs;
        bool ok = false, dup = false;
        std::string reason;

        // mining.submit is ALWAYS verified as real hashcash — quantum jobs
        // (see make_quantum_job) are real, ASIC-minable jobs with a genuine
        // difficulty target, so there is no separate "trust the miner" path
        // here. This is what makes share_hash_hex an honest SHA256D(header80)
        // for every batch, quantum-tagged or not.
        if (params && params->is_arr() && params->arr.size() >= 5 &&
            params->arr[0].is_str() && params->arr[1].is_str() && params->arr[2].is_str() &&
            params->arr[3].is_str() && params->arr[4].is_str()) {
            std::string worker = params->arr[0].as_str();
            std::string job_id = params->arr[1].as_str();
            std::string ex2 = params->arr[2].as_str();
            std::string ntime = params->arr[3].as_str();
            std::string nonce = params->arr[4].as_str();
            // Optional 6th param: version-rolling bits (BIP310). Real
            // firmware sends this whether or not mining.configure granted
            // the extension — see the note above mining.configure.
            std::string version_bits;
            if (params->arr.size() >= 6 && params->arr[5].is_str())
                version_bits = params->arr[5].as_str();
            if (worker.empty()) worker = c.worker;
            vs.worker = worker;
            reason = jobs_.verify_submit(job_id, ex2, ntime, nonce, version_bits,
                                          c.extranonce1_hex, vs, dup);
            ok = reason.empty();
        } else {
            reason = "malformed submit params";
        }

        if (ok) {
            shares_verified_++;
            send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_bool(true),
                          JValue::mk_null());
            sink_.on_verified_share(vs);
        } else {
            shares_rejected_++;
            int code = 23;
            if (dup) code = 24;
            else if (reason.find("stale") != std::string::npos ||
                     reason.find("unknown") != std::string::npos) code = 21;
            errv.push(JValue::mk_int(code));
            errv.push(JValue::mk_str(reason));
            errv.push(JValue::mk_null());
            send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_bool(false), errv);
            sink_.on_event("share_rejected", reason);
        }
        return;
    }

    if (method == "mining.quantum_submit") {
        // Deprecated / removed: this method used to accept operator-supplied
        // "quantum results" as a substitute for real proof-of-work, with no
        // hashcash check at all (see FORENSIC_REPORT.md). Quantum jobs are
        // now real hashcash jobs (make_quantum_job sets a genuine difficulty
        // target) redeemed the normal way, via mining.submit. This method is
        // kept only so old clients get a clear, non-silent rejection instead
        // of a share credit for free.
        JValue errv = JValue::mk_arr();
        errv.push(JValue::mk_int(20));
        errv.push(JValue::mk_str(
            "mining.quantum_submit is deprecated: quantum jobs now require real "
            "hashcash proof-of-work via standard mining.submit"));
        errv.push(JValue::mk_null());
        send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_null(), errv);
        sink_.on_event("quantum_submit_rejected", "deprecated method");
        return;
    }

    // unknown method
    JValue errv = JValue::mk_arr();
    errv.push(JValue::mk_int(20));
    errv.push(JValue::mk_str("method not found: " + method));
    errv.push(JValue::mk_null());
    send_response(c, idp ? *idp : JValue::mk_int(0), JValue::mk_null(), errv);
}

void StratumServer::housekeeping(int64_t now) {
    // periodic job push + idle timeout (10 min)
    static int64_t last_job_push = 0;
    if (job_interval_s_ > 0 && now - last_job_push > job_interval_s_ * 1000) {
        last_job_push = now;
        // Push quantum jobs if we're in quantum mode, otherwise regular jobs
        if (has_job_ && last_job_.quantum_mode) {
            // Generate a FRESH quantum job so the new job_id invalidates all
            // prior nonce entries in the dedup map. Re-pushing the same job
            // with clean:true makes the miner reset its nonce to 0, causing
            // every subsequent submission to hit the seen_ map as a duplicate.
            last_job_ = jobs_.make_quantum_job(
                last_job_.circuit_type,
                last_job_.circuit_d,
                last_job_.circuit_rounds,
                last_job_.shots_per_job,
                current_diff_
            );
            has_job_ = true;
            for (auto& [fd, c] : conns_) {
                (void)fd;
                if (c.subscribed) push_quantum_job(c, last_job_);
            }
        } else {
            last_job_ = jobs_.make_job(current_diff_);
            has_job_ = true;
            for (auto& [fd, c] : conns_) {
                (void)fd;
                if (c.subscribed) push_job(c, false);
            }
        }
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
        if (now - it->second.last_rx_ms > 600000) {
            int fd = it->first;
            it = conns_.erase(it);
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
            sink_.on_event("miner_timeout", "fd=" + std::to_string(fd));
        } else {
            ++it;
        }
    }
}

void StratumServer::run_once(int timeout_ms) {
    if (!has_job_) {
        // Start with a dummy job, will be replaced by quantum jobs from daemon
        last_job_ = jobs_.make_job(current_diff_);
        has_job_ = true;
    }
    epoll_event events[64];
    int n = ::epoll_wait(epoll_fd_, events, 64, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) { housekeeping(now_ms()); return; }
        return;
    }
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == listen_fd_) {
            accept_new();
            continue;
        }
        auto it = conns_.find(fd);
        if (it == conns_.end()) continue;
        if (events[i].events & (EPOLLHUP | EPOLLERR)) { close_conn(fd); continue; }
        if (events[i].events & EPOLLOUT) on_writable(it->second);
        if (conns_.find(fd) == conns_.end()) continue;
        if (events[i].events & EPOLLIN) {
            on_readable(it->second);
            continue;
        }
    }
    housekeeping(now_ms());
}

} // namespace asqs
