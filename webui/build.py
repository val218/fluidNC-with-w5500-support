#!/usr/bin/env python3
"""Build the dpCREATOR WebUI into a single gzipped HTML file.

No Node/npm needed: the page is plain JS + CSS, and three.js is vendored.
All sources are inlined into one index.html, which is gzipped for FluidNC.

    python3 webui/build.py                 # -> webui/dist/index.html(.gz)
    python3 webui/build.py --install       # also copy into FluidNC/data/
"""
import argparse
import base64
import json
import gzip
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
DIST = ROOT / "dist"
DATA = ROOT.parent / "FluidNC" / "data"
SD_DRIVE_CMD = ROOT.parent / "tools" / "windows" / "dpcreator-sd-drive.cmd"

# Order matters: later files use globals defined by earlier ones.
JS_FILES = ["util.js", "codes.js", "conn.js", "gcode.js", "viewer.js", "files.js", "monitor.js", "app.js"]


def version() -> str:
    try:
        rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
    except Exception:
        rev = "dev"
    return rev


def data_uri(path: pathlib.Path) -> str:
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode()


def build() -> pathlib.Path:
    template = (ROOT / "index.html").read_text(encoding="utf-8")
    css = (SRC / "style.css").read_text(encoding="utf-8")
    three = (ROOT / "vendor" / "three.min.js").read_text(encoding="utf-8")
    app = "\n".join(
        f"/* ---- {name} ---- */\n" + (SRC / name).read_text(encoding="utf-8") for name in JS_FILES
    )
    # Guard against a stray </script> inside inlined code ending the tag early.
    three = three.replace("</script", "<\\/script")
    app = app.replace("</script", "<\\/script")

    html = (
        template.replace("/*__CSS__*/", css)
        .replace("/*__THREE__*/", three)
        .replace("/*__APP__*/", app)
        .replace("__VERSION__", version())
        .replace("__LOGO__", data_uri(ROOT / "assets" / "logo.png"))
        .replace("__SD_DRIVE_CMD__", json.dumps(SD_DRIVE_CMD.read_bytes().decode("ascii")))
        .replace("__FAVICON__", data_uri(ROOT / "assets" / "favicon.png"))
    )
    DIST.mkdir(exist_ok=True)
    out = DIST / "index.html"
    out.write_text(html, encoding="utf-8")
    gz = DIST / "index.html.gz"
    with open(out, "rb") as f_in, gzip.GzipFile(gz, "wb", compresslevel=9, mtime=0) as f_out:
        shutil.copyfileobj(f_in, f_out)
    print(f"built {out} ({out.stat().st_size // 1024} KB), {gz.name} ({gz.stat().st_size // 1024} KB)")
    return gz


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", action="store_true", help="copy into FluidNC/data (index + legacy UI)")
    args = ap.parse_args()
    gz = build()
    if args.install:
        shutil.copy(gz, DATA / "index.html.gz")
        shutil.copy(ROOT / "vendor" / "legacy.html.gz", DATA / "legacy.html.gz")
        print(f"installed into {DATA}")


if __name__ == "__main__":
    main()
