"""ASQS engine mirror - independent Python implementation of the PROOF SPEC.

This module is the THIRD-PARTY AUDITOR: it re-implements, from the spec alone,
the deterministic core of ASQS:

  * SplitMix64 + xoroshiro128** PRNG (bit-exact with the C++ daemon)
  * Stabilizer tableau engine (H/S/Sdg/X/Z/CNOT, Z-measurements with the
    unique-subset sign solver over full (x,z) vectors)
  * Circuit library: rotated surface code memory (d=3,5) and seeded random
    Clifford circuits - identical construction to the daemon
  * Shot runner with the normative RNG draw order + Pauli noise model
  * Seed chain: share_hash -> seed -> per-shot PRNG seeds
  * Bitcoin-style target math (exact rationals, pure Python ints)

Every verified output file in outputs/ can be re-derived from its embedded
share using ONLY this module + hashlib. If the daemon and this module agree
bit-for-bit, the output is proven: hashcash-anchored, replayable, untampered.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

MASK64 = (1 << 64) - 1

# --------------------------------------------------------------------------
# PRNG (PROOF SPEC: splitmix64 seeding + xoroshiro128** stream)
# --------------------------------------------------------------------------


def _rotl(x: int, k: int) -> int:
    return ((x << k) | (x >> (64 - k))) & MASK64


class Prng:
    def __init__(self, seed: int):
        seed &= MASK64
        self._sm = seed
        self._sm, s0 = self._splitmix(self._sm)
        self._sm, s1 = self._splitmix(self._sm)
        if s0 == 0 and s1 == 0:
            s0 = 0x9E3779B97F4A7C15
        self._s0, self._s1 = s0, s1

    @staticmethod
    def _splitmix(state: int) -> Tuple[int, int]:
        state = (state + 0x9E3779B97F4A7C15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return state, z ^ (z >> 31)

    def next_u64(self) -> int:
        s0, s1 = self._s0, self._s1
        result = (_rotl((s0 * 5) & MASK64, 7) * 9) & MASK64
        s1 ^= s0
        self._s0 = (_rotl(s0, 24) ^ s1 ^ ((s1 << 16) & MASK64)) & MASK64
        self._s1 = _rotl(s1, 37)
        return result

    def bit(self) -> int:
        return self.next_u64() & 1

    def rand_below(self, k: int) -> int:
        if k <= 1:
            return 0
        return (self.next_u64() >> 32) % k

    def rand_double(self) -> float:
        return (self.next_u64() >> 11) * (1.0 / 9007199254740992.0)

    def bernoulli(self, p: float) -> bool:
        return self.rand_double() < p


# --------------------------------------------------------------------------
# Tableau engine (mirrors cpp tableau.cpp exactly)
# --------------------------------------------------------------------------


@dataclass
class Row:
    x: int = 0
    z: int = 0
    r: int = 0


class Tableau:
    def __init__(self, n: int):
        self.n = n
        self.rows = [Row(0, 1 << i, 0) for i in range(n)]  # |0...0>

    def h(self, a: int) -> None:
        for row in self.rows:
            xa = (row.x >> a) & 1
            za = (row.z >> a) & 1
            row.r ^= xa & za
            row.x = (row.x & ~(1 << a)) | (za << a)
            row.z = (row.z & ~(1 << a)) | (xa << a)

    def s(self, a: int) -> None:
        for row in self.rows:
            xa = (row.x >> a) & 1
            za = (row.z >> a) & 1
            row.r ^= xa & za
            row.z ^= xa << a

    def sdg(self, a: int) -> None:
        for row in self.rows:
            xa = (row.x >> a) & 1
            za = (row.z >> a) & 1
            row.r ^= xa & (za ^ 1)
            row.z ^= xa << a

    def px(self, a: int) -> None:
        for row in self.rows:
            row.r ^= (row.z >> a) & 1

    def pz(self, a: int) -> None:
        for row in self.rows:
            row.r ^= (row.x >> a) & 1

    def cnot(self, c: int, t: int) -> None:
        for row in self.rows:
            xc = (row.x >> c) & 1
            zt = (row.z >> t) & 1
            row.x ^= xc << t
            row.z ^= zt << c

    def has_random_bit(self, a: int) -> bool:
        return any((row.x >> a) & 1 for row in self.rows)

    def mz(self, a: int, rng_bit: Callable[[], int]) -> int:
        p = -1
        for i, row in enumerate(self.rows):
            if (row.x >> a) & 1:
                p = i
                break
        if p >= 0:
            outcome = rng_bit()
            src = self.rows[p]
            for i, row in enumerate(self.rows):
                if i != p and (row.x >> a) & 1:
                    row.x ^= src.x
                    row.z ^= src.z
                    row.r ^= src.r
            self.rows[p] = Row(0, 1 << a, outcome)
            return outcome
        return self._solve_deterministic(a)

    def mr(self, a: int, rng_bit: Callable[[], int]) -> int:
        outcome = self.mz(a, rng_bit)
        if outcome:
            self.px(a)
        return outcome

    def reset(self, a: int, rng_bit: Callable[[], int]) -> None:
        outcome = self.mz(a, rng_bit)
        if outcome:
            self.px(a)

    def _solve_deterministic(self, a: int) -> int:
        """Unique subset of rows whose full (x,z) vectors XOR to (0, e_a).

        Two-phase: RREF over all 2n columns (one pivot per row), then
        eliminate the target against pivot rows. The subset and sign are
        unique, so pivot order does not affect the result.
        """
        n = self.n
        wx = [row.x for row in self.rows]
        wz = [row.z for row in self.rows]
        wr = [row.r for row in self.rows]

        def has_bit(i: int, col: int) -> bool:
            if col < n:
                return (wx[i] >> col) & 1 != 0
            return (wz[i] >> (col - n)) & 1 != 0

        pivot_row_of = [-1] * (2 * n)
        used = 0
        for c in range(2 * n):
            piv = -1
            for i in range(n):
                if not ((used >> i) & 1) and has_bit(i, c):
                    piv = i
                    break
            if piv < 0:
                continue
            used |= 1 << piv
            pivot_row_of[c] = piv
            for i in range(n):
                if i != piv and has_bit(i, c):
                    wx[i] ^= wx[piv]
                    wz[i] ^= wz[piv]
                    wr[i] ^= wr[piv]
        tx, tz, tr = 0, 1 << a, 0

        def t_has(col: int) -> bool:
            if col < n:
                return (tx >> col) & 1 != 0
            return (tz >> (col - n)) & 1 != 0

        for c in range(2 * n):
            if t_has(c) and pivot_row_of[c] >= 0:
                p = pivot_row_of[c]
                tx ^= wx[p]
                tz ^= wz[p]
                tr ^= wr[p]
        if tx != 0 or tz != 0:
            return 0  # not in group (defensive)
        return tr


# --------------------------------------------------------------------------
# Circuits (mirrors cpp circuit.cpp exactly - construction order matters)
# --------------------------------------------------------------------------

H, S, SDG, PX, PZ, CNOT, MR, MZ, RESET = range(9)


@dataclass
class Instr:
    gate: int
    a: int = 0
    b: int = 0
    meas_index: int = 0


@dataclass
class Detector:
    meas: List[int] = field(default_factory=list)
    name: str = ""


@dataclass
class Observable:
    meas: List[int] = field(default_factory=list)
    name: str = ""


@dataclass
class Circuit:
    num_qubits: int = 0
    num_measurements: int = 0
    instrs: List[Instr] = field(default_factory=list)
    detectors: List[Detector] = field(default_factory=list)
    observables: List[Observable] = field(default_factory=list)


@dataclass
class NoiseConfig:
    p1_ppb: int = 500000
    p2_ppb: int = 1000000
    pm_ppb: int = 500000
    pr_ppb: int = 500000

    @staticmethod
    def from_dict(d: dict) -> "NoiseConfig":
        return NoiseConfig(int(d.get("p1_ppb", 500000)), int(d.get("p2_ppb", 1000000)),
                           int(d.get("pm_ppb", 500000)), int(d.get("pr_ppb", 500000)))

    def to_dict(self) -> dict:
        return {"p1_ppb": self.p1_ppb, "p2_ppb": self.p2_ppb, "pm_ppb": self.pm_ppb,
                "pr_ppb": self.pr_ppb}

    def any(self) -> bool:
        return bool(self.p1_ppb or self.p2_ppb or self.pm_ppb or self.pr_ppb)


def canonical(obj) -> str:
    """Normative canonical JSON (sorted keys, no whitespace)."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def circuit_identity(cfg: dict) -> dict:
    out = {"type": cfg["type"]}
    if cfg["type"] == "surface_code_memory":
        out["d"] = int(cfg.get("d", 3))
        out["rounds"] = int(cfg.get("rounds", 3))
    else:
        out["qubits"] = int(cfg.get("qubits", 24))
        out["gates"] = int(cfg.get("gates", 120))
    return out


