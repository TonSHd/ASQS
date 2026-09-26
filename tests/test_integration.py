#!/usr/bin/env python3
"""ASQS end-to-end integration test.

Phases:
  0. Python engine mirror self-test (noiseless surface code determinism,
     PRNG determinism, target math, canonical JSON).
  1. Spawn asqsd (fixed difficulty, small circuit), connect a stratum v1
     test client, CPU-mine N real shares, submit them.
  2. Wait for the pipeline to promote batches to outputs/.
  3. Verify EVERY output with the independent Python auditor (cross-language
     bit-exact replay), verify the ledger chain, run analyze.

Run:  python3 tests/test_integration.py
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

from asqs import engine, verify  # noqa: E402

PASS = 0
FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [ok] {msg}")
    else:
        FAIL += 1
        print(f"  [FAIL] {msg}")


# ---------------------------------------------------------------- phase 0
def phase0():
    print("phase 0: python engine mirror self-test")
    cfg = {"type": "surface_code_memory", "d": 3, "rounds": 3}
    ok, err = engine.surface_code_selfcheck(3)
    check(ok, f"surface selfcheck d=3 {err}")
    ok, err = engine.surface_code_selfcheck(5)
    check(ok, f"surface selfcheck d=5 {err}")
    circ = engine.build_circuit(cfg)
    check(circ.num_qubits == 17, "d=3 -> 17 qubits")
    seen = set()
    for s in range(10):
        rec = engine.run_shot(circ, engine.NoiseConfig(0, 0, 0, 0), 1000 + s)
        check_zero = all(d == 0 for d in rec.detectors)
        check_obs = all(o == 0 for o in rec.observables)
        if not (check_zero and check_obs):
            check(False, f"noiseless determinism shot {s}")
            return False
        seen.add("".join(map(str, rec.outcomes)))
    check(len(seen) > 1, "first-round randomness present")
    # PRNG determinism
    a = engine.Prng(1234)
    b = engine.Prng(1234)
    seq_a = [a.next_u64() for _ in range(10)]
    seq_b = [b.next_u64() for _ in range(10)]
    check(seq_a == seq_b, "prng determinism")
    # target math
    t = engine.target_from_difficulty(1, 1)
    check(t == engine.MAX_TARGET, "D=1 target == max")
    ok, h = engine.verify_hashcash("00" * 80, "1")
    check(not ok, "all-zero header fails D=1")
    # canonical json
    check(engine.canonical({"b": [], "a": 5}) == '{"a":5,"b":[]}',
          "canonical sorted keys")
    # noise shot produces detector activity
    n_rec = [engine.run_shot(circ, engine.NoiseConfig(2000000, 5000000, 2000000, 2000000), s)
             for s in range(8)]
    flips = sum(sum(r.detectors) for r in n_rec)
    check(flips > 0, f"noise produces flips ({flips})")
    return True


# ---------------------------------------------------------------- client
class StratumClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""
        self.msg_id = 10

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def recv_msg(self, timeout=20.0):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def wait_for(self, method, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.recv_msg(timeout=deadline - time.time())
            if msg.get("method") == method:
                return msg
        raise TimeoutError(method)

    def request(self, method, params):
        self.msg_id += 1
        self.send({"id": self.msg_id, "method": method, "params": params})
        return self.wait_response(self.msg_id)

    def wait_response(self, want_id, timeout=30.0):
        """Read messages until the response with matching id arrives
        (server notifications may interleave)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.recv_msg(timeout=max(0.1, deadline - time.time()))
            if msg.get("id") == want_id:
                return msg
        raise TimeoutError(f"response id={want_id}")


