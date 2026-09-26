# Forensic Verification Report: ASQS `outputs/` dataset

Scope: `outputs/asqs_out_58_43_08_23_09_2026.json` (primary subject), cross-checked
against all 25 files in `outputs/`, `ledger.jsonl`, `PROOF_SPEC.md`, and the C++/Python
reference implementations (`cpp/src/*.cpp`, `python/asqs/engine.py`, `python/asqs/verify.py`).

**This revises an earlier draft of this report.** Two of that draft's three "failed"
findings were wrong (see §4 and §5) but a real, systemic defeat of the project's
proof-of-work provenance model exists and is worse than what that draft flagged. Every
claim below was independently recomputed from the raw files and the project's own
reference implementations; commands are included so they can be re-run.

## Executive Summary

**The quantum-circuit simulation data itself is genuine.** Every shot in every output
file replays bit-exact from its embedded seed using the project's own Python reference
engine. The physics is not fabricated.

**The cryptographic provenance chain was not just inconsistent it was disabled.**
All 25 files currently in `outputs/` were produced through a code path
(`quantum_mode` jobs, `difficulty == "1"`) that skipped Bitcoin-style hashcash
proof-of-work entirely, on both the write side and the daemon's own validation side.
`share_hash_hex` in every one of those files is a hash of operator/software-supplied
data, not of a real ASIC's work. Running the project's own auditor confirms this:
**25/25 output files fail `asqs verify`'s hashcash check.**

**This has been fixed in the daemon's source (§7), and the fix has been
independently re-verified end-to-end**, including against a from-scratch,
protocol-faithful stratum client with no special knowledge of daemon internals a
real brute-force nonce search, submitted over plain `mining.submit`, is now required
and enforced, exactly as real Bitaxe/ESP-Miner firmware already does it. The 25
files already checked into `outputs/` pre-date the fix and have not been
regenerated; they remain correctly rejected by the fixed validator.

## Verified ✓

| Check | Status | Details |
|---|---|---|
| JSON parses correctly | ✓ PASS | Valid JSON, `schema: asqs.batch/1` |
| 16 shots / batch, 33 outcomes, 24 detectors, 1 observable | ✓ PASS | Matches `d=3` surface-code memory circuit |
| Gate statistics (6 flips / 384, 15,625 ppm) | ✓ PASS | Recomputed from raw detector arrays |
| 16 distinct outcome vectors | ✓ PASS | All 16 shots have unique `outcomes` |
| **Full deterministic shot replay** | ✓ PASS | 0/16 mismatches vs. the project's own Python engine, run from the embedded seed |
| **`distinct_syndromes: 4`** | ✓ PASS (see §3 this was previously flagged as wrong; it is correct) | Reproduced exactly via both reference implementations' own definition |
| Seed formula `sha256(share_hash‖header80)` | ✓ PASS (see §4 previously flagged as wrong; it is correct) | Correctly and consistently implemented |
| `ledger.jsonl` hash chain | ✓ PASS | All 25 entries verified via `asqs verify --ledger-only`; no post-write tampering |

## Failed ✗

| Check | Status | Details |
|---|---|---|
| **Hashcash proof-of-work** | ✗ FAIL **25/25 output files** | `asqs verify --all` reports `hashcash verification failed` on every file |
| `share_hash_hex == SHA256D(header80)` | ✗ FAIL 0/25 files | Root cause: header80 is a placeholder, not a real block header |
| Header80 structure | ✗ FAIL 25/25 files | Bytes 0–47 are all zero; bytes 48–79 are just `share_hash_hex` copied in |
| Website "syndrome discrepancy" badge (`index.html`) | ✗ Was a false positive | Client-side recount used a different, non-spec definition of "distinct syndromes" **fixed in this update** |

## 1. The core finding: proof-of-work is bypassed for every published batch

The README's central claim is that every shot is "purchased with hashcash" from a real
ASIC (`seed = SHA256(share_hash‖header80)`, where `share_hash = SHA256D(header80)` of a
genuine Bitcoin-style block header). That is exactly what `cpp/src/stratum.cpp:build_header()`
and `verify_hashcash()` implement for **normal** shares. It is not what produced the
files in `outputs/`.

`cpp/src/daemon.cpp`, in the `quantum_mode` branch of `shot_loop()`:

```cpp
// Create synthetic header based on quantum results
std::string synthetic_data = share.quantum_results_hex + share.job_id + share.worker;
std::vector<uint8_t> synthetic_hash(32);
sha256d((const uint8_t*)synthetic_data.data(), synthetic_data.size(), synthetic_hash.data());

// Create a dummy header (80 bytes)
std::vector<uint8_t> dummy_header(80, 0);
for (int i = 0; i < 32; ++i) dummy_header[80 - 32 + i] = synthetic_hash[i];

share.header_hex = to_hex(dummy_header);
share.share_hash_hex = to_hex(synthetic_hash);
```

`quantum_results_hex` is not ASIC output `verify_quantum_submit()` in
`cpp/src/stratum.cpp` populates it directly from the stratum submit's own
extranonce2/ntime/nonce parameters, with no work requirement attached. And
`make_quantum_job()` sets `difficulty = 1/1` with the comment *"no difficulty
filtering for quantum jobs."* So a `quantum_mode` share costs zero real hashing:
anything that can open a stratum connection and submit well-formed JSON gets a
"verified" batch.

Crucially, the daemon's **own validator** the code that decides what gets promoted
from `notvalidated/` into `outputs/` knows this and explicitly special-cases it.
`cpp/src/pipeline.cpp`, `validate_file()`:

