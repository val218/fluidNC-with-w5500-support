# dpCREATOR WebUI

Browser UI for FluidNC served from the board itself: 3D toolpath view, DRO,
jogging, job control with progress, overrides, SD/flash file manager, console.
Works on PC, tablet and phone. No internet needed; everything is in one file.

## How it ships

`python3 webui/build.py --install` inlines `src/*` and the vendored three.js
into one `index.html`, gzips it, and copies it to `FluidNC/data/index.html.gz`
(the stock FluidNC WebUI is kept as `legacy.html.gz`, reachable via
**Classic UI** in the header or `http://<board>/legacy.html`).

GitHub Actions runs this before building, so `littlefs.bin` and
`merged-flash.bin` always contain the current UI. The build artifact also has
`webui-index.html.gz` on its own.

Updating only the UI on a running board: upload `webui-index.html.gz` as
`index.html.gz` via the file manager (Flash tab), then reload the page.

## How it talks to FluidNC

| What | How |
|---|---|
| Commands, status | WebSocket `ws://<board>/` - lines of text, realtime bytes as binary |
| Status reports | `$RI=200` (auto-report every 200 ms), `?` polling as fallback |
| Files | WebDAV on `/sd/...` and `/flash/...` (PROPFIND, GET, PUT, DELETE) |
| Run job | `$SD/Run=/file.nc` or `$LocalFS/Run=/file.nc` |
| Progress | `SD:<percent>,<file>` field of the status report |

## TabUI pendant previews (.viz) and pendant status

The firmware builds `<file>.viz` itself whenever G-code is written to the SD
card (this UI, a mapped network drive, the classic UI) - see section 6 of
`W5500_ETHERNET.md`. The UI shows its `VizAutoBusy/VizAutoReady/VizAutoErr`
messages, hides `.viz` files from the list, and the **Pendant .viz** button on
a selected file sends `$Viz/Refresh=/sd/<file>` to rebuild by hand.

The header badge shows whether the TabUI pendant on UART1 is connected
(`$Pendant/Status` on connect, then `[MSG:Pendant:...]` on change).

## Job path while a job runs

When a job starts (from this UI, the pendant or anywhere else) the viewer
shows its path. While the machine is moving it only fetches the small pendant
`<file>.viz` and draws that as a blue 2D outline: FluidNC avoids serving big
files during motion, and the job is reading from the same card. The full 3D
path loads by itself as soon as the machine stops (hold, end of job), or now
with **Load path** (asks first). Files you have previewed before are cached
and show at once.

## Axis monitor

The strip at the bottom shows a status light and the current velocity
(mm/min) for every axis, and a 30 s chart of each axis' velocity, worked out
from MPos in the status reports (`$RI=100`). Lights: green moving, orange
hold/alarm, red limit switch active, grey stopped. **Link** blinks on every
status report and shows the report rate; it turns red after 3 s without any.
Click **Axis monitor** to fold it away.

## PC drive

Files → **PC drive** downloads `tools/windows/dpcreator-sd-drive.cmd` with this
board's IP filled in (asked from FluidNC with `[ESP111]` if the page was
opened by name). Double-click it on the PC to get the SD card as drive S:.

## Developing without a board

```
python3 webui/build.py
python3 webui/dev/mock_fluidnc.py        # http://127.0.0.1:8080/
python3 webui/dev/smoke_test.py          # headless checks + screenshots
```

Open the built page against a real board from your PC with
`webui/dist/index.html?host=192.168.10.104`.

Colours are CSS variables at the top of `src/style.css` (accent = brand blue
`#26acff`). The header logo and favicon are `assets/logo.png` and
`assets/favicon.png`, inlined by the build.
