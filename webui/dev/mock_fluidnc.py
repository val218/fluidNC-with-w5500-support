#!/usr/bin/env python3
"""Tiny FluidNC stand-in for developing the WebUI without a board.

Serves webui/dist/index.html on http://127.0.0.1:8080/, a WebSocket on the
same port at "/", and WebDAV-ish /sd and /flash. Pure stdlib (no pip).

    python3 webui/build.py && python3 webui/dev/mock_fluidnc.py
"""
import asyncio
import base64
import hashlib
import math
import pathlib
import re
import struct
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SESSIONS = set()          # out() of every open WebSocket
PENDANT = ["connected"]   # TabUI pendant link state (GET /mock/pendant?state=... to change)
GCODE_RE = re.compile(r"\.(nc|gcode|gc|ngc|tap|cnc|g)$", re.I)


def make_viz(gcode: bytes) -> bytes:
    """Rough .viz like the firmware's: header, then "x,y" per XY move."""
    x = y = 0.0
    pts = []
    for ln in gcode.decode(errors="replace").upper().splitlines():
        mx, my = re.search(r"X([-\d.]+)", ln), re.search(r"Y([-\d.]+)", ln)
        if mx or my:
            x = float(mx.group(1)) if mx else x
            y = float(my.group(1)) if my else y
            pts.append(f"{x:.3f},{y:.3f}")
    return (f"VIZ {len(pts)} 0 10 0 10\n" + "\n".join(pts) + "\n").encode()


PREP = ["none", ""]  # prepared job: state, /sd/file


async def broadcast(texts):
    for out in list(SESSIONS):
        try:
            await out(texts)
        except Exception:
            pass


async def auto_viz(rel):
    """Like the firmware: build <file>.viz after a write to /sd, report VizAuto*."""
    await asyncio.sleep(0.3)
    if rel not in FILES["sd"]:
        return
    src = "/sd" + rel
    await broadcast([f"[MSG:VizAutoQueued:{src}]"])
    n = FILES["sd"][rel].count(b"\n")
    await broadcast([f"[MSG:VizAutoBusy:{src}:0]"])
    await asyncio.sleep(0.2)
    FILES["sd"][rel + ".viz"] = make_viz(FILES["sd"][rel])
    await broadcast([f"[MSG:VizAutoReady:{src}.viz:{n}:0.000:10.000:0.000:10.000]"])


def sample_gcode() -> str:
    out = ["(mock job: pocket + circles)", "G21 G90 G17", "G0 Z5", "M3 S12000"]
    for depth in (-1, -2, -3):
        out += ["G0 X10 Y10", f"G1 Z{depth} F300"]
        for i in range(6):
            o = i * 4
            out += [f"G1 X{90-o} Y{10+o} F1200", f"G1 X{90-o} Y{60-o}", f"G1 X{10+o} Y{60-o}", f"G1 X{10+o} Y{10+o+4}"]
        out += ["G0 Z5"]
    for cx, cy in ((25, 85), (50, 85), (75, 85)):
        out += [f"G0 X{cx+8} Y{cy}", "G1 Z-2 F300", f"G2 X{cx+8} Y{cy} I-8 J0 F900", "G0 Z5"]
    out += ["M5", "G0 X0 Y0", "M30"]
    return "\n".join(out) + "\n"


FILES = {
    "sd": {"/job1.nc": sample_gcode().encode(), "/notes.txt": b"hello\n"},
    "flash": {"/config.yaml": b"name: mock\nboard: mock\n"},
}