```cpp
// 1) hashcash re-verification (skip for quantum mode)
if (difficulty != "1") {
    if (!verify_hashcash(header_hex, difficulty, hash))
        return invalid("hashcash failed");
    if (to_hex(hash) != share_hash_hex) return invalid("share hash mismatch");
} else {
    // Quantum mode: no hashcash re-verification.
    // The writer set share_hash_hex = synthetic_hash = sha256d(quantum_results+job_id+worker)
    // and derived the seed from that hash + dummy_header.
    hash = from_hex(share_hash_hex);
}
```

So this isn't a bug that let bad data slip past validation every file in `outputs/`
was promoted *by design*, under a branch that the daemon itself documents as skipping
the check `PROOF_SPEC.md` §11 says is step 1 of validation.

The Python auditor (`python/asqs/verify.py`), by contrast, has no such carve-out it
unconditionally calls `verify_hashcash()`. That asymmetry is exactly what the project's
own integration test is supposed to catch (`PROOF_SPEC.md`'s intro requires the C++ and
Python implementations to be "bit-exact" against each other); here they materially
disagree on whether these 25 batches are even valid.

### Reproduction

```bash
$ PYTHONPATH=python python3 -m asqs.cli verify --all --project-root .
  [FAIL] asqs_out_00_44_08_23_09_2026.json :: hashcash verification failed
  [FAIL] asqs_out_00_44_08_23_09_2026_2.json :: hashcash verification failed
  ... (23 more, all FAIL, same reason)
  [OK ] ledger.jsonl :: chain intact (25 entries)
```

25/25 output files fail. The ledger chain itself is internally consistent (nothing was
edited after being written) but that only proves the *ledger* wasn't tampered with,
not that the shares it credits are real.

### Scope check across all 25 files

```
header[:48] == 48 zero bytes:            25 / 25
header[48:80] == share_hash_hex bytes:   25 / 25
SHA256D(header80) == share_hash_hex:      0 / 25
seed == sha256(share_hash_hex‖header80):  25 / 25   (internally consistent, just built on a non-hashcash hash)
```

This is systemic, not a one-file anomaly. Every batch currently published went through
the `quantum_mode` path. `status.json` from this same daemon run shows
`hashrate_est_hps: ~1.18 TH/s` and `difficulty: "256"` elsewhere in its state i.e. a
real Bitaxe-speed miner was being tracked by the adaptive-difficulty controller at some
point in this run but none of the 25 promoted batches used that path; all used
`difficulty: "1"` / `quantum_mode`.

## 2. What *is* trustworthy: the simulation replay

Independent of the header/hashcash problem, the recorded `outcomes` / `detectors` /
`observables` for every shot **do** deterministically follow from the embedded
`seed_hex`, using the project's own reference simulator:

```python
seed = engine.derive_seed(share_hash, header)          # matches seed_hex: True
for k, embedded in enumerate(shots_j):
    rec = engine.run_shot(circ, noise, engine.derive_shot_prng_seed(seed, k))
    # canonical(rec) == canonical(embedded) for all 16 shots 0 mismatches
```

So: nothing about the *stabilizer-circuit sampling* was hand-edited or cherry-picked
relative to its own seed. The problem is entirely upstream, in how that seed is
allowed to be chosen and, per §1, an operator (or anyone who can reach the stratum
port) currently controls it with no proof-of-work cost.

## 3. Corrected: `distinct_syndromes: 4` is right, not wrong

The earlier draft of this report recomputed 5 distinct detector patterns and flagged
the stored value of 4 as an error. That recount included the all-zero ("clean", no
detectors fired) pattern as one of the "distinct" values. That is not how this project
defines the statistic. Both reference implementations agree, independently:

- `cpp/include/pipeline.h`: `min_distinct_syndromes` is commented *"distinct nonzero
  detector patterns."*
- `cpp/src/pipeline.cpp`: `if (any) syndrome_strings.insert(syn);` only inserts when
  at least one detector bit is set.
- `python/asqs/verify.py`: `if "1" in syn: syndromes.add(syn)` same rule.

Recomputing with that (correct, spec-consistent) definition against
`asqs_out_58_43_08_23_09_2026.json` gives exactly **4**, matching the file. The
`index.html` dashboard had the same bug the earlier report made its client-side
recount used `JSON.stringify(detectors)` over *all* shots, clean ones included, so it
was flagging a false "⚠ Discrepancy" badge on outputs that are in fact fine on this
axis. **Fixed in this update** (see §6).

## 4. Corrected: the seed-derivation formula is not wrong

The earlier draft treated `sha256(sha256d(header80) || header80)` (the documented
formula) and `sha256(share_hash || header80)` (what it computed) as two different,
conflicting formulas, and concluded the documentation was wrong. They are the same
formula: `PROOF_SPEC.md` §6 *defines* `share_hash = SHA256D(header80)`. Substituting
that definition in gives exactly the documented expression. There is no formula bug.

What's actually true, and is the real finding, is narrower and worse: the **value**
stored as `share_hash_hex` was not produced by hashing the header at all (§1) so
recomputing `SHA256D(header80)` independently and expecting it to equal the *stored*
`share_hash_hex` fails, not because the formula is wrong, but because the header80 was
never real to begin with.

## 5. Recommended actions

1. **Decide what `quantum_mode` is for.** As implemented, it is a way to mint fully
   valid-looking, ledger-credited output batches with zero proof-of-work and zero
   connection to any ASIC. If it's meant to stay (e.g. as a local/offline test-data
   generator), it should not write into the same `outputs/` directory or `ledger.jsonl`
   that real hashcash-backed shares use, and batches produced this way should be
   labeled as such in the schema (e.g. `proof.provenance: "synthetic"`) rather than
   carrying a `share_hash_hex` field that looks identical in shape to a real one.
