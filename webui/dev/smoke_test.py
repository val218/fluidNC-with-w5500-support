#!/usr/bin/env python3
"""Headless smoke test against dev/mock_fluidnc.py: loads the UI, exercises
DRO, jog, file preview, run, console errors, and saves screenshots."""
import sys
import pathlib
from playwright.sync_api import sync_playwright

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/webui-shots")
OUT.mkdir(parents=True, exist_ok=True)
URL = "http://127.0.0.1:8080/"
errors = []


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond:
        errors.append(msg)


with sync_playwright() as p:
    b = p.chromium.launch(args=["--use-gl=swiftshader", "--enable-webgl", "--ignore-gpu-blocklist"])
    page = b.new_page(viewport={"width": 1600, "height": 900})
    page.on("console", lambda m: m.type == "error" and errors.append("console: " + m.text))
    page.on("pageerror", lambda e: errors.append("pageerror: " + str(e)))
    page.goto(URL)
    page.wait_for_function("document.querySelector('#conn').textContent === 'online'", timeout=8000)
    check(True, "connected")
    page.wait_for_function("document.querySelector('#state').textContent === 'Idle'", timeout=5000)
    check(page.inner_text("#ax-X .wpos") == "100.000", "DRO X shows WPos = MPos - WCO (100.000)")

    # file list + preview
    page.wait_for_selector("#file-list li:has-text('job1.nc')", timeout=5000)
    page.click("#file-list li:has-text('job1.nc')")
    page.click("#preview")
    page.wait_for_function("document.querySelector('#viewer-file').textContent === 'job1.nc'", timeout=8000)
    info = page.inner_text("#viewer-info")
    check("×" in info and "mm" in info, f"preview info: {info}")
    page.screenshot(path=str(OUT / "desktop-preview.png"))

    # step jog X+ 10
    page.click("#step button[data-step='10']")
    page.click("button[data-jog='X1']")
    page.wait_for_function("document.querySelector('#ax-X .wpos').textContent === '110.000'", timeout=6000)
    check(True, "step jog moved X by 10")

    # zero X
    page.click("#ax-X .z0")
    page.wait_for_function("document.querySelector('#ax-X .wpos').textContent === '0.000'", timeout=4000)
    check(True, "zero X sets WPos 0")

    # console error
    page.fill("#cmd", "bad")
    page.press("#cmd", "Enter")
    page.wait_for_selector("#log .err", timeout=4000)
    check("Unsupported G-code" in page.inner_text("#log"), "error code described in console")

    # upload G-code to SD -> pendant .viz is built, sidecar hidden from list
    page.set_input_files("#upload", files=[{"name": "upl.nc", "mimeType": "text/plain",
                                            "buffer": b"G0 X0 Y0\nG1 X10 Y10 F500\nG1 X0\n"}])
    page.wait_for_selector("#log :text('VizAutoReady:/sd/upl.nc.viz')", timeout=6000)
    check(True, "upload to SD -> firmware builds .viz (VizAutoReady)")
    page.wait_for_selector("#file-list li:has-text('upl.nc')", timeout=4000)
    names = page.eval_on_selector_all("#file-list .fname", "els => els.map(e => e.textContent)")
    check("upl.nc.viz" not in names, f".viz sidecar hidden from list: {names}")
    check(page.evaluate("document.querySelector('.brand-logo').naturalWidth > 0"), "logo image renders")
    check(page.inner_text("#pendant") == "Pendant" and page.is_visible("#pendant"), "pendant badge shows connected")
    page.evaluate("fetch('/mock/pendant?state=disconnected')")
    page.wait_for_function("document.querySelector('#pendant').textContent === 'Pendant offline'", timeout=3000)
    check(True, "pendant badge follows disconnect message")
    page.screenshot(path=str(OUT / "desktop-pendant-offline.png"), clip={"x": 0, "y": 0, "width": 1600, "height": 70})
    page.evaluate("fetch('/mock/pendant?state=connected')")
    page.wait_for_function("document.querySelector('#pendant').textContent === 'Pendant'", timeout=3000)
    page.screenshot(path=str(OUT / "desktop-viz.png"))
    # monitor strip present with one row per axis
    check(page.locator(".mon-axis").count() == 4, "axis monitor has 4 axes")

    # PC drive script download has the board IP filled in
    with page.expect_download() as dl:
        page.click("#pc-drive")
    path = dl.value.path()
    body = open(path, "rb").read().decode()
    check('set "DEFAULT_IP=127.0.0.1"' in body and 'set "ASK_IP=0"' in body and "\r\n" in body,
          "PC drive script: IP filled in, no prompt, CRLF")

    # manual pendant .viz with confirmation
    page.once("dialog", lambda d: d.accept())
    page.click("#file-list li:has-text('job1.nc')")
    page.click("#make-viz")
    page.wait_for_selector("#log :text('VizAutoReady:/sd/job1.nc.viz')", timeout=6000)
    check(True, "Pendant .viz button (confirmed) builds job1.nc.viz")
    page.wait_for_selector("#file-list li:has-text('job1.nc') .vizdot.viz-ok", timeout=3000)
    check("viz-ok" in page.get_attribute("#make-viz", "class"), ".viz state: green dot + green button once built")
    page.click("#refresh")
    page.wait_for_selector("#file-list li:has-text('job1.nc') .vizdot.viz-ok", timeout=3000)
    check(True, ".viz state survives a list refresh (from the card listing)")

    # job started elsewhere while another file is shown -> 2D .viz outline, full path when stopped
    page.click("#file-list li:has-text('upl.nc')")
    page.click("#preview")
    page.wait_for_function("document.querySelector('#viewer-file').textContent === 'upl.nc'", timeout=6000)
    page.set_input_files("#upload", files=[{"name": "big.nc", "mimeType": "text/plain",
                                            "buffer": b"G0 X0 Y0\nG1 Z-1 F300\nG1 X40 Y0 F800\nG1 X40 Y30\nG1 X0 Y30\nG1 X0 Y0\n"}])
    page.wait_for_selector("#log :text('VizAutoReady:/sd/big.nc.viz')", timeout=6000)
    page.fill("#cmd", "$SD/Run=/big.nc")
    page.press("#cmd", "Enter")
    page.wait_for_function("document.querySelector('#viewer-file').textContent.includes('big.nc · preview')", timeout=6000)
    check(True, "running job from elsewhere shows the .viz preview while moving")
    page.screenshot(path=str(OUT / "desktop-job-viz.png"))
    page.click("button[data-cmd='stop']")
    page.wait_for_function("document.querySelector('#viewer-file').textContent === 'big.nc'", timeout=8000)
    check(True, "full 3D path loads once the machine stops")
    page.wait_for_function("document.querySelector('#state').textContent === 'Idle'", timeout=5000)
    page.wait_for_timeout(500)
    page.click("#file-list li:has-text('job1.nc')")

    # prepare -> shown on the pendant -> run from the bar
    page.on("dialog", lambda d: d.accept())
    check(page.inner_text("#run").strip() == "Prepare", "SD G-code file: button says Prepare")
    page.click("#run")
    page.wait_for_selector("#prepared:not([hidden]) .prep-state.loading", timeout=3000)
    page.wait_for_selector("#prepared .prep-state.ready", timeout=4000)
    check("job1.nc" in page.inner_text("#prep-name"), "prepared bar: job1.nc shown on the pendant")
    page.screenshot(path=str(OUT / "desktop-prepared.png"))
    page.click("#prep-cancel")
    page.wait_for_selector("#prepared[hidden]", state="attached", timeout=3000)
    page.wait_for_function("document.querySelector('#viewer-file').textContent === 'No file loaded'", timeout=3000)
    check(True, "Cancel clears the prepared job and its preview")
    page.click("#run")
    page.wait_for_selector("#prepared .prep-state.ready", timeout=4000)
    page.click("#prep-run")
    page.wait_for_function("!document.querySelector('#job').hidden", timeout=6000)
    page.wait_for_selector("#prepared[hidden]", state="attached", timeout=3000)
    check("job1.nc" in page.inner_text("#viewer-file"), "Run from the bar starts the job, clears the bar, keeps the preview")
    page.wait_for_timeout(2500)
    check("job1.nc" in page.inner_text("#job"), "job progress shown: " + page.inner_text("#job"))
    check(page.inner_text("#state") == "Run", "state Run during job")
    moving = page.evaluate("[...document.querySelectorAll('.mon-axis .led')].some(l => l.classList.contains('led-move'))")
    check(moving, "axis monitor shows moving axes during job")
    page.screenshot(path=str(OUT / "desktop-running.png"))
    page.click("button[data-cmd='hold']")
    page.wait_for_function("document.querySelector('#state').textContent.startsWith('Hold')", timeout=3000)
    check(True, "feed hold works")
    page.click("button[data-cmd='stop']")
    page.wait_for_function("document.querySelector('#state').textContent === 'Idle'", timeout=4000)
    check(True, "stop returns to Idle")

    # tablet + phone layouts
    page.set_viewport_size({"width": 1024, "height": 768})
    page.wait_for_timeout(500)
    page.screenshot(path=str(OUT / "tablet.png"), full_page=True)
    page.set_viewport_size({"width": 390, "height": 844})
    page.wait_for_timeout(400)
    page.screenshot(path=str(OUT / "phone-control.png"))
    page.click("#tabs button[data-view='viewer']")
    page.wait_for_timeout(500)
    page.screenshot(path=str(OUT / "phone-viewer.png"))
    overflow = page.evaluate("document.documentElement.scrollWidth > window.innerWidth")
    check(not overflow, "no horizontal overflow on phone")
    b.close()

print("\nERRORS:" if errors else "\nALL OK")
for e in errors:
    print(" -", e)
sys.exit(1 if errors else 0)