def circuit_sha256(cfg: dict) -> str:
    return hashlib.sha256(canonical(circuit_identity(cfg)).encode()).hexdigest()


# --------------------------------------------------------------------------
# Circuit program serialization (mirrors cpp circuit.cpp — PROOF_SPEC §13).
# The emitted dict must canonicalize IDENTICALLY to the C++ version.
# --------------------------------------------------------------------------

GATE_NAMES = ("H", "S", "SDG", "X", "Z", "CNOT", "MR", "MZ", "RESET")
_GNAME = {H: "H", S: "S", SDG: "SDG", PX: "X", PZ: "Z", CNOT: "CNOT",
          MR: "MR", MZ: "MZ", RESET: "RESET"}


def circuit_program(cfg: dict) -> dict:
    """Full instruction-level circuit program + provenance (PROOF_SPEC §13)."""
    circ = build_circuit(cfg)
    ops = []
    for ins in circ.instrs:
        g = _GNAME[ins.gate]
        if g == "CNOT":
            ops.append({"gate": g, "control": ins.a, "target": ins.b})
        elif g == "RESET":
            ops.append({"gate": g, "qubit": ins.a})
        elif g in ("MR", "MZ"):
            ops.append({"gate": g, "qubit": ins.a, "meas_index": ins.meas_index})
        else:
            ops.append({"gate": g, "qubit": ins.a})
    prog = {
        "encoding": "asqs.program/1",
        "num_qubits": circ.num_qubits,
        "num_measurements": circ.num_measurements,
        "operations": ops,
        "detectors": [{"name": d.name, "meas": list(d.meas)} for d in circ.detectors],
        "observables": [{"name": o.name, "meas": list(o.meas)} for o in circ.observables],
    }
    prov = {
        "generator": "asqs-circuit/1",
        "circuit_sha256": circuit_sha256(cfg),
    }
    if cfg["type"] == "random_clifford":
        dom = "asqs-circuit|" + canonical(circuit_identity(cfg))
        seed = int.from_bytes(hashlib.sha256(dom.encode()).digest()[:8], "big")
        prov["construction"] = ("PRNG-driven generation from the circuit identity "
                                "(PROOF_SPEC §8.2)")
        prov["seed_derivation"] = ('seed = BE64(SHA256("asqs-circuit|" || '
                                   'canonical(circuit identity))[0..8])')
        prov["seed_hex"] = f"{seed:016x}"
        prov["prng"] = ("splitmix64 (2 draws) seeding + xoroshiro128** stream "
                        "(PROOF_SPEC §3)")
        prov["domain"] = dom
        prov["gate_distribution"] = (
            "per gate: kind = rand_below(100); [0,30) H, [30,45) S, [45,60) Sdg, "
            "[60,65) X, [65,70) Z, [70,100) CNOT(a,b) with b != a drawn via "
            "rand_below(Q-1) + (t>=a)")
    else:
        prov["construction"] = (
            "normative rotated-surface-code memory experiment from (d, rounds) "
            "(PROOF_SPEC §8.1); self-validating: X/Z commutation, generator "
            "independence, logical independence (GF(2))")
        prov["seed_derivation"] = (
            "deterministic construction from (d, rounds); no PRNG")
    prog["provenance"] = prov
    return prog


