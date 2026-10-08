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

## TabUI pendant previews (.viz)

The pendant shows a preview from `<file>.viz` stored next to the G-code on
the SD card. With **Pendant .viz** ticked (default, remembered per browser),
every G-code file uploaded to the SD card, or saved from the editor, gets one
built automatically: the UI sends `$Viz/Delete=/sd/<file>` then
`$Viz/Generate=/sd/<file>` and shows `VizBusy`/`VizReady`/`VizErr` as they
arrive. Generation pauses the controller briefly, so it only runs while the
machine is Idle; requests made during a job wait until it finishes. The
**Pendant .viz** button on a selected file rebuilds it by hand (useful for
files copied onto the card with a PC). `.viz` files are hidden from the list
and deleted together with their G-code file.

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
