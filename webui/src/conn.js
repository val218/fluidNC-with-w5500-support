"use strict";
// FluidNC connection: one WebSocket at ws://<board>/ carries everything.
// Outgoing: lines of G-code / $commands, plus single realtime bytes.
// Incoming: binary frames with Grbl-style text output; text frames are
// control messages (currentID:n, PING).
const Conn = (() => {
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  const listeners = {};
  let ws = null;
  let buf = "";
  let state = "closed";
  let lastRx = 0;
  let lastStatus = 0;
  let pollTimer = null;
  let retryTimer = null;
  let retryDelay = 1000;

  function on(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); }
  function emit(ev, ...args) {
    for (const fn of listeners[ev] || []) {
      try { fn(...args); } catch (e) { console.error(e); }
    }
  }
  function setState(s) { state = s; emit("state", s); }

  function wsUrl() {
    return (location.protocol === "https:" ? "wss://" : "ws://") + HOST + "/";
  }

  function connect() {
    clearTimeout(retryTimer);
    if (ws) { try { ws.close(); } catch {} }
    setState("connecting");
    try {
      ws = new WebSocket(wsUrl());
    } catch (e) {
      scheduleRetry();
      return;
    }
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      retryDelay = 1000;
      buf = "";
      lastRx = Date.now();
      setState("open");
      // Ask FluidNC to push status reports on this channel every 200 ms.
      resync();
      startPolling();
    };
    ws.onclose = () => {
      stopPolling();
      setState("closed");
      scheduleRetry();
    };
    ws.onerror = () => {};
    ws.onmessage = (ev) => {
      lastRx = Date.now();
      if (typeof ev.data === "string") {
        // control channel: currentID / PING - nothing to do for us
        return;
      }
      buf += dec.decode(ev.data, { stream: true });
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i).replace(/\r$/, "");
        buf = buf.slice(i + 1);
        if (line.length) handleLine(line);
      }
    };
  }

  function scheduleRetry() {
    clearTimeout(retryTimer);
    retryTimer = setTimeout(connect, retryDelay);
    retryDelay = Math.min(retryDelay * 1.6, 8000);
  }

  // Fallback polling: if auto-reports stop (e.g. older firmware), ask with '?'.
  function startPolling() {
    stopPolling();
    pollTimer = setInterval(() => {
      if (state !== "open") return;
      const now = Date.now();
      if (now - lastStatus > 600) realtime(0x3f); // '?'
      if (now - lastRx > 8000) {
        // Socket looks dead (no data at all for 8 s): reconnect.
        try { ws.close(); } catch {}
      }
    }, 400);
  }
  function stopPolling() { clearInterval(pollTimer); pollTimer = null; }

  function handleLine(line) {
    if (line[0] === "<") {
      lastStatus = Date.now();
      emit("status", parseStatus(line), line);
    }
    emit("line", line);
  }

  function parseStatus(line) {
    const parts = line.slice(1, line.lastIndexOf(">")).split("|");
    const st = { raw: line, state: parts[0].split(":")[0], sub: parts[0].split(":")[1] };
    for (const p of parts.slice(1)) {
      const c = p.indexOf(":");
      const k = c < 0 ? p : p.slice(0, c);
      const v = c < 0 ? "" : p.slice(c + 1);
      switch (k) {
        case "MPos": st.mpos = v.split(",").map(Number); break;
        case "WPos": st.wpos = v.split(",").map(Number); break;
        case "WCO": st.wco = v.split(",").map(Number); break;
        case "FS": { const a = v.split(",").map(Number); st.feed = a[0]; st.speed = a[1]; break; }
        case "F": st.feed = Number(v); break;
        case "Ov": st.ov = v.split(",").map(Number); break;
        case "Pn": st.pins = v; break;
        case "A": st.acc = v; break;
        case "Ln": st.line = Number(v); break;
        case "SD": {
          const comma = v.indexOf(",");
          st.sdPct = parseFloat(comma < 0 ? v : v.slice(0, comma));
          st.sdFile = comma < 0 ? "" : v.slice(comma + 1);
          break;
        }
        default: break;
      }
    }
    if (!("pins" in st)) st.pins = "";
    if (!("acc" in st)) st.acc = "";
    return st;
  }

  function send(bytes) {
    if (!ws || ws.readyState !== 1) return false;
    ws.send(bytes);
    return true;
  }
  function sendLine(text, quiet = false) {
    if (!quiet) emit("tx", text);
    return send(enc.encode(text + "\n"));
  }
  // Realtime commands are single bytes, sent as a binary frame so values
  // >= 0x80 are not UTF-8 encoded into two bytes.
  function realtime(byte) { return send(new Uint8Array([byte])); }

  // Re-request auto reports and modal state (after a reset the firmware
  // prints its banner again and per-channel settings may be gone).
  function resync() { sendLine("$RI=100", true); sendLine("$G", true); sendLine("$Pendant/Status", true); sendLine("$Job/Prepared", true); }

  return { connect, on, sendLine, realtime, resync, get state() { return state; } };
})();
