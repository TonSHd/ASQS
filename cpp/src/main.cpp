// ASQS daemon entry point.
#include "config.h"
#include "daemon.h"
#include "circuit.h"

#include <cstdio>
#include <cstring>
#include <string>

static void usage() {
    std::printf(
        "asqsd - ASQS daemon (Application-Specific Quantum Simulation)\n"
        "\n"
        "Usage:\n"
        "  asqsd --ip IP --port PORT [options]\n"
        "  asqsd --scan-once [options]\n"
        "  asqsd --list-circuits\n"
        "  asqsd --version\n"
        "\n"
        "Options:\n"
        "  --config PATH              JSON config file (see config/asqs.example.json)\n"
        "  --project-root DIR         project root (default: cwd)\n"
        "  --ip IP                    bind address (default 0.0.0.0)\n"
        "  --port N                   stratum port (default 3333)\n"
        "  --circuit TYPE             surface_code_memory | random_clifford\n"
        "  --d N                      surface code distance (3|5)\n"
        "  --rounds N                 surface code rounds\n"
        "  --qubits N                 random_clifford qubits (2..64)\n"
        "  --gates N                  random_clifford gates\n"
        "  --noise P1,P2,PM,PR        noise ppb values, comma separated\n"
        "  --shots-per-share N        shots derived per verified share (default 16)\n"
        "  --validation-shots N        every N-th share (see --validation-every) derives a large\n"
        "                              statistical validation batch of N shots (default 0 = off)\n"
        "  --validation-every N        validation batch every N-th processed share (default 100)\n"
        "  --max-shares-per-sec F     adaptive controller target share rate (default 1.0)\n"
        "  --difficulty-mode M        adaptive | fixed\n"
        "  --init-difficulty D        initial difficulty (default 256; may be fractional)\n"
        "  --min-difficulty D         lower clamp (default 0.001)\n"
        "  --max-difficulty D         upper clamp (default 2^40)\n"
        "  --job-interval-s N         job push interval (default 30)\n"
        "  --local-time               file timestamps in local time (default UTC)\n");
}

