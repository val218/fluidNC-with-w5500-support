"use strict";
// 3D toolpath viewer (three.js, Z up). Renders only when something changed.
const Viewer = (() => {
  let renderer, scene, camera, host;
  let pathGroup, feedLines, rapidLines, bbox, tool, grid, axes;
  let job = null;            // parsed job (from Gcode.parse)
  let baseColors = null;     // per-vertex colours before progress tinting
  let doneSegs = 0;          // feed segments already tinted as done
  let dirty = true;

  // orbit state (spherical around target)
  const target = new THREE.Vector3(0, 0, 0);
  let radius = 200, theta = -Math.PI / 4, phi = Math.PI / 3.2; // theta: around Z, phi: from +Z

  const COLOR_DONE = new THREE.Color(0x3a6f4a);

  function init(el) {
    host = el;
    renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    renderer.setClearColor(0x070d1a);
    host.append(renderer.domElement);

    scene = new THREE.Scene();
    camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100000);
    camera.up.set(0, 0, 1);

    pathGroup = new THREE.Group();
    scene.add(pathGroup);

    axes = makeAxes(20);
    scene.add(axes);

    tool = makeTool(6);
    scene.add(tool);

    setGrid(-10, -10, 200, 200);
    bindControls(renderer.domElement);
    new ResizeObserver(resize).observe(host);
    resize();
    loop();
  }

  function makeAxes(len) {
    const g = new THREE.Group();
    const mk = (color, x, y, z) => {
      const geo = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0, 0, 0), new THREE.Vector3(x, y, z)]);
      g.add(new THREE.Line(geo, new THREE.LineBasicMaterial({ color })));
    };
    mk(0xf85149, len, 0, 0);
    mk(0x3fb950, 0, len, 0);
    mk(0x2f81f7, 0, 0, len);
    return g;
  }

  function makeTool(size) {
    const g = new THREE.Group();
    const cone = new THREE.Mesh(
      new THREE.ConeGeometry(size * 0.35, size, 20),
      new THREE.MeshBasicMaterial({ color: 0xf5c518, transparent: true, opacity: 0.85 })
    );
    cone.rotation.x = -Math.PI / 2; // point down -Z
    cone.position.z = size / 2;
    g.add(cone);
    const shaftGeo = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0, 0, size), new THREE.Vector3(0, 0, size * 3)]);
    g.add(new THREE.Line(shaftGeo, new THREE.LineBasicMaterial({ color: 0xf5c518 })));
    return g;
  }

  function setGrid(x0, y0, x1, y1) {
    if (grid) { scene.remove(grid); grid.geometry.dispose(); }
    const span = Math.max(x1 - x0, y1 - y0, 20);
    const step = Math.pow(10, Math.floor(Math.log10(span / 4)));
    const size = Math.ceil(span / step + 2) * step;
    const divs = Math.round(size / step);
    grid = new THREE.GridHelper(size, divs, 0x24365c, 0x162440);
    grid.rotation.x = Math.PI / 2; // XZ plane -> XY plane
    grid.position.set((x0 + x1) / 2, (y0 + y1) / 2, 0);
    grid.material.transparent = true;
    grid.material.opacity = 0.8;
    scene.add(grid);
    dirty = true;
  }

  // Colour feed moves by depth: top = cyan, deepest = orange.
  function depthColor(z, zmin, zmax, out) {
    const t = zmax > zmin ? (z - zmin) / (zmax - zmin) : 1;
    out.setHSL(0.08 + 0.45 * t, 0.85, 0.55);
    return out;
  }

  function load(parsed) {
    clear();
    job = parsed;
    // colour range from cutting moves only (rapids/retracts would flatten it)
    let zmin = Infinity, zmax = -Infinity;
    for (let i = 2; i < parsed.feed.length; i += 3) {
      const z = parsed.feed[i];
      if (z < zmin) zmin = z;
      if (z > zmax) zmax = z;
    }

    if (parsed.feed.length) {
      const geo = new THREE.BufferGeometry();
      geo.setAttribute("position", new THREE.BufferAttribute(parsed.feed, 3));
      const colors = new Float32Array(parsed.feed.length);
      const c = new THREE.Color();
      for (let i = 0; i < parsed.feed.length; i += 3) {
        depthColor(parsed.feed[i + 2], zmin, zmax, c);
        colors[i] = c.r; colors[i + 1] = c.g; colors[i + 2] = c.b;
      }
      baseColors = colors.slice();
      geo.setAttribute("color", new THREE.BufferAttribute(colors, 3));
      feedLines = new THREE.LineSegments(geo, new THREE.LineBasicMaterial({ vertexColors: true }));
      pathGroup.add(feedLines);
    }
    if (parsed.rapid.length) {
      const geo = new THREE.BufferGeometry();
      geo.setAttribute("position", new THREE.BufferAttribute(parsed.rapid, 3));
      const span = Math.max(...parsed.max.map((v, i) => v - parsed.min[i]), 1);
      rapidLines = new THREE.LineSegments(
        geo,
        new THREE.LineDashedMaterial({ color: 0x6b7fa6, dashSize: span / 120, gapSize: span / 160, transparent: true, opacity: 0.7 })
      );
      rapidLines.computeLineDistances();
      pathGroup.add(rapidLines);
    }
    const box = new THREE.Box3(new THREE.Vector3(...parsed.min), new THREE.Vector3(...parsed.max));
    bbox = new THREE.Box3Helper(box, 0x3a4f78);
    pathGroup.add(bbox);

    setGrid(Math.min(parsed.min[0], 0), Math.min(parsed.min[1], 0), Math.max(parsed.max[0], 0), Math.max(parsed.max[1], 0));
    const size = Math.max(...parsed.max.map((v, i) => v - parsed.min[i]), 10);
    scene.remove(tool);
    tool = makeTool(Math.max(size / 25, 2));
    scene.add(tool);
    scene.remove(axes);
    axes = makeAxes(Math.max(size / 8, 5));
    scene.add(axes);
    fit();
  }

  function clear() {
    for (const o of [feedLines, rapidLines, bbox]) {
      if (o) { pathGroup.remove(o); o.geometry && o.geometry.dispose(); o.material && o.material.dispose(); }
    }
    feedLines = rapidLines = bbox = null;
    job = null; baseColors = null; doneSegs = 0;
    dirty = true;
  }

  // fraction 0..1 of the file processed -> tint completed feed segments
  function setProgress(fraction) {
    if (!job || !feedLines) return;
    const byte = fraction * job.bytes;
    const offs = job.feedOff;
    let lo = 0, hi = offs.length; // count segments with offset < byte
    while (lo < hi) { const mid = (lo + hi) >> 1; if (offs[mid] < byte) lo = mid + 1; else hi = mid; }
    const n = lo;
    if (n === doneSegs) return;
    const col = feedLines.geometry.attributes.color;
    const a = col.array;
    if (n > doneSegs) {
      for (let s = doneSegs; s < n; s++) {
        for (let v = 0; v < 6; v += 3) {
          a[s * 6 + v] = COLOR_DONE.r; a[s * 6 + v + 1] = COLOR_DONE.g; a[s * 6 + v + 2] = COLOR_DONE.b;
        }
      }
    } else {
      a.set(baseColors.subarray(n * 6, doneSegs * 6), n * 6);
    }
    doneSegs = n;
    col.needsUpdate = true;
    dirty = true;
  }

  function setTool(x, y, z) {
    if (![x, y, z].every(Number.isFinite)) return;
    if (tool.position.x === x && tool.position.y === y && tool.position.z === z) return;
    tool.position.set(x, y, z);
    dirty = true;
  }

  function bounds() {
    if (job) return { min: job.min, max: job.max };
    const p = tool.position;
    return { min: [Math.min(0, p.x) - 50, Math.min(0, p.y) - 50, Math.min(0, p.z) - 20], max: [Math.max(0, p.x) + 50, Math.max(0, p.y) + 50, Math.max(0, p.z) + 20] };
  }

  function fit() {
    const b = bounds();
    target.set((b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2);
    const diag = Math.hypot(b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]) || 100;
    radius = (diag / 2) / Math.sin((camera.fov * Math.PI) / 360) * 1.05;
    if (camera.aspect < 1) radius /= camera.aspect;
    dirty = true;
  }

  function view(name) {
    const eps = 1e-4;
    if (name === "top") { theta = -Math.PI / 2; phi = eps; }
    else if (name === "front") { theta = -Math.PI / 2; phi = Math.PI / 2; }
    else if (name === "right") { theta = 0; phi = Math.PI / 2; }
    else { theta = -Math.PI / 4; phi = Math.PI / 3.2; }
    fit();
  }

  function updateCamera() {
    const sp = Math.sin(phi);
    camera.position.set(
      target.x + radius * sp * Math.cos(theta),
      target.y + radius * sp * Math.sin(theta),
      target.z + radius * Math.cos(phi)
    );
    camera.near = Math.max(radius / 1000, 0.01);
    camera.far = radius * 50;
    camera.updateProjectionMatrix();
    camera.lookAt(target);
  }

  function resize() {
    const w = host.clientWidth, h = host.clientHeight;
    if (!w || !h) return;
    renderer.setSize(w, h, false);
    const changed = Math.abs(camera.aspect - w / h) > 0.05;
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
    if (changed) fit();
    dirty = true;
  }

  function loop() {
    requestAnimationFrame(loop);
    if (!dirty) return;
    dirty = false;
    updateCamera();
    renderer.render(scene, camera);
  }

  // ---- mouse / touch orbit, pan, zoom ----
  function bindControls(canvas) {
    const pointers = new Map();
    let mode = null, last = null, pinchDist = 0;

    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    canvas.addEventListener("pointerdown", (e) => {
      canvas.setPointerCapture(e.pointerId);
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pointers.size === 1) {
        mode = e.button === 2 || e.button === 1 || e.shiftKey ? "pan" : "rotate";
        last = { x: e.clientX, y: e.clientY };
      } else if (pointers.size === 2) {
        mode = "pinch";
        const [a, b] = [...pointers.values()];
        pinchDist = Math.hypot(a.x - b.x, a.y - b.y);
        last = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
      }
    });
    canvas.addEventListener("pointermove", (e) => {
      if (!pointers.has(e.pointerId)) return;
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (mode === "rotate" && pointers.size === 1) {
        const dx = e.clientX - last.x, dy = e.clientY - last.y;
        theta -= dx * 0.008;
        phi = Math.min(Math.PI - 1e-4, Math.max(1e-4, phi - dy * 0.008));
        last = { x: e.clientX, y: e.clientY };
        dirty = true;
      } else if (mode === "pan" && pointers.size === 1) {
        pan(e.clientX - last.x, e.clientY - last.y);
        last = { x: e.clientX, y: e.clientY };
      } else if (mode === "pinch" && pointers.size === 2) {
        const [a, b] = [...pointers.values()];
        const d = Math.hypot(a.x - b.x, a.y - b.y);
        if (pinchDist > 0) radius *= pinchDist / d;
        pinchDist = d;
        const c = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
        pan(c.x - last.x, c.y - last.y);
        last = c;
        dirty = true;
      }
    });
    const up = (e) => {
      pointers.delete(e.pointerId);
      if (pointers.size === 1) {
        const [p] = [...pointers.values()];
        mode = "rotate"; last = { x: p.x, y: p.y };
      } else if (pointers.size === 0) mode = null;
    };
    canvas.addEventListener("pointerup", up);
    canvas.addEventListener("pointercancel", up);
    canvas.addEventListener("wheel", (e) => {
      e.preventDefault();
      radius *= Math.exp(e.deltaY * 0.0015);
      dirty = true;
    }, { passive: false });
    canvas.addEventListener("dblclick", fit);
  }

  function pan(dx, dy) {
    const h = host.clientHeight || 1;
    const scale = (2 * radius * Math.tan((camera.fov * Math.PI) / 360)) / h;
    const right = new THREE.Vector3().setFromMatrixColumn(camera.matrix, 0);
    const up = new THREE.Vector3().setFromMatrixColumn(camera.matrix, 1);
    target.addScaledVector(right, -dx * scale).addScaledVector(up, dy * scale);
    dirty = true;
  }

  return { init, load, clear, setProgress, setTool, view, fit, resize, get job() { return job; } };
})();
