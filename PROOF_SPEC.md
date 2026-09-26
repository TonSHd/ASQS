# ASQS Proof Specification (v1, normative)

This document specifies everything an independent auditor needs to re-derive
and verify any ASQS output. The C++ daemon (`cpp/`) and the Python auditor
(`python/asqs/engine.py`) are both implementations of THIS spec; the
integration test enforces their bit-exact agreement.

## 1. Canonical JSON

All embedded hashes are computed over canonical JSON:

- keys sorted (bytewise, ascending) at every nesting level
- no whitespace; separators `,` and `:`
- strings: UTF-8, escaped exactly as: `"` → `\"`, `\` → `\\`, control
  characters `\n \r \t \b \f`, others < 0x20 → `\u00xx` (lowercase hex)
- integers: decimal, no leading zeros, no `+`
- booleans `true`/`false`, `null`
- **doubles are FORBIDDEN** in canonical form (probabilities are integers in
  ppb; difficulty is a decimal string)

Python equivalent: `json.dumps(obj, sort_keys=True, separators=(',',':'), ensure_ascii=False)`

## 2. Hash functions

SHA-256 (FIPS 180-4). `SHA256D(x) = SHA256(SHA256(x))`. Hex is lowercase.

## 3. PRNG (normative constants and draw order)

Seeding: SplitMix64 sequence from the 64-bit seed, two outputs fill the state.

```
SplitMix64:  state += 0x9E3779B97F4A7C15
             z = state
             z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9
             z = (z ^ (z>>27)) * 0x94D049BB133111EB
             return z ^ (z>>31)          (all arithmetic mod 2^64)
```

Stream: xoroshiro128**:

```
result = rotl(s0 * 5, 7) * 9
s1 ^= s0; s0 = rotl(s0,24) ^ s1 ^ (s1<<16); s1 = rotl(s1,37)
```

Draws (each consumes exactly one `next_u64`):
- `bit()`     → `next_u64() & 1`
- `rand_below(k)` → `(next_u64() >> 32) % k`   (k ≥ 1; k ≤ 1 → 0)
- `rand_double()` → `(next_u64() >> 11) * 2^-53`
- `bernoulli(p)` → `rand_double() < p`

## 4. Difficulty / target math

Difficulty is an exact rational `num/den` parsed from a decimal string
(integer part + ≤ 9 fraction digits; e.g. "0.001" → 1/1000).

```
MAX_TARGET = 0x00000000FFFF0000...0000  (= 0xFFFF * 2^208, Bitcoin bdiff-1)
target     = floor(MAX_TARGET * den / num)      (num=0 → 2^256-1)
```

Share validity: `LE_value(SHA256D(header80)) <= target`, i.e. compare
`reverse(SHA256D(header80))` against `target` big-endian.

## 5. Block header (80 bytes) and share

```
coinbase   = coinbase1 || extranonce1 || extranonce2 || coinbase2
header80   = LE32(version) || prevhash_field(32) || SHA256D(coinbase)
             || LE32(ntime) || LE32(nbits) || LE32(nonce)