def program_sha256(cfg: dict) -> str:
    return hashlib.sha256(canonical(circuit_program(cfg)).encode()).hexdigest()


def program_sha256_of(prog: dict) -> str:
    """Hash of an already-built program dict (used by the verifier)."""
    return hashlib.sha256(canonical(prog).encode()).hexdigest()


# --------------------------------------------------------------------------
# Noise-model semantics block (mirrors cpp circuit.cpp — PROOF_SPEC §14).
# The ppb values PLUS the exact application rules, machine-readable.
# --------------------------------------------------------------------------


def noise_model_semantics(noise: NoiseConfig) -> dict:
    return {
        "p1_ppb": noise.p1_ppb, "p2_ppb": noise.p2_ppb,
        "pm_ppb": noise.pm_ppb, "pr_ppb": noise.pr_ppb,
        "probability_semantics": (
            "p = ppb * 1e-9 (exact rational). Each bernoulli draw consumes one "
            "u64: (next_u64 >> 11) * 2^-53 < p"),
        "single_qubit_gates": {
            "applies_to": ["H", "S", "SDG", "X", "Z"],
            "after_gate": True,
            "channel": ("with probability p1, a uniformly random Pauli X/Y/Z "
                        "(each p1/3) is applied to the gate's qubit after the gate"),
        },
        "two_qubit_gates": {
            "applies_to": ["CNOT"],
            "after_gate": True,
            "qubit_order": "control first, then target",
            "channel": ("for each of (control, target) in order, independently "
                        "with probability p2, a uniformly random Pauli X/Y/Z "
                        "(each p2/3) is applied to that qubit after the gate"),
        },
        "measurement": {
            "applies_to": ["MR", "MZ"],
            "after_measurement": True,
            "channel": ("with probability pm, the RECORDED outcome bit is "
                        "flipped (classical bit flip; the post-measurement "
                        "quantum state is unchanged). Equivalent to an "
                        "X_ERROR(pm) immediately before the measurement for "
                        "terminal MZ and for MR in general"),
        },
        "reset": {
            "applies_to": ["RESET"],
            "after_reset": True,
            "channel": ("with probability pr, X is applied to the qubit after "
                        "the reset to |0> (state becomes |1>)"),
        },
        "y_convention": "Y is applied as X then Z",
        "measurement_rng": ("one u64 bit is consumed iff the tableau measurement "
                            "branch is random; the deterministic branch consumes "
                            "none (PROOF_SPEC §7)"),
        "application_order": ("instructions in program order; within an "
                              "instruction: gate first, then the noise draws in "
                              "the stated order"),
    }


