# ASQS Quantum Simulation Dashboard

An interactive web dashboard for exploring ASQS (Application-Specific
Quantum Simulation) proof batches. Every number and animation is driven
by the real contents of the batch files — nothing is fabricated.

## Quick start

```bash
cd website
python3 serve.py              # serves on http://localhost:8000
# or explicitly:
python3 serve.py --port 8000 --project-root ..
```

`--project-root` is the directory containing `outputs/` and
`ledger.jsonl` (default: the parent of the `website/` directory).

## What you get

- **Lazy, one-at-a-time loading.** The browser fetches only a small
  metadata index (`/api/index`) — never the batch contents. A batch
  file is fetched only when you click its row or its page number.
  Validation batches (~27 MB raw) are gzipped on the wire (~1 MB).
- **Numbered pager rows (1 2 3 4 5 …)** at the bottom of the table and
  inside the viewer. Click a number to load that single batch.
- **Real gate-by-gate circuit replay (Play button).** For circuits with
  an embedded program (`asqs.batch/2`), the play button replays the
  actual instruction list for the selected shot:
  - the 24 qubits on a grid, gates pulsing as they execute,
  - CNOT arrows drawn from control to target,
  - injected errors flashing in red at the exact instruction where the
    recorded event happened (`shots[].errors` carries the op index),
  - measurement outcomes settling to the real recorded bits,
  - the observable at the end.
  - Controls: Play/Pause (resume), ◂ shot / shot ▸, speed (0.5–4×),
    jump-to-shot (useful for 65,536-shot validation batches), sound.
  - Click any qubit for its real per-shot record (ops touching it,
    error events, outcome).
- **Surface-code batches** keep the spatial stabilizer layout with
  round-by-round detector animation.
- **Export GIF button.** One click re-plays the current shot from the
  top and records the **entire visualization** — header (batch id,
  timestamp, circuit, shot number), the live qubit stage, the op
  readout + progress + legend, the event log, and the validation
  metrics footer — into a downloadable animated `.gif` (e.g.
  `asqs_<batch>_shot1.gif`). Every frame is a snapshot of the real
  viewer state, so the GIF shows exactly what you saw, in your theme.
  Encoding is done by a dependency-free GIF89a encoder embedded in the
  page (quantizer + LZW), so it works offline; nothing is uploaded
  anywhere. During the few seconds of recording the transport is
  locked to keep the capture deterministic.
- **Metrics computed from shot data, not the proof block.**
  - surface-code: flips from `shots[].detectors`
  - random_clifford: flips from `shots[].errors` — displayed as
    `errors / (shots × measurements)`, rate in ppm, distinct per-shot
    error signatures as syndromes, distinct outcome strings.
  - The `proof.gate` block is only a fallback for records without shots.

## Architecture

```
website/
├── index.html     # single-file dashboard (no build step, no deps;
│                  #   includes the embedded ASQS_GIF encoder)
└── serve.py       # stdlib-only server

serve.py endpoints:
  GET /                -> index.html
  GET /api/index       -> metadata for every batch in outputs/
                          (ledger.jsonl skeleton + background
                          enrichment computed from the batch files;
                          progress reported while indexing)
  GET /api/batch?file= -> one batch file (gzip if accepted, cached)
  GET /outputs/<name>  -> legacy path to the same file
```

The background indexer parses batch files smallest-first, so the table
is populated within milliseconds; the large validation batches take a
few seconds each. Indexing state is exposed as
`{"indexing": bool, "indexed": n, "total": m}` and the frontend polls
until it completes.

## Data sources

- `outputs/asqs_out_*.json` — validated proof batches
- `ledger.jsonl` — hash-chained share ledger (worker, difficulty,
  seq, timestamps)

Batch files without a ledger entry (pre-fix or orphaned by a ledger
reset) are listed with a warning banner and "no ledger" status.

## Notes

- `--generate-only` still regenerates the legacy `outputs_list.json`
  for compatibility with older tooling; the dashboard itself no longer
  uses it.
- Tested with Chrome/Edge 80+, Firefox 75+, Safari 13+.
- The GIF export rasterizes the stage SVG per frame; text inside the
  stage uses the system monospace font (web fonts cannot be loaded in
  that rendering context), so glyph shapes can differ slightly from
  the on-screen stage. Colors, geometry, data and text content are
  identical.

## License

Apache-2.0 — same as the main ASQS project.
