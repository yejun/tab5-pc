// A thin cap of snow on the top edge of a rounded box at (x, y), w wide: a low, bumpy top and a few short drips
function snowCap(x, y, w, seed, sink = 7) {
  const r = rng(seed), L = x - 1.5, R = x + w + 1.5, n = Math.max(3, Math.round(w / 22));
  let d = `M${f(L)},${f(y + 5)} Q${f(L - 1)},${f(y - 1)} ${f(L + 7)},${f(y - 2.5)} `;
  for (let i = 0; i < n; i++) {
    const x1 = L + 7 + (R - L - 14) * (i + 1) / n, xm = L + 7 + (R - L - 14) * (i + .5) / n;
    d += `Q${f(xm)},${f(y - 5.5 - r() * 2.5)} ${f(x1)},${f(y - 2.5 - r() * 1.2)} `;
  }
  d += `Q${f(R + 1)},${f(y - 1)} ${f(R)},${f(y + 5)} `;
  const m = Math.round(w / 12);
  for (let i = m - 1; i >= 0; i--) {
    const xa = L + (R - L) * i / m, xm = L + (R - L) * (i + .5) / m;
    const drip = r() < .28 ? sink + r() * 5 : 1.5 + r() * 3;
    d += `Q${f(xm)},${f(y + 3.5 + drip)} ${f(xa)},${f(y + 3.5 + r() * 1.5)} `;
  }
  return `<path d="${d}Z" fill="${SNOW_SHADE}" transform="translate(0 1.2)"/><path d="${d}Z" fill="${SNOW}"/>`;
}
const SNOW = '#F3F7FB', SNOW_SHADE = '#C4D2E0';