def _surface_checks(d: int):
    """Returns (xchecks, zchecks, logical_row0); lists of data-qubit lists."""
    xchecks, zchecks = [], []

    def data_id(r, c):
        return r * d + c

    for a in range(d - 1):
        for b in range(d - 1):
            ck = [data_id(a, b), data_id(a + 1, b), data_id(a, b + 1), data_id(a + 1, b + 1)]
            (xchecks if (a + b) % 2 == 0 else zchecks).append(ck)
    for c in range(d - 1):
        if c % 2 == 1:
            xchecks.append([data_id(0, c), data_id(0, c + 1)])
        if c % 2 == 0:
            xchecks.append([data_id(d - 1, c), data_id(d - 1, c + 1)])
    for r in range(d - 1):
        if r % 2 == 0:
            zchecks.append([data_id(r, 0), data_id(r + 1, 0)])
        if r % 2 == 1:
            zchecks.append([data_id(r, d - 1), data_id(r + 1, d - 1)])
    logical_row0 = [data_id(0, c) for c in range(d)]
    return xchecks, zchecks, logical_row0


def surface_code_selfcheck(d: int) -> Tuple[bool, str]:
    if d not in (3, 5):
        return False, "d must be 3 or 5"
    xc, zc, logical = _surface_checks(d)
    want = (d * d - 1) // 2
    if len(xc) != want or len(zc) != want:
        return False, "check count mismatch"
    for x in xc:
        for z in zc:
            if len(set(x) & set(z)) % 2 != 0:
                return False, "X/Z pair anticommutes"
    # GF(2) rank helper on bitmasks
    def rank(rows):
        rows = list(rows)
        r = 0
        for bit in range(d * d - 1, -1, -1):
            piv = None
            for i in range(r, len(rows)):
                if (rows[i] >> bit) & 1:
                    piv = i
                    break
            if piv is None:
                continue
            rows[r], rows[piv] = rows[piv], rows[r]
            for i in range(len(rows)):
                if i != r and (rows[i] >> bit) & 1:
                    rows[i] ^= rows[r]
            r += 1
        return r

    def mask(qs):
        m = 0
        for q in qs:
            m |= 1 << q
        return m

    xm = [mask(c) for c in xc]
    zm = [mask(c) for c in zc]
    if rank(xm) != len(xm):
        return False, "X checks dependent"
    if rank(zm) != len(zm):
        return False, "Z checks dependent"
    lz = mask(logical)
    for x in xm:
        if bin(x & lz).count("1") % 2 != 0:
            return False, "logical Z anticommutes with an X check"
    if rank(zm + [lz]) == rank(zm):
        return False, "logical Z is in the Z-check span"
    return True, ""


