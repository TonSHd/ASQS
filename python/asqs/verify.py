"""ASQS output verification - the third-party audit path.

`verify_batch(path)` re-derives everything in a batch/output file from the
embedded share alone (engine mirror + hashlib) and checks:
  1. schema and field sanity (asqs.batch/1 or asqs.batch/2)
  2. hashcash: SHA256D(header80) LE-value <= target(difficulty)
  3. embedded share hash matches the recomputed one
  4. seed derivation: sha256(share_hash || header80)
  5. circuit identity hash
  5b. (v2) circuit program: embedded operations/provenance hash, and that
      the program equals the one rebuilt from the embedded identity
  5c. (v2) noise_model semantics block matches the normative semantics
  6. FULL deterministic replay of every shot (bit-exact match)
  7. gate statistics recomputation
  8. (outputs only) proof block consistency + ledger entry existence
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass
from typing import List, Optional, Tuple

from . import engine


@dataclass
class VerifyResult:
    ok: bool
    path: str
    reason: str = ""
    shots: int = 0
    detector_flips: int = 0
    detector_total: int = 0
    distinct_syndromes: int = 0
    is_output: bool = False
    ledger_seq: Optional[int] = None


def _load(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def verify_batch(path: str, ledger_path: Optional[str] = None) -> VerifyResult:
    res = VerifyResult(ok=False, path=path)
    try:
        batch = _load(path)
    except Exception as e:  # noqa: BLE001
        res.reason = f"unreadable/json error: {e}"
        return res

    def fail(msg: str) -> VerifyResult:
        res.reason = msg
        return res

    if batch.get("schema") not in ("asqs.batch/1", "asqs.batch/2"):
        return fail("bad schema")
    is_v2 = batch.get("schema") == "asqs.batch/2"
    sh = batch.get("share") or {}
    sd = batch.get("seed") or {}
    shots_j = batch.get("shots")
    circ_j = batch.get("circuit")
    if not isinstance(shots_j, list) or not circ_j:
        return fail("missing fields")

    header_hex = sh.get("header_hex", "")
    difficulty = sh.get("difficulty", "")
    share_hash_hex = sh.get("share_hash_hex", "")
    seed_hex = sd.get("seed_hex", "")
    if len(header_hex) != 160 or len(share_hash_hex) != 64 or len(seed_hex) != 64:
        return fail("bad hex field lengths")

    # 1) hashcash
    ok, share_hash = engine.verify_hashcash(header_hex, difficulty)
    if not ok:
        return fail("hashcash verification failed")
    if share_hash.hex() != share_hash_hex:
        return fail("share hash mismatch")

    # 2) seed derivation
    header = bytes.fromhex(header_hex)
    seed = engine.derive_seed(share_hash, header)
    if seed.hex() != seed_hex:
        return fail("seed derivation mismatch")

    # 3) circuit identity
    ident = engine.circuit_identity(circ_j)
    if engine.circuit_sha256(circ_j) != batch.get("circuit_sha256"):
        return fail("circuit hash mismatch")
    try:
        circ = engine.build_circuit(circ_j)
    except Exception as e:  # noqa: BLE001
        return fail(f"circuit build failed: {e}")

    # 3b) v2: circuit program — the full instruction list is embedded and
    # pinned by its own hash; rebuild it from the identity and require an
    # exact match (bit-for-bit reconstruction without the builder).
    if is_v2:
        prog = batch.get("circuit_program")
        prog_hash = batch.get("circuit_program_sha256")
        if not isinstance(prog, dict) or not prog_hash:
            return fail("v2 missing circuit program")
        if engine.program_sha256_of(prog) != prog_hash:
            return fail("circuit program hash mismatch")
        if engine.program_sha256(circ_j) != prog_hash:
            return fail("circuit program does not match its identity")
    # 3c) v2: noise_model semantics must equal the normative block for the
    # embedded ppb values.
    noise_cfg = engine.NoiseConfig.from_dict(batch.get("noise") or {})
    if is_v2:
        nm = batch.get("noise_model")
        if not isinstance(nm, dict):
            return fail("v2 missing noise_model")
        if engine.canonical(nm) != engine.canonical(
                engine.noise_model_semantics(noise_cfg)):
            return fail("noise_model semantics mismatch")

    # 4) full replay
    noise = noise_cfg
    res.shots = len(shots_j)
    flips = 0
    total = 0
    syndromes = set()
    for k, embedded in enumerate(shots_j):
        ps = engine.derive_shot_prng_seed(seed, k)
        rec = engine.run_shot(circ, noise, ps)
        mine = engine.canonical(engine.shot_to_dict(rec))
        theirs = {key: val for key, val in embedded.items() if key != "index"}
        if mine != engine.canonical(theirs):
            return fail(f"replay mismatch at shot {k}")
        flips += sum(rec.detectors)
        total += len(rec.detectors)
        syn = "".join(str(b) for b in rec.detectors)
        if "1" in syn:
            syndromes.add(syn)
    res.detector_flips = flips
    res.detector_total = total
    res.distinct_syndromes = len(syndromes)

    # 5) proof block (outputs only)
    proof = batch.get("proof")
    if proof is not None:
        res.is_output = True
        if proof.get("replay") != "match":
            return fail("proof missing replay match")
        shots_arr = [engine.shot_to_dict(engine.run_shot(
            circ, noise, engine.derive_shot_prng_seed(seed, k)))
            for k in range(len(shots_j))]
        replay_hash = hashlib.sha256(
            engine.canonical(shots_arr).encode()).hexdigest()
        if proof.get("replay_sha256") != replay_hash:
            return fail("replay_sha256 mismatch")
        gate = proof.get("gate") or {}
        if gate.get("detector_flips") != flips or gate.get("detector_total") != total:
            return fail("gate statistics mismatch")
        res.ledger_seq = proof.get("ledger_seq")
        # ledger cross-check
        if ledger_path and os.path.exists(ledger_path) and res.ledger_seq is not None:
            found = False
            with open(ledger_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    entry = json.loads(line)
                    if entry.get("seq") == res.ledger_seq:
                        found = True
                        if entry.get("entry_hash") != proof.get("ledger_entry_hash"):
                            return fail("ledger entry hash mismatch")
                        if entry.get("share_hash") != share_hash_hex:
                            return fail("ledger share hash mismatch")
                        break
            if not found:
                return fail(f"ledger entry seq={res.ledger_seq} not found")

    res.ok = True
    res.reason = "verified" if not res.is_output else "verified (with proof)"
    return res


def verify_ledger(path: str) -> Tuple[bool, str, int]:
    """Full hash-chain verification of ledger.jsonl."""
    if not os.path.exists(path):
        return False, "no ledger file", 0
    prev = "0" * 64
    expect_seq = 1
    count = 0
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except Exception as e:  # noqa: BLE001
                return False, f"corrupt line {count + 1}: {e}", count
            if entry.get("seq") != expect_seq:
                return False, f"seq gap at {expect_seq}", count
            if entry.get("prev_hash") != prev:
                return False, f"prev_hash mismatch at seq {expect_seq}", count
            body = {k: v for k, v in entry.items() if k != "entry_hash"}
            recomputed = hashlib.sha256(engine.canonical(body).encode()).hexdigest()
            if recomputed != entry.get("entry_hash"):
                return False, f"entry_hash mismatch at seq {expect_seq}", count
            prev = entry["entry_hash"]
            expect_seq += 1
            count += 1
    return True, "chain intact", count
