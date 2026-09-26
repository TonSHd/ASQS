#!/usr/bin/env python3
"""
ASQS Quantum Simulation Dashboard Server (v4)

Serves the dashboard website plus a small JSON API so the browser loads
ONE proof batch at a time instead of all of them:

  GET /                     -> index.html
  GET /api/index            -> per-batch metadata (instant skeleton from
                               ledger.jsonl, enriched in the background by
                               parsing the real batch files; the table never
                               needs the file contents in the browser)
  GET /api/batch?file=NAME  -> ONE batch file (gzipped when the client
                               accepts it; validation batches shrink from
                               ~27 MB to ~1 MB on the wire)
  GET /outputs/NAME         -> same batch file (legacy path)
  GET /FORENSIC_REPORT.md   -> static file from the website directory
  GET /README.md            -> static file from the website directory

The background indexer parses batch files smallest-first, so the table
fills in within milliseconds for regular batches; the large 65,536-shot
validation batches take a few seconds each and progress is reported via
{"indexing": true, "indexed": n, "total": m}.

Table metrics are computed from the shot records themselves
(detectors for surface-code circuits, injected error events for
random_clifford circuits), matching what the frontend computes in the
viewer from the same data.

v4 adds no server-side functionality — the new "Export GIF" button
in the viewer is implemented entirely client-side inside index.html
(embedded GIF89a/LZW encoder; see website/README.md).
"""

import argparse
import gzip
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

WEBSITE_DIR = Path(__file__).resolve().parent
DEFAULT_ROOT = WEBSITE_DIR.parent
SAFE_NAME = re.compile(r"^asqs_out_[A-Za-z0-9_.\-]+\.json$")
GZIP_CACHE_LIMIT = 200  # cached compressed responses


def batch_metrics(batch):
    """Compute the table metrics from the shot records of one batch.

    Surface-code circuits carry detectors -> flip statistics come from
    shots[].detectors.  random_clifford circuits carry no detectors, so
    the metrics are computed from the recorded error events in
    shots[].errors (injected Pauli / measurement-flip events), per the
    dashboard convention:

      flips      = total number of recorded error events
      total      = shots x measurement count
      rate ppm   = flips / total * 1e6
      syndromes  = distinct per-shot error signatures
      outcomes   = distinct measurement outcome strings
    """
    shots = batch.get("shots") or []
    flips = 0
    total = 0
    n_meas = 0
    syndromes = set()
    outcomes = set()
    has_detectors = False

    for s in shots:
        dets = s.get("detectors") or []
        if dets:
            has_detectors = True
            dstr = "".join(str(int(bool(x))) for x in dets)
            flips += sum(1 for x in dets if x)
            total += len(dets)
            if "1" in dstr:
                syndromes.add(dstr)
        outs = s.get("outcomes") or []
        if len(outs) > n_meas:
            n_meas = len(outs)
        outcomes.add("".join(str(int(bool(x))) for x in outs))

    if not has_detectors:
        flips = 0
        esig = set()
        for s in shots:
            errs = s.get("errors") or []
            flips += len(errs)
            if errs:
                esig.add("|".join(sorted(
                    "%d:%d" % (e.get("q", -1), e.get("p", -1)) for e in errs)))
        total = len(shots) * n_meas
        syndromes = esig

    rate = round(flips / total * 1e6) if total else 0
    return {
        "flips": flips,
        "total": total,
        "rate_ppm": rate,
        "syndromes": len(syndromes),
        "distinct_outcomes": len(outcomes),
        "error_based": not has_detectors,
    }


