/*
 * gen_track_fixtures.js — build RDMTRK fixtures using RDM Studio's REAL encoder.
 *
 * The functions under test are lifted out of rdm7-desktop/src/tauri-overlay.html
 * by name and executed here, rather than re-implemented. That is the entire
 * value of this file: if the two repos ever drift on the byte layout, the C
 * test fails, instead of both sides agreeing with a third copy that is wrong.
 *
 *   node tests/native/gen_track_fixtures.js [path-to-rdm7-desktop]
 *
 * Writes tests/native/fixtures/track_map/*.rdmtrk
 */
import fs from 'fs';
import path from 'path';

const DESKTOP = process.argv[2] || 'C:/Users/ruuva/workspace/rdm7-desktop';
const SRC = path.join(DESKTOP, 'src', 'tauri-overlay.html');

if (!fs.existsSync(SRC)) {
  console.error('cannot find ' + SRC + '\nPass the rdm7-desktop path as argv[2].');
  process.exit(2);
}
const src = fs.readFileSync(SRC, 'utf8');

/* Pull a top-level `function name(...) { ... }` out by brace matching. */
function grab(name) {
  const needle = 'function ' + name + '(';
  const i = src.indexOf(needle);
  if (i < 0) throw new Error('not found in overlay: ' + name);
  let j = src.indexOf('{', i), depth = 0, k = j;
  for (; k < src.length; k++) {
    if (src[k] === '{') depth++;
    else if (src[k] === '}') { depth--; if (depth === 0) { k++; break; } }
  }
  return src.slice(i, k);
}

const NAMES = ['gpToLocalM', 'gpRdpKeep', 'gpOutlineSimplify', 'gpMetres',
               'gpTrackSlug', 'gpTrkNameBytes', 'gpOutlineClosed', 'gpTrackAssetBuild'];
const body = NAMES.map(grab).join('\n');

const mod = new Function(
  'GP_OUTLINE_MAX', 'GP_OUTLINE_TOL_M', 'GP_TRK_MAGIC', 'GP_TRK_VERSION',
  'GP_TRK_HEADER', 'GP_TRK_NAME_LEN', 'TextEncoder',
  body + '; return {' + NAMES.join(',') + '};'
)(400, 1.5, 'RDMTRK', 1, 52, 32, TextEncoder);

const { gpOutlineSimplify, gpTrackAssetBuild, gpTrackSlug } = mod;

/* Anchored to THIS file, not to cwd — the test runner invokes it from the repo
 * root and a human runs it from tests/native, and both must land in one place. */
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const OUT = path.join(HERE, 'fixtures', 'track_map');
fs.mkdirSync(OUT, { recursive: true });

function write(file, track) {
  const bytes = gpTrackAssetBuild(track);
  if (!bytes) throw new Error('encoder returned null for ' + file);
  fs.writeFileSync(path.join(OUT, file), Buffer.from(bytes));
  console.log(file.padEnd(22), bytes.length + ' bytes,',
              track.outline.pts.length + ' points, slug=' + gpTrackSlug(track.name));
}

/* --- a closed circuit at Winton, roughly the real place ------------------ */
const cLat = -36.5231, cLon = 146.0894;
const loop = [];
for (let i = 0; i <= 600; i++) {
  const a = (i / 600) * 2 * Math.PI;
  /* Deliberately not a circle: a lobed shape exercises the simplifier and
   * makes a mirrored projection obvious. */
  const r = 0.0055 * (1 + 0.35 * Math.sin(a * 3) + 0.12 * Math.cos(a * 5));
  loop.push([cLat + r * Math.cos(a), cLon + r * Math.sin(a) / Math.cos(cLat * Math.PI / 180)]);
}
write('winton.rdmtrk', { name: 'Winton', outline: { pts: gpOutlineSimplify(loop) } });

/* --- a point-to-point run that must NOT be flagged closed ---------------- */
const hill = [];
for (let i = 0; i <= 300; i++) {
  const t = i / 300;
  hill.push([-34.90 + t * 0.02, 138.70 + t * 0.03 + 0.002 * Math.sin(t * 12)]);
}
write('hillclimb.rdmtrk', { name: 'Mount Barker Hillclimb', outline: { pts: gpOutlineSimplify(hill) } });

/* --- a name longer than the field, with multi-byte characters ------------ */
write('longname.rdmtrk', {
  name: 'Nürburgring Nordschleife — Gesamtstrecke Grand Prix',
  outline: { pts: gpOutlineSimplify(loop) },
});

console.log('\nfixtures written to ' + OUT);
