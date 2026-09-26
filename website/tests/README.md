# Website encoder tests

These tests verify the GIF encoder embedded in `../index.html` (the
"Export GIF" feature). They extract the `ASQS_GIF` library verbatim
from the shipped HTML between the `GIFLIB-START` / `GIFLIB-END`
markers, so what is tested is exactly what ships.

```bash
# from the website/ directory (Node.js >= 14, Python >= 3.8 + Pillow)
node tests/test_giflib.js        # quantizer + encoder unit tests,
                                 # writes tests/gif_test_out/*.gif + expected JSON
python3 tests/verify_gifs.py     # decodes them with Pillow and asserts
                                 # bit-exact palette indices, delays, looping
```

What is covered:

- syntax compile of every inline `<script>` block in index.html
- quantizer: exact seed colors, 256-entry cap, nearest-neighbor mapping
- GIF89a structure: header, logical screen, global color table,
  NETSCAPE looping, per-frame graphic control extensions, sub-blocks
- LZW: variable-width codes (9 to 12 bits) and the CLEAR-based table
  reset once 4096 dictionary entries are reached
- round trip through Pillow's strict decoder with pixel-exact indices
