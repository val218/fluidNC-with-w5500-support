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
  Conn.on("line", (line) => {
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
    if ((m = /^\[MSG:Viz(Ready|Busy|Err|Deleted|Status):?(.*)\]$/.exec(line))) {
      onVizMsg(m[1], m[2]);
      log(line, m[1] === "Err" ? "err" : "msg");
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
    if (s !== "open") setStateBadge("Unknown");
  });

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
    if (st.state === "Idle") flushViz();
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
    jobStatus(st);
  });

  // ------------------------------------------------------------- job progress
  function jobStatus(st) {
    const running = st.sdPct !== undefined && !Number.isNaN(st.sdPct);
    const jobEl = $("#job");
    if (running) {
      if (!S.job || S.job.file !== st.sdFile) {
        S.job = { file: st.sdFile, start: Date.now() };
        autoPreview(st.sdFile);
      }
      const pct = Math.max(0, Math.min(100, st.sdPct));
      const elapsed = (Date.now() - S.job.start) / 1000;
      const eta = pct > 0.5 ? (elapsed * (100 - pct)) / pct : NaN;
      jobEl.hidden = false;
      jobEl.textContent = `${st.sdFile.split("/").pop()} · ${pct.toFixed(1)}% · ${fmtTime(elapsed)} · ETA ${fmtTime(eta)}`;
      $("#progress-bar").style.width = pct + "%";
      Viewer.setProgress(pct / 100);
    } else if (S.job) {
      // job finished or stopped
      if (idle()) {
        log(`Job ended: ${S.job.file} (${fmtTime((Date.now() - S.job.start) / 1000)})`, "msg");
        S.job = null;
        jobEl.hidden = true;
      }
    }
  }

  // When a job is started from elsewhere (pendant, other browser), show its path.
  async function autoPreview(sdFile) {
    if (!sdFile) return;
    const path = sdFile.replace(/^\/sd/, "");
    if (Viewer.job && $("#viewer-file").dataset.path === "sd:" + path) return;
    try { await previewFile("sd", path); } catch (e) { /* preview is best-effort */ }
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

  async function previewFile(fs, path) {
    const label = $("#viewer-file");
    label.textContent = "Loading " + path + "…";
    const text = await Files.getText(fs, path);
    label.textContent = "Parsing " + path + "…";
    const job = await Gcode.parse(text, {
      onProgress: (f) => (label.textContent = `Parsing ${path}… ${Math.round(f * 100)}%`),
    });
    Viewer.load(job);
    label.textContent = path.split("/").pop();
    label.dataset.path = fs + ":" + path;
    const size = job.max.map((v, i) => v - job.min[i]);
    $("#viewer-info").textContent =
      `${size.map((v) => v.toFixed(1)).join(" × ")} mm · Z ${job.min[2].toFixed(2)}…${job.max[2].toFixed(2)}` +
      ` · cut ${(job.feedLen / 1000).toFixed(2)} m · est. ${fmtTime(job.estMinutes * 60)}` +
      (job.tools ? ` · ${job.tools} tool change(s)` : "");
    $("#progress-bar").style.width = "0%";
  }

  // ------------------------------------------------------------- TabUI pendant .viz
  // The pendant shows a preview from "<file>.viz" next to the G-code on the SD card.
  // FluidNC's VizGenerator builds it on request ($Viz/Generate=/sd/<file>). It runs
  // on the protocol task and blocks motion briefly, so requests wait for Idle.
  const GCODE_RE = /\.(nc|gcode|gc|ngc|tap|cnc|g)$/i;
  const autoViz = $("#auto-viz");
  autoViz.checked = Prefs.get("autoViz", true);
  autoViz.onchange = () => Prefs.set("autoViz", autoViz.checked);
  const vizQueue = [];
  let vizActive = null;
  let vizTimer = 0;

  function queueViz(path) {
    const p = "/sd" + path;
    if (!vizQueue.includes(p) && vizActive !== p) vizQueue.push(p);
    if (!idle()) toast("Pendant .viz will be built when the machine is idle");
    flushViz();
  }
  function flushViz() {
    if (vizActive || !vizQueue.length || !idle() || Conn.state !== "open") return;
    vizActive = vizQueue.shift();
    // Delete first: Generate returns the old .viz unchanged if one already exists.
    Conn.sendLine("$Viz/Delete=" + vizActive, true);
    Conn.sendLine("$Viz/Generate=" + vizActive, true);
    vizTimer = setTimeout(() => { vizActive = null; flushViz(); }, 120000);
  }
  function onVizMsg(kind, rest) {
    const name = (vizActive || rest).replace(/^.*\//, "").replace(/\.viz.*$/, "");
    if (kind === "Busy") {
      const pct = /:(\d+)$/.exec(rest);
      if (pct) $("#viz-status").textContent = `Pendant .viz ${name}: ${pct[1]}%`;
      return;
    }
    if (kind !== "Ready" && kind !== "Err") return;
    clearTimeout(vizTimer);
    $("#viz-status").textContent = "";
    if (kind === "Ready") {
      const pts = rest.split(":")[1];
      toast(`Pendant .viz ready: ${name}` + (pts ? ` (${pts} points)` : ""));
    } else {
      toast("Pendant .viz failed: " + rest, true);
    }
    vizActive = null;
    setTimeout(flushViz, 200);
  }
  $("#make-viz").onclick = () => S.sel && queueViz(S.sel.path);

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
      for (const it of items) {
        if (!it.dir && /\.viz(\.tmp)?$/i.test(it.name)) continue; // pendant preview sidecars
        const li = el("li", { class: it.dir ? "dir" : "" },
          el("span", { class: "fname" }, it.name),
          el("span", { class: "fsize" }, it.dir ? "" : fmtSize(it.size)));
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
  }

  function previewSelected() {
    if (!S.sel) return;
    previewFile(S.fs, S.sel.path).catch((e) => toast(e.message, true));
    showPane("viewer");
  }
  $("#preview").onclick = previewSelected;

  $("#run").onclick = async () => {
    const it = S.sel;
    if (!it) return;
    if (!idle()) return toast(`Can't start a job while ${S.state}`, true);
    if (!confirm(`Run ${it.name}?`)) return;
    if (!(Viewer.job && $("#viewer-file").dataset.path === S.fs + ":" + it.path)) {
      previewFile(S.fs, it.path).catch(() => {});
    }
    send(Files.runCommand(S.fs, it.path));
  };

  $("#del").onclick = async () => {
    const it = S.sel;
    if (!it || !confirm(`Delete ${it.name}?`)) return;
    try {
      await Files.remove(S.fs, it.path);
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
        toast("Uploaded " + f.name);
        if (S.fs === "sd" && autoViz.checked && GCODE_RE.test(f.name)) {
          queueViz((S.dir.endsWith("/") ? S.dir : S.dir + "/") + f.name);
        }
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
      toast("Saved " + S.sel.name + (S.sel.name === "config.yaml" ? " - restart ($bye) to apply" : ""));
      if (S.fs === "sd" && GCODE_RE.test(S.sel.name)) queueViz(S.sel.path);
      refresh();
    } catch (e) { toast(e.message, true); }
  });

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