2. **Make the C++ validator match the Python auditor.** `pipeline.cpp`'s
   `difficulty != "1"` carve-out in `validate_file()` should not silently diverge from
   `verify.py`, which has no such carve-out. The integration test that's supposed to
   enforce bit-exact agreement between the two implementations should cover this case
   and currently doesn't.
3. **Regenerate `outputs/` from real shares**, or clearly re-label the existing 25
   files as synthetic, before presenting them anywhere as hashcash-provenanced data.
4. **`index.html`**: the syndrome-discrepancy recount is fixed in this update (§6); no
   further action needed there.
5. **`README.md` / `PROOF_SPEC.md`**: consider a note that `quantum_mode` jobs are
   exempt from §5/§11's hashcash requirement, since right now the documents describe a
   guarantee the shipped data doesn't currently meet.

## 6. Changes made to the website in this update

- `index.html`: both client-side "distinct syndromes" recounts now exclude the
  all-zero pattern, matching `PROOF_SPEC.md` and both reference implementations. This
  removes the false "⚠ Discrepancy" badges the old (incorrect) recount was producing.
- `index.html`: added a provenance banner summarizing §1 above, linking to this report.
- This report: rewritten to correct §3/§4's false positives and to document the actual,
  systemic hashcash-bypass finding in §1, with file/line citations and reproducible
  commands.

## 7. The bypass has been fixed and independently re-verified end-to-end

Following §5's recommendations, the code path itself has been fixed not just
documented. Summary of the change (see the repo's commit history / diff for exact
lines):

- **`cpp/src/stratum.cpp` `JobManager::make_quantum_job()`**: now takes a real
  `DiffRat difficulty` and sets `j.target = target_from_difficulty(difficulty)` and a
  genuine pseudorandom `prevhash_field`, instead of `difficulty = 1/1` +
  `target = saturate_max()` (accept-everything) + an all-zero prevhash.
- **`cpp/src/stratum.cpp` `handle_line()`**: `mining.submit` now *always* calls the
  real `verify_submit()` (hashcash) path. The old branch that treated
  `quantum_mode` jobs as "trust the miner, no proof-of-work" is gone.
- **`mining.quantum_submit`**: deprecated it now unconditionally returns an error
  instead of minting a free, unverified share. `JobManager::verify_quantum_submit()`
  (the function that had no hashcash check) has been removed entirely.
- **`cpp/src/daemon.cpp`**: `on_verified_share()` / `shot_loop()` no longer construct
  a synthetic `dummy_header`/`synthetic_hash` for quantum-tagged shares. Every share,
  quantum-tagged or not, now derives its seed from a real `header_hex`/`share_hash_hex`
  produced by `verify_submit()`.
- **`cpp/src/pipeline.cpp`** `validate_file()`: the `difficulty != "1"` carve-out
  (`// Quantum mode: no hashcash re-verification`) is gone. Hashcash is re-verified
  unconditionally, for every batch, matching `python/asqs/verify.py` exactly the
  asymmetry described in §1 no longer exists.
- Two real wire-protocol bugs, found while testing the fix against an actual stratum
  client, were also fixed both would have broken compatibility with real ASIC
  firmware even after the security fix: `push_quantum_job()` was broadcasting a
  hardcoded all-zero `prevhash` in `mining.notify` instead of the job's real one
  (the pool's own `verify_submit()` would then reconstruct a *different* header than
  what the miner actually hashed, so no real share could ever validate), and it never
  sent `mining.set_difficulty` at all.

### Still ASIC-computable: full end-to-end proof

To confirm the fix doesn't just move the bypass somewhere else, and that a real
Bitaxe/ESP-Miner (which only speaks plain stratum v1: `subscribe` →
`authorize` → read `notify`/`set_difficulty` → brute-force the nonce using only
broadcast fields → `submit`) can still redeem one of these jobs, two independent test
clients were built with **zero special knowledge of daemon internals** everything
they use comes off the wire, the same as real firmware would see:

1. A Python stratum client performing the full handshake and a real brute-force
   SHA256d nonce search.
2. A second, independent C++ client using raw POSIX sockets and the project's own
   `sha256d()`/JSON code (built to rule out any Python-specific sandbox networking
   quirk as a confound).

Both were run against a rebuilt `asqsd` (fixed code, all 1259 unit tests still
passing) on a scratch project directory, at a low fixed difficulty (`1e-8`, chosen
only so nonce search finishes in milliseconds rather than requiring real ASIC-class
hashrate the target-comparison logic being exercised is identical at any
difficulty). Result, C++ client, five separate real-mined shares in one run:

```
shares_received: 5   shares_verified: 5   batches_written: 5
batches_validated: 5   batches_useful: 5   batches_useless_deleted: 0   batches_invalid_deleted: 0
```

And, critically, running the project's own Python auditor against the resulting
`outputs/` the same auditor that failed 25/25 on the old dataset in §1 now
passes all of them:

```
$ asqs verify --all
  [OK] asqs_out_22_33_03_24_09_2026.json    shots=16 flips=8/384  syndromes=4
  [OK] asqs_out_23_33_03_24_09_2026.json    shots=16 flips=6/384  syndromes=3
  [OK] asqs_out_23_33_03_24_09_2026_2.json  shots=16 flips=17/384 syndromes=7
  [OK] asqs_out_23_33_03_24_09_2026_3.json  shots=16 flips=1/384  syndromes=1
  [OK] asqs_out_24_33_03_24_09_2026.json    shots=16 flips=2/384  syndromes=1
  [OK] ledger.jsonl :: chain intact (5 entries)
```

Spot-checking one file directly against the raw JSON confirms the structural fix
described in §1 is real, not just a passing test:

