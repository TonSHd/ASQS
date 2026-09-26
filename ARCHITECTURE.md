# ASQS Architecture

```
                       ┌────────────────────────────────────────────────┐
                       │                asqsd (C++20)                   │
 Bitaxe Gamma 601      │                                                │
 (ESP-Miner, stratum)  │  ┌──────────────┐   verified shares (queue)    │
 ───stratum v1 TCP───► │  │ StratumServer│ ─────────────► ┌─────────┐   │
                       │  │  epoll loop  │                │ shot    │   │
                       │  │ job factory  │                │ runner  │   │
                       │  │ share verify │ ◄─difficulty── │ thread  │   │
                       │  └──────┬───────┘   controller   └────┬────┘   │
                       │         │ events                      │ N      │
                       │         ▼                             │ shots  │
                       │      asqs.log                  ┌──────▼────┐   │
                       │                                │ pipeline  │   │
                       │  ┌──────────────┐              │ writer    │   │
                       │  │ status.json  │◄1s heartbeat │ notvalid. │   │
                       │  └──────────────┘              └──────┬────┘   │
                       │                                       │ file   │
                       │                                ┌──────▼────┐   │
                       │                                │ validator │   │
                       │                                │ thread    │   │
                       │                                │ FIFO,mtime│   │
                       │                                └───────────┘   │
                       └──────────────────────────────────────────┼─────┘
                          notvalidated/*.json ──(valid+useful)──► │outputs/
                                                                  ▼
                                                          ledger.jsonl
```

## Components

| Component | File(s) | Notes |
|---|---|---|
| Stratum v1 server | `cpp/stratum.*` | non-blocking epoll, per-connection state, flood guard (200 submits/s), 10-min idle timeout, job push every `job_interval_s` |
| Job factory / share verifier | `cpp/stratum.*` | synthetic headers (ASQS coinbase payload embeds the circuit tag), 256-bit target math, per-job difficulty snapshot, dedupe, 10-min job retention |
| Difficulty controller | `cpp/daemon.cpp` | adaptive: hashrate estimate from observed shares; retarget when |desired/current − 1| > 30% (≥30 s apart); emergency ×8 on backlog; clamp [min,max] |
| Shot runner | `cpp/shot.*` | seed chain, PRNG draw order per PROOF SPEC, noise, ShotRecord |
| Tableau engine | `cpp/tableau.*` | stabilizer-half-only, u64 bitmask rows, unique-subset deterministic-outcome solver (full (x,z) vectors) |
| Circuit library | `cpp/circuit.*` | surface code d=3/5 + random Clifford; GF(2) self-validation |
| Pipeline | `cpp/pipeline.*` | atomic writes (tmp+fsync+rename), FIFO validation, gate, promotion, proof |
| Ledger | `cpp/ledger.h` | hash-chained append-only JSONL |
| Daemon | `cpp/daemon.*` | threads, signals, status heartbeat, scan-once mode |
| Python auditor | `python/asqs/engine.py` | independent spec implementation (PRNG, tableau, circuits, shots, targets) |
| Python CLI | `python/asqs/cli.py` | host/status/validate/verify/analyze/crosscheck/circuits/ledger |
| Reference crosscheck | `python/asqs/reference.py` + stim | converts circuit+noise to stim, samples an independent reference, statistical validation (permutation TVD + Bonferroni chi-square battery), embeds `reference`/`validation` blocks |

## Threading

- **Network thread** (main): epoll loop (`run_once` 200 ms slices), controller
  tick 1 s, status write 1 s.
- **Shot runner thread**: pops verified shares from a bounded queue (4096),
  derives seeds, runs `shots_per_share` shots, writes pending batches.
- **Validator thread**: scans `notvalidated/` oldest-first (mtime), full
  replay + gate, promotes or deletes; sleeps 50–200 ms when idle.

Shutdown: SIGINT/SIGTERM → stop flag → network loop exits → threads joined →
final status flush. Crash safety: all files appear via atomic rename; stale
`.tmp.` files are cleaned at startup.

## Data integrity chain

```
share (hashcash proof)
  └─ seed = SHA256(share_hash ‖ header80)
       └─ shot k = PRNG(LE64(SHA256(seed‖LE32(k))))
            └─ record (outcomes, detectors, observables, errors)
                 └─ replay_sha256 (canonical)
                      └─ ledger entry_hash (hash chain)
```

Every layer is re-derivable by a third party from the layer above it.

## Config precedence

CLI flags > config file > built-in defaults (see `config/asqs.example.json`).