def build_circuit(cfg: dict) -> Circuit:
    ctype = cfg["type"]
    if ctype == "surface_code_memory":
        d = int(cfg.get("d", 3))
        rounds = int(cfg.get("rounds", 3))
        ok, err = surface_code_selfcheck(d)
        if not ok:
            raise ValueError(f"surface code self-check failed: {err}")
        xc, zc, logical = _surface_checks(d)
        ndata, nx, nz = d * d, len(xc), len(zc)
        circ = Circuit()
        circ.num_qubits = ndata + nx + nz
        x_anc = lambda i: ndata + i
        z_anc = lambda i: ndata + nx + i
        anc_meas = [[] for _ in range(nx + nz)]
        data_meas = [0] * ndata
        midx = 0
        instrs = circ.instrs

        for rnd in range(1, rounds + 1):
            for i in range(nx):
                a = x_anc(i)
                instrs.append(Instr(H, a=a, meas_index=0))
                for t in sorted(xc[i]):
                    instrs.append(Instr(CNOT, a=a, b=t, meas_index=0))
                instrs.append(Instr(H, a=a, meas_index=0))
                instrs.append(Instr(MR, a=a, meas_index=midx))
                midx += 1
                anc_meas[i].append(midx - 1)
            for i in range(nz):
                a = z_anc(i)
                for t in sorted(zc[i]):
                    instrs.append(Instr(CNOT, a=t, b=a, meas_index=0))
                instrs.append(Instr(MR, a=a, meas_index=midx))
                midx += 1
                anc_meas[nx + i].append(midx - 1)
        for q in range(ndata):
            instrs.append(Instr(MZ, a=q, meas_index=midx))
            midx += 1
            data_meas[q] = midx - 1
        circ.num_measurements = midx

        for i in range(nx):
            m = anc_meas[i]
            for r in range(1, len(m)):
                circ.detectors.append(Detector([m[r - 1], m[r]], f"X{i}r{r + 1}"))
        for i in range(nz):
            m = anc_meas[nx + i]
            if m:
                circ.detectors.append(Detector([m[0]], f"Z{i}r1"))
            for r in range(1, len(m)):
                circ.detectors.append(Detector([m[r - 1], m[r]], f"Z{i}r{r + 1}"))
            if m:
                det = [m[-1]] + [data_meas[q] for q in zc[i]]
                circ.detectors.append(Detector(det, f"Z{i}final"))
        circ.observables.append(
            Observable([data_meas[q] for q in logical], "ZL_row0"))
        return circ

    if ctype == "random_clifford":
        qubits = int(cfg.get("qubits", 24))
        gates = int(cfg.get("gates", 120))
        dom = "asqs-circuit|" + canonical(circuit_identity(cfg))
        seed_bytes = hashlib.sha256(dom.encode()).digest()
        seed = int.from_bytes(seed_bytes[:8], "big")
        prng = Prng(seed)
        circ = Circuit()
        circ.num_qubits = qubits
        midx = 0
        for _ in range(gates):
            kind = prng.rand_below(100)
            if kind < 30:
                instrs_g, a = H, prng.rand_below(qubits)
                circ.instrs.append(Instr(instrs_g, a=a))
            elif kind < 45:
                circ.instrs.append(Instr(S, a=prng.rand_below(qubits)))
            elif kind < 60:
                circ.instrs.append(Instr(SDG, a=prng.rand_below(qubits)))
            elif kind < 65:
                circ.instrs.append(Instr(PX, a=prng.rand_below(qubits)))
            elif kind < 70:
                circ.instrs.append(Instr(PZ, a=prng.rand_below(qubits)))
            else:
                a = prng.rand_below(qubits)
                t = prng.rand_below(qubits - 1)
                if t >= a:
                    t += 1
                circ.instrs.append(Instr(CNOT, a=a, b=t))
        for q in range(qubits):
            circ.instrs.append(Instr(MZ, a=q, meas_index=midx))
            midx += 1
        circ.num_measurements = midx
        circ.observables.append(Observable(list(range(midx)), "parity_all"))
        return circ

    raise ValueError(f"unknown circuit type: {ctype}")