share_hash = SHA256D(header80)                       (32 raw bytes)
```

Stratum notify carries `hex(bswap32-per-word(prevhash_field))` the
real-world wire convention (pools send word_reverse(display prevhash);
firmware inserts the per-word-swapped wire bytes into the header, verified
byte-exact against ESP-Miner v2.15.3 / Bitaxe BM1370) and BE hex of the
u32 fields; submits carry BE hex of ntime/nonce. Any even-length
extranonce2 (≤ 64 bytes) is accepted.

## 6. Seed chain

```
share_hash = SHA256D(header80)
seed       = SHA256(share_hash || header80)          (32 bytes)
shot_k     = SHA256(seed || LE32(k))                  (32 bytes)
prng_seed_k = LE64(shot_k[0..8])
```

Shot k of a batch is simulated with `Prng(prng_seed_k)` and NOTHING else.

## 7. Stabilizer tableau engine (n ≤ 64 qubits)

State = n stabilizer rows; row = (x-mask u64, z-mask u64, sign bit r);
bit i = qubit i. Initial state |0...0⟩: row_i = (0, 1<<i, 0).

Gate rules (applied to every row; xa/za are the row's bits on qubit a):

```
H(a):    r ^= xa & za;            swap xa, za
S(a):    r ^= xa & za;            za ^= xa          (X->Y, Y->-X)
Sdg(a):  r ^= xa & (za ^ 1);      za ^= xa          (X->-Y, Y->X)
X(a):    r ^= za                                        (Z components flip)
Z(a):    r ^= xa                                        (X components flip)
CNOT(c,t): x_t ^= x_c;  z_c ^= z_t
```

Measurement MZ(a):
- If some row has bit a of x set → **random branch**: outcome = one PRNG
  bit; row_p (first such row) is XORed into every other row with x bit a
  set; then row_p := (0, 1<<a, outcome).
- Else → **deterministic branch** (no PRNG consumption): outcome = sign of
  the unique subset of rows whose full (x,z) vectors XOR to (0, e_a).
  Find it by Gaussian elimination over the 2n columns (phase 1: RREF, one
  pivot per row; phase 2: eliminate the target (0, e_a) against pivot rows
  in ascending column order, XORing sign bits). The subset is unique, so
  any pivot order yields the same outcome.

MR(a) = MZ(a) then, if outcome = 1, X(a). RESET(a) = MR(a) with the outcome
discarded (still consumes the PRNG bit on the random branch).

## 8. Circuit library

### 8.1 Rotated surface code memory (d ∈ {3,5})

Data qubits (r,c), 0 ≤ r,c < d, index r*d+c. Checks:

- Inner face F(a,b), 0 ≤ a,b ≤ d-2, corners (a,b),(a+1,b),(a,b+1),(a+1,b+1):
  X-check if a+b even, else Z-check. Enumerated row-major.
- Boundary stubs (weight 2), appended in this order after the faces:
  - for c = 0..d-2: c odd → top X-stub {(0,c),(0,c+1)}; c even → bottom
    X-stub {(d-1,c),(d-1,c+1)}
  - for r = 0..d-2: r even → left Z-stub {(r,0),(r+1,0)}; r odd → right
    Z-stub {(r,d-1),(r+1,d-1)}
- Qubit ids: data 0..d²-1; X-ancillas next (check enumeration order);
  Z-ancillas after.
- Each round: for each X-ancilla a (in order): H(a); CNOT(a→t) for t in
  support sorted by qubit id; H(a); MR(a). Then for each Z-ancilla a:
  CNOT(t→a) for t in support sorted; MR(a).
- After R rounds: MZ on every data qubit in index order.
- Detectors: X-ancilla round-overlaps (r ≥ 2); Z-ancilla first-round value;
  Z-ancilla round-overlaps; Z-ancilla last round XOR final data parity over
  its support (support order, not sorted).
- Observable: final parity of row-0 data qubits (logical Z).
- The builder self-validates: X/Z commutation, generator independence,
  logical independence (GF(2)).

### 8.2 Random Clifford

`circuit_seed = BE64(SHA256("asqs-circuit|" + canonical(identity_json))[0..8])`
where identity_json = {"type":"random_clifford","qubits":Q,"gates":G}.
(`BE64` = first 8 bytes big-endian; both implementations agree bit-exactly
this corrects an earlier LE64 typo in this document; the code was always
big-endian and the cross-language tests pin it.)
`Prng(circuit_seed)` then draws, per gate: `kind = rand_below(100)`:

```
[0,30): H(target)        [30,45): S(target)      [45,60): Sdg(target)
[60,65): X(target)       [65,70): Z(target)      [70,100): CNOT(a,b)
target = rand_below(Q);  CNOT: a = rand_below(Q); t = rand_below(Q-1);
                          if t >= a: t += 1
```

Finally MZ on all qubits 0..Q-1. One observable: parity of all outcomes.

## 9. Shot execution (normative RNG draw order)

Per instruction i, in circuit order, with p = ppb × 1e-9:

- H/S/Sdg/X/Z: apply gate; `bernoulli(p1)` → if hit, pauli =
  "XYZ"[rand_below(3)] applied after the gate (Y = X then Z), recorded
  (i, qubit, 0|1|2).
- CNOT: apply gate; for q in (control, target) order: `bernoulli(p2)` →
  pauli = "XYZ"[rand_below(3)], applied and recorded.
- MR: measure (one PRNG bit iff random branch); then `bernoulli(pm)` →
  flip the recorded outcome; recorded as pauli code 3.
- MZ: same as MR without the reset.
- RESET: reset (one PRNG bit iff random branch); then `bernoulli(pr)` →
  apply X, recorded as pauli code 0.

Detectors/observables are parities of the recorded outcomes.

## 10. Batch file (`asqs.batch/2`; v1 = same minus the v2-only fields)

```
schema ("asqs.batch/2"), batch_id, created_ms, created_utc,
circuit (identity json), circuit_sha256, noise {p1_ppb,p2_ppb,pm_ppb,pr_ppb},
noise_model (v2, §14),
shots_per_share (ACTUAL embedded shot count),
validation_batch (bool, v2 set on large statistical validation batches),
circuit_program (v2, §13), circuit_program_sha256 (v2),
share {worker, job_id, extranonce1, extranonce2, ntime, nonce, difficulty,
       header_hex, share_hash_hex, received_ms},
