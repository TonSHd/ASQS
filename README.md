# ASQS

**Application-Specific Quantum Simulation using a SHA-256 ASIC as a verifiable mining work source for reproducible randomness.**

ASQS uses a SHA-256 mining ASIC such as a Bitaxe Gamma 601 as a verifiable randomness source for stabilizer-circuit simulation.

The ASIC does not simulate the circuit. The host CPU does that. The ASIC produces mining work and the resulting share is used to derive the seed for each batch.

This makes each batch reproducible and lets another person check where its randomness came from.

## How it works

```text
Bitaxe
  │
  │ Stratum v1 share
  ▼
ASQS daemon
  │
  ├─ verify SHA-256D proof
  ├─ derive seed from share
  ├─ simulate stabilizer circuit
  ├─ replay and verify results
  └─ write ledger entry
  │
  ▼
outputs/
```

For each valid share:

1. ASQS verifies the SHA-256D hash against the current target.
2. A seed is derived from the share hash and block header.
3. The configured circuit is sampled using that seed.
4. The entire batch is replayed deterministically.
5. If the batch passes the usefulness checks. It is moved to `outputs/`.
6. The share and batch are recorded in the ledger.

Everything needed for replay is stored in the output file.

## Quickstart

### Requirements

* Linux
* C++20 compiler
* Python 3.10+
* A Stratum v1 SHA-256 miner such as a Bitaxe running ESP-Miner

The C++ daemon has no external dependencies.

### Build

```bash
make
make tests
```

This builds:

```text
build/asqsd
```

The Python CLI can optionally be installed with:

```bash
pip install -e python/
```

### Start the daemon

```bash
./build/asqsd \
  --ip 0.0.0.0 \
  --port 3333 \
  --project-root ./asqs-data
```

Or through the Python CLI:

```bash
python3 python/asqs/cli.py host \
  --ip 0.0.0.0 \
  --port 3333 \
  --project-root ./asqs-data
```

### Configure the Bitaxe

In the Bitaxe web interface set the mining pool to:

```text
URL:  stratum+tcp://<your-pc-ip>:3333
User: <worker-name>
Pass: x
```

!!!ASQS speaks Stratum v1!!! which is what the standard ESP-Miner firmware uses.

### Check the daemon

```bash
python3 -m asqs.cli status --project-root ./asqs-data
```

Run the verifier:

```bash
python3 -m asqs.cli verify --all --project-root ./asqs-data
```

View aggregate results:

```bash
python3 -m asqs.cli analyze --project-root ./asqs-data
```

## Data layout

Each share starts in `notvalidated/`. After validation. it is moved to `outputs/`.

```text
asqs-data/
├── notvalidated/
│   └── asqs_notvalidated_{timestamp}_{batch_id}.json
├── outputs/
│   └── asqs_out_{timestamp}_{batch_id}.json
├── ledger.jsonl
├── status.json
└── asqs.log
```

### `notvalidated/`

Batches waiting for validation.

The validator processes them oldest-first using file modification time. A newer batch cannot skip an older one.

### `outputs/`

Validated batches. The proof information is written into the record before the file is moved here.

### `ledger.jsonl`

A hash-chained record of valid useful batches.

### `status.json`

The daemon heartbeat and current status.

### `asqs.log`

Daemon log.

Timestamps are UTC by default and use:

```text
ss_mm_HH_dd_MM_yyyy
```

Use `--local-time` for local timestamps.

## What is checked?

A batch must pass all of these checks before it is promoted.

### 1. Hashcash verification

The daemon re-computes:

```text
SHA256D(header80)
```

and checks that the resulting little-endian value is below the share target.

### 2. Seed derivation

The simulation seed is derived from:

```text
SHA256(share_hash || header80)
```

The exact derivation is defined in `PROOF_SPEC.md`.

### 3. Deterministic replay

Every shot is simulated again from the stored seed.

The replay must match the recorded output bit-for-bit.

### 4. Statistical sanity checks

The batch must contain detector activity without exceeding the configured sanity limits.

The checks include detector flip activity and syndrome diversity.

Batches that fail validation are deleted and logged. They do not receive ledger credit.

## Batch format

Current records use the `asqs.batch/2` schema.

A batch contains the information needed to reproduce and audit it:

```text
┌────────────────────────────────────────────┐
│ Circuit                                    │
│  • instruction list                        │
│  • canonical program hash                  │
├────────────────────────────────────────────┤
│ Simulation                                 │
│  • circuit semantics                       │
│  • noise model                             │
├────────────────────────────────────────────┤
│ Mining work                                │
│  • header                                  │
│  • nonce                                   │
│  • share hash                              │
│  • derived seed                            │
├────────────────────────────────────────────┤
│ Samples                                    │
│  • simulated shots                         │
├────────────────────────────────────────────┤
│ Optional reference                         │
│  • independent Stim samples                │
│  • statistical comparison                  │
├────────────────────────────────────────────┤
│ Verification                               │
│  • replay proof                            │
├────────────────────────────────────────────┤
│ Provenance                                 │
│  • ledger sequence                         │
│  • ledger entry hash                       │
└────────────────────────────────────────────┘
```

The `circuit_program` field contains the actual instruction list. For example:

```json
{"gate":"H","qubit":3}
```

and:

```json
{"gate":"CNOT","control":3,"target":7}
```

It also contains detector and observable definitions.

For generated circuits such as `random_clifford` the generation seed is included too.

An auditor can therefore reconstruct the circuit without having to rely on the original circuit builder.

## Validation batches

Normal shares produce a small number of shots. The default is 16.

That is enough to test the pipeline. but not enough for strong statistical comparisons.

For larger validation batches:

```bash
./build/asqsd \
  ... \
  --shots-per-share 16 \
  --validation-shots 1024 \
  --validation-every 100
```

With this configuration
every 100th processed share produces a 1024-shot validation batch.

Validation batches are marked with:

```json
"validation_batch": true
```

Supported validation batch sizes are 16 through 65536 shots.

## Independent cross checking