class Machine:
    def __init__(self):
        self.state = "Idle"
        self.mpos = [0.0, 0.0, 0.0, 0.0]
        self.wco = [-100.0, -50.0, -30.0, 0.0]
        self.target = None
        self.feed = 0
        self.job = None  # (name, start, duration)
        self.report_ms = 0
        self.wcs = "G54"

    def status(self) -> str:
        if self.job:
            name, start, dur = self.job
            pct = min(100.0, (time.time() - start) / dur * 100)
            if pct >= 100:
                self.job = None
                self.state = "Idle"
            elif not self.state.startswith("Hold"):
                self.state = "Run"
        f = "|SD:%.2f,/sd%s" % (pct, name) if self.job else ""
        mp = ",".join("%.3f" % v for v in self.mpos)
        wco = ",".join("%.3f" % v for v in self.wco)
        return f"<{self.state}|MPos:{mp}|FS:{self.feed},0|WCO:{wco}|Ov:100,100,100{f}>"

    def tick(self, dt):
        if self.job and self.state.startswith("Hold"):
            self.job = (self.job[0], self.job[1] + dt, self.job[2])  # paused
            self.feed = 0
            return
        if self.job:
            name, start, dur = self.job
            t = (time.time() - start) / dur
            self.mpos[0] = -100 + 50 + 40 * math.cos(t * 20)
            self.mpos[1] = -50 + 35 + 25 * math.sin(t * 20)
            self.mpos[2] = -30 - 2
            self.feed = 1200
            return
        if self.target is None:
            self.feed = 0
            return
        step = self.speed * dt
        d = [t - p for t, p in zip(self.target, self.mpos)]
        dist = math.sqrt(sum(x * x for x in d))
        if dist <= step:
            self.mpos = list(self.target)
            self.target = None
            self.state = "Idle"
            self.feed = 0
        else:
            self.mpos = [p + x / dist * step for p, x in zip(self.mpos, d)]

    def line(self, ln: str):
        u = ln.strip().upper()
        if u.startswith("$J="):
            words = dict((m[0], float(m[1])) for m in re.findall(r"([XYZA])([-\d.]+)", u))
            f = float(re.search(r"F([\d.]+)", u).group(1))
            tgt = list(self.mpos)
            for i, a in enumerate("XYZA"):
                if a in words:
                    tgt[i] += max(-300, min(300, words[a]))
            self.target, self.speed, self.state, self.feed = tgt, f / 60, "Jog", f
            return ["ok"]
        mri = re.fullmatch(r"\$(?:RI|REPORT/INTERVAL)=(\d+)", u)
        if mri:
            self.report_ms = int(mri.group(1))
            return ["ok"]
        if u == "[ESP111]":
            return ["127.0.0.1", "ok"]
        if u == "$G":
            return [f"[GC:G0 {self.wcs} G17 G21 G90 G94 M5 M9 T0 F0 S0]", "ok"]
        if u == "$H":
            self.target, self.speed, self.state = [0, 0, 0, 0], 50, "Home"
            return ["ok"]
        if u == "$X":
            self.state = "Idle"
            return ["[MSG:Caution: Unlocked]", "ok"]
        if u.startswith("G10 L20 P0"):
            for m in re.findall(r"([XYZA])([-\d.]+)", u):
                i = "XYZA".index(m[0])
                self.wco[i] = self.mpos[i] - float(m[1])
            return ["ok"]
        if re.fullmatch(r"G5[4-9]", u):
            self.wcs = u
            return ["ok"]
        if u.startswith("$SD/RUN="):
            self.job = (ln.split("=", 1)[1].strip(), time.time(), 40)
            if PREP[1] and PREP[1][3:] == self.job[0]:
                PREP[0], PREP[1] = "none", ""
                return ["[MSG:Prepared:none:]", "ok"]
            return ["ok"]
        if u.startswith("$JOB/PREPARE="):
            src = ln.split("=", 1)[1].strip()
            src = src if src.startswith("/sd/") else "/sd" + src
            if PENDANT[0] != "connected":
                PREP[0], PREP[1] = "nopendant", src
                return [f"[MSG:Prepared:nopendant:{src}]", "ok"]
            PREP[0], PREP[1] = "loading", src
            async def shown():  # the pendant loads the path, then confirms
                await asyncio.sleep(1.0)
                if PREP[1] == src and PREP[0] == "loading":
                    PREP[0] = "ready"
                    await broadcast([f"[MSG:Prepared:ready:{src}]"])
            asyncio.get_event_loop().create_task(shown())
            return [f"[MSG:Prepared:loading:{src}]", "ok"]
        if u == "$JOB/UNPREPARE":
            PREP[0], PREP[1] = "none", ""
            return ["[MSG:Prepared:none:]", "ok"]
        if u == "$JOB/PREPARED":
            return [f"[MSG:Prepared:{PREP[0]}:{PREP[1]}]", "ok"]
        if u.startswith("G0") or u.startswith("G90 G0"):
            tgt = list(self.mpos)
            for m in re.findall(r"([XYZ])([-\d.]+)", u):
                i = "XYZ".index(m[0])
                tgt[i] = float(m[1]) + self.wco[i]
            self.target, self.speed, self.state = tgt, 80, "Run"
            return ["ok"]
        if u == "$PENDANT/STATUS":
            return [f"[MSG:Pendant:{PENDANT[0]}]", "ok"]
        if u.startswith("$VIZ/REFRESH="):
            src = ln.split("=", 1)[1].strip()
            asyncio.get_event_loop().create_task(auto_viz(src[3:] if src.startswith("/sd/") else src))
            return ["ok"]
        if u.startswith("$VIZ/DELETE="):
            FILES["sd"].pop(ln.split("=", 1)[1].strip()[3:] + ".viz", None)
            return ["[MSG:VizDeleted]", "ok"]
        if u.startswith("$VIZ/GENERATE="):
            src = ln.split("=", 1)[1].strip()
            rel = src[3:] if src.startswith("/sd/") else src
            if rel not in FILES["sd"]:
                return [f"[MSG:VizErr:cannot open:{src}]", "ok"]
            n = FILES["sd"][rel].count(b"\n")
            FILES["sd"][rel + ".viz"] = make_viz(FILES["sd"][rel])
            return [f"[MSG:VizBusy:{src}:50]", f"[MSG:VizReady:{src}.viz:{n}:0.000:10.000:0.000:10.000]", "ok"]
        if u == "BAD":
            return ["error:20"]
        return ["ok"]

    def realtime(self, b: int):
        if b == 0x3F:
            return [self.status()]
        if b == 0x85 and self.state == "Jog":
            self.target, self.state = None, "Idle"
        if b == 0x21 and self.state in ("Run", "Jog"):
            self.state = "Hold:0"
        if b == 0x7E and self.state.startswith("Hold"):
            self.state = "Run" if self.job else "Idle"
        if b == 0x18:
            self.target, self.job, self.state = None, None, "Idle"
            return ["", "Grbl 4.0 [FluidNC mock (wifi) '$' for help]"]
        return []