# --------------------------------------------------------------------------
# Shot runner (normative RNG draw order - mirrors cpp shot.cpp)
# --------------------------------------------------------------------------


@dataclass
class ShotRecord:
    outcomes: List[int]
    detectors: List[int]
    observables: List[int]
    err_instr: List[int] = field(default_factory=list)
    err_qubit: List[int] = field(default_factory=list)
    err_pauli: List[int] = field(default_factory=list)


def derive_seed(share_hash: bytes, header80: bytes) -> bytes:
    return hashlib.sha256(share_hash + header80).digest()


def derive_shot_prng_seed(seed: bytes, k: int) -> int:
    h = hashlib.sha256(seed + k.to_bytes(4, "little")).digest()
    return int.from_bytes(h[:8], "little")


def _apply_pauli(tab: Tableau, q: int, p: int) -> None:
    if p == 0:
        tab.px(q)
    elif p == 1:
        tab.px(q)
        tab.pz(q)
    else:
        tab.pz(q)


def compute_detectors_observables(circ: Circuit, outcomes: List[int]):
    dets = []
    for d in circ.detectors:
        par = 0
        for m in d.meas:
            par ^= outcomes[m] & 1
        dets.append(par)
    obs = []
    for o in circ.observables:
        par = 0
        for m in o.meas:
            par ^= outcomes[m] & 1
        obs.append(par)
    return dets, obs