ASQS can compare its results with [Stim](https://github.com/quantumlib/Stim), an independent stabilizer-circuit simulator!

Install Stim:

```bash
pip install stim
```

Then run:

```bash
python3 -m asqs.cli crosscheck \
  --validation-only \
  --project-root ./asqs-data
```

Or for a specific output:

```bash
asqs crosscheck \
  --file outputs/asqs_out_....json \
  --reference-shots 8192
```

The cross-check:

* reconstructs the circuit from the embedded program
* converts the noise model to Stim semantics
* performs noiseless structural checks
* generates an independent reference distribution
* compares the ASQS and reference samples
* stores the reference samples and statistical results in the batch

The statistical checks include:

* permutation testing of total variation distance
* per-bit two-proportion chi-square tests
* Bonferroni correction
* detector flip-rate comparison
* syndrome-weight chi-square testing for surface-code circuits

The reference and validation blocks are stored in the output file and can be checked again by `asqs verify`.

Stim is only being used as an independent cross-check. The Bitaxe is not doing the quantum simulation.

## Adaptive difficulty

The daemon can adjust mining difficulty based on the observed share rate.

The controller estimates hashrate from accepted shares:

```text
hashrate ≈ shares × difficulty × 2³² / elapsed_time
```

It then adjusts the target so the share rate stays near:

```text
--max-shares-per-sec
```

The default is 1 share per second.

If the simulation queue starts growing, ASQS can temporarily increase the difficulty by up to 8×.

This keeps the host workload bounded instead of letting the miner submit shares faster than the simulator can process them.

## Circuits

ASQS currently supports two circuit types.

| Circuit               | Configuration                 | Description                            |
| --------------------- | ----------------------------- | -------------------------------------- |
| `surface_code_memory` | d=3 or d=5, 1–16 rounds       | Rotated surface code memory experiment |
| `random_clifford`     | 2–64 qubits, up to 100k gates | Seeded random Clifford circuits        |

### Surface code

The current implementation supports:

* distance 3: 17 qubits
* distance 5: 49 qubits
* 1–16 rounds
* detector construction
* logical Z observable

The circuit builder performs consistency checks at daemon startup including commutation and GF(2) independence checks.

The noiseless surface-code circuit is also checked by the C++ test. Its detectors should be deterministically zero in the noiseless case.

### Random Clifford

Random Clifford circuits are generated deterministically from their configuration and seed.

The generated program is stored in the batch, so the circuit can be reconstructed later without the original generator state.

## Noise model

Noise probabilities are represented in parts per billion (ppb) using exact integer/rational semantics.

The current model contains independent Pauli channels for:

* `p1`: errors after single-qubit gates
* `p2`: errors associated with CNOT targets
* `pm`: measurement flips
* `pr`: reset errors

Noise realizations are stored per shot.

This allows the error pattern to be used later for things such as decoder experiments.

## CLI

```text
asqs host
    --ip 0.0.0.0
    --port 3333
    [--project-root DIR]
    [--circuit TYPE]
    [--d 3|5]
    [--rounds N]
    [--qubits N]
    [--gates N]
    [--noise P1,P2,PM,PR_ppb]
    [--shots-per-share N]
    [--validation-shots N]
    [--validation-every N]
    [--max-shares-per-sec F]
    [--difficulty-mode adaptive|fixed]
    [--init-difficulty D]
    [--config FILE]
    [--local-time]

asqs status
    [--project-root DIR]

asqs validate
    [--project-root DIR]

asqs verify
    [--file F]
    [--all]
    [--ledger-only]

asqs analyze
    [--out summary.json]

asqs crosscheck
    [--file F | --all | --validation-only]
    [--reference-shots N]
    [--permutations N]
    [--alpha A]
    [--dry-run]
    [--out REPORT]

asqs circuits

asqs ledger
    [--dump]
```

## Verification architecture

ASQS has three main verification components.

### C++ daemon

The daemon:

* serves Stratum v1
* verifies shares
* generates simulation batches
* performs deterministic replay
* validates circuit and noise-model data
* maintains the ledger

### Python auditor

`python/asqs/engine.py` contains a separate implementation of the proof rules.

`asqs verify` checks:

* share proof
* seed derivation
* circuit program
* noise model
* every recorded shot
* deterministic replay
* embedded reference validation
* ledger hashes

The Python implementation is kept separate from the C++ implementation so that the verifier does not simply trust the daemon's results.

### Stim reference

`python/asqs/reference.py` converts the stored circuit and noise semantics to Stim and produces an independent reference sample.

The integration test runs the pieces together:

```text
CPU/ASIC mining > Stratum daemon > batch validation > Python verification > Stim cross-check
```

## Performance

The ASIC performs the SHA-256 search.

The host performs the stabilizer simulation because the mining ASIC is designed for double SHA-256 and cannot execute the tableau operations used by ASQS.

For small circuits, the host side cost is small. A d=3 surface-code batch with 16 shots is intended to take tens of microseconds on the host.

Actual performance depends on the circuit, shot count, and hardware.

## Limitations

* Only stabilizer/Clifford circuits are supported.
* T gates and general state-vector simulation are not supported.
* The ASIC does not perform the quantum simulation.
* Simulation runs on the host CPU.
* The current tableau implementation supports up to 64 qubits.
* Larger surface-code distances require extending the tableau implementation.
* Stratum v1 is currently supported. Stratum v2 is not.
* Small batches are useful for provenance and pipeline testing, but are not enough for strong statistical conclusions.

## Testing

Run the test with:

```bash
./tests/run_tests.sh
```

The test currently contains more than 1,200 C++ checks plus the end-to-end integration test.

The integration test:

1. starts a local ASQS daemon
2. CPU-mines test shares
3. submits them through Stratum v1
4. validates the resulting batches
5. verifies the outputs with the Python auditor
6. checks the ledger chain
7. runs the independent reference cross-check

## Proof specification

The detailed protoocol is written in `PROOF_SPEC.md`.

It defines the parts of the format that need to match between implementations including:

* canonical JSON encoding
* PRNG constants
* tableau operations
* circuit construction
* seed derivation
* RNG draw order
* share target calculation
* circuit program encoding
* noise model semantics
* independent reference conversion

The README explains how the system works. `PROOF_SPEC.md` defines the exact rules.

## Project Clarification

ASQS is focused on reproducible stabilizer circuit experiments and testing the provenance pipeline with real SHA-256 mining hardware.

The ASIC provides the mining work and the entropy source. The host performs the simulation.

It is not a quantum computer and ASQS does not claim that a Bitcoin ASIC performs quantum simulation.

## AI contribution

Generative AI tools were used substantially in writing the C++ daemon, the Python auditor, and the website code in this repository. ChatGPT (GPT-5.6 Luna), Claude (Sonnet 5), and Z.ai (GLM-5.3) were used under the author's direction and review.
The author designed the protocol and ran the physical Bitaxe hardware and is responsible for the correctness of the released code and data.
Independent verification of the data, including SHA256D proof rederivation, full shot-by-shot replay, ledger chain integrity, and checking the embedded Stim statistics against the raw shot records, was performed before release with two AI assistants: Claude (Sonnet 5) and ChatGPT (GPT-5.6 Luna).
AI tools were used as tools, not as the source of the research. The author reviewed the output and takes responsibility for it.

## REMINDER

**PLEASE TEST THIS CAREFULLY!**
You don't want thousands of outputs being pushed to your CPU within one second, right?
Please read "Adaptive difficulty" Section before doing anything else!

## License

MIT. See `LICENSE`.