class Indexer:
    """Builds and continuously refreshes the /api/index payload."""

    def __init__(self, root):
        self.root = Path(root)
        self.outputs_dir = self.root / "outputs"
        self.ledger_path = self.root / "ledger.jsonl"
        self.lock = threading.Lock()
        self.batches = {}   # filename -> row dict
        self.order = []     # filenames in listing order
        self.total = 0
        self.indexed = 0
        self.indexing = True

    # -- public ----------------------------------------------------
    def start(self):
        t = threading.Thread(target=self._run, daemon=True)
        t.start()
        return t

    def snapshot(self):
        with self.lock:
            return {
                "indexing": self.indexing,
                "indexed": self.indexed,
                "total": self.total,
                "batches": [dict(self.batches[f]) for f in self.order],
            }

    def known_file(self, name):
        with self.lock:
            return name in self.batches

    # -- internals -------------------------------------------------
    def _run(self):
        while True:
            self._rebuild()
            time.sleep(5)  # pick up newly written batches

    def _rebuild(self):
        ledger = self._read_ledger()
        files = []
        if self.outputs_dir.is_dir():
            files = sorted(
                p.name for p in self.outputs_dir.glob("asqs_out_*.json")
                if ".tmp." not in p.name)
        with self.lock:
            self.total = len(files)
            self.indexed = 0
            self.batches = {}
            self.order = files
            for f in files:
                led = ledger.get(f)
                self.batches[f] = self._skeleton(f, led)

        # Enrich smallest-first so the common case lands fast.
        for f in sorted(files, key=lambda n: (self._size(n), n)):
            try:
                self._enrich(f)
            except Exception as e:  # noqa: BLE001 - one bad file must not kill the index
                with self.lock:
                    self.batches[f]["parse_error"] = str(e)[:200]
            with self.lock:
                self.indexed += 1
        with self.lock:
            self.indexing = False

    def _size(self, name):
        try:
            return (self.outputs_dir / name).stat().st_size
        except OSError:
            return 0

    def _read_ledger(self):
        out = {}
        try:
            with open(self.ledger_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        e = json.loads(line)
                    except ValueError:
                        continue
                    if e.get("type") != "share_credit":
                        continue
                    name = e.get("batch")
                    if isinstance(name, str) and name.startswith("asqs_notvalidated_"):
                        # ledger stores the pre-promotion name; the promoted
                        # output replaces the leading prefix.
                        name = name.replace("asqs_notvalidated_", "asqs_out_", 1)
                    if name:
                        out[name] = e
        except OSError:
            pass
        return out

    def _skeleton(self, name, led):
        st = None
        try:
            st = (self.outputs_dir / name).stat()
        except OSError:
            pass
        row = {
            "file": name,
            "size": st.st_size if st else 0,
            "created_ms": (int(st.st_mtime * 1000) if st else None),
            "ledger": bool(led),
        }
        if led:
            row.update({
                "seq": led.get("seq"),
                "created_ms": led.get("ts_ms") or row["created_ms"],
                "worker": led.get("worker"),
                "difficulty": led.get("difficulty"),
                "circuit": led.get("circuit"),
                "circuit_sha256": led.get("circuit_sha256"),
                "shots": led.get("shots"),
                # detector statistics from the ledger are the truth for
                # surface-code batches; random_clifford rows get their
                # error-based metrics once the file is parsed.
                "ledger_flips": led.get("detector_flips"),
                "ledger_total": led.get("detector_total"),
            })
        return row

    def _enrich(self, name):
        path = self.outputs_dir / name
        with open(path, "r", encoding="utf-8") as f:
            batch = json.load(f)
        share = batch.get("share") or {}
        circ = batch.get("circuit") or {}
        row = {
            "file": name,
            "size": path.stat().st_size,
            "ledger": None,  # filled below under lock
            "batch_id": batch.get("batch_id"),
            "created_ms": batch.get("created_ms"),
            "worker": share.get("worker"),
            "difficulty": share.get("difficulty"),
            "share_hash": share.get("share_hash_hex"),
            "circuit": circ.get("type"),
            "circuit_detail": {
                k: v for k, v in circ.items() if k != "noise"
            },
            "circuit_sha256": batch.get("circuit_sha256"),
            "shots": len(batch.get("shots") or []),
            "validation_batch": bool(batch.get("validation_batch")),
            "schema": batch.get("schema"),
        }
        row.update(batch_metrics(batch))
        with self.lock:
            prev = self.batches.get(name) or {}
            row["ledger"] = prev.get("ledger", False)
            row["seq"] = prev.get("seq")
            if not row["created_ms"]:
                row["created_ms"] = prev.get("created_ms")
            self.batches[name] = row


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    indexer = None      # set by serve()
    gz_cache = {}       # name -> (mtime, size, gz_bytes)
    gz_order = []

    # -- helpers ---------------------------------------------------
    def _send(self, code, body, ctype, extra=None, raw=False):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _static(self, path, ctype):
        try:
            body = path.read_bytes()
        except OSError:
            self._send(404, "not found", "text/plain")
            return
        self._send(200, body, ctype)

    def _batch_file(self, name):
        if not SAFE_NAME.match(name or ""):
            return None
        idx = self.indexer
        if idx is None:
            return None
        path = (idx.outputs_dir / name).resolve()
        if path.parent != idx.outputs_dir.resolve():
            return None
        if not path.is_file():
            return None
        return path

    def _serve_batch(self, name):
        path = self._batch_file(name)
        if path is None:
            self._send(404, '{"error":"unknown batch"}', "application/json")
            return
        st = path.stat()
        accept_gz = "gzip" in (self.headers.get("Accept-Encoding") or "")
        if accept_gz:
            key = path.name
            hit = Handler.gz_cache.get(key)
            if not hit or hit[0] != st.st_mtime or hit[1] != st.st_size:
                with open(path, "rb") as f:
                    raw = f.read()
                gz = gzip.compress(raw, 6)
                Handler.gz_cache[key] = (st.st_mtime, st.st_size, gz)
                Handler.gz_order.append(key)
                while len(Handler.gz_order) > GZIP_CACHE_LIMIT:
                    Handler.gz_cache.pop(Handler.gz_order.pop(0), None)
            else:
                gz = hit[2]
            self._send(200, gz, "application/json",
                      extra={"Content-Encoding": "gzip"})
        else:
            with open(path, "rb") as f:
                raw = f.read()
            self._send(200, raw, "application/json")

    # -- HTTP ------------------------------------------------------
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        route = url.path
        if route in ("/", "/index.html"):
            self._static(WEBSITE_DIR / "index.html", "text/html; charset=utf-8")
            return
        if route == "/api/index":
            snap = self.indexer.snapshot() if self.indexer else {}
            self._send(200, json.dumps(snap), "application/json")
            return
        if route == "/api/batch":
            qs = parse_qs(url.query)
            name = (qs.get("file") or [""])[0]
            self._serve_batch(name)
            return
        if route.startswith("/outputs/"):
            self._serve_batch(route[len("/outputs/"):])
            return
        if route == "/FORENSIC_REPORT.md":
            self._static(WEBSITE_DIR / "FORENSIC_REPORT.md", "text/markdown; charset=utf-8")
            return
        if route == "/README.md":
            self._static(WEBSITE_DIR / "README.md", "text/markdown; charset=utf-8")
            return
        self._send(404, "not found", "text/plain")

    def log_message(self, fmt, *args):  # quieter console
        pass


def generate_outputs_list(root):
    """Legacy helper kept for --generate-only compatibility."""
    outputs = Path(root) / "outputs"
    files = sorted(
        p.name for p in outputs.glob("asqs_out_*.json") if ".tmp." not in p.name
    ) if outputs.is_dir() else []
    listing = {"files": files, "count": len(files), "directory": str(outputs)}
    out = WEBSITE_DIR / "outputs_list.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(listing, f, indent=2)
    print(f"Generated outputs_list.json with {len(files)} files "
          "(legacy format; the dashboard now uses /api/index)")


def serve(port, root):
    indexer = Indexer(root)
    Handler.indexer = indexer
    print(f"ASQS Dashboard Server (v4 - lazy loading + client-side GIF export)")
    print(f"   Website : {WEBSITE_DIR}")
    print(f"   Outputs : {indexer.outputs_dir}")
    print(f"   URL     : http://localhost:{port}")
    indexer.start()
    with ThreadingHTTPServer(("", port), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nshutting down server...")


def main():
    ap = argparse.ArgumentParser(description="ASQS Quantum Simulation Dashboard Server")
    ap.add_argument("--port", type=int, default=8000, help="Port to serve on (default 8000)")
    ap.add_argument("--project-root", default=str(DEFAULT_ROOT),
                    help="Project root containing outputs/ and ledger.jsonl")
    ap.add_argument("--generate-only", action="store_true",
                    help="Only regenerate the legacy outputs_list.json, don't start the server")
    args = ap.parse_args()
    if args.generate_only:
        generate_outputs_list(args.project_root)
    else:
        serve(args.port, args.project_root)


if __name__ == "__main__":
    main()
