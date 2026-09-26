"""ASQS analyze - aggregate statistics over validated outputs/."""
from __future__ import annotations

import glob
import json
import os
from collections import Counter
from typing import Dict, List


def analyze_outputs(outputs_dir: str) -> Dict:
    files = sorted(
        f for f in glob.glob(os.path.join(outputs_dir, "asqs_out_*.json"))
        if os.path.basename(f).find(".tmp.") < 0
    )
    per_circuit: Dict[str, dict] = {}
    total_shots = 0
    total_files = 0
    first_ts = None
    last_ts = None
    for path in files:
        try:
            with open(path, "r", encoding="utf-8") as f:
                batch = json.load(f)
        except Exception:  # noqa: BLE001
            continue
        total_files += 1
        circ = batch.get("circuit") or {}
        key = json.dumps(engine_key(circ), sort_keys=True)
        agg = per_circuit.setdefault(
            key,
            {
                "circuit": circ,
                "batches": 0,
                "shots": 0,
                "detector_flips": 0,
                "detector_total": 0,
                "observable_flips": 0,
                "observable_total": 0,
                "error_pauli_counts": Counter(),
                "distinct_syndromes": set(),
                "workers": set(),
                "difficulty": Counter(),
            },
        )
        agg["batches"] += 1
        shots = batch.get("shots") or []
        agg["shots"] += len(shots)
        total_shots += len(shots)
        for s in shots:
            dets = s.get("detectors") or []
            agg["detector_flips"] += sum(dets)
            agg["detector_total"] += len(dets)
            obs = s.get("observables") or []
            agg["observable_flips"] += sum(obs)
            agg["observable_total"] += len(obs)
            syn = "".join(str(b) for b in dets)
            if "1" in syn:
                agg["distinct_syndromes"].add(syn)
            for e in s.get("errors") or []:
                agg["error_pauli_counts"][pauli_name(e.get("p", 0))] += 1
        agg["workers"].add((batch.get("share") or {}).get("worker", "?"))
        agg["difficulty"][(batch.get("share") or {}).get("difficulty", "?")] += 1
        ts = batch.get("created_ms")
        if ts:
            if first_ts is None or ts < first_ts:
                first_ts = ts
            if last_ts is None or ts > last_ts:
                last_ts = ts

    summary = {
        "outputs_dir": outputs_dir,
        "files": total_files,
        "shots_total": total_shots,
        "first_created_ms": first_ts,
        "last_created_ms": last_ts,
        "circuits": [],
    }
    for key, agg in sorted(per_circuit.items()):
        dr = (agg["detector_flips"] / agg["detector_total"]
              if agg["detector_total"] else 0.0)
        obr = (agg["observable_flips"] / agg["observable_total"]
               if agg["observable_total"] else 0.0)
        summary["circuits"].append({
            "circuit": agg["circuit"],
            "batches": agg["batches"],
            "shots": agg["shots"],
            "workers": sorted(agg["workers"]),
            "detector_flip_rate": round(dr, 8),
            "detector_flip_rate_ppm": int(dr * 1e6) if agg["detector_total"] else 0,
            "observable_flip_rate": round(obr, 8),
            "distinct_syndromes": len(agg["distinct_syndromes"]),
            "error_pauli_histogram": dict(agg["error_pauli_counts"]),
            "difficulties": dict(agg["difficulty"]),
        })
    return summary


def engine_key(circ: dict) -> dict:
    out = {"type": circ.get("type")}
    if circ.get("type") == "surface_code_memory":
        out["d"] = circ.get("d")
        out["rounds"] = circ.get("rounds")
    else:
        out["qubits"] = circ.get("qubits")
        out["gates"] = circ.get("gates")
    return out


def pauli_name(p: int) -> str:
    return {0: "X", 1: "Y", 2: "Z", 3: "M"}.get(int(p), "?")


def print_summary(summary: Dict) -> None:
    print(f"ASQS outputs analysis: {summary['outputs_dir']}")
    print(f"  files: {summary['files']}  shots: {summary['shots_total']}")
    for c in summary["circuits"]:
        print(f"  circuit {c['circuit']} (sha256-identified in files)")
        print(f"    batches={c['batches']} shots={c['shots']} workers={c['workers']}")
        print(f"    detector flip rate: {c['detector_flip_rate']:.6f} "
              f"({c['detector_flip_rate_ppm']} ppm)")
        print(f"    observable flip rate: {c['observable_flip_rate']:.6f}")
        print(f"    distinct syndromes: {c['distinct_syndromes']}")
        print(f"    error histogram: {c['error_pauli_histogram']}")
        print(f"    difficulties: {c['difficulties']}")