```
header[:48] all zero?              : False   (was True for all 25 old files)
header[48:] == share_hash bytes?   : False   (was True for all 25 old files)
SHA256D(header) == share_hash_hex  : True    (was False for all 25 old files)
```

**This is the important negative control too**: with `shots_per_share=4` (rather
than 16), the very first mined share in this same setup was correctly accepted by
hashcash re-verification but then legitimately discarded by the *unrelated*,
pre-existing statistical quality gate (`min_detector_flips: 1`) 
`USELESS: no detector activity (noise not observable)` Simply because 4 shots at
this noise level had a real chance of showing zero detector flips. That the daemon
went out of its way to compute the real physics, mine and verify a real hashcash
share, and *then* discard it for being statistically uninteresting is itself
evidence that the hashcash path is genuinely being enforced now rather than
short-circuited.

### What this does and doesn't mean for the shipped `outputs/`

The fix has been implemented and verified in the daemon's source and against a live
test instance. It has **not** been retroactively applied to the 25 files already
checked into `outputs/` in this repo those are exactly the synthetic batches
described in §1 and remain invalid under the fixed validator (as they should: they
were never backed by real proof-of-work). Regenerating `outputs/` with the fixed
daemon against a real miner (or re-running the fixed daemon long enough at a real
difficulty) is the remaining action item from §5's recommendation #3.


## 8. Two further protocol bugs found testing against real ASIC hardware

Testing §7's fix against a real Bitaxe (BM1370, ESP-Miner firmware) surfaced two
more bugs both pre-existing in the codebase's stratum implementation, not
introduced by §7, but only now exercised because §7 made quantum-mode shares go
through real verification for the first time.

**8a. Missing `mining.configure` handling.** ESP-Miner sends `mining.configure`
right after `mining.subscribe` to negotiate version-rolling (BIP310) an ASIC
efficiency feature where the miner mutates bits of the header's `version` field as
extra search space. The daemon didn't recognize the method at all, so it fell
through to a generic `"method not found"` error. ESP-Miner has a documented history
of not reliably treating that as "no" and rolling version bits anyway, which
desyncs the header the ASIC actually hashed from the one `verify_submit()`
reconstructs (fixed version). Fixed: `cpp/src/stratum.cpp` now handles
`mining.configure` explicitly and returns the spec-defined refusal,
`{"result":{"version-rolling":false},"error":null}`.