def ws_frame(payload: bytes, opcode=0x2) -> bytes:
    n = len(payload)
    if n < 126:
        hdr = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        hdr = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return hdr + payload


async def ws_session(reader, writer):
    m = Machine()
    writer.write(ws_frame(b"currentID:0", 0x1))
    lines = bytearray()

    async def out(texts):
        for t in texts:
            writer.write(ws_frame((t + "\r\n").encode()))
        await writer.drain()

    async def ticker():
        last_report = 0
        while True:
            await asyncio.sleep(0.05)
            m.tick(0.05)
            if m.report_ms and time.time() - last_report > m.report_ms / 1000:
                last_report = time.time()
                await out([m.status()])

    t = asyncio.create_task(ticker())
    SESSIONS.add(out)
    try:
        while True:
            b1, b2 = await reader.readexactly(2)
            op, n = b1 & 0x0F, b2 & 0x7F
            if n == 126:
                n = struct.unpack("!H", await reader.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", await reader.readexactly(8))[0]
            mask = await reader.readexactly(4) if b2 & 0x80 else b"\0\0\0\0"
            data = bytes(c ^ mask[i % 4] for i, c in enumerate(await reader.readexactly(n)))
            if op == 0x8:
                break
            if op == 0x1 and data.startswith(b"PING:"):
                continue
            for byte in data:
                if byte in (0x3F, 0x21, 0x7E, 0x18) or byte >= 0x80:
                    await out(m.realtime(byte))
                elif byte == 0x0A:
                    await out(m.line(lines.decode(errors="replace")))
                    lines.clear()
                elif byte != 0x0D:
                    lines.append(byte)
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        SESSIONS.discard(out)
        t.cancel()
        writer.close()


def propfind(fs, path):
    path = "/" + path.strip("/")
    items = []
    names = set()
    for p, data in FILES[fs].items():
        parent = p.rsplit("/", 1)[0] or "/"
        if parent == path or (path == "/" and p.count("/") == 1):
            items.append((p, len(data)))
    body = ['<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">']
    body.append(f"<d:response><d:href>/{fs}{path}</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>")
    for p, size in items:
        body.append(f"<d:response><d:href>/{fs}{p}</d:href><d:propstat><d:prop><d:resourcetype/><d:getcontentlength>{size}</d:getcontentlength></d:prop></d:propstat></d:response>")
    body.append("</d:multistatus>")
    return "".join(body).encode()


async def handle(reader, writer):
    try:
        head = await reader.readuntil(b"\r\n\r\n")
    except Exception:
        writer.close()
        return
    lines = head.decode(errors="replace").split("\r\n")
    method, target, _ = lines[0].split(" ", 2)
    hdrs = {k.lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
    path = urllib.parse.unquote(urllib.parse.urlsplit(target).path)

    if hdrs.get("upgrade", "").lower() == "websocket":
        accept = base64.b64encode(hashlib.sha1((hdrs["sec-websocket-key"] + GUID).encode()).digest()).decode()
        writer.write(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n".encode())
        await writer.drain()
        await ws_session(reader, writer)
        return

    body = b""
    if "content-length" in hdrs:
        body = await reader.readexactly(int(hdrs["content-length"]))

    def reply(code, data=b"", ctype="text/plain"):
        writer.write(f"HTTP/1.1 {code} X\r\nContent-Type: {ctype}\r\nContent-Length: {len(data)}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n".encode() + data)

    m = re.match(r"^/(sd|flash)(/.*)?$", path)
    if m:
        fs, p = m.group(1), m.group(2) or "/"
        if method == "PROPFIND":
            reply(207, propfind(fs, p), "application/xml")
        elif method == "GET":
            reply(200, FILES[fs][p]) if p in FILES[fs] else reply(404)
        elif method == "PUT":
            FILES[fs][p] = body
            reply(201)
            if fs == "sd" and GCODE_RE.search(p):
                asyncio.get_event_loop().create_task(auto_viz(p))
        elif method == "DELETE":
            FILES[fs].pop(p, None)
            if fs == "sd":
                FILES[fs].pop(p + ".viz", None)
            reply(204)
        else:
            reply(405)
    elif path == "/mock/pendant":
        PENDANT[0] = urllib.parse.parse_qs(urllib.parse.urlsplit(target).query).get("state", ["connected"])[0]
        await broadcast([f"[MSG:Pendant:{PENDANT[0]}]"])
        reply(200, b"ok")
    elif path in ("/", "/index.html"):
        reply(200, (ROOT / "dist" / "index.html").read_bytes(), "text/html")
    else:
        reply(404)
    await writer.drain()
    writer.close()


async def main():
    srv = await asyncio.start_server(handle, "127.0.0.1", 8080)
    print("mock FluidNC on http://127.0.0.1:8080/")
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
