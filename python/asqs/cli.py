"""ASQS command line interface."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

from . import analyze as analyze_mod
from . import verify as verify_mod

VERSION = "0.1.0"


def _crosscheck_targets(args, root):
    targets = []
    if args.file:
        targets = [args.file]
    elif args.validation_only:
        outdir = os.path.join(root, "outputs")
        if os.path.isdir(outdir):
            for f in sorted(os.listdir(outdir)):
                p = os.path.join(outdir, f)
                if f.startswith("asqs_out_") and ".tmp." not in f:
                    try:
                        with open(p, "r", encoding="utf-8") as fh:
                            b = json.load(fh)
                        if b.get("validation_batch") and "validation" not in b:
                            targets.append(p)
                    except Exception:  # noqa: BLE001
                        pass
    else:
        outdir = os.path.join(root, "outputs")
        if os.path.isdir(outdir):
            targets = [os.path.join(outdir, f) for f in sorted(os.listdir(outdir))
                       if f.startswith("asqs_out_") and ".tmp." not in f]
    return targets


def cmd_crosscheck(args) -> int:
    """Independent-reference statistical validation (stim + PROOF_SPEC §15)."""
    try:
        from . import reference as ref_mod
    except Exception as e:  # noqa: BLE001
        print(f"cannot import reference module: {e}", file=sys.stderr)
        return 2
    if ref_mod.stim is None:
        print("stim is required for the reference crosscheck.\n"
              "  pip install stim\n  (or run this command with a python that "
              "has stim installed)", file=sys.stderr)
        return 2
    root = args.project_root
    targets = _crosscheck_targets(args, root)
    if not targets:
        print("no crosscheck targets (use --file F, --all, or "
              "--validation-only with promoted validation batches)",
              file=sys.stderr)
        return 1
    bad = 0
    for t in targets:
        r = ref_mod.crosscheck_file(
            t, reference_shots=args.reference_shots, alpha=args.alpha,
            permutations=args.permutations, write=not args.dry_run)
        mark = "OK " if r.ok else "FAIL"
        v = r.report.get("validation", {}) if r.report else {}
        extra = ""
        if v:
            extra = (f" asqs={v.get('asqs_shots')} ref={v.get('reference_shots')}"
                     f" tvd={v.get('tvd_observed')} perm_p={v.get('permutation_p_value')}"
                     f" min_bit_p={None if not v.get('per_bit') else v['per_bit'].get('min_p_value')}")
        print(f"  [{mark}] {os.path.basename(t)} :: {r.reason}{extra}")
        if args.out and r.report:
            with open(args.out, "a", encoding="utf-8") as f:
                json.dump(r.report, f, indent=2)
                f.write("\n")
        if not r.ok:
            bad += 1
    if args.out:
        print(f"  report written: {args.out}")
    return 1 if bad else 0


def _find_daemon() -> str:
    candidates = []
    if os.environ.get("ASQSD_BIN"):
        candidates.append(os.environ["ASQSD_BIN"])
    here = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(here, "..", "..", "..", "build", "asqsd"))
    candidates.append(os.path.join(os.getcwd(), "build", "asqsd"))
    found = shutil.which("asqsd")
    if found:
        candidates.append(found)
    for c in candidates:
        c = os.path.abspath(c)
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return ""


def _default_root() -> str:
    return os.environ.get("ASQS_ROOT", os.getcwd())


def cmd_host(args) -> int:
    daemon = _find_daemon()
    if not daemon:
        print("asqsd daemon binary not found. Build it first:\n"
              "  make asqsd\nor set ASQSD_BIN=/path/to/asqsd",
              file=sys.stderr)
        return 2
    cmd = [daemon,
           "--ip", args.ip,
           "--port", str(args.port),
           "--project-root", args.project_root,
           "--shots-per-share", str(args.shots_per_share),
           "--max-shares-per-sec", str(args.max_shares_per_sec),
           "--difficulty-mode", args.difficulty_mode,
           "--init-difficulty", args.init_difficulty,
           "--job-interval-s", str(args.job_interval_s)]
    if args.circuit:
        cmd += ["--circuit", args.circuit]
    if args.d is not None:
        cmd += ["--d", str(args.d)]
    if args.rounds is not None:
        cmd += ["--rounds", str(args.rounds)]
    if args.qubits is not None:
        cmd += ["--qubits", str(args.qubits)]
    if args.gates is not None:
        cmd += ["--gates", str(args.gates)]
    if args.noise:
        cmd += ["--noise", args.noise]
    if args.local_time:
        cmd += ["--local-time"]
    if args.config:
        cmd = [daemon, "--config", args.config, "--project-root",
               args.project_root] + cmd[4:]
    print(f"[asqs] launching daemon: {' '.join(cmd)}", file=sys.stderr)
    sys.stdout.flush()
    sys.stderr.flush()
    os.execv(cmd[0], cmd)
    return 0  # unreachable


def cmd_status(args) -> int:
    path = os.path.join(args.project_root, "status.json")
    if not os.path.isfile(path):
        print(f"no status file at {path} (is the daemon running?)", file=sys.stderr)
        return 1
    with open(path, "r", encoding="utf-8") as f:
        s = json.load(f)
    keys = ["now_utc", "uptime_s", "version", "miners_connected", "difficulty",
            "difficulty_mode", "hashrate_est_hps", "shares_received",
            "shares_verified", "shares_rejected", "batches_written",
            "batches_validated", "batches_useful", "batches_useless_deleted",
            "batches_invalid_deleted", "shots_total", "pending_batches",
            "queue_depth", "circuit", "circuit_sha256", "shots_per_share",
            "last_event", "last_error"]
    for k in keys:
        if k in s:
            print(f"  {k:26} {s[k]}")
    return 0


def cmd_validate(args) -> int:
    daemon = _find_daemon()
    if not daemon:
        print("asqsd not found (run: make asqsd)", file=sys.stderr)
        return 2
    return subprocess.call([daemon, "--scan-once", "--project-root", args.project_root])


def cmd_verify(args) -> int:
    root = args.project_root
    ledger = os.path.join(root, "ledger.jsonl")
    targets = []
    if args.all or (not args.file and not args.ledger_only):
        outdir = os.path.join(root, "outputs")
        if os.path.isdir(outdir):
            targets = sorted(
                os.path.join(outdir, f) for f in os.listdir(outdir)
                if f.startswith("asqs_out_") and ".tmp." not in f)
    if args.file:
        targets = [args.file] + [t for t in targets if t != args.file]
    if not targets and not args.ledger_only:
        print("nothing to verify (no --file, no outputs/)", file=sys.stderr)
        return 1
    bad = 0
    for t in targets:
        r = verify_mod.verify_batch(t, ledger)
        mark = "OK " if r.ok else "FAIL"
        extra = (f" shots={r.shots} flips={r.detector_flips}/{r.detector_total}"
                 f" syndromes={r.distinct_syndromes}") if r.ok else f" :: {r.reason}"
        print(f"  [{mark}] {os.path.basename(t)}{extra}")
        if not r.ok:
            bad += 1
            continue
        # If a reference/validation block is embedded, re-verify its
        # deterministic parts from the embedded samples.
        try:
            with open(t, "r", encoding="utf-8") as f:
                b = json.load(f)
            if "validation" in b:
                from . import reference as ref_mod
                ok2, why2 = ref_mod.verify_validation_block(b)
                mark2 = "OK " if ok2 else "FAIL"
                print(f"  [{mark2}]   ... validation block: {why2}")
                if not ok2:
                    bad += 1
        except Exception as e:  # noqa: BLE001
            print(f"  [FAIL]   ... validation block unreadable: {e}")
            bad += 1
    if args.ledger_only or os.path.exists(ledger):
        ok, msg, count = verify_mod.verify_ledger(ledger)
        print(f"  [{'OK ' if ok else 'FAIL'}] ledger.jsonl :: {msg} ({count} entries)")
        if not ok:
            bad += 1
    return 1 if bad else 0


def cmd_analyze(args) -> int:
    outdir = os.path.join(args.project_root, "outputs")
    summary = analyze_mod.analyze_outputs(outdir)
    analyze_mod.print_summary(summary)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"  summary written: {args.out}")
    return 0


def cmd_circuits(args) -> int:
    from . import engine
    for spec in [{"type": "surface_code_memory", "d": 3, "rounds": 3},
                 {"type": "surface_code_memory", "d": 5, "rounds": 5},
                 {"type": "random_clifford", "qubits": 24, "gates": 120}]:
        try:
            circ = engine.build_circuit(spec)
            print(f"  {spec['type']:22} {json.dumps({k: v for k, v in spec.items() if k != 'type'}, sort_keys=True)}"
                  f" qubits={circ.num_qubits} meas={circ.num_measurements}"
                  f" detectors={len(circ.detectors)} sha256={engine.circuit_sha256(spec)}")
        except Exception as e:  # noqa: BLE001
            print(f"  {spec} FAILED: {e}")
            return 1
    return 0


def cmd_ledger(args) -> int:
    path = os.path.join(args.project_root, "ledger.jsonl")
    ok, msg, count = verify_mod.verify_ledger(path)
    print(f"  {path}: {msg} ({count} entries)")
    if ok and args.dump:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    print("  " + line.strip())
    return 0 if ok else 1


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="asqs",
                                description="ASQS - Application-Specific Quantum Simulation")
    p.add_argument("--version", action="version", version=f"asqs {VERSION}")
    sub = p.add_subparsers(dest="cmd", required=True)

    h = sub.add_parser("host", help="run the ASQS daemon (stratum server)")
    h.add_argument("--ip", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    h.add_argument("--port", type=int, default=3333)
    h.add_argument("--project-root", default=_default_root())
    h.add_argument("--circuit", default=None,
                   choices=["surface_code_memory", "random_clifford"])
    h.add_argument("--d", type=int, default=None, help="surface code distance")
    h.add_argument("--rounds", type=int, default=None)
    h.add_argument("--qubits", type=int, default=None)
    h.add_argument("--gates", type=int, default=None)
    h.add_argument("--noise", default=None, help="P1,P2,PM,PR in ppb")
    h.add_argument("--shots-per-share", type=int, default=16)
    h.add_argument("--max-shares-per-sec", type=float, default=1.0)
    h.add_argument("--difficulty-mode", default="adaptive",
                   choices=["adaptive", "fixed"])
    h.add_argument("--init-difficulty", default="256")
    h.add_argument("--job-interval-s", type=int, default=30)
    h.add_argument("--config", default=None, help="JSON config file")
    h.add_argument("--local-time", action="store_true")
    h.set_defaults(func=cmd_host)

    s = sub.add_parser("status", help="show daemon status")
    s.add_argument("--project-root", default=_default_root())
    s.set_defaults(func=cmd_status)

    v = sub.add_parser("validate", help="run one validation pass over notvalidated/")
    v.add_argument("--project-root", default=_default_root())
    v.set_defaults(func=cmd_validate)

    vf = sub.add_parser("verify", help="audit outputs / a batch file / the ledger")
    vf.add_argument("--project-root", default=_default_root())
    vf.add_argument("--file", default=None, help="specific file to verify")
    vf.add_argument("--all", action="store_true", help="verify all outputs/")
    vf.add_argument("--ledger-only", action="store_true")
    vf.set_defaults(func=cmd_verify)

    a = sub.add_parser("analyze", help="aggregate statistics over outputs/")
    a.add_argument("--project-root", default=_default_root())
    a.add_argument("--out", default=None, help="write summary JSON here")
    a.set_defaults(func=cmd_analyze)

    c = sub.add_parser("circuits", help="list built-in circuits")
    c.set_defaults(func=cmd_circuits)

    x = sub.add_parser("crosscheck", help="independent-reference statistical "
                     "validation against stim (PROOF_SPEC §15)")
    x.add_argument("--project-root", default=_default_root())
    x.add_argument("--file", default=None, help="specific output/batch file")
    x.add_argument("--all", action="store_true", help="crosscheck every output")
    x.add_argument("--validation-only", action="store_true",
                   help="crosscheck only unvalidated validation batches")
    x.add_argument("--reference-shots", type=int, default=8192)
    x.add_argument("--permutations", type=int, default=200)
    x.add_argument("--alpha", type=float, default=0.01)
    x.add_argument("--dry-run", action="store_true",
                   help="report only; do not write into the files")
    x.add_argument("--out", default=None, help="append JSON reports here")
    x.set_defaults(func=cmd_crosscheck)

    l = sub.add_parser("ledger", help="verify the share ledger chain")
    l.add_argument("--project-root", default=_default_root())
    l.add_argument("--dump", action="store_true")
    l.set_defaults(func=cmd_ledger)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
