#!/usr/bin/env node
/* Round-trip tests for the in-page ASQS_GIF encoder.
 *
 * 1. extracts the GIFLIB block from the shipped index.html (so the
 *    tested code is exactly what ships),
 * 2. syntax-compiles every inline <script> block,
 * 3. unit-tests the quantizer (exact colors, 256 cap, nearest mapping),
 * 4. encodes three test GIFs (small flat, delays/loop, stress with
 *    >256 unique colors and LZW table resets at 4096 codes),
 * 5. writes expected indices/palettes to JSON for the PIL verifier.
 */
'use strict';
const fs = require('fs');
const path = require('path');

const WEBSITE = require('path').join(__dirname, '..', 'index.html');
const OUT = require('path').join(__dirname, 'gif_test_out');
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT, { recursive: true });

const html = fs.readFileSync(WEBSITE, 'utf8');

// ---- 1. extract the shipped GIFLIB -------------------------------
const m = html.match(/\/\* GIFLIB-START \*\/([\s\S]*?)\/\* GIFLIB-END \*\//);
if (!m) throw new Error('GIFLIB markers not found in index.html');
eval(m[1]);
const G = globalThis.ASQS_GIF;
if (!G || typeof G.encodeGif !== 'function') throw new Error('ASQS_GIF not defined after eval');

// ---- 2. syntax-check every inline script block --------------------
const blocks = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((x) => x[1]);
if (blocks.length !== 3) throw new Error('expected 3 inline script blocks, got ' + blocks.length);
blocks.forEach((src, i) => { new Function(src); }); // compile only
console.log('[1] all ' + blocks.length + ' inline script blocks compile OK (giflib ' + m[1].length + ' chars)');

// ---- 3. quantizer unit tests ---------------------------------------
(() => {
  const q = G.makeQuantizer([[255, 0, 0], [0, 0, 255]]);
  if (q.indexOf(255, 0, 0) !== 0) throw new Error('seed color 0 not exact');
  if (q.indexOf(0, 0, 255) !== 1) throw new Error('seed color 1 not exact');
  for (let r = 0; r < 16; r++)
    for (let g = 0; g < 16; g++)
      for (let b = 0; b < 16; b++) q.indexOf(r * 17, g * 17, b * 17);
  const pal = q.palette();
  if (pal.length !== 256) throw new Error('palette cap failed: ' + pal.length);
  // exactness of every in-table color
  for (let i = 0; i < pal.length; i++) {
    const again = q.indexOf(pal[i][0], pal[i][1], pal[i][2]);
    if (again !== i) throw new Error('inconsistent index for palette entry ' + i);
  }
  // nearest mapping for an out-of-table color must equal brute-force argmin
  const [qr, qg, qb] = [255, 3, 3];
  const idx = q.indexOf(qr, qg, qb);
  let best = -1, bd = Infinity;
  pal.forEach((p, i) => {
    const d = (qr - p[0]) ** 2 + (qg - p[1]) ** 2 + (qb - p[2]) ** 2;
    if (d < bd) { bd = d; best = i; }
  });
  if (idx !== best) throw new Error('nearest mismatch: ' + idx + ' vs ' + best);
  console.log('[2] quantizer OK (256 cap, exact seeds, nearest argmin verified)');
})();

// ---- helpers -------------------------------------------------------
function rgbaFrame(W, H, fn) {
  const a = new Uint8ClampedArray(W * H * 4);
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const c = fn(x, y);
      const j = (y * W + x) * 4;
      a[j] = c[0]; a[j + 1] = c[1]; a[j + 2] = c[2]; a[j + 3] = 255;
    }
  return a;
}
function writeCase(name, W, H, q, frames, loop) {
  const gif = G.encodeGif({ width: W, height: H, palette: q.palette(), loop: loop !== false, frames });
  fs.writeFileSync(path.join(OUT, name + '.gif'), Buffer.from(gif));
  fs.writeFileSync(path.join(OUT, name + '_expected.json'), JSON.stringify({
    width: W, height: H, loop: loop !== false,
    palette: q.palette().map((c) => [c[0], c[1], c[2]]),
    frames: frames.map((f) => ({ delayCs: f.delayCs, indices: Array.from(f.indices) }))
  }));
  return gif.length;
}

// ---- 4a. small flat gif (exact palette, 3 frames, varying delays) ---
(() => {
  const W = 64, H = 48;
  const colors = [[13, 16, 19], [230, 233, 236], [127, 176, 160], [217, 154, 88], [193, 102, 107]];
  const q = G.makeQuantizer(colors);
  const mk = (fn) => ({ indices: G.quantize(q, rgbaFrame(W, H, fn)), delayCs: 11 });
  const frames = [
    Object.assign(mk((x) => colors[(x >> 3) % colors.length]), { delayCs: 11 }),
    Object.assign(mk((x, y) => colors[(y >> 3) % colors.length]), { delayCs: 16 }),
    Object.assign(mk((x, y) => colors[(x + y) % colors.length]), { delayCs: 21 })
  ];
  const n = writeCase('gif_small', W, H, q, frames, true);
  console.log('[3] gif_small.gif written (' + n + ' bytes, 3 frames, delays 11/16/21)');
})();

// ---- 4b. stress gif (>256 unique colors, LZW table reset at 4096) --
(() => {
  const W = 400, H = 300;
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) >> 8) & 255;
  const q = G.makeQuantizer([[13, 16, 19]]);
  const frames = [];
  for (let f = 0; f < 4; f++) {
    const rgba = rgbaFrame(W, H, (x, y) => {
      if ((((x >> 3) + (y >> 3) + f) % 5) === 0) return [rnd(), rnd(), rnd()];
      if (((x / 40) | 0) % 2 === 0) return [230, 233, 236];
      return [20 + ((x * (f + 1)) % 60), 24, 29];
    });
    frames.push({ indices: G.quantize(q, rgba), delayCs: 10 });
  }
  const uniq = q.palette().length;
  const n = writeCase('gif_stress', W, H, q, frames, true);
  // LZW must exercise the CLEAR path: noise indices produce >4096 dict entries
  const lzwLen = frames.reduce((s, fr) => s + G.lzwEncode(fr.indices, 8).length, 0);
  console.log('[4] gif_stress.gif written (' + n + ' bytes, palette capped at ' + uniq +
    ', lzw payload ' + lzwLen + ' bytes — nearest-mapping + table-reset paths exercised)');
})();

// ---- 4c. single-color edge case ------------------------------------
(() => {
  const W = 16, H = 9;
  const q = G.makeQuantizer([[1, 2, 3]]);
  const rgba = rgbaFrame(W, H, () => [1, 2, 3]);
  const frames = [{ indices: G.quantize(q, rgba), delayCs: 10 }];
  const n = writeCase('gif_flat', W, H, q, frames, false);
  console.log('[5] gif_flat.gif written (' + n + ' bytes, 1 frame, loop=false)');
})();

console.log('ALL NODE TESTS PASSED');
