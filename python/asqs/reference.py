"""ASQS reference crosscheck - independent simulator validation via stim.

                    ┌── ASQS pipeline (hashcash-seeded tableau engine)
same circuit ───────┤
                    └── trusted reference simulator (stim, quantumlib/Stim)

Then compare the two sample sets statistically and write `reference` +
`validation` blocks into the batch record (PROOF_SPEC §15).

Noise-model conversion (ASQS semantics -> stim), normative:
  * H/S/S_DAG/X/Z       -> gate, then DEPOLARIZE1(p1) on the qubit
                            (ASQS: with prob p1 a uniform X/Y/Z -> identical
                            channel: each Pauli with p1/3)
  * CNOT                -> CX, then DEPOLARIZE1(p2) on control and
                            DEPOLARIZE1(p2) on target (independent per qubit)
  * MR                  -> X_ERROR(pm), then MR.
                            ASQS flips the recorded bit; an X immediately
                            before MR flips the same bit AND leaves the
                            post-measurement state at |0> either way - the
                            channels are exactly equivalent.
  * MZ (terminal)       -> X_ERROR(pm), then M.
                            Equivalent to a recorded-outcome flip ONLY when
                            the MZ is terminal (post-measurement state is then
                            never used). The converter ASSERTS terminality.
  * RESET               -> R, then X_ERROR(pr) (state |1> with prob pr)

Statistics (stdlib only - no scipy dependency):
  * primary: two-sample permutation test on total-variation distance of the
    JOINT outcome distribution (assumption-free, handles bit correlations
    and sparse support)
  * secondary: per-bit two-proportion chi-square (Yates), Bonferroni-corrected
  * surface code: per-detector flip-rate comparison + syndrome-weight
    chi-square + observable (logical) flip rates
  * conversion faithfulness: noiseless structural cross-checks between the
    ASQS engine and stim (independent physics agreement)

Everything deterministic in the resulting report (per-bit counts, chi-square
values, TVD) is recomputed by `asqs verify`; the reference SAMPLES are
embedded in the record so any third party can redo every statistic from the
file alone.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import random
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from . import engine

try:
    import stim  # type: ignore
except ImportError:  # pragma: no cover
    stim = None


# ---------------------------------------------------------------------------
# Pure-stdlib statistics (chi-square survival via regularized incomplete gamma)
# ---------------------------------------------------------------------------

def _gser(a: float, x: float) -> float:
    """Regularized lower incomplete gamma P(a, x) - series (x < a+1)."""
    ap = a
    s = 1.0 / a
    d = s
    for n in range(1, 1000):
        ap += 1.0
        d *= x / ap
        s += d
        if abs(d) < abs(s) * 1e-15:
            break
    return s * math.exp(-x + a * math.log(x) - math.lgamma(a))


def _gcf(a: float, x: float) -> float:
    """Regularized upper incomplete gamma Q(a, x) - continued fraction."""
    tiny = 1e-300
    b = x + 1.0 - a
    c = 1.0 / tiny
    d = 1.0 / b
    h = d
    for i in range(1, 1000):
        an = -i * (i - a)
        b += 2.0
        d = an * d + b
        if abs(d) < tiny:
            d = tiny
        c = b + an / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        de = d * c
        h *= de
        if abs(de - 1.0) < 1e-15:
            break
    return math.exp(-x + a * math.log(x) - math.lgamma(a)) * h


def gammaincc(a: float, x: float) -> float:
    if x < 0 or a <= 0:
        raise ValueError("bad args")
    if x == 0:
        return 1.0
    if x < a + 1.0:
        return 1.0 - _gser(a, x)
    return _gcf(a, x)


def chi2_sf(x: float, df: int) -> float:
    """Survival function of the chi-square distribution (df >= 1)."""
    if x <= 0:
        return 1.0
    return gammaincc(df / 2.0, x / 2.0)


def two_proportion_chi2(x1: int, n1: int, x2: int, n2: int) -> Optional[float]:
    """Pooled two-proportion chi-square with Yates correction (df=1).

    Returns None when the table is degenerate (pooled rate 0 or 1).
    """
    if n1 <= 0 or n2 <= 0:
        return None
    p = (x1 + x2) / (n1 + n2)
    if p <= 0.0 or p >= 1.0:
        return None
    cells = [(x1, p * n1), (n1 - x1, (1 - p) * n1),
             (x2, p * n2), (n2 - x2, (1 - p) * n2)]
    chi2 = 0.0
    for obs, exp in cells:
        d = abs(obs - exp) - 0.5
        if d > 0:
            chi2 += d * d / exp
    return chi2


# ---------------------------------------------------------------------------
# Bit packing for embedded reference samples
# ---------------------------------------------------------------------------

def pack_bits(bits: List[int]) -> str:
    """Pack a bit vector into hex (bit b -> byte b>>3, position 7-(b&7))."""
    out = bytearray((len(bits) + 7) // 8)
    for b, v in enumerate(bits):
        if v:
            out[b >> 3] |= 1 << (7 - (b & 7))
    return out.hex()


def unpack_bits(hexstr: str, n: int) -> List[int]:
    raw = bytes.fromhex(hexstr)
    if len(raw) != (n + 7) // 8:
        raise ValueError("bad packed length")
    return [(raw[b >> 3] >> (7 - (b & 7))) & 1 for b in range(n)]


# ---------------------------------------------------------------------------
# ASQS circuit -> stim circuit conversion (normative, see module docstring)
# ---------------------------------------------------------------------------

def to_stim(circ: engine.Circuit, noise: engine.NoiseConfig) -> "stim.Circuit":
    if stim is None:
        raise RuntimeError("stim is not installed (pip install stim)")
    c = stim.Circuit()
    p1 = noise.p1_ppb * 1e-9
    p2 = noise.p2_ppb * 1e-9
    pm = noise.pm_ppb * 1e-9
    pr = noise.pr_ppb * 1e-9

    # Terminality assertion for MZ (outcome-flip equivalence requirement).
    first_mz = None
    for pos, ins in enumerate(circ.instrs):
        if ins.gate == engine.MZ:
            first_mz = pos
            break
    if first_mz is not None:
        for ins in circ.instrs[first_mz:]:
            if ins.gate not in (engine.MZ,):
                raise ValueError(
                    "non-terminal MZ: X_ERROR-before-measure is not equivalent "
                    "to an ASQS outcome flip when the post-measurement state "
                    "is used")

    for ins in circ.instrs:
        g = ins.gate
        if g in (engine.H, engine.S, engine.SDG, engine.PX, engine.PZ):
            name = {engine.H: "H", engine.S: "S", engine.SDG: "S_DAG",
                    engine.PX: "X", engine.PZ: "Z"}[g]
            c.append(name, [ins.a])
            if p1 > 0:
                c.append("DEPOLARIZE1", [ins.a], p1)
        elif g == engine.CNOT:
            c.append("CX", [ins.a, ins.b])
            if p2 > 0:
                c.append("DEPOLARIZE1", [ins.a], p2)
                c.append("DEPOLARIZE1", [ins.b], p2)
        elif g == engine.MR:
            if pm > 0:
                c.append("X_ERROR", [ins.a], pm)
            c.append("MR", [ins.a])
        elif g == engine.MZ:
            if pm > 0:
                c.append("X_ERROR", [ins.a], pm)
            c.append("M", [ins.a])
        elif g == engine.RESET:
            c.append("R", [ins.a])
            if pr > 0:
                c.append("X_ERROR", [ins.a], pr)
        else:
            raise ValueError(f"unconvertible gate {g}")

    M = circ.num_measurements
    for det in circ.detectors:
        c.append("DETECTOR", [stim.target_rec(m - M) for m in det.meas])
    for ob in circ.observables:
        c.append("OBSERVABLE_INCLUDE",
                 [stim.target_rec(m - M) for m in ob.meas], 0)
    return c


def sample_stim(scirc: "stim.Circuit", shots: int,
                seed: Optional[int] = None) -> List[List[int]]:
    """Sample `shots` measurement records from a stim circuit.

    Uses stim's own sampler RNG (independent of the ASQS PRNG chain).
    """
    if seed is None:
        sampler = scirc.compile_sampler()
    else:
        sampler = scirc.compile_sampler(seed=seed)
    res = sampler.sample(shots=shots)
    return [[int(v) for v in row] for row in res]


# ---------------------------------------------------------------------------
# Conversion faithfulness: noiseless structural cross-checks
# ---------------------------------------------------------------------------

def conversion_checks(circ: engine.Circuit, noise: engine.NoiseConfig,
                      shots: int = 256) -> dict:
    """Independent physics agreement between the ASQS engine and stim.

    With noise disabled, both engines must agree that:
      - every DETECTOR is deterministically 0 (surface code memory), and
      - every measurement bit is either deterministic (fixed 0/1 value) or
        exactly uniform; the classification and the deterministic VALUES
        must match between the engines.
    """
    zero = engine.NoiseConfig(0, 0, 0, 0)

    # ASQS side
    asqs_bits: List[List[int]] = []
    asqs_dets: List[List[int]] = []
    for s in range(shots):
        rec = engine.run_shot(circ, zero, 0xA5A5100 + s)
        asqs_bits.append(rec.outcomes)
        asqs_dets.append(rec.detectors)
    asqs_det_clean = all(all(d == 0 for d in ds) for ds in asqs_dets)

    # stim side
    scirc = to_stim(circ, zero)
    ref_bits = sample_stim(scirc, shots)
    # stim detector values are available via the circuit's detector
    # structure; sample() returns only measurement bits, so recompute
    # detector parities from the bits with the same parity rule.
    ref_dets = []
    for bits in ref_bits:
        ds = []
        for det in circ.detectors:
            p = 0
            for m in det.meas:
                p ^= bits[m] & 1
            ds.append(p)
        ref_dets.append(ds)
    ref_det_clean = all(all(d == 0 for d in ds) for ds in ref_dets)

    # Per-bit classification agreement (deterministic value vs uniform)
    nbits = circ.num_measurements
    mismatches = []
    for b in range(nbits):
        a_vals = {row[b] for row in asqs_bits}
        r_vals = {row[b] for row in ref_bits}
        a_det = len(a_vals) == 1
        r_det = len(r_vals) == 1
        if a_det != r_det or (a_det and a_vals != r_vals):
            mismatches.append(b)

    return {
        "shots": shots,
        "asqs_noiseless_detectors_all_zero": asqs_det_clean,
        "stim_noiseless_detectors_all_zero": ref_det_clean,
        "noiseless_bit_classification_mismatches": mismatches,
        "passed": asqs_det_clean and ref_det_clean and not mismatches,
    }


# ---------------------------------------------------------------------------
# Distribution comparison
# ---------------------------------------------------------------------------

@dataclass
class BitStats:
    bit: int
    asqs_ones: int
    ref_ones: int
    chi2: Optional[float]
    p_value: Optional[float]
    abs_rate_diff: float


def _tvd(keys: List[str], c_a: Dict[str, int], n_a: int,
         c_b: Dict[str, int], n_b: int) -> float:
    tot = 0.0
    for k in keys:
        tot += abs(c_a.get(k, 0) / n_a - c_b.get(k, 0) / n_b)
    return tot


def _perm_p_value(sample_a: List[str], sample_b: List[str],
                  permutations: int, rng: random.Random) -> Tuple[float, float, float, float]:
    """Two-sample permutation test on TVD of the joint distribution.

    Returns (tvd_observed, p_value, null_mean, null_max).
    """
    n_a, n_b = len(sample_a), len(sample_b)
    c_a = {}
    for s in sample_a:
        c_a[s] = c_a.get(s, 0) + 1
    c_b = {}
    for s in sample_b:
        c_b[s] = c_b.get(s, 0) + 1
    keys = sorted(set(c_a) | set(c_b))
    tvd_obs = _tvd(keys, c_a, n_a, c_b, n_b)

    pooled = sample_a + sample_b
    ge = 0
    nulls = []
    for _ in range(permutations):
        rng.shuffle(pooled)
        sa, sb = pooled[:n_a], pooled[n_a:]
        ca = {}
        for s in sa:
            ca[s] = ca.get(s, 0) + 1
        cb = {}
        for s in sb:
            cb[s] = cb.get(s, 0) + 1
        t = _tvd(keys, ca, n_a, cb, n_b)
        nulls.append(t)
        if t >= tvd_obs:
            ge += 1
    p = (ge + 1) / (permutations + 1)  # add-one (valid, conservative)
    null_mean = sum(nulls) / len(nulls) if nulls else tvd_obs
    return tvd_obs, p, null_mean, (max(nulls) if nulls else tvd_obs)


def compare_samples(asqs_bits: List[List[int]], ref_bits: List[List[int]],
                     circ: engine.Circuit, alpha: float = 0.01,
                     permutations: int = 200) -> dict:
    """Full statistical comparison of two sample sets (PROOF_SPEC §15).

    alpha defaults to 0.01: the overall verdict requires the permutation
    test AND the per-bit Bonferroni battery AND the per-detector battery to
    pass, so the null-true false-alarm rate is a few percent at alpha=0.05.
    A FAIL verdict should be re-examined (more reference shots, more
    permutations) before conclusions are drawn - it is an "investigate"
    signal, not a fraud verdict.
    """
    n_asqs = len(asqs_bits)
    n_ref = len(ref_bits)
    nbits = circ.num_measurements

    # --- primary: permutation test on TVD of the joint distribution ---
    asqs_strs = ["".join(map(str, row)) for row in asqs_bits]
    ref_strs = ["".join(map(str, row)) for row in ref_bits]
    rng = random.Random(0xC0FFEE)  # fixed for determinism of the report
    tvd_obs, perm_p, null_mean, null_max = _perm_p_value(
        asqs_strs, ref_strs, permutations, rng)

    # --- secondary: per-bit two-proportion chi-square, Bonferroni ---
    bit_stats: List[BitStats] = []
    for b in range(nbits):
        a1 = sum(row[b] for row in asqs_bits)
        r1 = sum(row[b] for row in ref_bits)
        chi2 = two_proportion_chi2(a1, n_asqs, r1, n_ref)
        p = chi2_sf(chi2, 1) if chi2 is not None else None
        bit_stats.append(BitStats(
            bit=b, asqs_ones=a1, ref_ones=r1, chi2=chi2, p_value=p,
            abs_rate_diff=abs(a1 / n_asqs - r1 / n_ref)))
    finite_ps = [s.p_value for s in bit_stats if s.p_value is not None]
    min_p = min(finite_ps) if finite_ps else None
    bonferroni_alpha = alpha / max(1, len(finite_ps))
    per_bit_passed = all(
        (s.p_value is None) or (s.p_value > bonferroni_alpha) for s in bit_stats)

    # --- detectors (surface code) ---
    det_block = None
    if circ.detectors:
        a_flips = [0] * len(circ.detectors)
        r_flips = [0] * len(circ.detectors)
        for row in asqs_bits:
            for di, det in enumerate(circ.detectors):
                p = 0
                for m in det.meas:
                    p ^= row[m] & 1
                a_flips[di] += p
        for row in ref_bits:
            for di, det in enumerate(circ.detectors):
                p = 0
                for m in det.meas:
                    p ^= row[m] & 1
                r_flips[di] += p
        det_chi2 = []
        det_ps = []
        for di in range(len(circ.detectors)):
            chi2 = two_proportion_chi2(a_flips[di], n_asqs, r_flips[di], n_ref)
            det_chi2.append(chi2)
            det_ps.append(chi2_sf(chi2, 1) if chi2 is not None else None)
        finite = [p for p in det_ps if p is not None]
        det_min_p = min(finite) if finite else None
        det_bonf = alpha / max(1, len(finite))
        det_block = {
            "count": len(circ.detectors),
            "asqs_flip_counts": a_flips,
            "ref_flip_counts": r_flips,
            "asqs_rate": sum(a_flips) / (n_asqs * len(circ.detectors)),
            "ref_rate": sum(r_flips) / (n_ref * len(circ.detectors)),
            "max_abs_rate_diff": max(
                abs(a_flips[i] / n_asqs - r_flips[i] / n_ref)
                for i in range(len(circ.detectors))),
            "min_p_value": det_min_p,
            "bonferroni_alpha": det_bonf,
            "passed": all((p is None) or (p > det_bonf) for p in det_ps),
        }

    # --- syndrome-weight chi-square (surface code) ---
    syn_block = None
    if circ.detectors:
        def weights(bits_rows):
            out = {}
            for row in bits_rows:
                w = 0
                for det in circ.detectors:
                    p = 0
                    for m in det.meas:
                        p ^= row[m] & 1
                    w += p
                out[w] = out.get(w, 0) + 1
            return out

        a_w = weights(asqs_bits)
        r_w = weights(ref_bits)
        all_w = sorted(set(a_w) | set(r_w))
        # Pool adjacent weight bins until every expected count >= 5
        # (standard chi-square validity rule), then compute the 2xK
        # contingency chi-square with df = K-1.
        bins: List[Tuple[int, int]] = []  # (asqs_count, ref_count)
        for w in all_w:
            bins.append((a_w.get(w, 0), r_w.get(w, 0)))
        merged: List[List[int]] = []
        for cell in bins:
            if merged:
                last = merged[-1]
                exp = (last[0] + last[1] + cell[0] + cell[1])
                if (last[0] + cell[0]) * (last[1] + cell[1]) / max(1, exp) < 5 \
                        and (last[0] + cell[1]) * (last[0] + last[1]) / max(1, exp) < 5:
                    merged[-1] = [last[0] + cell[0], last[1] + cell[1]]
                    continue
            merged.append([cell[0], cell[1]])
        if len(merged) >= 2:
            n_tot = n_asqs + n_ref
            chi2 = 0.0
            for a_c, r_c in merged:
                col = a_c + r_c
                e_a = col * n_asqs / n_tot
                e_r = col * n_ref / n_tot
                if e_a > 0:
                    chi2 += (a_c - e_a) ** 2 / e_a
                if e_r > 0:
                    chi2 += (r_c - e_r) ** 2 / e_r
            df = len(merged) - 1
            syn_block = {
                "bins": len(merged),
                "chi2": round(chi2, 6),
                "df": df,
                "p_value": chi2_sf(chi2, df),
            }

    # --- observables ---
    obs_block = []
    for oi, ob in enumerate(circ.observables):
        def flip_rate(bits_rows):
            flips = 0
            for row in bits_rows:
                p = 0
                for m in ob.meas:
                    p ^= row[m] & 1
                flips += p
            return flips, flips / len(bits_rows)
        a_n, a_r = flip_rate(asqs_bits)
        r_n, r_r = flip_rate(ref_bits)
        chi2 = two_proportion_chi2(a_n, n_asqs, r_n, n_ref)
        obs_block.append({
            "name": ob.name,
            "asqs_flips": a_n, "asqs_rate": round(a_r, 8),
            "ref_flips": r_n, "ref_rate": round(r_r, 8),
            "chi2": chi2,
            "p_value": chi2_sf(chi2, 1) if chi2 is not None else None,
        })

    passed = (perm_p > alpha) and per_bit_passed and (
        det_block is None or det_block["passed"])

    return {
        "method": (
            "permutation test on total-variation distance of the joint "
            "outcome distribution (assumption-free); per-bit two-proportion "
            "chi-square with Bonferroni correction; per-detector comparison "
            "and syndrome-weight chi-square where detectors exist "
            "(PROOF_SPEC §15)"),
        "asqs_shots": n_asqs,
        "reference_shots": n_ref,
        "permutations": permutations,
        "tvd_observed": round(tvd_obs, 6),
        "tvd_null_mean": round(null_mean, 6),
        "tvd_null_max": round(null_max, 6),
        "permutation_p_value": round(perm_p, 6),
        "per_bit": {
            "count": nbits,
            "asqs_ones": [s.asqs_ones for s in bit_stats],
            "ref_ones": [s.ref_ones for s in bit_stats],
            "chi2": [s.chi2 for s in bit_stats],
            "p_values": [s.p_value for s in bit_stats],
            "max_abs_rate_diff": round(max(s.abs_rate_diff for s in bit_stats), 8),
            "min_p_value": min_p,
            "bonferroni_alpha": bonferroni_alpha,
            "passed": per_bit_passed,
        },
        "per_detector": det_block,
        "syndrome_weight_chi2": syn_block,
        "observables": obs_block,
        "alpha": alpha,
        "passed": passed,
    }


# ---------------------------------------------------------------------------
# File-level crosscheck
# ---------------------------------------------------------------------------

@dataclass
class CrosscheckResult:
    ok: bool
    path: str = ""
    reason: str = ""
    report: dict = field(default_factory=dict)


def crosscheck_file(path: str, reference_shots: int = 8192,
                    alpha: float = 0.01, permutations: int = 200,
                    structural_shots: int = 256, write: bool = True,
                    stim_seed: Optional[int] = None) -> CrosscheckResult:
    """Run the independent-reference statistical validation on one record.

    Reads a batch/output file, samples `reference_shots` shots from the
    converted stim circuit, compares, and (write=True) embeds `reference`
    and `validation` blocks into the file atomically.
    """
    if stim is None:
        return CrosscheckResult(False, path, "stim not installed (pip install stim)")
    try:
        with open(path, "r", encoding="utf-8") as f:
            batch = json.load(f)
    except Exception as e:  # noqa: BLE001
        return CrosscheckResult(False, path, f"unreadable: {e}")

    if batch.get("schema") not in ("asqs.batch/1", "asqs.batch/2"):
        return CrosscheckResult(False, path, "bad schema")
    shots_j = batch.get("shots")
    circ_j = batch.get("circuit")
    if not isinstance(shots_j, list) or not shots_j or not isinstance(circ_j, dict):
        return CrosscheckResult(False, path, "missing shots/circuit")
    if "validation" in batch:
        return CrosscheckResult(False, path, "already validated (use --redo)")

    try:
        circ = engine.build_circuit(circ_j)
    except Exception as e:  # noqa: BLE001
        return CrosscheckResult(False, path, f"circuit build: {e}")
    noise = engine.NoiseConfig.from_dict(batch.get("noise") or {})
    asqs_bits = [s.get("outcomes") or [] for s in shots_j]
    if any(len(r) != circ.num_measurements for r in asqs_bits):
        return CrosscheckResult(False, path, "shot width mismatch")

    # v1 records carry no circuit program; the circuit is rebuilt from the
    # embedded identity (the identity hash is still verified by `asqs verify`).
    # 1) conversion faithfulness (noiseless structural agreement)
    conv = conversion_checks(circ, noise, shots=structural_shots)
    if not conv["passed"]:
        return CrosscheckResult(False, path, f"conversion checks failed: {conv}")

    # 2) reference samples from stim (independent RNG)
    scirc = to_stim(circ, noise)
    ref_bits = sample_stim(scirc, reference_shots, seed=stim_seed)

    # 3) statistical comparison
    stats = compare_samples(asqs_bits, ref_bits, circ, alpha=alpha,
                            permutations=permutations)

    ref_block = {
        "simulator": "stim",
        "version": stim.__version__,
        "url": "https://github.com/quantumlib/Stim",
        "shots": reference_shots,
        "seed": stim_seed,  # null = fresh entropy (samples are embedded)
        "num_measurements": circ.num_measurements,
        "per_bit_ones": [sum(row[b] for row in ref_bits)
                         for b in range(circ.num_measurements)],
        "samples_packed": [pack_bits(row) for row in ref_bits],
        "noise_model_conversion": {
            "single_qubit": "DEPOLARIZE1(p1) after each H/S/S_DAG/X/Z",
            "cnot": "DEPOLARIZE1(p2) on control, DEPOLARIZE1(p2) on target "
                    "(independent, after CX)",
            "measurement": "X_ERROR(pm) immediately before MR / terminal MZ "
                            "(recorded-outcome-flip equivalence)",
            "reset": "R then X_ERROR(pr)",
            "bit_packing": "bit b -> byte b>>3, position 7-(b&7), hex encoded",
        },
        "conversion_checks": conv,
    }
    val_block = dict(stats)
    val_block["crosschecked_utc"] = _now_utc()

    report = {
        "file": os.path.basename(path),
        "reference": {k: v for k, v in ref_block.items()
                     if k != "samples_packed"},
        "validation": val_block,
    }

    if write:
        batch["reference"] = ref_block
        batch["validation"] = val_block
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(batch, f, indent=2)
        os.replace(tmp, path)

    return CrosscheckResult(
        ok=bool(stats["passed"]), path=path,
        reason="consistent with reference" if stats["passed"]
        else "REJECTED: statistically inconsistent with reference",
        report=report)


def _now_utc() -> str:
    import datetime
    return datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def verify_validation_block(batch: dict) -> Tuple[bool, str]:
    """Re-verify the deterministic parts of an embedded validation block.

    Recomputes, from the embedded ASQS shots + embedded reference samples:
      - per-bit counts and chi-square/p-values (exact)
      - per-detector counts and stats (exact)
      - TVD observed (exact)
    and compares them to the embedded values. The permutation p-value is
    stochastic by nature; anyone can redo it from the embedded samples.
    """
    shots_j = batch.get("shots")
    ref = batch.get("reference")
    val = batch.get("validation")
    if not isinstance(shots_j, list) or not isinstance(ref, dict) \
            or not isinstance(val, dict):
        return False, "missing reference/validation/shots"

    circ_j = batch.get("circuit")
    if not isinstance(circ_j, dict):
        return False, "missing circuit"
    try:
        circ = engine.build_circuit(circ_j)
    except Exception as e:  # noqa: BLE001
        return False, f"circuit build: {e}"

    nbits = circ.num_measurements
    packed = ref.get("samples_packed")
    if not isinstance(packed, list) or not packed:
        return False, "missing embedded reference samples"
    try:
        ref_bits = [unpack_bits(h, nbits) for h in packed]
    except Exception as e:  # noqa: BLE001
        return False, f"bad packed samples: {e}"
    asqs_bits = [s.get("outcomes") or [] for s in shots_j]
    n_asqs, n_ref = len(asqs_bits), len(ref_bits)
    if val.get("asqs_shots") != n_asqs or val.get("reference_shots") != n_ref:
        return False, "shot count mismatch"

    # per-bit exact recomputation
    pb = val.get("per_bit") or {}
    for b in range(nbits):
        a1 = sum(row[b] for row in asqs_bits)
        r1 = sum(row[b] for row in ref_bits)
        if pb.get("asqs_ones", [])[b] != a1 or pb.get("ref_ones", [])[b] != r1:
            return False, f"per-bit counts mismatch at bit {b}"
        chi2 = two_proportion_chi2(a1, n_asqs, r1, n_ref)
        p = chi2_sf(chi2, 1) if chi2 is not None else None
        emb_chi2 = (pb.get("chi2") or [])[b]
        emb_p = (pb.get("p_values") or [])[b]
        if chi2 is None:
            if emb_chi2 is not None:
                return False, f"per-bit chi2 mismatch at bit {b} (null vs value)"
        else:
            if emb_chi2 is None or abs(emb_chi2 - chi2) > 1e-9:
                return False, f"per-bit chi2 mismatch at bit {b}"
            if emb_p is None or abs(emb_p - p) > 1e-9:
                return False, f"per-bit p-value mismatch at bit {b}"

    # per-detector exact recomputation
    det = val.get("per_detector")
    if det and circ.detectors:
        for di, d in enumerate(circ.detectors):
            a = 0
            r = 0
            for row in asqs_bits:
                p = 0
                for m in d.meas:
                    p ^= row[m] & 1
                a += p
            for row in ref_bits:
                p = 0
                for m in d.meas:
                    p ^= row[m] & 1
                r += p
            if det.get("asqs_flip_counts", [])[di] != a \
                    or det.get("ref_flip_counts", [])[di] != r:
                return False, f"detector counts mismatch at {d.name}"

    # TVD observed exact recomputation
    asqs_strs = ["".join(map(str, row)) for row in asqs_bits]
    ref_strs = ["".join(map(str, row)) for row in ref_bits]
    c_a: Dict[str, int] = {}
    for s in asqs_strs:
        c_a[s] = c_a.get(s, 0) + 1
    c_b: Dict[str, int] = {}
    for s in ref_strs:
        c_b[s] = c_b.get(s, 0) + 1
    keys = sorted(set(c_a) | set(c_b))
    tvd = _tvd(keys, c_a, n_asqs, c_b, n_ref)
    if abs((val.get("tvd_observed") or 0) - tvd) > 1e-6:
        return False, "tvd_observed mismatch"

    return True, "validation block internally consistent"
