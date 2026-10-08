"use strict";
// Small helpers shared by all modules.
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const c of children) e.append(c);
  return e;
}

function fmt(n, d = 3) {
  return Number.isFinite(n) ? n.toFixed(d) : "--";
}

function fmtSize(b) {
  if (b < 1024) return b + " B";
  if (b < 1048576) return (b / 1024).toFixed(1) + " KB";
  return (b / 1048576).toFixed(2) + " MB";
}

function fmtTime(sec) {
  if (!Number.isFinite(sec) || sec < 0) return "--";
  sec = Math.round(sec);
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  return (h ? h + "h " : "") + (h || m ? m + "m " : "") + s + "s";
}

let toastTimer;
function toast(msg, isErr = false) {
  const t = $("#toast");
  t.textContent = msg;
  t.className = "toast" + (isErr ? " err" : "");
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), isErr ? 6000 : 2500);
}

// Per-browser preferences only; never relied on.
const Prefs = {
  get(k, d) { try { const v = localStorage.getItem("dp." + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem("dp." + k, JSON.stringify(v)); } catch {} },
};

// Base URL for HTTP/WebDAV calls. ?host=1.2.3.4 lets you open the page from a PC
// file or another server and still talk to a board.
const HOST = new URLSearchParams(location.search).get("host") || location.host;
const HTTP_BASE = (location.protocol === "https:" ? "https://" : "http://") + HOST;