def run_shot(circ: Circuit, noise: NoiseConfig, prng_seed: int) -> ShotRecord:
    prng = Prng(prng_seed)
    tab = Tableau(circ.num_qubits)
    rng_bit = prng.bit
    p1 = noise.p1_ppb * 1e-9
    p2 = noise.p2_ppb * 1e-9
    pm = noise.pm_ppb * 1e-9
    pr = noise.pr_ppb * 1e-9
    rec = ShotRecord(outcomes=[0] * circ.num_measurements, detectors=[], observables=[])
    for i, ins in enumerate(circ.instrs):
        g = ins.gate
        if g in (H, S, SDG, PX, PZ):
            if g == H:
                tab.h(ins.a)
            elif g == S:
                tab.s(ins.a)
            elif g == SDG:
                tab.sdg(ins.a)
            elif g == PX:
                tab.px(ins.a)
            else:
                tab.pz(ins.a)
            if prng.bernoulli(p1):
                p = prng.rand_below(3)
                _apply_pauli(tab, ins.a, p)
                rec.err_instr.append(i)
                rec.err_qubit.append(ins.a)
                rec.err_pauli.append(p)
        elif g == CNOT:
            tab.cnot(ins.a, ins.b)
            for q in (ins.a, ins.b):
                if prng.bernoulli(p2):
                    p = prng.rand_below(3)
                    _apply_pauli(tab, q, p)
                    rec.err_instr.append(i)
                    rec.err_qubit.append(q)
                    rec.err_pauli.append(p)
        elif g in (MR, MZ):
            out = tab.mr(ins.a, rng_bit) if g == MR else tab.mz(ins.a, rng_bit)
            if prng.bernoulli(pm):
                out ^= 1
                rec.err_instr.append(i)
                rec.err_qubit.append(ins.a)
                rec.err_pauli.append(3)
            rec.outcomes[ins.meas_index] = out
        elif g == RESET:
            tab.reset(ins.a, rng_bit)
            if prng.bernoulli(pr):
                tab.px(ins.a)
                rec.err_instr.append(i)
                rec.err_qubit.append(ins.a)
                rec.err_pauli.append(0)
    rec.detectors, rec.observables = compute_detectors_observables(circ, rec.outcomes)
    return rec


def shot_to_dict(rec: ShotRecord) -> dict:
    return {
        "outcomes": list(rec.outcomes),
        "detectors": list(rec.detectors),
        "observables": list(rec.observables),
        "errors": [
            {"i": rec.err_instr[e], "q": rec.err_qubit[e], "p": rec.err_pauli[e]}
            for e in range(len(rec.err_instr))
        ],
    }


# --------------------------------------------------------------------------
# Target math (exact rationals, pure Python ints)
# --------------------------------------------------------------------------

MAX_TARGET = 0x00000000FFFF0000000000000000000000000000000000000000000000000000
SATURATE = (1 << 256) - 1


def parse_difficulty(s: str) -> Tuple[int, int]:
    """Exact rational num/den from a decimal string (<= 9 fraction digits)."""
    s = s.strip()
    if not s:
        return 256, 1
    if "." in s:
        ip, fp = s.split(".", 1)
    else:
        ip, fp = s, ""
    fp = fp[:9]
    if ip and not ip.isdigit():
        return 256, 1
    if fp and not fp.isdigit():
        return 256, 1
    numstr = (ip + fp) or "0"
    numstr = numstr.lstrip("0") or "0"
    num = int(numstr)
    if num == 0:
        return 0, 1
    den = 10 ** len(fp)
    return num, den


def diff_to_string(num: int, den: int) -> str:
    if num == 0:
        return "0"
    if den == 1:
        return str(num)
    ip = str(num // den)
    rem = num % den
    if rem == 0:
        return ip
    fp = ""
    while rem and len(fp) < 18:
        rem *= 10
        fp += str(rem // den)
        rem %= den
    fp = fp.rstrip("0")
    return ip + ("." + fp if fp else "")


def target_from_difficulty(num: int, den: int) -> int:
    if num == 0 or den == 0:
        return SATURATE
    if den >= (1 << 32):
        return SATURATE
    return min(MAX_TARGET * den // num, SATURATE)


def verify_hashcash(header_hex: str, difficulty_str: str) -> Tuple[bool, bytes]:
    header = bytes.fromhex(header_hex)
    if len(header) != 80:
        return False, b""
    share_hash = hashlib.sha256(hashlib.sha256(header).digest()).digest()
    num, den = parse_difficulty(difficulty_str)
    target = target_from_difficulty(num, den)
    le_val = int.from_bytes(share_hash, "little")
    return le_val <= target, share_hash
