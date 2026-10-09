"use strict";
// Minimal G-code interpreter for previewing toolpaths.
// Produces flat Float32 segment lists (x1,y1,z1,x2,y2,z2) for feed and rapid
// moves, plus the source byte offset of every segment so job progress (the
// "SD:<percent>" field FluidNC reports) can be mapped onto the drawing.
const Gcode = (() => {
  class Grow {
    constructor(Type, n = 6 * 4096) { this.Type = Type; this.a = new Type(n); this.n = 0; }
    push(...v) {
      if (this.n + v.length > this.a.length) {
        const b = new this.Type(Math.max(this.a.length * 2, this.n + v.length));
        b.set(this.a);
        this.a = b;
      }
      for (const x of v) this.a[this.n++] = x;
    }
    done() { return this.a.subarray(0, this.n); }
  }

  const WORD = /([A-Z])\s*([-+]?(?:\d+\.?\d*|\.\d+))/g;

  // Parses text in chunks so large files don't freeze the page.
  // onProgress(fraction) is called between chunks.
  async function parse(text, { rapidRate = 3000, onProgress } = {}) {
    const feed = new Grow(Float32Array), feedOff = new Grow(Uint32Array, 4096);
    const rapid = new Grow(Float32Array), rapidOff = new Grow(Uint32Array, 4096);
    const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];

    const s = { motion: 0, abs: true, inch: false, plane: 17, f: 0, x: 0, y: 0, z: 0 };
    let feedLen = 0, rapidLen = 0, feedTime = 0, lines = 0, tools = 0;

    const extend = (x, y, z) => {
      if (x < min[0]) min[0] = x; if (x > max[0]) max[0] = x;
      if (y < min[1]) min[1] = y; if (y > max[1]) max[1] = y;
      if (z < min[2]) min[2] = z; if (z > max[2]) max[2] = z;
    };
    const seg = (isRapid, x1, y1, z1, x2, y2, z2, off) => {
      const d = Math.hypot(x2 - x1, y2 - y1, z2 - z1);
      if (d === 0) return;
      if (isRapid) { rapid.push(x1, y1, z1, x2, y2, z2); rapidOff.push(off); rapidLen += d; }
      else {
        feed.push(x1, y1, z1, x2, y2, z2); feedOff.push(off); feedLen += d;
        if (s.f > 0) feedTime += d / s.f;
      }
      extend(x1, y1, z1); extend(x2, y2, z2);
    };

    let pos = 0;
    const total = text.length;
    let lastYield = performance.now();

    while (pos < total) {
      let end = text.indexOf("\n", pos);
      if (end < 0) end = total;
      const off = pos;
      let line = text.slice(pos, end);
      pos = end + 1;
      lines++;

      // strip comments
      const sc = line.indexOf(";");
      if (sc >= 0) line = line.slice(0, sc);
      if (line.indexOf("(") >= 0) line = line.replace(/\([^)]*\)/g, "");
      line = line.toUpperCase();
      if (!line.trim() || line[0] === "$" || line[0] === "%") continue;

      const w = {};
      let g53 = false, motionWord = null;
      WORD.lastIndex = 0;
      let m;
      while ((m = WORD.exec(line))) {
        const L = m[1], v = parseFloat(m[2]);
        if (L === "G") {
          switch (v) {
            case 0: case 1: case 2: case 3: motionWord = v; break;
            case 17: case 18: case 19: s.plane = v; break;
            case 20: s.inch = true; break;
            case 21: s.inch = false; break;
            case 90: s.abs = true; break;
            case 91: s.abs = false; break;
            case 53: g53 = true; break;
            case 80: s.motion = -1; break;
            default: break; // G28/G30/G92 etc are ignored for preview
          }
        } else if (L === "M") {
          if (v === 6) tools++;
        } else {
          w[L] = v;
        }
      }
      if (motionWord !== null) s.motion = motionWord;
      const u = s.inch ? 25.4 : 1;
      if (w.F !== undefined) s.f = w.F * u;

      const hasAxis = w.X !== undefined || w.Y !== undefined || w.Z !== undefined;
      if (!hasAxis || g53 || s.motion < 0) continue; // G53 is machine coords: skip in work view

      const tx = w.X === undefined ? s.x : s.abs ? w.X * u : s.x + w.X * u;
      const ty = w.Y === undefined ? s.y : s.abs ? w.Y * u : s.y + w.Y * u;
      const tz = w.Z === undefined ? s.z : s.abs ? w.Z * u : s.z + w.Z * u;

      if (s.motion === 0 || s.motion === 1) {
        seg(s.motion === 0, s.x, s.y, s.z, tx, ty, tz, off);
      } else if (s.motion === 2 || s.motion === 3) {
        arc(s, w, u, tx, ty, tz, off, seg);
      }
      s.x = tx; s.y = ty; s.z = tz;

      if (onProgress && performance.now() - lastYield > 40) {
        onProgress(pos / total);
        await new Promise((r) => setTimeout(r, 0));
        lastYield = performance.now();
      }
    }
    if (!Number.isFinite(min[0])) { min.fill(0); max.fill(0); }
    return {
      feed: feed.done(), feedOff: feedOff.done(),
      rapid: rapid.done(), rapidOff: rapidOff.done(),
      min, max, bytes: total, lines, tools,
      feedLen, rapidLen,
      estMinutes: feedTime + rapidLen / rapidRate,
    };
  }

  // Linearise G2/G3 in the active plane; helical moves interpolate the third axis.
  function arc(s, w, u, tx, ty, tz, off, seg) {
    const cw = s.motion === 2;
    // map plane -> (a, b, c) axis indices of [x, y, z]
    const [ia, ib, ic] = s.plane === 17 ? [0, 1, 2] : s.plane === 18 ? [2, 0, 1] : [1, 2, 0];
    const p0 = [s.x, s.y, s.z], p1 = [tx, ty, tz];
    const offKeys = s.plane === 17 ? ["I", "J"] : s.plane === 18 ? ["K", "I"] : ["J", "K"];
    let ca, cb;
    if (w.R !== undefined) {
      const r = w.R * u;
      const dx = p1[ia] - p0[ia], dy = p1[ib] - p0[ib];
      const h2 = r * r - (dx * dx + dy * dy) / 4;
      if (h2 < -1e-6) { seg(false, ...p0, ...p1, off); return; }
      let h = Math.sqrt(Math.max(h2, 0)) / Math.hypot(dx, dy);
      if (cw !== r < 0) h = -h;
      ca = p0[ia] + dx / 2 - h * dy;
      cb = p0[ib] + dy / 2 + h * dx;
    } else {
      ca = p0[ia] + (w[offKeys[0]] || 0) * u;
      cb = p0[ib] + (w[offKeys[1]] || 0) * u;
    }
    const r = Math.hypot(p0[ia] - ca, p0[ib] - cb);
    let a0 = Math.atan2(p0[ib] - cb, p0[ia] - ca);
    let a1 = Math.atan2(p1[ib] - cb, p1[ia] - ca);
    let sweep = a1 - a0;
    if (cw && sweep >= -1e-9) sweep -= 2 * Math.PI;
    if (!cw && sweep <= 1e-9) sweep += 2 * Math.PI;
    const n = Math.min(256, Math.max(4, Math.ceil((Math.abs(sweep) * r) / 0.5)));
    let prev = p0.slice();
    for (let i = 1; i <= n; i++) {
      const t = i / n, ang = a0 + sweep * t;
      const p = [0, 0, 0];
      p[ia] = ca + r * Math.cos(ang);
      p[ib] = cb + r * Math.sin(ang);
      p[ic] = p0[ic] + (p1[ic] - p0[ic]) * t;
      if (i === n) { p[ia] = p1[ia]; p[ib] = p1[ib]; }
      seg(false, ...prev, ...p, off);
      prev = p;
    }
  }

  // A pendant ".viz" file (header line, then "x,y" per line, work coords) as a
  // flat 2D job: small enough to fetch while a job is running from the card.
  function fromViz(text) {
    const Z = 0.2; // just above the grid so the outline does not vanish into it
    const feed = new Grow(Float32Array), feedOff = new Grow(Uint32Array, 4096);
    const min = [Infinity, Infinity, Z], max = [-Infinity, -Infinity, Z];
    let px = null, py = null, n = 0, feedLen = 0;
    for (const line of text.split(/\r?\n/)) {
      const c = line.indexOf(",");
      if (c < 0) continue;
      const x = parseFloat(line), y = parseFloat(line.slice(c + 1));
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      if (px !== null && (x !== px || y !== py)) {
        feed.push(px, py, Z, x, y, Z); feedOff.push(n); feedLen += Math.hypot(x - px, y - py);
      }
      if (x < min[0]) min[0] = x; if (x > max[0]) max[0] = x;
      if (y < min[1]) min[1] = y; if (y > max[1]) max[1] = y;
      px = x; py = y; n++;
    }
    if (!Number.isFinite(min[0])) { min.fill(0); max.fill(0); }
    return {
      feed: feed.done(), feedOff: feedOff.done(), rapid: new Float32Array(0), rapidOff: new Uint32Array(0),
      min, max, bytes: n, lines: n, tools: 0, feedLen, rapidLen: 0, estMinutes: NaN, flat: true,
    };
  }

  return { parse, fromViz };
})();