def sha256d(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def bswap_per_word(b: bytes) -> bytes:
    """reverse_endianness_per_word (ESP-Miner utils.c:208): bswap32 within
    each 4-byte word. This is what the firmware inserts into the header for
    the wire prevhash (mining.c:78-80)."""
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def mine_share(job, ex1_hex, difficulty, worker, ex2_hex, vbits=0):
    """CPU-mine a nonce for the job; returns (submit_params, header_hex) or None.

    Builds the header exactly like real firmware (ESP-Miner v2.15.3 /
    Bitaxe BM1370, mining.c construct_bm_job + test_nonce_value):
      prevhash = bswap32-per-word(hex2bin(notify.prevhash))
      merkle   = sha256d(coinbase), inserted RAW
      version  = base | vbits   (BIP310 6th submit param, when rolling)
    """
    version = int(job["version"], 16)
    nbits = int(job["nbits"], 16)
    ntime = int(job["ntime"], 16)
    prev_field = bswap_per_word(bytes.fromhex(job["prevhash"]))
    cb = (bytes.fromhex(job["coinbase1"]) + bytes.fromhex(ex1_hex) +
          bytes.fromhex(ex2_hex) + bytes.fromhex(job["coinbase2"]))
    merkle = sha256d(cb)
    header = bytearray(80)
    header[0:4] = struct.pack("<I", version | vbits)
    header[4:36] = prev_field
    header[36:68] = merkle
    header[68:72] = struct.pack("<I", ntime)
    header[72:76] = struct.pack("<I", nbits)
    num, den = engine.parse_difficulty(str(difficulty))
    target = engine.target_from_difficulty(num, den)
    for nonce in range(0, 60_000_000):
        header[76:80] = struct.pack("<I", nonce)
        h = sha256d(bytes(header))
        if int.from_bytes(h, "little") <= target:
            submit = [worker, job["job_id"], ex2_hex,
                      job["ntime"], f"{nonce:08x}"]
            if vbits:
                submit.append(f"{vbits:08x}")  # BIP310 6th param
            return (["mining.submit", submit],
                    bytes(header).hex(), h.hex())
    return None


# ---------------------------------------------------------------- phase 1
def phase1(root, port, n_shares=3):
    print(f"phase 1: spawn asqsd on 127.0.0.1:{port}, mine {n_shares} shares")
    daemon = os.path.join(ROOT, "build", "asqsd")
    args = [daemon,
            "--ip", "127.0.0.1", "--port", str(port),
            "--project-root", root,
            "--difficulty-mode", "fixed", "--init-difficulty", "0.001",
            "--max-shares-per-sec", "50",
            "--shots-per-share", "4",
            "--validation-shots", "512", "--validation-every", "3",
            "--job-interval-s", "5",
            "--circuit", "surface_code_memory", "--d", "3", "--rounds", "3",
            "--noise", "5000000,10000000,5000000,5000000"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        cli = StratumClient("127.0.0.1", port)
        resp = cli.request("mining.subscribe", ["integration-test/1.0"])
        ex1 = resp["result"][1]
        check(len(ex1) == 8, f"subscribe extranonce1 = {ex1}")
        resp = cli.request("mining.authorize", ["pytest_worker", "x"])
        check(resp.get("result") is True, "authorize ok")
        diff_msg = cli.wait_for("mining.set_difficulty")
        difficulty = diff_msg["params"][0]
        job_msg = cli.wait_for("mining.notify")
        job = {
            "job_id": job_msg["params"][0],
            "prevhash": job_msg["params"][1],
            "coinbase1": job_msg["params"][2],
            "coinbase2": job_msg["params"][3],
            "version": job_msg["params"][5],
            "nbits": job_msg["params"][6],
            "ntime": job_msg["params"][7],
        }
        print(f"    job {job['job_id']} difficulty {difficulty}")
        accepted = 0
        # Last share exercises BIP310 version-rolling (6th submit param),
        # exactly like a real Bitaxe does on every share.
        for i in range(n_shares):
            ex2 = f"{i:08x}"
            vbits = 0x0002A000 if i == n_shares - 1 else 0
            found = None
            t0 = time.time()
            while time.time() - t0 < 120:
                found = mine_share(job, ex1, difficulty, "pytest_worker", ex2, vbits)
                if found:
                    break
            check(found is not None, f"share {i} mined")
            if not found:
                return None
            params, header_hex, hash_hex = found
            submit_id = 100 + i
            cli.send({"id": submit_id, "method": params[0], "params": params[1]})
            resp = cli.wait_response(submit_id)
            ok = resp.get("result") is True
            check(ok, f"share {i} accepted (hash {hash_hex[:16]}...)")
            if ok:
                accepted += 1
        check(accepted == n_shares, f"{accepted}/{n_shares} shares accepted")
        return proc
    except Exception as e:  # noqa: BLE001
        print(f"  [FAIL] phase1 error: {e}")
        proc.terminate()
        proc.wait()
        return None


def wait_for_outputs(root, want, timeout=90):
    outdir = os.path.join(root, "outputs")
    deadline = time.time() + timeout
    while time.time() < deadline:
        files = [f for f in os.listdir(outdir)
                 if f.startswith("asqs_out_")] if os.path.isdir(outdir) else []
        if len(files) >= want:
            return files
        time.sleep(1.0)
    return ([f for f in os.listdir(outdir) if f.startswith("asqs_out_")]
            if os.path.isdir(outdir) else [])


def phase2(root, want):
    print(f"phase 2: wait for {want} promoted outputs")
    files = wait_for_outputs(root, want)
    check(len(files) >= want, f"outputs promoted: {len(files)}")
    return files


def phase3(root, files):
    print("phase 3: independent verification (cross-language replay)")
    ledger = os.path.join(root, "ledger.jsonl")
    n_validation = 0
    for f in files:
        path = os.path.join(root, "outputs", f)
        r = verify.verify_batch(path, ledger)
        check(r.ok, f"verify {f}: {r.reason} "
                    f"(shots={r.shots} flips={r.detector_flips}/{r.detector_total})")
        with open(path, "r", encoding="utf-8") as fh:
            b = json.load(fh)
        check(b.get("schema") == "asqs.batch/2", f"{f} is schema v2")
        if b.get("validation_batch"):
            n_validation += 1
            check(b.get("shots_per_share") == 512 or len(b["shots"]) == 512,
                  f"{f} is a 512-shot validation batch")
    check(n_validation == 1, f"exactly one validation batch among 3 shares "
                            f"(every 3rd; got {n_validation})")
    ok, msg, count = verify.verify_ledger(ledger)
    check(ok, f"ledger chain: {msg} ({count} entries)")
    from asqs import analyze
    summary = analyze.analyze_outputs(os.path.join(root, "outputs"))
    check(summary["shots_total"] > 0,
          f"analyze: {summary['files']} files, {summary['shots_total']} shots")
    c = summary["circuits"][0] if summary["circuits"] else {}
    if c:
        print(f"    detector flip rate: {c['detector_flip_rate']:.6f} "
              f"({c['detector_flip_rate_ppm']} ppm), "
              f"syndromes: {c['distinct_syndromes']}")

    # phase 3b: independent-reference statistical validation (stim).
    # Skipped when stim is not importable (it is an optional dependency of
    # the audit tooling, not of the core spec).
    try:
        from asqs import reference
        if reference.stim is None:
            raise ImportError("stim not installed")
    except ImportError:
        print("phase 3b: SKIP crosscheck (stim not installed)")
        return
    print("phase 3b: crosscheck validation batch against stim")
    val_files = [f for f in files if _is_validation_batch(os.path.join(root, "outputs", f))]
    check(len(val_files) >= 1, "validation batch present for crosscheck")
    for f in val_files:
        path = os.path.join(root, "outputs", f)
        r = reference.crosscheck_file(path, reference_shots=2048,
                                       permutations=50)
        check(r.ok, f"crosscheck {f}: {r.reason} "
                    f"(perm_p={r.report['validation']['permutation_p_value']})")
        # the crosscheck must have written reference+validation blocks
        with open(path, "r", encoding="utf-8") as fh:
            b = json.load(fh)
        check("reference" in b and "validation" in b,
              f"{f} carries embedded reference+validation blocks")
        v = b["validation"]
        check(v["passed"] is True, f"{f} validation.passed")
        check(v["asqs_shots"] == 512 and v["reference_shots"] == 2048,
              f"{f} shot counts recorded")
        check(len(b["reference"]["samples_packed"]) == 2048,
              f"{f} embeds all reference samples")
        # re-verify with the deterministic re-verification path
        ok2, why2 = reference.verify_validation_block(b)
        check(ok2, f"re-verify validation block of {f}: {why2}")
    # negative control: tamper an embedded reference sample -> re-verify fails
    f = val_files[0]
    path = os.path.join(root, "outputs", f)
    with open(path, "r", encoding="utf-8") as fh:
        b = json.load(fh)
    b["reference"]["samples_packed"][0] = "00" * 5  # zeroed first sample
    ok3, why3 = reference.verify_validation_block(b)
    check(not ok3, f"tampered reference sample detected: {why3}")


def _is_validation_batch(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return bool(json.load(fh).get("validation_batch"))
    except Exception:  # noqa: BLE001
        return False


def main():
    if not phase0():
        print(f"\n{PASS} passed, {FAIL} failed")
        return 1
    # fresh project root
    root = os.path.join(ROOT, "build", "test_tmp", f"integration_{int(time.time())}")
    os.makedirs(root, exist_ok=True)
    # free port
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    proc = phase1(root, port)
    if not proc:
        print(f"\n{PASS} passed, {FAIL} failed")
        return 1
    try:
        files = phase2(root, 3)
        phase3(root, files)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print(f"\n{PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
