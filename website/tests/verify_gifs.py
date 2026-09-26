#!/usr/bin/env python3
"""PIL-based verifier for the GIFs produced by test_giflib.js.

Decodes each GIF like a normal viewer would (Pillow's strict GIF
decoder) and asserts, against the expected JSON written by Node:
  - file format, pixel size, frame count
  - infinite looping (NETSCAPE extension)
  - per-frame delay (duration)
  - per-frame palette INDICES are bit-exact (lossless transport)
  - global palette RGB values match the encoder's palette
"""
import json
import sys
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parent / 'gif_test_out'


def check(name: str) -> None:
    gif = OUT / f"{name}.gif"
    exp = json.loads((OUT / f"{name}_expected.json").read_text())

    im = Image.open(gif)
    assert im.format == "GIF", f"{name}: format {im.format}"
    assert im.size == (exp["width"], exp["height"]), f"{name}: size {im.size}"
    n = getattr(im, "n_frames", 1)
    assert n == len(exp["frames"]), f"{name}: {n} frames != {len(exp['frames'])}"

    if exp.get("loop"):
        assert im.info.get("loop") == 0, f"{name}: loop {im.info.get('loop')!r}"
    else:
        assert "loop" not in im.info or im.info.get("loop", 0) == 0, f"{name}: unexpected loop"

    for i in range(n):
        im.seek(i)
        want_ms = exp["frames"][i]["delayCs"] * 10
        got_ms = im.info.get("duration")
        assert got_ms == want_ms, f"{name} f{i}: duration {got_ms} != {want_ms}"
        want = exp["frames"][i]["indices"]
        # Pillow decodes the first frame in P mode but converts later
        # frames to RGB (palette applied). Handle both: map RGB tuples
        # back to palette indices via the encoder's own palette.
        pal = exp["palette"]
        if im.mode == "P":
            idx = list(im.getdata())
        else:
            color_to_idx = {(c[0], c[1], c[2]): j for j, c in enumerate(pal)}
            rgb = im.convert("RGB")
            idx = [color_to_idx.get(px, -1) for px in rgb.getdata()]
        assert len(idx) == len(want), f"{name} f{i}: {len(idx)} px != {len(want)}"
        bad = [k for k in range(len(want)) if idx[k] != want[k]]
        assert not bad, f"{name} f{i}: {len(bad)} index mismatches, first at {bad[0]}"
    pal = im.getpalette()
    if pal is None:  # later frames are RGB — read the global palette from frame 0
        im.seek(0)
        im.load()
        pal = im.getpalette()
    assert pal is not None, f"{name}: no palette"
    for j, c in enumerate(exp["palette"]):
        off = j * 3
        assert (pal[off], pal[off + 1], pal[off + 2]) == tuple(c), \
            f"{name}: palette[{j}] {(pal[off], pal[off+1], pal[off+2])} != {tuple(c)}"

    print(f"  {name}.gif OK — {n} frames, {im.size}, "
          f"{'infinite loop' if exp.get('loop') else 'no loop'}, "
          f"delays {[f['delayCs'] for f in exp['frames']]}, indices+palette exact")


def main() -> int:
    names = sys.argv[1:] or ["gif_small", "gif_stress", "gif_flat"]
    for nm in names:
        check(nm)
    print("PIL VERIFY PASSED for", ", ".join(names))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
