"use strict";
// UI glue: DRO, jogging, machine buttons, overrides, files, console, job progress.
(() => {
  const AXES = ["X", "Y", "Z", "A", "B", "C"];
  const S = {
    state: "Unknown", mpos: [0, 0, 0], wco: [0, 0, 0], nAxes: 3, pins: "", acc: "",
    alarm: null, ov: [100, 100, 100], job: null, fs: "sd", dir: "/", sel: null,
  };
  const idle = () => S.state === "Idle";
  const canJog = () => S.state === "Idle" || S.state === "Jog";

  // ------------------------------------------------------------- console
  const logEl = $("#log");
  const showStatus = $("#show-status");
  function log(text, cls = "") {
    const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 30;
    const line = el("span", cls ? { class: cls } : {}, text + "\n");
    logEl.append(line);
    while (logEl.childNodes.length > 1500) logEl.firstChild.remove();
    if (atBottom) logEl.scrollTop = logEl.scrollHeight;
  }
  Conn.on("tx", (t) => log("> " + t, "tx"));
  $("#clear-console").onclick = () => (logEl.textContent = "");

  const hist = Prefs.get("history", []);
  let histPos = hist.length;
  $("#cmd-form").onsubmit = (e) => {
    e.preventDefault();
    const input = $("#cmd");
    const t = input.value.trim();
    if (!t) return;
    send(t);
    if (hist[hist.length - 1] !== t) hist.push(t);
    while (hist.length > 100) hist.shift();
    Prefs.set("history", hist);
    histPos = hist.length;
    input.value = "";
  };
  $("#cmd").addEventListener("keydown", (e) => {
    if (e.key === "ArrowUp" && histPos > 0) { e.target.value = hist[--histPos]; e.preventDefault(); }
    if (e.key === "ArrowDown") { histPos = Math.min(hist.length, histPos + 1); e.target.value = hist[histPos] || ""; e.preventDefault(); }
  });

  function send(line) {
    if (!Conn.sendLine(line)) toast("Not connected", true);
  }

  // ------------------------------------------------------------- incoming lines
  let ipWaiter = null;  // [ESP111] answer for the PC-drive script
  Conn.on("line", (line) => {
    if (ipWaiter && /^\d{1,3}(\.\d{1,3}){3}$/.test(line.trim())) {
      ipWaiter(line.trim());
      ipWaiter = null;
      return;
    }
    if (line[0] === "<") {
      if (showStatus.checked) log(line, "st");
      return;
    }
    if (line === "ok") return;
    let m;
    if ((m = /^error:(\d+)/.exec(line))) {
      const desc = ERRORS[m[1]] || "";
      log(line + (desc ? "  (" + desc + ")" : ""), "err");
      toast("Error " + m[1] + (desc ? ": " + desc : ""), true);
      return;
    }
    if ((m = /^ALARM:(\d+)/i.exec(line))) {
      S.alarm = Number(m[1]);
      log(line + "  (" + (ALARMS[m[1]] || "alarm") + ")", "err");
      renderAlarm();
      return;
    }
    if ((m = /^\[GC:(.*)\]/.exec(line))) {
      const wcs = /G5[4-9](\.\d)?/.exec(m[1]);
      if (wcs) $("#wcs").value = wcs[0].slice(0, 3);
      log(line, "msg");
      return;
    }
    if ((m = /^\[MSG:Prepared:(\w+):(.*)\]$/.exec(line))) {
      setPrepared(m[1], m[2]);
      log(line, "msg");
      return;
    }
    if ((m = /^\[MSG:JobWait:(.*)\]$/.exec(line))) {
      // The board holds a job until the pendant shows its preview.
      toast("Job: " + m[1], m[1].includes("cancel"));
      log(line, "msg");
      return;
    }
    if ((m = /^\[MSG:Pendant:(\w+)\]$/.exec(line))) {
      setPendant(m[1]);
      return;
    }
    if ((m = /^\[MSG:Viz(Auto(?:Ready|Busy|Err|Queued)|Ready|Busy|Err|Deleted|Status):?(.*)\]$/.exec(line))) {
      onVizMsg(m[1], m[2]);
      log(line, m[1].endsWith("Err") ? "err" : "msg");
      return;
    }
    if (/^Grbl |^\[VER:/.test(line) && line.startsWith("Grbl")) {
      log(line, "msg");
      setTimeout(Conn.resync, 300);
      return;
    }
    log(line, line[0] === "[" ? "msg" : "");
  });

  // ------------------------------------------------------------- connection state
  Conn.on("state", (s) => {
    const c = $("#conn");
    c.textContent = s === "open" ? "online" : s === "connecting" ? "connecting…" : "offline";
    c.className = "conn conn-" + s;
    if (s !== "open") { setStateBadge("Unknown"); $("#pendant").hidden = true; }
  });

  // TabUI pendant on uart_channel1 ($Pendant/Status, plus a message on change)
  function setPendant(st) {
    const p = $("#pendant");
    p.hidden = st === "none";
    p.className = "pendant pendant-" + st;
    p.textContent = st === "connected" ? "Pendant" : "Pendant offline";
    p.title = st === "connected" ? "TabUI pendant connected (UART1)" : "No TabUI pendant on UART1";
  }

  function setStateBadge(state) {
    const b = $("#state");
    b.textContent = state === "Unknown" ? "--" : state;
    b.className = "state state-" + state.toLowerCase();
  }
  function renderAlarm() {
    const a = $("#alarm");
    if (S.state === "Alarm") {
      a.hidden = false;
      a.textContent = S.alarm ? `Alarm ${S.alarm}: ${ALARMS[S.alarm] || ""}` : "Alarm - unlock ($X) or home";
    } else {
      a.hidden = true;
      S.alarm = null;
    }
  }

  // ------------------------------------------------------------- DRO
  const dro = $("#dro");
  function buildDro(n) {
    dro.textContent = "";
    for (let i = 0; i < n; i++) {
      const ax = AXES[i];
      dro.append(
        el("div", { class: "axis", id: "ax-" + ax },
          el("span", { class: "name" }, ax),
          el("span", { class: "wpos" }, "0.000"),
          el("span", { class: "mpos" }, "0.000"),
          el("button", { class: "btn z0", title: `Set ${ax} work zero here`, onclick: () => zeroAxis(ax) }, ax + "0"))
      );
    }
  }
  buildDro(3);

  function zeroAxis(ax) {
    if (!idle()) return toast("Machine must be idle", true);
    send(`G10 L20 P0 ${ax}0`);
  }

  Conn.on("status", (st) => {
    S.state = st.state;
    setStateBadge(st.state);
    renderAlarm();
    if (st.wco) S.wco = st.wco;
    let m = st.mpos, w = st.wpos;
    if (m) w = m.map((v, i) => v - (S.wco[i] || 0));
    else if (w) m = w.map((v, i) => v + (S.wco[i] || 0));
    if (!m) return;
    S.mpos = m;
    if (m.length !== S.nAxes) { S.nAxes = m.length; buildDro(m.length); }
    for (let i = 0; i < m.length; i++) {
      const row = $("#ax-" + AXES[i]);
      row.children[1].textContent = fmt(w[i]);
      row.children[2].textContent = fmt(m[i]);
      row.classList.toggle("limit", st.pins.includes(AXES[i]));
    }
    Viewer.setTool(w[0], w[1], w[2]);
    Monitor.status(st, m);
    $("#pins").textContent = st.pins ? "Inputs active: " + st.pins.split("").join(" ") : "";

    if (st.ov) {
      S.ov = st.ov;
      $("#ov-f").textContent = st.ov[0] + "%";
      $("#ov-r").textContent = st.ov[1] + "%";
      $("#ov-s").textContent = st.ov[2] + "%";
    }
    if (st.feed !== undefined) $("#feedspeed").textContent = `F ${Math.round(st.feed)} · S ${Math.round(st.speed || 0)}`;
    $("#btn-flood").classList.toggle("on", st.acc.includes("F"));
    $("#btn-mist").classList.toggle("on", st.acc.includes("M"));
    $$('[data-cmd="spindle-cw"]').forEach((b) => b.classList.toggle("on", st.acc.includes("S")));
    jobStatus(st, w);
  });

  // ------------------------------------------------------------- job progress
  function jobStatus(st, wpos) {
    const running = st.sdPct !== undefined && !Number.isNaN(st.sdPct);
    const jobEl = $("#job");
    if (running) {
      if (!S.job || S.job.file !== st.sdFile) S.job = { file: st.sdFile, start: Date.now() };
      ensurePreview(st.sdFile);
      const filePct = Math.max(0, Math.min(100, st.sdPct));
      // Accurate %: where the tool is on the parsed path, weighted by estimated
      // time. Falls back to FluidNC's file-read % (runs ahead of the tool by
      // the planner buffer, and counts bytes, not machining time).
      const [jfs, jpath] = jobFsPath(st.sdFile);
      const onScreen = Viewer.job && $("#viewer-file").dataset.path === cacheKey(jfs, jpath);
      const p = onScreen ? Viewer.setProgress(filePct / 100, wpos, st.line) : null;
      const pct = p === null ? filePct : p * 100;
      const elapsed = (Date.now() - S.job.start) / 1000;
      // Page opened mid-job: measure the rate from the first % we saw.
      if (S.job.p0 === undefined) S.job.p0 = pct;
      const done = pct - S.job.p0;
      const eta = done > 0.3 && elapsed > 5 ? (elapsed * (100 - pct)) / done : NaN;
      jobEl.hidden = false;
      jobEl.textContent = `${st.sdFile.split("/").pop()} · ${pct.toFixed(1)}% · ${fmtTime(elapsed)} · ETA ${fmtTime(eta)}`;
      jobEl.title = `Tool position on the path: ${p === null ? "n/a" : (p * 100).toFixed(1) + "%"} · file read by FluidNC: ${filePct.toFixed(1)}%`;
      $("#progress-bar").style.width = pct + "%";
    } else if (S.job) {
      // job finished or stopped
      if (idle()) {
        log(`Job ended: ${S.job.file} (${fmtTime((Date.now() - S.job.start) / 1000)})`, "msg");
        if ($("#viewer-file").dataset.partial) ensurePreview(S.job.file);
        S.job = null;
        jobEl.hidden = true;
      }
    }
  }

  // ------------------------------------------------------------- machine buttons
  const actions = {
    home: () => send("$H"),
    unlock: () => send("$X"),
    reset: () => Conn.realtime(0x18),
    hold: () => Conn.realtime(0x21),     // !
    resume: () => Conn.realtime(0x7e),   // ~
    stop: () => { Conn.realtime(0x21); setTimeout(() => Conn.realtime(0x18), 250); },
    "jog-cancel": () => Conn.realtime(0x85),
    "spindle-cw": () => send("M3 S" + (Number($("#rpm").value) || 0)),
    "spindle-off": () => send("M5"),
    flood: () => Conn.realtime(0xa0),
    mist: () => Conn.realtime(0xa1),
    "zero-all": () => {
      if (!idle()) return toast("Machine must be idle", true);
      send("G10 L20 P0 " + AXES.slice(0, S.nAxes).map((a) => a + "0").join(" "));
    },
    "goto-xy0": () => { if (!idle()) return toast("Machine must be idle", true); send("G90 G0 X0 Y0"); },
    "goto-z0": () => { if (!idle()) return toast("Machine must be idle", true); send("G90 G0 Z0"); },
  };
  $$("[data-cmd]").forEach((b) => b.addEventListener("click", () => actions[b.dataset.cmd]()));
  $$("[data-ovr]").forEach((b) => b.addEventListener("click", () => Conn.realtime(parseInt(b.dataset.ovr, 16))));
  $("#wcs").addEventListener("change", (e) => { send(e.target.value); send("$G"); });

  // ------------------------------------------------------------- jogging
  let step = Prefs.get("step", "1");
  const feedInput = $("#jogfeed");
  feedInput.value = Prefs.get("jogfeed", 1500);
  feedInput.onchange = () => Prefs.set("jogfeed", Number(feedInput.value));
  function setStep(v) {
    step = v;
    Prefs.set("step", v);
    $$("#step button").forEach((b) => b.classList.toggle("active", b.dataset.step === v));
  }
  setStep(step);
  $$("#step button").forEach((b) => (b.onclick = () => setStep(b.dataset.step)));

  // "X1 Y-1" -> {X:1, Y:-1}
  const dirs = (spec) => Object.fromEntries(spec.split(" ").map((t) => [t[0], Number(t.slice(1))]));

  function jog(spec, distance) {
    if (!canJog()) { toast(`Can't jog while ${S.state}`, true); return false; }
    const f = Math.max(10, Number(feedInput.value) || 1000);
    const words = Object.entries(dirs(spec)).map(([a, d]) => a + (d * distance).toFixed(4)).join(" ");
    Conn.sendLine(`$J=G91 G21 ${words} F${f}`, true);
    return true;
  }
  let continuous = false;
  function jogStart(spec) {
    if (step === "cont") {
      continuous = jog(spec, 10000) ; // long move; cancelled on release
    } else {
      jog(spec, Number(step));
    }
  }
  function jogStop() {
    if (continuous) { Conn.realtime(0x85); continuous = false; }
  }
  $$("[data-jog]").forEach((b) => {
    b.addEventListener("pointerdown", (e) => { e.preventDefault(); b.setPointerCapture(e.pointerId); jogStart(b.dataset.jog); });
    b.addEventListener("pointerup", jogStop);
    b.addEventListener("pointercancel", jogStop);
    b.addEventListener("lostpointercapture", jogStop);
  });
  // safety: stop continuous jog whenever focus is lost
  window.addEventListener("blur", jogStop);
  document.addEventListener("visibilitychange", () => document.hidden && jogStop());
  Conn.on("state", (s) => s !== "open" && (continuous = false));

  const kb = $("#kbjog");
  kb.checked = false;
  const keyMap = { ArrowLeft: "X-1", ArrowRight: "X1", ArrowUp: "Y1", ArrowDown: "Y-1", PageUp: "Z1", PageDown: "Z-1" };
  document.addEventListener("keydown", (e) => {
    if (!kb.checked || !keyMap[e.key] || /INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName)) return;
    e.preventDefault();
    if (e.repeat) return;
    jogStart(keyMap[e.key]);
  });
  document.addEventListener("keyup", (e) => { if (keyMap[e.key]) jogStop(); });

  // ------------------------------------------------------------- viewer
  Viewer.init($("#viewer"));
  $$("#views button").forEach((b) => (b.onclick = () => {
    $$("#views button").forEach((x) => x.classList.toggle("active", x === b));
    Viewer.view(b.dataset.v);
  }));
  $("#fit").onclick = () => Viewer.fit();

  // Parsed files are kept (last 3) so re-opening or running a file is instant.
  const parsedCache = new Map();
  const cacheKey = (fs, path) => fs + ":" + path;
  const forget = (fs, path) => parsedCache.delete(cacheKey(fs, path));
  const moving = () => ["Run", "Jog", "Home"].includes(S.state);

  function showJob(job, key, partial) {
    const label = $("#viewer-file");
    Viewer.load(job);
    const name = key.slice(key.indexOf(":") + 1).split("/").pop();
    label.textContent = name + (partial ? " · preview" : "");
    label.dataset.path = key;
    label.dataset.partial = partial ? "1" : "";
    $("#load-full").hidden = !partial;
    const size = job.max.map((v, i) => v - job.min[i]);
    $("#viewer-info").textContent = partial
      ? `${size.map((v) => v.toFixed(1)).join(" × ")} mm · simplified path from the .viz · ` +
        "the full 3D path loads when the machine stops"
      : `${size.map((v) => v.toFixed(1)).join(" × ")} mm · Z ${job.min[2].toFixed(2)}…${job.max[2].toFixed(2)}` +
        ` · cut ${(job.feedLen / 1000).toFixed(2)} m · est. ${fmtTime(job.estMinutes * 60)}` +
        (job.tools ? ` · ${job.tools} tool change(s)` : "");
  }

  async function previewFile(fs, path) {
    const key = cacheKey(fs, path);
    const label = $("#viewer-file");
    let job = parsedCache.get(key);
    if (!job) {
      label.textContent = "Loading " + path + "…";
      const text = await Files.getText(fs, path);
      label.textContent = "Parsing " + path + "…";
      job = await Gcode.parse(text, {
        onProgress: (f) => (label.textContent = `Parsing ${path}… ${Math.round(f * 100)}%`),
      });
      parsedCache.set(key, job);
      while (parsedCache.size > 3) parsedCache.delete(parsedCache.keys().next().value);
    }
    showJob(job, key, false);
    $("#progress-bar").style.width = "0%";
  }

  // "/sd/a.nc" -> ["sd", "/a.nc"]; "/littlefs/a.nc" -> ["flash", "/a.nc"]
  function jobFsPath(file) {
    const m = /^\/(sd|littlefs|spiffs|localfs)(\/.*)$/i.exec(file);
    if (!m) return ["sd", file.startsWith("/") ? file : "/" + file];
    return [m[1].toLowerCase() === "sd" ? "sd" : "flash", m[2]];
  }

  // Make sure the running job's path is on screen. Called on every status
  // report while a job runs; does at most one fetch at a time. While the
  // machine moves it only fetches the small pendant .viz (FluidNC avoids
  // serving big files mid-cut, and the job reads from the same card); the
  // full 3D path is loaded as soon as the machine stops (hold, end of job).
  let previewBusy = false, previewRetryAt = 0;
  async function ensurePreview(file) {
    if (!file || previewBusy || Date.now() < previewRetryAt) return;
    const [fs, path] = jobFsPath(file);
    const key = cacheKey(fs, path);
    const cur = $("#viewer-file").dataset;
    const shown = Viewer.job && cur.path === key;
    if (shown && !cur.partial) return;
    if (shown && moving()) return;  // 2D outline already up; wait for a stop
    previewBusy = true;
    try {
      if (parsedCache.has(key) || !moving()) {
        await previewFile(fs, path);
      } else {
        let job = null;
        if (fs === "sd") job = Gcode.fromViz(await Files.getText("sd", path + ".viz").catch(() => ""));
        if (job && job.feed.length) {
          showJob(job, key, true);
        } else {
          Viewer.clear();
          $("#viewer-file").textContent = key.split("/").pop();
          $("#viewer-file").dataset.path = key;
          $("#viewer-file").dataset.partial = "1";
          $("#load-full").hidden = false;
          $("#viewer-info").textContent = "No pendant .viz for this file - the path loads when the machine stops, or press Load path";
          previewRetryAt = Date.now() + 4000;  // FluidNC is building it; look again shortly
        }
      }
    } catch (e) {
      $("#viewer-info").textContent = "Could not load the job's path: " + e.message + " (retrying)";
      previewRetryAt = Date.now() + 10000;
    } finally {
      previewBusy = false;
    }
  }
  $("#load-full").onclick = () => {
    const key = $("#viewer-file").dataset.path;
    if (!key) return;
    if (moving() && !confirm("Load the full path now? This reads the whole file from the SD card while the job is running from it; on large files it can slow the job. Otherwise it loads by itself when the machine stops.")) return;
    const fs = key.slice(0, key.indexOf(":")), path = key.slice(key.indexOf(":") + 1);
    previewBusy = true;
    previewFile(fs, path).catch((e) => toast(e.message, true)).finally(() => (previewBusy = false));
  };

  // ------------------------------------------------------------- TabUI pendant .viz
  // The pendant shows a preview from "<file>.viz" next to the G-code on the SD card.
  // FluidNC's VizGenerator builds it on request ($Viz/Generate=/sd/<file>), a few
  // milliseconds at a time, so the controller keeps answering meanwhile.
  const GCODE_RE = /\.(nc|gcode|gc|ngc|tap|cnc|g)$/i;
  // The firmware queues the build itself whenever G-code lands on /sd (this UI,
  // a mapped network drive, the classic UI). Its messages say VizAuto* so the
  // pendant, which loads any "VizReady" it sees, is not disturbed.
  function onVizMsg(kind, rest) {
    const auto = kind.startsWith("Auto");
    const k = auto ? kind.slice(4) : kind;
    const name = rest.split(":")[0].replace(/^.*\//, "").replace(/\.viz$/, "");
    const ncPath = rest.split(":")[0].replace(/\.viz$/, "");
    if (k === "Queued" || k === "Busy") setVizState(ncPath, "building");
    else if (k === "Ready") setVizState(ncPath, "ok");
    else if (k === "Err") { const m = /(\/sd\/[^:()]*?)(\.viz)?(?: \(|:|$)/i.exec(rest); if (m) setVizState(m[1].trim(), "missing"); }
    if (k === "Queued") {
      $("#viz-status").textContent = moving() || S.job
        ? `Pendant .viz ${name}: waiting until the machine stops / the job ends`
        : `Pendant .viz ${name}: starting…`;
      return;
    }
    if (k === "Busy") {
      const pct = /:(\d+)$/.exec(rest);
      $("#viz-status").textContent = `Building pendant .viz ${name}` + (pct ? `: ${pct[1]}%` : "…");
      return;
    }
    if (k !== "Ready" && k !== "Err") return;
    $("#viz-status").textContent = "";
    if (k === "Ready") {
      // The running job's preview was just built (FluidNC builds it when a job
      // starts without one): show it instead of the "no .viz" placeholder.
      const vp = rest.split(":")[0].replace(/\.viz$/, "");
      const cur = $("#viewer-file").dataset;
      if (cur.partial && !Viewer.job && cur.path === "sd:" + vp.replace(/^\/sd/i, "")) delete cur.path;
    }
    if (!auto) return;  // pendant's own requests: leave them to the pendant
    if (k === "Ready") {
      const pts = rest.split(":")[1];
      toast(`Pendant .viz ready: ${name}` + (pts ? ` (${pts} points)` : ""));
    } else {
      toast("Pendant .viz failed: " + rest, true);
    }
  }
  // ------------------------------------------------------------- PC drive script
  // Downloads tools/windows/dpcreator-sd-drive.cmd with this board's IP filled
  // in: double-click on the PC maps the SD card as drive S: (WebDAV over LAN).
  const SD_DRIVE_CMD = __SD_DRIVE_CMD__;
  function boardIp() {
    const host = HOST.replace(/:\d+$/, "");
    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(host)) return Promise.resolve(host);
    // Opened by name (e.g. fluidnc.local): ask FluidNC for its IP.
    return new Promise((resolve) => {
      ipWaiter = resolve;
      Conn.sendLine("[ESP111]", true);
      setTimeout(() => { if (ipWaiter === resolve) { ipWaiter = null; resolve(host); } }, 2500);
    });
  }
  $("#pc-drive").onclick = async () => {
    const ip = await boardIp();
    const text = SD_DRIVE_CMD
      .replace(/set "DEFAULT_IP=[^"]*"/, `set "DEFAULT_IP=${ip}"`)
      .replace(/set "ASK_IP=1"/, 'set "ASK_IP=0"');
    const a = el("a", { href: URL.createObjectURL(new Blob([text], { type: "application/octet-stream" })),
                         download: `dpcreator-sd-drive-${ip}.cmd` });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    toast(`Downloaded the drive script for ${ip}: double-click it on your PC (not as administrator)`);
  };

  $("#make-viz").onclick = () => {
    const it = S.sel;
    if (!it) return;
    const big = it.size > 2 * 1024 * 1024;
    const msg =
      `Build the TabUI pendant preview for ${it.name} (${fmtSize(it.size)})?\n\n` +
      "The controller reads the whole file in the background" +
      (big ? " - for a file this size that can take a minute or more." : ".") +
      "\nIt starts only while the machine is not moving and no job is running.";
    if (!confirm(msg)) return;
    send("$Viz/Refresh=/sd" + it.path);
  };

  // ------------------------------------------------------------- files
  const listEl = $("#file-list");
  $$("#fs button").forEach((b) => (b.onclick = () => {
    $$("#fs button").forEach((x) => x.classList.toggle("active", x === b));
    S.fs = b.dataset.fs; S.dir = "/"; refresh();
  }));
  $("#refresh").onclick = () => refresh();

  async function refresh() {
    selectFile(null);
    $("#file-path").textContent = (S.fs === "sd" ? "SD" : "Flash") + ":" + S.dir;
    listEl.textContent = "";
    listEl.append(el("li", { class: "muted" }, "Loading…"));
    try {
      const items = await Files.list(S.fs, S.dir);
      listEl.textContent = "";
      if (S.dir !== "/") {
        listEl.append(el("li", { class: "dir", onclick: () => { S.dir = S.dir.replace(/\/[^/]+\/?$/, "") || "/"; refresh(); } },
          el("span", { class: "fname" }, "..")));
      }
      // Which G-code files already have their pendant preview (<file>.viz).
      const vizNames = new Set(items.filter((i) => !i.dir && /\.viz$/i.test(i.name)).map((i) => i.name.slice(0, -4)));
      for (const it of items) {
        if (!it.dir && /\.viz(\.tmp)?$/i.test(it.name)) continue; // pendant preview sidecars
        const gcode = !it.dir && S.fs === "sd" && GCODE_RE.test(it.name);
        if (gcode) it.viz = vizNames.has(it.name) ? "ok" : "missing";
        const li = el("li", { class: it.dir ? "dir" : "" },
          el("span", { class: "fname" }, it.name),
          gcode ? el("span", { class: "vizdot" }) : "",
          el("span", { class: "fsize" }, it.dir ? "" : fmtSize(it.size)));
        if (gcode) { li.dataset.path = it.path; li._item = it; paintViz(li, it.viz); }
        li.onclick = () => {
          if (it.dir) { S.dir = it.path; refresh(); return; }
          $$("li", listEl).forEach((x) => x.classList.toggle("sel", x === li));
          selectFile(it);
        };
        li.ondblclick = () => !it.dir && previewSelected();
        listEl.append(li);
      }
      if (!items.length) listEl.append(el("li", { class: "muted" }, "Empty"));
    } catch (e) {
      listEl.textContent = "";
      listEl.append(el("li", { class: "muted" }, e.message + (S.fs === "sd" ? " - is an SD card inserted?" : "")));
    }
  }

  function selectFile(it) {
    S.sel = it;
    $("#file-run").hidden = !it;
    if (!it) return;
    $("#sel-name").textContent = it.name;
    $("#sel-size").textContent = fmtSize(it.size);
    const isText = /\.(nc|gcode|gc|ngc|tap|txt|yaml|yml|json|cnc|g|macro)$/i.test(it.name);
    $("#edit").disabled = !isText || it.size > 256 * 1024;
    $("#preview").disabled = !GCODE_RE.test(it.name);
    $("#make-viz").hidden = S.fs !== "sd" || !GCODE_RE.test(it.name);
    $("#run").textContent = S.fs === "sd" && GCODE_RE.test(it.name) ? "Prepare" : "▶ Run";
    paintVizButton();
  }

  // Pendant preview state of a file: ok (green), missing (red), building (amber).
  const VIZ_TITLES = {
    ok: "Pendant preview (.viz) is ready",
    missing: "No pendant preview (.viz) yet - it is built automatically when the pendant or a job needs it; click to build it now",
    building: "Building the pendant preview (.viz)…",
  };
  function paintViz(li, st) {
    const dot = li.querySelector(".vizdot");
    if (!dot) return;
    dot.className = "vizdot viz-" + st;
    dot.title = VIZ_TITLES[st];
  }
  function paintVizButton() {
    const b = $("#make-viz"), st = S.sel && S.sel.viz;
    b.classList.remove("viz-ok", "viz-missing", "viz-building");
    if (st) b.classList.add("viz-" + st);
    b.title = st ? VIZ_TITLES[st] + (st === "ok" ? " - click to rebuild it" : "") : "Build the TabUI pendant preview (.viz)";
  }
  // "/sd/dir/a.nc" (from a Viz message) -> update its dot and the button
  function setVizState(sdPath, st) {
    const path = sdPath.replace(/^\/sd/i, "");
    if (S.fs !== "sd") return;
    for (const li of $$("li", listEl)) {
      if (li.dataset.path === path && li._item) { li._item.viz = st; paintViz(li, st); }
    }
    if (S.sel && S.sel.path === path) paintVizButton();
  }

  function previewSelected() {
    if (!S.sel) return;
    previewFile(S.fs, S.sel.path).catch((e) => toast(e.message, true));
    showPane("viewer");
  }
  $("#preview").onclick = previewSelected;

  // ------------------------------------------------------------- prepared job
  // "Prepare" puts the file on the pendant's screen (and in this viewer); the
  // job is then started - or cancelled - from either side. The board keeps
  // the state and reports it as [MSG:Prepared:<state>:<file>].
  const PREP_STATES = {
    loading: "loading on the pendant…",
    ready: "shown on the pendant",
    nopendant: "no pendant connected",
  };
  let prepFile = "";
  function setPrepared(st, file) {
    const box = $("#prepared");
    if (st === "none" || !file) { prepFile = ""; box.hidden = true; return; }
    prepFile = file;
    box.hidden = false;
    $("#prep-name").textContent = file.replace(/^\/sd\//i, "");
    const s = $("#prep-state");
    s.textContent = PREP_STATES[st] || st;
    s.className = "prep-state " + st;
    // show the same file here
    const path = file.replace(/^\/sd/i, "");
    if ($("#viewer-file").dataset.path !== "sd:" + path) previewFile("sd", path).catch(() => {});
  }
  $("#prep-run").onclick = () => {
    if (!prepFile) return;
    if (!idle()) return toast(`Can't start a job while ${S.state}`, true);
    const path = prepFile.replace(/^\/sd/i, "");
    if (!confirm(`Run ${path.replace(/^.*\//, "")}?`)) return;
    send(Files.runCommand("sd", path));
  };
  $("#prep-cancel").onclick = () => send("$Job/Unprepare");

  $("#run").onclick = async () => {
    const it = S.sel;
    if (!it) return;
    if (!idle()) return toast(`Can't start a job while ${S.state}`, true);
    if (S.fs === "sd" && GCODE_RE.test(it.name)) {
      // Prepare: pendant + this viewer show it; Run / Cancel from the bar
      send("$Job/Prepare=" + it.path);
      previewFile("sd", it.path).catch(() => {});
      showPane("viewer");
      return;
    }
    if (!confirm(`Run ${it.name}?`)) return;
    if (!(Viewer.job && $("#viewer-file").dataset.path === S.fs + ":" + it.path && !$("#viewer-file").dataset.partial)) {
      previewFile(S.fs, it.path).catch(() => {});
    }
    send(Files.runCommand(S.fs, it.path));
  };

  $("#del").onclick = async () => {
    const it = S.sel;
    if (!it || !confirm(`Delete ${it.name}?`)) return;
    try {
      await Files.remove(S.fs, it.path);
      forget(S.fs, it.path);
      if (S.fs === "sd" && GCODE_RE.test(it.name)) await Files.remove("sd", it.path + ".viz").catch(() => {});
      toast("Deleted " + it.name);
      refresh();
    }
    catch (e) { toast(e.message, true); }
  };

  $("#upload").onchange = async (e) => {
    const files = Array.from(e.target.files || []);
    e.target.value = "";
    const bar = $("#upload-progress");
    for (const f of files) {
      bar.hidden = false;
      try {
        await Files.upload(S.fs, S.dir, f, (p) => (bar.firstElementChild.style.width = (p * 100).toFixed(0) + "%"));
        forget(S.fs, (S.dir.endsWith("/") ? S.dir : S.dir + "/") + f.name);
        toast("Uploaded " + f.name);
      } catch (err) { toast(err.message, true); }
    }
    bar.hidden = true;
    bar.firstElementChild.style.width = "0";
    refresh();
  };

  // simple text editor (config.yaml, small G-code, macros)
  const dlg = $("#editor");
  $("#edit").onclick = async () => {
    const it = S.sel;
    if (!it) return;
    try {
      $("#editor-text").value = await Files.getText(S.fs, it.path);
      $("#editor-title").textContent = (S.fs === "sd" ? "SD" : "Flash") + ":" + it.path;
      dlg.showModal();
    } catch (e) { toast(e.message, true); }
  };
  dlg.addEventListener("close", async () => {
    if (dlg.returnValue !== "save" || !S.sel) return;
    try {
      await Files.putText(S.fs, S.sel.path, $("#editor-text").value);
      forget(S.fs, S.sel.path);
      toast("Saved " + S.sel.name + (S.sel.name === "config.yaml" ? " - restart ($bye) to apply" : ""));
      refresh();
    } catch (e) { toast(e.message, true); }
  });

  // ------------------------------------------------------------- axis monitor
  Monitor.init($("#monitor"));
  function setMonitor(open) {
    $("#monitor").classList.toggle("collapsed", !open);
    $("#mon-toggle").textContent = "Axis monitor " + (open ? "▾" : "▸");
    document.documentElement.style.setProperty("--mon-h", open ? "74px" : "40px");
    Monitor.setVisible(open);
    Prefs.set("monitor", open);
    requestAnimationFrame(() => Viewer.resize());
  }
  $("#mon-toggle").onclick = () => setMonitor($("#monitor").classList.contains("collapsed"));
  setMonitor(Prefs.get("monitor", true));

  // ------------------------------------------------------------- phone tabs
  function showPane(name) {
    $$("#tabs button").forEach((b) => b.classList.toggle("active", b.dataset.view === name));
    $$(".layout > .panel").forEach((p) => p.classList.toggle("shown", p.dataset.pane === name));
    if (name === "viewer") requestAnimationFrame(() => Viewer.resize());
  }
  $$("#tabs button").forEach((b) => (b.onclick = () => showPane(b.dataset.view)));
  showPane("control");

  // ------------------------------------------------------------- start
  Conn.connect();
  refresh();
})();