int main(int argc, char** argv) {
    std::string config_path, ip, port_str, project_root, circuit_type;
    std::string d_str, rounds_str, qubits_str, gates_str, noise_str, shots_str, rate_str;
    std::string diff_mode, init_diff, min_diff, max_diff, job_interval, local_time;
    std::string validation_shots_str, validation_every_str;
    bool scan_once = false, list_circuits = false, show_version = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--version") show_version = true;
        else if (a == "--config") config_path = next();
        else if (a == "--ip") ip = next();
        else if (a == "--port") port_str = next();
        else if (a == "--project-root") project_root = next();
        else if (a == "--circuit") circuit_type = next();
        else if (a == "--d") d_str = next();
        else if (a == "--rounds") rounds_str = next();
        else if (a == "--qubits") qubits_str = next();
        else if (a == "--gates") gates_str = next();
        else if (a == "--noise") noise_str = next();
        else if (a == "--shots-per-share") shots_str = next();
        else if (a == "--validation-shots") validation_shots_str = next();
        else if (a == "--validation-every") validation_every_str = next();
        else if (a == "--max-shares-per-sec") rate_str = next();
        else if (a == "--difficulty-mode") diff_mode = next();
        else if (a == "--init-difficulty") init_diff = next();
        else if (a == "--min-difficulty") min_diff = next();
        else if (a == "--max-difficulty") max_diff = next();
        else if (a == "--job-interval-s") job_interval = next();
        else if (a == "--local-time") local_time = "1";
        else if (a == "--scan-once") scan_once = true;
        else if (a == "--list-circuits") list_circuits = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (show_version) {
        std::printf("asqsd 0.1.0\n");
        return 0;
    }

    if (list_circuits) {
        const char* types[] = {"surface_code_memory", "random_clifford"};
        int ds[] = {3, 5};
        for (const char* t : types) {
            asqs::CircuitConfig c;
            c.type = t;
            if (c.type == "surface_code_memory") {
                for (int d : ds) {
                    c.d = d;
                    c.rounds = d;
                    try {
                        asqs::Circuit circ = asqs::build_circuit(c);
                        std::printf("%s d=%d rounds=%d qubits=%u meas=%u detectors=%zu obs=%zu %s\n",
                                    t, d, c.rounds, circ.num_qubits, circ.num_measurements,
                                    circ.detectors.size(), circ.observables.size(),
                                    c.sha256_hex().c_str());
                    } catch (std::exception& e) {
                        std::printf("%s d=%d FAILED: %s\n", t, d, e.what());
                        return 1;
                    }
                }
            } else {
                asqs::CircuitConfig rc;
                rc.type = "random_clifford";
                rc.qubits = 24;
                rc.gates = 120;
                asqs::Circuit circ = asqs::build_circuit(rc);
                std::printf("%s qubits=%d gates=%d meas=%u %s\n", t, rc.qubits, rc.gates,
                            circ.num_measurements, rc.sha256_hex().c_str());
            }
        }
        return 0;
    }

    // Load config file, then apply CLI overrides.
    asqs::Config cfg;
    std::string err;
    
    if (!config_path.empty()) {
        cfg = asqs::Config::from_json_file(config_path, err);
        if (!err.empty()) {
            std::fprintf(stderr, "config error: %s\n", err.c_str());
            return 2;
        }

    }
    if (!ip.empty()) cfg.ip = ip;
    if (!port_str.empty()) cfg.port = uint16_t(std::stoi(port_str));
    if (!project_root.empty()) cfg.project_root = project_root;
    if (!circuit_type.empty()) cfg.circuit.type = circuit_type;
    if (!d_str.empty()) cfg.circuit.d = std::stoi(d_str);
    if (!rounds_str.empty()) cfg.circuit.rounds = std::stoi(rounds_str);
    if (!qubits_str.empty()) cfg.circuit.qubits = std::stoi(qubits_str);
    if (!gates_str.empty()) cfg.circuit.gates = std::stoi(gates_str);
    if (!noise_str.empty()) {
        auto parts = asqs::split(noise_str, ',');
        if (parts.size() == 4) {
            cfg.circuit.noise.p1_ppb = std::stoll(parts[0]);
            cfg.circuit.noise.p2_ppb = std::stoll(parts[1]);
            cfg.circuit.noise.pm_ppb = std::stoll(parts[2]);
            cfg.circuit.noise.pr_ppb = std::stoll(parts[3]);
        } else {
            std::fprintf(stderr, "--noise expects P1,P2,PM,PR (ppb)\n");
            return 2;
        }
    }
    if (!shots_str.empty()) cfg.shots_per_share = std::stoi(shots_str);
    if (!validation_shots_str.empty()) cfg.validation_shots = std::stoi(validation_shots_str);
    if (!validation_every_str.empty()) cfg.validation_every_n_shares = std::stoi(validation_every_str);
    if (!rate_str.empty()) cfg.max_shares_per_sec = std::stod(rate_str);
    if (!diff_mode.empty()) cfg.difficulty_mode = diff_mode;
    if (!init_diff.empty()) cfg.init_difficulty = init_diff;
    if (!min_diff.empty()) cfg.min_difficulty = min_diff;
    if (!max_diff.empty()) cfg.max_difficulty = max_diff;
    if (!job_interval.empty()) cfg.job_interval_s = std::stoi(job_interval);
    if (local_time == "1") cfg.local_time = true;

    // Re-validate the merged circuit config.
    {
        asqs::JValue cj = cfg.circuit.to_json();
        asqs::JValue full = asqs::JValue::mk_obj();
        full.set("type", asqs::JValue::mk_str(cfg.circuit.type));
        if (cfg.circuit.type == "surface_code_memory") {
            full.set("d", asqs::JValue::mk_int(cfg.circuit.d));
            full.set("rounds", asqs::JValue::mk_int(cfg.circuit.rounds));
        } else {
            full.set("qubits", asqs::JValue::mk_int(cfg.circuit.qubits));
            full.set("gates", asqs::JValue::mk_int(cfg.circuit.gates));
        }
        std::string cerr2;
        asqs::CircuitConfig chk = asqs::CircuitConfig::from_json(full, cerr2);
        if (!cerr2.empty()) {
            std::fprintf(stderr, "circuit config error: %s\n", cerr2.c_str());
            return 2;
        }
        (void)chk;
        (void)cj;
    }
    asqs::Config::validate(cfg, err);
    if (!err.empty()) {
        std::fprintf(stderr, "config error: %s\n", err.c_str());
        return 2;
    }

    asqs::Daemon daemon(cfg);
    if (scan_once) return daemon.scan_once();
    return daemon.run();
}
