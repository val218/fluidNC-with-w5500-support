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
    page.wait_for_selector("#log :text('VizReady:/sd/upl.nc.viz')", timeout=6000)
    check(True, "upload to SD triggers $Viz/Generate and VizReady")
    page.wait_for_selector("#file-list li:has-text('upl.nc')", timeout=4000)
    names = page.eval_on_selector_all("#file-list .fname", "els => els.map(e => e.textContent)")
    check("upl.nc.viz" not in names, f".viz sidecar hidden from list: {names}")
    check(page.evaluate("document.querySelector('.brand-logo').naturalWidth > 0"), "logo image renders")
    page.screenshot(path=str(OUT / "desktop-viz.png"))
    page.click("#file-list li:has-text('job1.nc')")

    # run job
    page.on("dialog", lambda d: d.accept())
    page.click("#run")
    page.wait_for_function("!document.querySelector('#job').hidden", timeout=6000)
    page.wait_for_timeout(2500)
    check("job1.nc" in page.inner_text("#job"), "job progress shown: " + page.inner_text("#job"))
    check(page.inner_text("#state") == "Run", "state Run during job")
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