**8b. Wrong prevhash byte order on the wire (the actual cause of "hash above
target" persisting after 8a).** `push_job()` and `push_quantum_job()` both sent
`mining.notify`'s prevhash field as `reverse_all_bytes(prevhash_field)`. That is
not the convention real Bitcoin stratum implementations use. The wire value real
pools send is the prevhash **exactly as it appears in the raw little-endian
header** no transformation. (The "word-reversed" description sometimes used to
explain this field describes its relationship to a *different*, human-readable
RPC format e.g. `getblocktemplate`'s `previousblockhash` not a transformation
the wire protocol itself needs; applying `wordReverse` to that RPC-style hash is
what recovers the raw header bytes in the first place.) Concretely, for bytes
`b0 b1 ... b31` as they sit in the header:

```
explorer/RPC-style hash = reverse_all(raw_header_bytes)
stratum wire prevhash   = word_reverse(explorer_hash) = raw_header_bytes   (identity)
```

`build_header()` already used `job.prevhash_field` unreversed (correct, per
`PROOF_SPEC.md`'s own definition of that field). So the daemon was internally
self-consistent, but a real ASIC build against the real-world convention above
received a full-byte-reversal of the correct value, reconstructed a *different*
header than the one `verify_submit()` checks against, and got a different hash.
Every genuinely-valid-effort share failed, at exactly the rate you'd expect from
real hashrate against the broadcast difficulty (which is why the rejections kept
coming steadily rather than never at all the ASIC was doing real work, just
against the wrong bytes). Fixed: both functions now send `to_hex(prevhash_field)`
directly, no reversal.

This was independently confirmed two ways: a small worked numeric example against
the documented real-world convention (not just re-reading this project's own
code), and an end-to-end test with a from-scratch client rebuilt to the *corrected*
convention, which mined and had 5/5 shares accepted where the old convention
produced 100% rejections under identical settings. What this **doesn't** prove is
## 9. Version-rolling: the fix that actually mattered

§8a's `mining.configure` fix was real and shipped correctly (confirmed live against
the user's Bitaxe: `configure version-rolling declined`). It did not resolve the
rejections. A raw firmware-side trace made that clear immediately:

```
tx: {"id":30,"method":"mining.submit","params":["Bitaxe","00000003","23000000","6ab5f586","982a8ec5","0002a000"]}
asic_result: ... ver: 2002A000 Nonce 982A8EC5 diff 535.5 of 256.
rx: {"id":30,"result":false,"error":[23,"hash above target (low difficulty)",null]}
```

`mining.submit` carries **six** parameters, not five ESP-Miner sends a 6th
"version bits" value and ORs it into the broadcast version regardless of what
`mining.configure` said (`0x20000000 | 0x0002a000 = 0x2002A000`, exactly the `ver`
value the ASIC itself logs). Confirmed against three independent examples from the
trace, all exact matches. Declining the extension doesn't stop this firmware from
rolling; the pool has to actually read the bit and use it.

`build_header()` and `verify_submit()` never looked at a 6th parameter at all the
header was always built from the bare job version, so every genuinely-valid,
real-proof-of-work share the ASIC found was checked against the wrong header and
rejected. Fixed:

- `mining.configure` now grants the extension honestly (`{"version-rolling":true,
  "version-rolling.mask":"1fffe000"}`) instead of declining it, since declining
  doesn't change what the hardware actually does.
- `build_header()` takes a `version_bits` argument and ORs it into `job.version`
  before hashing.
- `verify_submit()` reads an optional 6th `mining.submit` parameter and threads it
  through to `build_header()`. Absent (5-param submits, e.g. from the test clients
  in §7) still works exactly as before `version_bits` defaults to 0.

Verified by replaying the **exact bits from the user's own trace**: a test client
rolling `version = 0x20000000 | 0x0002a000` and submitting `"0002a000"` as a 6th
parameter the identical value their real Bitaxe used got accepted 3/3 against
the fixed daemon, under the same `random_clifford` + adaptive-difficulty settings
they were running. `header80`'s first four bytes decode to the rolled version
(`0x2002a000`), confirming it's actually baked into the hashed header, not just
accepted incidentally.




## 10. Round 3: the two byte-order bugs actually responsible for "hash above target"

§9's version-rolling fix was correct but not sufficient: real-hardware testing
the same day (Bitaxe Gamma 601, `random_clifford`, adaptive difficulty 256)
still produced **100% rejections** 40+ genuine shares at diff 268–13982 of
256, each rejected `error 23 "hash above target (low difficulty)"`. The
ASIC was doing real work against the wrong bytes, again. Two more
divergences existed between `build_header()` and the header the BM1370
actually hashes, this time in the *block-body* region (header bytes 4–68):

**10a. Merkle root inserted reversed.** `build_header()` wrote
`reverse(sha256d(coinbase))` into bytes 36–68 (`for (i) push(m[31-i])`),
and `PROOF_SPEC.md` §5 + the normative comment in `stratum.h` normatively
documented that reversal. Real firmware inserts the digest **raw**:
ESP-Miner v2.15.3 `components/stratum/mining.c` computes the merkle root
and copies it into the ASIC midstate verbatim (line 88,
`memcpy(midstate_data + 36, merkle_root, 28)`), and `test_nonce_value()`
reconstructs the same way (line 155, `reverse_32bit_words(job->merkle_root,
header + 36)` the job stored a word-reversed copy at line 75, so the
double reversal is the identity). 32 bytes differ.

**10b. Wrong prevhash wire convention (§8b's model was inverted).** §8b
removed the full-array reversal from `mining.notify` and asserted that
"real firmware inserts this field into the header verbatim." It does not.
Firmware places `bswap32-per-word(hex2bin(notify.prevhash))` into the
header `mining.c:78-80` applies `reverse_endianness_per_word()` to the
wire bytes, and the midstate at `mining.c:87` uses those same swapped
bytes. The real-world pool-side convention is therefore to send the
**per-word-swapped** form of the raw header field (equivalently
`word_reverse(display prevhash)`). The daemon sent the raw field, so the
ASIC hashed swapped bytes while `build_header()` checked raw bytes:
bytes 4–36 differ. `push_job()`/`push_quantum_job()` now send
`to_hex(wire_prevhash(prevhash_field))` (per-word bswap), while
`build_header()` keeps using `job.prevhash_field` verbatim matching
firmware on both sides.

Fixed in the same pass, for BIP310 hygiene: the rolled version is now
reconstructed as `(base & ~mask) | (bits & mask)` with the granted mask
`0x1fffe000`, instead of a bare OR (equivalent for this pool's
`0x20000000` base, but now impossible to drift out of spec). And one
pre-existing non-stratum bug surfaced by the integration test:
`benchmarking_` was initialized from `enable_benchmark` (default true)
regardless of mode, but is only ever cleared inside `controller_tick()`'s
**adaptive** branch so in `fixed` mode every verified share was counted
as "benchmark" and silently dropped forever. The flag now only starts when
`difficulty_mode == "adaptive"` (user's production path unchanged).

**Why every previous round missed 10a/10b:** all in-repo verification was
self-referential. The §7 test clients (Python and C++) and
`tests/test_integration.py`'s `mine_share()` each re-implemented whatever
convention the daemon then used `test_integration.py` still hashed
`prevhash[::-1]` + `merkle[::-1]` (the pre-§8b wire form, silently broken
since §8b). The unit tests mine *and* verify through the same
`build_header()`. Nothing in the suite ever compared bytes against real
firmware; only the physical Bitaxe could falsify it.

The firmware-side ground truth was pinned independently this round by a
reference validator built against ESP-Miner v2.15.3's own unit-test vectors
(`components/stratum/test/test_mining.c`: merkle roots ×2, nonce diffs ×2,
full-pipeline diff-683 all 5 vectors pass), then cross-checked against
the v2.15.3 sources cited above.

### Verification (all reproducible)

```
Differential live test (same daemon, two clients, low fixed difficulty):
  BEFORE fix:  firmware-convention client 0/3 accepted  ("hash above target")
               daemon-convention client  2/2 accepted   (self-consistent bug)
  AFTER  fix:  firmware-convention client 3/3 accepted
               daemon-convention client  0/1 accepted   (old convention dead)

Byte-exact check (outputs/ batches vs firmware reference, wire data only):
  header80 == LE32(0x2002A000) || pswap(wire prevhash) || raw sha256d(coinbase)
           || LE32(ntime) || LE32(nbits) || LE32(nonce)   3/3 batches exact
  (version-rolling bits 0x0002a000 replayed from the user's real trace)

Unit tests: 1259/1259. Integration test: 24/24 | 3/3 shares accepted
(including one BIP310 version-rolled share), batches promoted, and the
independent Python auditor verifies every batch + the ledger chain.
```

## Conclusion

**Verdict: simulation data genuine; the provenance gap has been fixed and
independently re-verified end-to-end, but the 25 files already in `outputs/`
pre-date the fix and remain unbacked by real proof-of-work.**

- Trust the **quantum simulation results** (outcomes, detector flips, error rates,
  gate statistics including `distinct_syndromes`) they replay bit-exact from their
  seeds under the project's own reference implementation, for every file currently in
  the repo.
- Do **not** treat any of the 25 *existing* files in `outputs/` as evidence of real
  Bitaxe/ASIC work `asqs verify --all` still rejects all of them, and should,
  since they were produced before the fix.
- The daemon code that produced that gap (`quantum_mode`'s hashcash bypass, on both
  the write side and the validator side) has been removed (§7). Two independently
  built, protocol-faithful test clients with no special knowledge of daemon
  internals beyond the public stratum wire format mined and submitted real shares
  against the fixed daemon, which the project's own auditor then verified as valid,
  hashcash-backed batches.
- §7's own test clients still weren't a full substitute for real hardware: testing
  against an actual Bitaxe surfaced three more pre-existing wire-protocol bugs
  missing `mining.configure` handling (§8a), a wrong byte-order convention for the
  prevhash field (§8b), and, the one that took a second round of real-hardware
  testing to catch, unhandled version-rolling bits sent as a 6th `mining.submit`
  parameter (§9). All three are now fixed and re-verified §9 specifically by
  replaying the exact bits from the user's own real-hardware trace and confirming
  acceptance. But unlike §7's circuit-level claims, which the project's own
  auditor can check independently real-ASIC compliance specifically only gets
  checked one bug at a time, against real hardware; there may be more.
- That warning proved right: §10's two block-body byte-order bugs (reversed merkle
  root, wrong prevhash wire convention) were only distinguishable from §9's fix by
  another round against the physical Bitaxe. They are now fixed and verified by
  differential live testing, byte-exact header comparison, and the full test
  suite. The stratum layer is now byte-identical to ESP-Miner v2.15.3's own
  header construction, pinned by its upstream unit-test vectors.
- §11: the first successful full live run (real Bitaxe, benchmark + adaptive
  difficulty, batches promoted) surfaced three file/trace-hygiene bugs
  same-second batch-filename collisions, a millisecond clock that always read
  `.000`, and a tmp-file sweep whose match pattern could never hit all fixed
  and re-verified. None of the three touched the provenance chain.
- §12: the evidence itself was the next frontier. Records now carry the full
  circuit program (instruction list + provenance, hash-pinned), machine-readable
  noise-model semantics, optional 1024+ shot validation batches, and the
  decisive upgrade an independent stim reference distribution embedded with
  a statistical comparison (permutation TVD + Bonferroni chi-square battery).
  The claim is no longer "our own replay matches" but "an independent
  simulator agrees, statistically".
- Remaining action: regenerate `outputs/` with the fixed daemon before presenting
  the dataset as hashcash-provenanced, ideally from a real ASIC run now that §8–12
  are fixed.

## 11. Round 4 (post-deployment): batch-filename collisions, fake-millisecond clocks, and a tmp-file sweep that never matched

After §10 the daemon ran against the real Bitaxe for the first time end-to-end
(benchmark 806 GH/s, adaptive difficulty engaged, batches promoted). The run
surfaced three more bugs none in the provenance path, all in file/trace
hygiene:

**11a. Batch filenames collide within the same second.** Filenames were
`asqs_notvalidated_{ss}_{mm}_{HH}_{dd}_{MM}_{yyyy}.json` with one-second
granularity. The Bitaxe delivered several verified shares per second, so every
batch in a given second shared one base name; the collision guard appended
`_2`, `_3`, `_4`… to the promoted `asqs_out_` files (and the *pending* name
was silently reused after each promotion, making the daemon's own
`batch written:` log lines indistinguishable). Fix: the filename now carries
the batch id as a tail `…_{yyyy}_{batch_id}.json`, where `batch_id` is the
same 32-hex field already embedded in the JSON (`sha256(header_hex||nonce_hex)`
truncated to 16 bytes). Names are now unique per share *by construction*
(different nonces → different ids); the `_N` suffix remains only as a
last-resort guard. The promoted output keeps the same tail, so
`asqs_out_…_{batch_id}.json` is greppable straight to its ledger entry.

**11b. `now_ms()` returned seconds×1000 the millisecond field was always 000.**
Every log line read `…T12:18:46.000Z`; `created_ms`, `ts_ms`, `received_ms`
and the controller/benchmark windows all had one-second granularity
masquerading as milliseconds. Fixed with `clock_gettime(CLOCK_REALTIME)`;
timestamps now carry real sub-second precision, which also makes same-second
events distinguishable in logs. (`ntime` always used `now_sec()` and is
unaffected.)

**11c. `cleanup_tmp_files()` could never match a real tmp file.**
`write_file_atomic()` stages as `<final>.json.tmp.<pid>` marker at the *end*
but the sweep looked for the prefix `asqs_notvalidated_.tmp.`, which no staged
file can ever have. Crash leftovers accumulated forever (harmless to
correctness, since pending-count and FIFO scanning both exclude `.tmp.`
names, but a disk leak). Fixed to match batch-prefixed names *containing*
`.tmp.`; verified by planting fake leftovers and restarting.

Also corrected: the benchmark log said "using minimum difficulty" when it is
the *initial* difficulty that is in force during the benchmark window.

**Verification (all reproducible):** unit tests 1259/1259; integration test
24/24; firmware-convention differential client (5 shares, incl. version-rolled,
same trace as §10) 5/5 accepted, 5 batches written in the *same second* with
five distinct filenames, no `_2` suffixes, promoted 5/5, ledger chain intact,
and the independent Python auditor verifies all outputs (filename `batch_id`
tail == embedded `batch_id` field, hashcash + replay + proof all green).

## 12. Round 5: the missing evidence circuit definition, independent reference, and statistical validation

**The gap.** The proof record carried `circuit: {type, qubits, gates}` plus a
hash but not the circuit itself. An independent verifier had to re-implement
the builder to reconstruct what was simulated, had no reference result to
compare against (only self-replay), was limited to 16-shot evidence, and had
to infer noise-application semantics from prose. The claim "the ASIC produced
16 non-degenerate bitstrings" was reproducible but not *externally validated*.

**12a. The circuit program (schema `asqs.batch/2`).** Every record now embeds
`circuit_program`: the exact instruction list
(`{"gate":"H","qubit":3}`, `{"gate":"CNOT","control":3,"target":7}`, `{"gate":
"MZ","qubit":0,"meas_index":0}`, …), detectors, observables, and a provenance
block (for `random_clifford`: the actual 64-bit generation seed, its exact
derivation, the domain string and the per-gate draw rule; for surface codes:
the normative construction reference). `circuit_program_sha256` pins it; the
daemon validator *and* the Python auditor rebuild the program from the
embedded identity and reject any divergence cross-language golden hashes
are unit-tested (rc 24/120, sc d=3 r=3, sc d=5 r=5). Tampering a single
operation now fails validation with "circuit program hash mismatch".

**12b. Noise-model semantics, machine-readable.** `noise_model` embeds the ppb
values *plus* the exact application rules: which channel applies to which
gate class, before/after ordering, per-qubit independence on CNOT, the
recorded-outcome-flip semantics of pm (and its X_ERROR equivalence), the
Y=X·Z convention, the one-u64-per-bernoulli rule, and the program-order
application order. Two simulators can no longer silently interpret the same
numbers differently; both auditors recompute the normative block and reject
mismatches.

**12c. Validation batches.** `--validation-shots K --validation-every N`:
every N-th processed share derives a K-shot batch (16..65536; e.g. 1024 or
10000+) marked `"validation_batch": true`, while normal shares stay light.
This implements the three-tier evidence ladder: 16 shots = smoke test,
~1,000 = basic statistics, 10,000+ = meaningful distribution comparison.

**12d. Independent reference + statistical validation (`asqs crosscheck`).**
The same circuit is converted to **stim** (Google's stabilizer simulator
conversion table in PROOF_SPEC §15; the noiseless conversion is first pinned
by structural cross-checks where both engines must independently agree that
all detectors are deterministic and bit classifications match), sampled with
stim's own RNG, and compared: a two-sample permutation test on total-variation
distance of the joint outcome distribution (assumption-free), a per-bit
two-proportion chi-square battery with Bonferroni correction, per-detector
flip-rate comparison, and a syndrome-weight chi-square. The verdict and
**all reference samples, bit-packed** are embedded into the record
(`reference` + `validation` blocks); `asqs verify` re-derives every
deterministic part from the file alone. This upgrades the claim to: *"Given
circuit C and simulation semantics S, the ASQS pipeline produced samples
statistically consistent with an independently computed reference
distribution"* with the hashcash anchor proving which entropy produced them.

**Verification (all reproducible):** unit tests 1269/1269 (10 new: golden
program hashes, program determinism, meas_index sequencing, noise-model
stability); integration test 37/37 including a 512-shot validation batch
crosschecked against stim (perm_p=0.86) and a tampered-reference negative
control; live firmware-convention dataset: rc 24/120 (8 batches, 2×1024-shot
validation, perm_p 0.31/0.62) and sc d=3 r=3 (6 batches, 2×1024-shot
validation, perm_p 0.51/0.92), all 8192-shot stim references embedded,
ledger chains intact; negative controls: tampered shot → replay rejection,
tampered program → program-hash rejection, tampered noise_model → semantics
rejection, 10×-noise wrong reference → statistical rejection (min bit
p = 5.3e-13).

One honest statistical note: the crosscheck verdict gates on three tests at
α (default 0.01). A null-true batch therefore fails a few percent of the
time; a FAIL is an "investigate" signal (re-run with more reference shots /
permutations), not a fraud verdict. The default was set at 0.01 rather than
0.05 for exactly this reason, and the reasoning is documented in the tool.

## 13. Round 6: the dashboard lazy loading, a real circuit replay, and metrics from shot data

**The problem.** The website loaded *every* batch file into the browser at
startup (`Promise.all` over `outputs_list.json`), which is untenable with a
real dataset (88 batches, 112 MB, four of them 27 MB validation batches).
For `random_clifford` circuits the viewer showed "No spatial layout
showing event log only", and the Play button did nothing (the transport
loop only implemented the surface-code rounds). The table and viewer
metrics were read from the `proof.gate` block, which for random_clifford
records is always `0 / 0` (the circuit has no detectors), hiding the
interesting signal the injected error events.

**The changes (website/serve.py + website/index.html, no core daemon code
touched):**

1. **One-at-a-time loading.** `serve.py` now exposes `/api/index`
   (per-batch metadata: instant skeleton from `ledger.jsonl`, enriched in a
   background thread by parsing the real batch files smallest-first) and
   `/api/batch?file=` (a single batch, gzipped on demand and cached a
   28.7 MB validation batch travels as 1.15 MB). The browser fetches the
   index plus exactly one batch per click. Verified live: after browsing
   two batches the browser had made 2 batch requests, not 88.
2. **Numbered pager rows (1 2 3 4 5 …)** at the bottom of the table and
   inside the viewer (windowed for large N), plus ←/→ keyboard navigation.
   Clicking a number loads that single batch.
3. **A real circuit replay for the Play button.** `asqs.batch/2` embeds
   the full circuit program; each shot embeds its recorded error events
   *with the instruction index where they were injected*. The new player
   replays the actual instruction list for the selected shot: gates pulse
   in program order, CNOTs draw control→target arrows, errors flash red
   at their exact op (persistent ring + Pauli badge + log line naming the
   gate, e.g. "injected Z on q23 at op 63 (CNOT 19→23)"), terminal
   measurements settle to the real recorded outcome bits, and the
   observable closes the shot. Play/Pause resumes mid-shot; speed
   0.5–4×; jump-to-shot handles 65,536-shot validation batches; clicking
   a qubit shows its real per-shot record. The surface-code player is
   unchanged.
4. **Metrics computed from the shot records** (`shots[].errors` for
   random_clifford: `flips / (shots × measurements)` in ppm, distinct
   per-shot error signatures as syndromes, distinct outcome strings;
   `shots[].detectors` for surface-code), with `proof.gate` demoted to a
   fallback for shot-less records. The same formulas are computed
   server-side for the table index and client-side in the viewer.

**Verification (live, against the real 88-batch / 263,488-shot dataset):**
`/api/index` enriches 88/88 rows (richest validation row: 8484 error
events / 1,572,864 opportunities, 5394 ppm, 525 distinct signatures);
security paths return 404 (traversal, absolute paths, missing files);
error events verified to fire at the exact recorded op with the badge
persisting through subsequent gate pulses (bug found and fixed during
testing); qubit-info reads real per-shot data; pager navigation swaps
batches with one fetch each; the surface-code player still plays; orphaned
batches (25 stale + 18 ledger-orphaned) are detected and banner-flagged
(43 files in the raw working directory). Headless-browser screenshots
reviewed for layout regressions: none.

**Honesty note.** The replay is a *visualization of recorded data*, not a
re-simulation: the browser animates the embedded program and the recorded
per-shot events. Bit-level verification remains the job of `asqs verify`
/ `asqs crosscheck`, which re-derive everything from the file.

## 14. Round 7: Export GIF the entire visualization as an animated .gif

**The request.** A button that exports the visualization (the whole
thing: qubit stage, event log, metrics) as a GIF file.

**What was added (website/index.html only serve.py and all core code
are untouched).**

1. **"Export GIF" button** in the viewer transport row (next to Play).
   Clicking it re-runs the CURRENT shot from op 0 through the observable
   using the exact same player code that powers Play then encodes
   what was shown into a downloadable `asqs_<batch>_shot<n>.gif`.
   The button shows live progress (`rec N` while recording frames,
   `enc N%` while compressing, `saved ✓ <size>` on completion); the
   transport controls are disabled and Close/Escape are blocked for the
   few seconds of recording so the capture cannot be torn by navigation.
2. **Every frame is a real snapshot of the live DOM**: the stage is the
   actual viewer SVG serialized per frame (CSS variables resolved, so
   the GIF matches the on-screen theme dark and light both verified);
   the event log column is re-rendered from the live log lines with the
   same colors; the header carries batch id, timestamp, circuit label
   and shot number; the readout strip carries the live op progress and
   op text; the footer carries the validation metrics computed from
   that batch's shot records. Nothing in the GIF is synthesized.
3. **A dependency-free GIF89a encoder embedded in the page**
   (`ASQS_GIF`: 256-color incremental quantizer with exact seed colors
   for the UI palette, LZW with variable-width 9→12-bit codes and
   CLEAR-based table resets, NETSCAPE looping). No CDN, no library, no
   network calls the export works offline, like the rest of the site.
   The encoder is delimited by `GIFLIB-START/END` markers and unit
   tested by extracting the shipped code verbatim (Node) and decoding
   its output with Pillow.

**Verification.**

- Node round-trip (shipped code, extracted verbatim): 3 test GIFs
  flat frames, per-frame delays + infinite loop, and a stress case with
  more than 256 unique colors (nearest-color mapping) and an LZW table
  that fills past 4096 entries (CLEAR/reset path) all decode under
  Pillow with **bit-exact palette indices** and matching delays.
  One real bug was found and fixed this way: the code-width increase
  was emitted one code too early (the decoder's table runs one entry
  behind the encoder's, so the width must grow at `(1<<size)+1`), which
  Pillow rejected as a broken data stream before the fix.
- Live browser (headless Chromium against the real 131-file directory):
  - random_clifford batch: 48 frames, 926×570, ~1.6 MB. VLM review of
    extracted frames confirmed the batch id header, the 24-qubit grid
    with real outcome bits, CNOT arrows mid-replay, the event log
    (including `observable parity_all = 1`), and the footer metrics
    (flips 2/384 · 5,208 ppm · 2 syndromes · 16 outcomes) matching the
    table row exactly.
  - surface-code batch: 28 frames, 709×519 d=3 lattice (9 data
    circles + 8 ancilla squares with link lines), round-by-round
    X/Z/final-measurement log; the four green qubits were
    cross-checked against the log's `data: 0 1 1 0 0 0 1 1 0` line.
  - 65,536-shot validation batch: jumped to shot 1000, exported the
    GIF header reads "shot 1000 / 65536" and the footer carries that
    batch's own shot-derived metrics.
  - Both themes export with their real colors; zero console errors;
    transport controls lock during export and unlock after; normal
    Play/Pause playback after an export is unaffected.

**Honesty note.** A GIF is a *recording of the visualization*, i.e. of
the same recorded per-shot data the viewer animates. It is evidence of
what the site shows, not an independent verification channel use
`asqs verify` / `asqs crosscheck` for bit-level proof.
