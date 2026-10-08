"use strict";
// SD card / flash file access through FluidNC's WebDAV endpoints
// (/sd/... and /flash/...): PROPFIND to list, PUT to upload, GET, DELETE.
const Files = (() => {
  const roots = { sd: "/sd", flash: "/flash" };

  const encPath = (p) => p.split("/").map(encodeURIComponent).join("/");
  const url = (fs, path) => HTTP_BASE + roots[fs] + encPath(path);

  async function list(fs, dir) {
    const d = dir.endsWith("/") ? dir : dir + "/";
    const r = await fetch(url(fs, d), { method: "PROPFIND", headers: { Depth: "1" } });
    if (!r.ok) throw new Error(`List failed (${r.status})`);
    const xml = new DOMParser().parseFromString(await r.text(), "application/xml");
    const out = [];
    for (const resp of xml.getElementsByTagNameNS("*", "response")) {
      const href = (resp.getElementsByTagNameNS("*", "href")[0] || {}).textContent || "";
      let p = decodeURIComponent(href);
      if (p.startsWith(roots[fs])) p = p.slice(roots[fs].length);
      if (!p.startsWith("/")) p = "/" + p;
      const isDir = resp.getElementsByTagNameNS("*", "collection").length > 0;
      const clean = p.replace(/\/+$/, "") || "/";
      if (clean === (d.replace(/\/+$/, "") || "/")) continue; // the directory itself
      const name = clean.slice(clean.lastIndexOf("/") + 1);
      if (!name || name.startsWith(".")) continue;
      const len = resp.getElementsByTagNameNS("*", "getcontentlength")[0];
      out.push({ name, path: clean, dir: isDir, size: len ? Number(len.textContent) : 0 });
    }
    out.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    return out;
  }

  function upload(fs, dir, file, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      const target = (dir.endsWith("/") ? dir : dir + "/") + file.name;
      xhr.open("PUT", url(fs, target));
      xhr.upload.onprogress = (e) => e.lengthComputable && onProgress && onProgress(e.loaded / e.total);
      xhr.onload = () => (xhr.status >= 200 && xhr.status < 300 ? resolve() : reject(new Error(`Upload failed (${xhr.status})`)));
      xhr.onerror = () => reject(new Error("Upload failed (network)"));
      xhr.send(file);
    });
  }

  async function getText(fs, path) {
    const r = await fetch(url(fs, path), { cache: "no-store" });
    if (!r.ok) throw new Error(`Read failed (${r.status})`);
    return r.text();
  }

  async function putText(fs, path, text) {
    const r = await fetch(url(fs, path), { method: "PUT", body: new Blob([text], { type: "text/plain" }) });
    if (!r.ok) throw new Error(`Save failed (${r.status})`);
  }

  async function remove(fs, path) {
    const r = await fetch(url(fs, path), { method: "DELETE" });
    if (!r.ok) throw new Error(`Delete failed (${r.status})`);
  }

  // FluidNC command that runs a file from the given filesystem.
  function runCommand(fs, path) {
    return (fs === "sd" ? "$SD/Run=" : "$LocalFS/Run=") + path;
  }

  return { list, upload, getText, putText, remove, runCommand };
})();
