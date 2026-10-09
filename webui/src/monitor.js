"use strict";
// Axis heartbeat monitor (bottom strip): one status light per axis plus a
// scrolling 30 s chart of each axis' velocity, derived from successive MPos
// values in the status reports. A "link" light blinks on every report so a
// stalled connection is obvious at a glance.
const Monitor = (() => {
  const AXES = ["X", "Y", "Z", "A", "B", "C"];
  const COLORS = ["#ff6b6b", "#4cd97b", "#26acff", "#c792ff", "#ffb547", "#5ee6e6"];
  const SAMPLE_MS = 100, SPAN_S = 30, N = (SPAN_S * 1000) / SAMPLE_MS;
  const MOVE_EPS = 1; // mm/min below this counts as stopped

  let canvas, ctx, chips, linkLed, rateEl;
  let nAxes = 3;
  let hist = [];            // ring of Float32Array(nAxes), oldest first
  let last = null;          // { t, m: [...] } previous report
  let vel = [0, 0, 0];      // latest velocity per axis (mm/min)
  let lastReport = 0, reports = [];
  let state = "Unknown", pins = "";
  let timer = 0, visible = true;

  function init(root) {
    canvas = root.querySelector("canvas");
    ctx = canvas.getContext("2d");
    chips = root.querySelector(".mon-axes");
    linkLed = root.querySelector(".mon-link .led");
    rateEl = root.querySelector(".mon-rate");
    buildChips(3);
    timer = setInterval(sample, SAMPLE_MS);
    new ResizeObserver(draw).observe(canvas);
  }

  function setVisible(v) { visible = v; if (v) draw(); }

  function buildChips(n) {
    nAxes = n;
    vel = new Array(n).fill(0);
    hist = [];
    chips.textContent = "";
    for (let i = 0; i < n; i++) {
      chips.append(
        el("div", { class: "mon-axis", id: "mon-" + AXES[i] },
          el("span", { class: "led" }),
          el("b", { style: `color:${COLORS[i]}` }, AXES[i]),
          el("span", { class: "mon-v" }, "0"))
      );
    }
  }

  // Called with every parsed status report.
  function status(st, mpos) {
    const now = performance.now();
    state = st.state;
    pins = st.pins || "";
    if (mpos.length !== nAxes) buildChips(mpos.length);
    if (last && now > last.t) {
      const dt = now - last.t;
      for (let i = 0; i < nAxes; i++) vel[i] = ((mpos[i] - last.m[i]) / dt) * 60000;
    }
    last = { t: now, m: mpos.slice() };
    lastReport = now;
    reports.push(now);
    while (reports.length && now - reports[0] > 2000) reports.shift();
    linkLed.classList.remove("beat");
    void linkLed.offsetWidth; // restart the CSS animation
    linkLed.classList.add("beat");
  }

  function sample() {
    const now = performance.now();
    // Reports stop when nothing changes (idle) - that means zero velocity.
    // During motion they arrive every 100-200 ms, so 600 ms of silence = stopped.
    if (now - lastReport > 600) vel.fill(0);
    if (now - lastReport > 3000) linkLed.classList.add("dead");
    else linkLed.classList.remove("dead");
    hist.push(Float32Array.from(vel));
    if (hist.length > N) hist.shift();
    const hz = reports.length > 1 ? (reports.length - 1) / ((reports[reports.length - 1] - reports[0]) / 1000) : 0;
    rateEl.textContent = hz ? `${hz.toFixed(1)} Hz` : "idle";
    for (let i = 0; i < nAxes; i++) {
      const chip = document.getElementById("mon-" + AXES[i]);
      if (!chip) continue;
      const v = vel[i];
      chip.querySelector(".mon-v").textContent = Math.abs(v) < MOVE_EPS ? "0" : Math.round(v).toString();
      const led = chip.querySelector(".led");
      led.className = "led " + (
        pins.includes(AXES[i]) ? "led-limit" :
        state === "Alarm" ? "led-alarm" :
        Math.abs(v) >= MOVE_EPS ? "led-move" :
        state === "Hold" || state.startsWith("Hold") ? "led-hold" : "led-idle");
    }
    if (visible) draw();
  }

  function niceMax(v) {
    if (v < 100) return 100;
    const p = Math.pow(10, Math.floor(Math.log10(v)));
    for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
    return 10 * p;
  }

  function draw() {
    if (!canvas || !canvas.clientWidth) return;
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    const css = getComputedStyle(document.documentElement);
    const grid = css.getPropertyValue("--line").trim() || "#24365c";
    const muted = css.getPropertyValue("--muted").trim() || "#8b9bb8";

    let peak = 0;
    for (const s of hist) for (let i = 0; i < s.length; i++) peak = Math.max(peak, Math.abs(s[i]));
    const ymax = niceMax(peak * 1.1);
    const left = 44, right = w - 4, top = 4, bottom = h - 14;
    const mid = (top + bottom) / 2, half = (bottom - top) / 2;
    const y = (v) => mid - (v / ymax) * half;
    const x = (k) => right - ((N - 1 - k) / (N - 1)) * (right - left);

    ctx.font = "10px ui-monospace, Menlo, Consolas, monospace";
    ctx.fillStyle = muted;
    ctx.strokeStyle = grid;
    ctx.lineWidth = 1;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (const f of [1, 0.5, 0, -0.5, -1]) {
      const yy = Math.round(y(f * ymax)) + 0.5;
      ctx.globalAlpha = f === 0 ? 1 : 0.5;
      ctx.beginPath(); ctx.moveTo(left, yy); ctx.lineTo(right, yy); ctx.stroke();
      ctx.globalAlpha = 1;
      if (f !== 0.5 && f !== -0.5) ctx.fillText(f === 0 ? "0" : (f * ymax).toFixed(0), left - 4, yy);
    }
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (let s = 0; s <= SPAN_S; s += 5) {
      const xx = right - (s / SPAN_S) * (right - left);
      ctx.fillText(s ? `-${s}s` : "now", Math.min(xx, right - 12), bottom + 2);
    }

    const off = N - hist.length;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = "round";
    for (let i = 0; i < nAxes; i++) {
      ctx.strokeStyle = COLORS[i];
      ctx.beginPath();
      for (let k = 0; k < hist.length; k++) {
        const v = hist[k][i] || 0;
        const px = x(k + off), py = y(v);
        k ? ctx.lineTo(px, py) : ctx.moveTo(px, py);
      }
      ctx.stroke();
    }
    ctx.textAlign = "left";
    ctx.fillStyle = muted;
    ctx.fillText("mm/min", left + 4, top);
  }

  return { init, status, setVisible, draw };
})();