seed {derivation, shot_seed, prng_seed, seed_hex},
shots [ {index, outcomes[], detectors[], observables[], errors[{i,q,p}]} ],
proof (outputs only) {validated_ms, validated_utc, replay:"match",
       replay_sha256, verdict:"useful", reason,
       gate {detector_flips, detector_total, flip_rate_ppm,
             distinct_syndromes, distinct_outcomes},
       ledger_seq, ledger_entry_hash},
reference (v2, after crosscheck, §15), validation (v2, after crosscheck, §15)
```

`replay_sha256 = SHA256(canonical([shot_record_k] for all k))` where
shot_record = shot JSON without the `index` key.
`circuit_program_sha256 = SHA256(canonical(circuit_program))`.

## 11. Validation (daemon, per batch)

1. hashcash re-verify from `header_hex` + `difficulty`
2. `share_hash_hex` must equal the recomputed hash
3. seed re-derivation must equal `seed_hex`
4. rebuild circuit from embedded identity; hashes must match
4b. (v2) rebuild the circuit program from the embedded identity; the
    embedded program must hash to `circuit_program_sha256` AND equal the
    rebuilt program; the `noise_model` block must equal the normative
    semantics (§14) for the embedded ppb values
5. replay every shot from the seed; bit-exact match required
6. statistical gate (see README); useless → delete; invalid → delete + CRITICAL
7. useful → append ledger entry, write proof, atomically move to outputs/
8. (after promotion, optional) `asqs crosscheck`: independent-reference
   statistical validation (§15)

## 12. Ledger

`ledger.jsonl`, one canonical-JSON object per line:

```
{seq, type:"share_credit", ts_ms, batch, worker, share_hash, difficulty,
 circuit, circuit_sha256, shots, detector_flips, detector_total,
 flip_rate_ppm, proof_sha256, prev_hash, entry_hash}
```

`entry_hash = SHA256(canonical(entry without entry_hash))`;
`prev_hash` = previous entry's `entry_hash` (genesis: 64 × '0').
Verification replays the whole chain.

## 13. Circuit program (`asqs.program/1`)

The full instruction-level circuit, embedded in every v2 batch so an
independent verifier can reconstruct the circuit WITHOUT re-implementing
the builder (§8). Canonical encoding:

```
circuit_program = {
  "encoding": "asqs.program/1",
  "num_qubits": n,
  "num_measurements": m,
  "operations": [
    {"gate":"H","qubit":3},
    {"gate":"S","qubit":7}, {"gate":"SDG",...}, {"gate":"X",...},
    {"gate":"Z",...},
    {"gate":"CNOT","control":3,"target":7},
    {"gate":"MR","qubit":9,"meas_index":0},
    {"gate":"MZ","qubit":0,"meas_index":24},
    {"gate":"RESET","qubit":5}
  ],
  "detectors":   [{"name":"X0r2","meas":[0,8]}, ...],
  "observables": [{"name":"ZL_row0","meas":[24,25,26]}, ...],
  "provenance": {
    "generator": "asqs-circuit/1",
    "circuit_sha256": <identity hash>,
    "construction": <how the circuit was built>,
    "seed_derivation": <exact seed rule>,
    "seed_hex": <64-bit generation seed, random_clifford only>,
    "prng": <PRNG spec, random_clifford only>,
    "domain": <domain-separation string, random_clifford only>,
    "gate_distribution": <per-gate draw rule, random_clifford only>
  }
}
```

Rules: `meas_index` is the record position (0..m-1) of the MR/MZ outcome,
assigned in program order; detectors/observables are parities over those
indices; the provenance block fully answers "how was this circuit
generated" for random_clifford it includes the actual 64-bit seed and the
domain string, for surface codes it names the normative construction.

`circuit_program_sha256 = SHA256(canonical(circuit_program))` pins the
program; the daemon's validator and the Python auditor both re-derive it
from the identity and reject any mismatch. Golden cross-language pins
(unit-tested):

```
random_clifford q=24 g=120: 44727d248640f1d34c0caeae4e0181ab0ac5174eb4f2d94bb2763c9c484de638
surface_code_memory d=3 r=3: 8269390faaca41fe218f34c9bbfad5c1b79d2484dd13ec827bb3bfcd6e612b82
surface_code_memory d=5 r=5: 8b680df7d2f0abffe383f14fe5ffd784d427a79263cb531c8187563ef80a0256
```

## 14. Noise model semantics block

Every v2 batch embeds `noise_model`: the four ppb values PLUS the exact,
machine-readable rules for how they are applied. This makes the record
self-describing so two simulators cannot silently interpret the same
numbers differently. The block contains:

- `probability_semantics`: `p = ppb * 1e-9` (exact rational); one u64 per
  bernoulli: `(next_u64 >> 11) * 2^-53 < p`
- `single_qubit_gates`: applies_to [H,S,SDG,X,Z], after the gate, uniform
  Pauli X/Y/Z each with p1/3
- `two_qubit_gates`: CNOT, after the gate, control then target, each
  independently uniform Pauli X/Y/Z with p2/3
- `measurement`: MR/MZ, after the measurement, the RECORDED bit is flipped
  (classical; post-measurement state unchanged)
- `reset`: after R to |0>, X with probability pr
- `y_convention`: Y = X then Z
- `measurement_rng`: a bit is consumed iff the tableau branch is random
- `application_order`: program order; gate first, then noise

The daemon and the Python auditor both recompute the normative block for
the embedded ppb values and reject any divergence.

## 15. Independent reference & statistical validation (crosscheck)

The strongest form of evidence: the SAME circuit run through the ASQS
pipeline and through an INDEPENDENT trusted simulator, then compared.

```
                    ┌── ASQS pipeline (hashcash-seeded tableau engine)