function rng(seed) { let s = seed >>> 0 || 1; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
const f = n => Math.round(n * 10) / 10;
function icicles(x0, x1, y, seed, p, maxLen = 14) {
  const r = rng(seed); let s = '', x = x0 + 4;
  while (x < x1 - 4) { const len = 4 + r() * maxLen, w = 2.4 + r() * 2.2;
    s += `<path d="M${f(x - w)},${y} L${f(x + w)},${y} L${f(x + r() - .5)},${f(y + len)}Z" fill="url(#${p}ice)"/>`; x += 5 + r() * 9; }
  return s;
}
function flake(x, y, size, op = .7, rot = 0) {
  let s = `<g transform="translate(${f(x)} ${f(y)}) rotate(${rot})" stroke="#E4EDF6" stroke-width="${size > 8 ? 1.4 : 1.1}" stroke-linecap="round" opacity="${op}">`;
  for (let i = 0; i < 3; i++) s += `<g transform="rotate(${i * 60})"><line x1="${-size}" y1="0" x2="${size}" y2="0"/>
    <path d="M${f(size * .55)},0 l${f(size * .25)},${f(-size * .25)} M${f(size * .55)},0 l${f(size * .25)},${f(size * .25)} M${f(-size * .55)},0 l${f(-size * .25)},${f(-size * .25)} M${f(-size * .55)},0 l${f(-size * .25)},${f(size * .25)}" fill="none"/></g>`;
  return s + '</g>';
}
function dots(region, n, seed, op = .55) {
  const r = rng(seed); let s = '';
  for (let i = 0; i < n; i++) s += `<circle cx="${f(region[0] + r() * region[2])}" cy="${f(region[1] + r() * region[3])}" r="${f(1 + r() * 1.6)}" fill="#E4EDF6" opacity="${f(op * (.5 + r() * .5))}"/>`;
  return s;
}
function drift(x0, x1, base, h, seed) {
  const r = rng(seed); let d = `M${x0},720 L${x0},${base - h * .5} `; const n = 4;
  for (let i = 0; i < n; i++) { const xa = x0 + (x1 - x0) * (i + 1) / n, xm = x0 + (x1 - x0) * (i + .5) / n;
    d += `Q${f(xm)},${f(base - h - r() * h * .6)} ${f(xa)},${f(base - h * (.3 + r() * .4))} `; }
  return `<path d="${d} L${x1},720Z" fill="${SNOW}"/>`;
}
function leaf(x, y, len, rot, c) {
  const pts = [], n = 6; let d = `M0,0 `;
  for (let i = 1; i <= n; i++) { const t = i / n, w = Math.sin(Math.PI * t) * len * .32 * (i % 2 ? .62 : 1); d += `L${f(len * t)},${f(-w)} `; }
  for (let i = n - 1; i >= 1; i--) { const t = i / n, w = Math.sin(Math.PI * t) * len * .32 * (i % 2 ? .62 : 1); d += `L${f(len * t)},${f(w)} `; }
  return `<g transform="translate(${x} ${y}) rotate(${rot})"><path d="${d}Z" fill="${c}"/><line x1="1" y1="0" x2="${len - 2}" y2="0" stroke="#0F3D1C" stroke-width="1"/></g>`;
}
function holly(x, y, k = 1) {
  return `<g transform="translate(${x} ${y}) scale(${k})">${leaf(0, 0, 24, 200, '#2E8B47')}${leaf(0, 0, 22, 320, '#23753A')}${leaf(0, 0, 18, 255, '#1D6631')}
    <circle cx="-2" cy="-1" r="4.2" fill="#D9343A"/><circle cx="4" cy="2" r="4.2" fill="#E5484D"/><circle cx="1" cy="-6" r="3.8" fill="#C92A30"/>
    <circle cx="-3" cy="-2.4" r="1.2" fill="#fff" opacity=".7"/><circle cx="3" cy=".6" r="1.2" fill="#fff" opacity=".7"/></g>`;
}
function bauble(cx, cy, r, color, lit, p, glow) {
  return `${lit && glow ? `<circle cx="${cx}" cy="${cy}" r="${r + 9}" fill="url(#${p}${glow})"/>` : ''}
    <line x1="${cx}" y1="${cy - r - 4}" x2="${cx}" y2="${cy - r - 14}" stroke="#7A828C" stroke-width="1"/>
    <rect x="${cx - 3.5}" y="${cy - r - 5}" width="7" height="5" rx="1" fill="#C9A23A"/>
    <circle cx="${cx}" cy="${cy}" r="${r}" fill="${color}"/>
    <ellipse cx="${cx - r * .35}" cy="${cy - r * .38}" rx="${r * .28}" ry="${r * .18}" fill="#fff" opacity="${lit ? .6 : .18}" transform="rotate(-35 ${cx - r * .35} ${cy - r * .38})"/>`;
}
function santaHat(x, y) {   // brim's left end at (x, y)
  return `<path d="M${x + 4},${y} C${x + 8},${y - 26} ${x + 34},${y - 34} ${x + 52},${y - 22} C${x + 58},${y - 18} ${x + 60},${y - 10} ${x + 58},${y - 4} C${x + 50},${y - 16} ${x + 40},${y - 16} ${x + 40},${y}Z" fill="#D7262E"/>
    <path d="M${x + 12},${y - 6} C${x + 16},${y - 22} ${x + 30},${y - 28} ${x + 44},${y - 22}" stroke="#F04A50" stroke-width="2" fill="none" opacity=".7"/>
    <rect x="${x - 2}" y="${y - 6}" width="48" height="11" rx="5.5" fill="${SNOW}"/><circle cx="${x + 58}" cy="${y - 2}" r="6" fill="${SNOW}"/>`;
}
function present(x, base, w, h, c, rib) {
  const y = base - h;
  return `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="2" fill="${c}"/><rect x="${x + w / 2 - 3}" y="${y}" width="6" height="${h}" fill="${rib}"/>
    <rect x="${x}" y="${y + h * .38}" width="${w}" height="5" fill="${rib}" opacity=".9"/>
    <ellipse cx="${x + w / 2 - 6}" cy="${y - 3}" rx="7" ry="4" fill="none" stroke="${rib}" stroke-width="3" transform="rotate(-20 ${x + w / 2 - 6} ${y - 3})"/>
    <ellipse cx="${x + w / 2 + 6}" cy="${y - 3}" rx="7" ry="4" fill="none" stroke="${rib}" stroke-width="3" transform="rotate(20 ${x + w / 2 + 6} ${y - 3})"/>`;
}
function snowman(x, base) {
  return `<circle cx="${x}" cy="${base - 13}" r="13" fill="${SNOW}"/><circle cx="${x}" cy="${base - 33}" r="9" fill="${SNOW}"/>
    <circle cx="${x - 3}" cy="${base - 35}" r="1.3" fill="#222"/><circle cx="${x + 3}" cy="${base - 35}" r="1.3" fill="#222"/>
    <path d="M${x},${base - 32} l7,2 l-7,1Z" fill="#F08A24"/><rect x="${x - 10}" y="${base - 27}" width="20" height="4" rx="2" fill="#D7262E"/>
    <rect x="${x + 4}" y="${base - 27}" width="4" height="10" rx="2" fill="#D7262E"/>
    <path d="M${x - 8},${base - 39} Q${x - 7},${base - 52} ${x},${base - 52} Q${x + 7},${base - 52} ${x + 8},${base - 39}Z" fill="#D7262E"/>
    <rect x="${x - 9}" y="${base - 42}" width="18" height="4" rx="2" fill="${SNOW_SHADE}"/><circle cx="${x}" cy="${base - 53}" r="3" fill="${SNOW}"/>`;
}
function garland(x0, x1, y, sag, seed, p) {
  const r = rng(seed), pt = t => [x0 + (x1 - x0) * t, y + 4 * sag * t * (1 - t)];
  let back = '', front = '';
  for (let i = 0; i <= 46; i++) {
    const [gx, gy] = pt(i / 46);
    back += `<circle cx="${f(gx)}" cy="${f(gy)}" r="${f(6 + r() * 2.5)}" fill="#15452A"/>`;
    for (let k = 0; k < 4; k++) { const a = r() * Math.PI * 2, L = 5 + r() * 6;
      front += `<line x1="${f(gx)}" y1="${f(gy)}" x2="${f(gx + Math.cos(a) * L)}" y2="${f(gy + Math.sin(a) * L)}" stroke="${['#2E8B4E', '#3FA862', '#246F3E'][k % 3]}" stroke-width="2" stroke-linecap="round"/>`; }
  }
  let orn = '';
  [[.2, '#E5484D'], [.5, '#F2C14E'], [.8, '#E5484D']].forEach(([t, c], i) => { const [bx, by] = pt(t);
    orn += bauble(f(bx), f(by + 18 + (i === 1 ? 4 : 0)), 6.5, c, true, p); });
  const [cx, cy] = pt(.5);
  const bow = `<g transform="translate(${f(cx)} ${f(cy - 2)})"><path d="M0,0 C-14,-12 -20,4 -4,4Z M0,0 C14,-12 20,4 4,4Z" fill="#D7262E"/>
    <path d="M-2,3 L-9,16 M2,3 L9,16" stroke="#D7262E" stroke-width="3.4" stroke-linecap="round"/><circle r="3.6" fill="#B71C24"/></g>`;
  return back + front + orn + (seed % 2 ? bow : '');
}
