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

## Developing without a board

```
python3 webui/build.py
python3 webui/dev/mock_fluidnc.py        # http://127.0.0.1:8080/
python3 webui/dev/smoke_test.py          # headless checks + screenshots
```

Open the built page against a real board from your PC with
`webui/dist/index.html?host=192.168.10.104`.

Colours are CSS variables at the top of `src/style.css`.