same circuit ───────┤
                    └── trusted reference simulator (stim)
```

Tool: `asqs crosscheck [--file F | --all | --validation-only]
                      [--reference-shots N] [--permutations N] [--alpha A]`

Conversion (ASQS semantics → stim), normative:

| ASQS | stim |
|---|---|
| H/S/SDG/X/Z then p1 channel | gate, then `DEPOLARIZE1(p1)` on the qubit |
| CNOT then p2 channel on (control, target) | `CX`, then `DEPOLARIZE1(p2)` on control and on target |
| MR then pm outcome flip | `X_ERROR(pm)` then `MR` (exactly equivalent) |
| MZ (terminal) then pm outcome flip | `X_ERROR(pm)` then `M` (equivalent for terminal MZ; the converter asserts terminality) |
| RESET then pr X | `R` then `X_ERROR(pr)` |

Before sampling, the converter runs noiseless structural cross-checks: both
engines must independently agree that every detector is deterministically 0
and that every measurement bit is deterministic (same fixed value) or
uniform a cross-language physics referee.

Statistics (all pure stdlib; no scipy):

1. **Primary**: two-sample permutation test on total-variation distance of
   the JOINT outcome distribution (assumption-free, handles correlations
   and sparse support). p = (1 + #{null ≥ observed}) / (permutations + 1).
2. Per-bit two-proportion chi-square (Yates), Bonferroni-corrected over
   all measured bits high power against marginal drift.
3. Surface code: per-detector flip-rate comparison (Bonferroni),
   syndrome-weight chi-square, observable (logical) flip rates.

Verdict: `passed = permutation_p > alpha AND per-bit passes AND
per-detector passes`, default alpha = 0.01 (a FAIL is an “investigate”
signal re-run with more reference shots/permutations not a fraud
verdict; at alpha the null-true false-alarm rate is a few percent).

The record then carries:

```
reference { simulator:"stim", version, url, shots, num_measurements,
            per_bit_ones[], samples_packed[] (ALL reference samples,
            bit-packed: bit b -> byte b>>3, position 7-(b&7)),
            noise_model_conversion {...}, conversion_checks {...} }
validation { method, asqs_shots, reference_shots, permutations,
             tvd_observed, tvd_null_mean/max, permutation_p_value,
             per_bit {...}, per_detector {...}, syndrome_weight_chi2 {...},
             observables [...], alpha, passed, crosschecked_utc }
```

`asqs verify` re-derives every DETERMINISTIC part of these blocks from the
embedded samples (per-bit counts, chi-squares, p-values, detector counts,
TVD) and rejects mismatches; the permutation p-value is stochastic by
nature and can be redone by anyone from the embedded samples.

Validation batches (daemon): with `--validation-shots K
--validation-every N`, every N-th processed share derives a K-shot batch
(16 ≤ K ≤ 65536) marked `"validation_batch": true` e.g. K=1024 with
16-shot normal shares, or K=65536 for a publication-grade run. Normal
shares keep the pipeline light; validation batches carry the statistical
weight.
