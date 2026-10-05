#!/usr/bin/env python3
"""Run the desktop simulator through a fixed sequence and keep screenshots and a log summary.

Modelled on crosspoint-jp's scripts/sim_run.ts. One invocation:

  1. assembles a simulated SD card (default .pio/sim-sd/: --book EPUBs under /Books,
     --fonts-dir .cpfont families under /fonts, settings.json and state.json edits),
  2. builds the simulator env when asked or when the binary is missing,
  3. starts .pio/build/<env>/program with CROSSPOINT_SIM_* set, under a timeout,
  4. converts the BMP screenshots to PNG (Pillow, when installed) and summarises the
     log: activity transitions, ERR lines, "Outside range" draws, sanitizer reports.

Exit status is non-zero when the simulator crashed or the build failed.

Examples:
  python3 scripts/sim_run.py --shot 1500:home
  python3 scripts/sim_run.py --env simulator_x3 --fonts-dir ../fonts-out --sd-font NotoSansJP \\
      --book ../epub-build/test_vertical_ja.epub --open /Books/test_vertical_ja.epub \\
      --shot 8000:page1 --script '9000:DOWN;11000:DOWN' --shot 12500:page3
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
KEYS = "BACK ENTER LEFT RIGHT UP DOWN POWER SLEEP HOME QUIT PREV NEXT TAP SWIPE"


def die(msg, code=2):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def coerce(value):
    if value == "true":
        return True
    if value == "false":
        return False
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    if re.fullmatch(r"-?\d+\.\d+", value):
        return float(value)
    return value


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)


def parse_args():
    p = argparse.ArgumentParser(
        description="CrossPoint desktop simulator runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    g = p.add_argument_group("environment")
    g.add_argument("--env", default="simulator_x3", help="PlatformIO env (default: simulator_x3)")
    g.add_argument("--build", action="store_true", help="run pio run -e <env> first")
    g.add_argument("--pio", default=os.environ.get("PIO", "pio"), help="pio executable (default: $PIO or pio)")
    g.add_argument("--timeout", type=float, default=60, help="seconds before the simulator is killed")
    g.add_argument(
        "--display",
        choices=["auto", "window", "headless"],
        default="auto",
        help="headless uses SDL's dummy video driver; auto picks it when $DISPLAY is unset",
    )

    g = p.add_argument_group("simulated SD card (default: assemble .pio/sim-sd/)")
    g.add_argument("--sd", help="use this directory as the card (it is written to)")
    g.add_argument("--book", action="append", default=[], help="EPUB to copy into /Books (repeatable)")
    g.add_argument("--fonts-dir", help="directory holding <Family>/<Family>_<size>.cpfont, copied to /fonts")
    g.add_argument("--sd-font", help="family name to select as the SD reading font")
    g.add_argument("--setting", action="append", default=[], help="settings.json key=value (repeatable)")
    g.add_argument("--open", help="EPUB path on the card to open at boot, e.g. /Books/x.epub")
    g.add_argument("--reset-cache", action="store_true", help="delete .crosspoint/epub_* and xtc_* first")
    g.add_argument("--reset-settings", action="store_true", help="delete settings.json and state.json first")

    g = p.add_argument_group("input and screenshots")
    g.add_argument("--script", help=f"CROSSPOINT_SIM_INPUT_SCRIPT, '<ms>:<KEY>[:<hold>];...' with KEY in {KEYS}")
    g.add_argument("--quit-at", type=int, help="ms at which QUIT is sent when the script has none (default: last event + 3000)")
    g.add_argument("--shot", action="append", default=[], help="<ms>:<name> screenshot (repeatable)")
    g.add_argument("--out", help="output directory (default: .pio/sim-out/<timestamp>/)")
    g.add_argument("--heap", type=int, help="CROSSPOINT_SIM_FREE_HEAP override in bytes")
    g.add_argument("--max-alloc", type=int, help="CROSSPOINT_SIM_MAX_ALLOC_HEAP override in bytes")
    return p.parse_args()


def assemble_sd(args):
    if args.sd:
        sd = Path(args.sd).resolve()
        if not sd.is_dir():
            die(f"SD directory not found: {sd}")
    else:
        sd = REPO_ROOT / ".pio" / "sim-sd"
    (sd / "Books").mkdir(parents=True, exist_ok=True)
    crosspoint = sd / ".crosspoint"
    crosspoint.mkdir(parents=True, exist_ok=True)

    if args.reset_cache:
        for entry in crosspoint.iterdir():
            if entry.is_dir() and (entry.name.startswith("epub_") or entry.name.startswith("xtc_")):
                shutil.rmtree(entry)
    if args.reset_settings:
        for name in ("settings.json", "state.json"):
            try:
                (crosspoint / name).unlink()
            except FileNotFoundError:
                pass

    for book in args.book:
        src = Path(book)
        if not src.is_file():
            die(f"book not found: {src}")
        dst = sd / "Books" / src.name
        if not dst.exists() or dst.stat().st_mtime < src.stat().st_mtime:
            shutil.copy2(src, dst)

    if args.fonts_dir:
        root = Path(args.fonts_dir)
        if (root / "fonts").is_dir():
            root = root / "fonts"
        families = [d for d in root.iterdir() if d.is_dir() and any(d.glob("*.cpfont"))]
        if not families:
            die(f"no <Family>/*.cpfont under {root}")
        for fam in families:
            dst = sd / "fonts" / fam.name
            dst.mkdir(parents=True, exist_ok=True)
            for f in fam.glob("*.cpfont"):
                target = dst / f.name
                if not target.exists() or target.stat().st_size != f.stat().st_size:
                    shutil.copy2(f, target)

    settings_path = crosspoint / "settings.json"
    settings = read_json(settings_path)
    dirty = False
    if args.sd_font:
        if not any((sd / d / args.sd_font).is_dir() for d in ("fonts", ".fonts")):
            die(f"family {args.sd_font} is not on the card (use --fonts-dir)")
        settings["sdFontFamilyName"] = args.sd_font
        dirty = True
    for kv in args.setting:
        key, sep, value = kv.partition("=")
        if not sep:
            die(f"--setting wants key=value: {kv}")
        settings[key] = coerce(value)
        dirty = True
    if dirty:
        write_json(settings_path, settings)

    if args.open:
        state_path = crosspoint / "state.json"
        state = read_json(state_path)
        # main.cpp skips Home and opens the reader when openEpubPath is set and the last
        # sleep came from the reader; a load count above zero is read as a reader crash.
        state["openEpubPath"] = args.open
        state["lastSleepFromReader"] = True
        state["readerActivityLoadCount"] = 0
        write_json(state_path, state)
    return sd


def build(args, program):
    if not args.build and program.exists():
        return
    print(f"[sim_run] {args.pio} run -e {args.env}")
    proc = subprocess.run([args.pio, "run", "-e", args.env], cwd=REPO_ROOT, capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    if proc.returncode != 0:
        for line in text.splitlines():
            if re.search(r"error|undefined reference|FAILED", line):
                print(line, file=sys.stderr)
        die("build failed; an undefined Hal* symbol means the simulator fork needs a stub", 1)
    for line in text.splitlines():
        if re.search(r"SUCCESS|Took", line):
            print(line)


def parse_events(args):
    events = []
    if args.script:
        for part in args.script.split(";"):
            m = re.fullmatch(r"\s*(\d+):(.+?)\s*", part)
            if not m:
                die(f"bad --script item: {part}")
            events.append((int(m.group(1)), m.group(2)))
    shots = []
    for s in args.shot:
        m = re.fullmatch(r"(\d+):([\w.-]+)", s)
        if not m:
            die(f"--shot wants <ms>:<name>: {s}")
        shots.append((int(m.group(1)), m.group(2)))
    if not any(text.startswith("QUIT") for _, text in events):
        last = max([0] + [ms for ms, _ in events] + [ms for ms, _ in shots])
        events.append((args.quit_at if args.quit_at is not None else last + 3000, "QUIT"))
    events.sort(key=lambda e: e[0])
    return events, shots


def to_png(bmp, png):
    try:
        from PIL import Image  # noqa: WPS433
    except ImportError:
        return False
    with Image.open(bmp) as im:
        im.save(png)
    return True


def main():
    args = parse_args()
    program = REPO_ROOT / ".pio" / "build" / args.env / "program"
    sd = assemble_sd(args)
    build(args, program)
    if not program.exists():
        die(f"simulator binary missing: {program}", 1)

    out = Path(args.out).resolve() if args.out else REPO_ROOT / ".pio" / "sim-out" / datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    events, shots = parse_events(args)

    env = os.environ.copy()
    env["CROSSPOINT_SIM_SD"] = str(sd)
    env["CROSSPOINT_SIM_INPUT_SCRIPT"] = ";".join(f"{ms}:{text}" for ms, text in events)
    if shots:
        env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:{out / (name + '.bmp')}" for ms, name in shots)
    if args.heap is not None:
        env["CROSSPOINT_SIM_FREE_HEAP"] = str(args.heap)
    if args.max_alloc is not None:
        env["CROSSPOINT_SIM_MAX_ALLOC_HEAP"] = str(args.max_alloc)
    headless = args.display == "headless" or (args.display == "auto" and not env.get("DISPLAY") and not env.get("WAYLAND_DISPLAY"))
    if headless:
        env.setdefault("SDL_VIDEODRIVER", "dummy")

    print(f"[sim_run] sd={sd}")
    print(f"[sim_run] script={env['CROSSPOINT_SIM_INPUT_SCRIPT']}")
    print(f"[sim_run] out={out} display={'headless' if headless else 'window'}")

    started = time.monotonic()
    proc = subprocess.Popen([str(program)], cwd=REPO_ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    timed_out = False
    try:
        log, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        log, _ = proc.communicate()
    elapsed = time.monotonic() - started
    (out / "sim.log").write_text(log, encoding="utf-8")

    lines = log.splitlines()
    activities = [re.sub(r"^\[\d+\]\s*\[DBG\]\s*\[ACT\]\s*", "", l) for l in lines if re.search(r"\[ACT\] (Entering|Exiting) activity", l)]
    errs = [l for l in lines if "[ERR]" in l and "outside range" not in l.lower()]
    outside = sum(1 for l in lines if "Outside range" in l)
    sanitizer = [l for l in lines if re.search(r"runtime error|AddressSanitizer|SUMMARY:", l)]

    print("\n=== Activity ===")
    for a in activities:
        print("  " + a)
    print(f"\n=== ERR ({len(errs)}) ===")
    for e in errs[:20]:
        print("  " + e)
    if outside:
        print(f"\n!! Outside range draws: {outside} (layout overflow; see {out / 'sim.log'})")
    if sanitizer:
        print("\n!! sanitizer:")
        for s in sanitizer[:10]:
            print("  " + s)

    print("\n=== Screenshots ===")
    for ms, name in shots:
        bmp = out / (name + ".bmp")
        if not bmp.exists():
            print(f"  {name}: not captured (process ended before {ms} ms?)")
            continue
        png = out / (name + ".png")
        if to_png(bmp, png):
            bmp.unlink()
            print(f"  {png}")
        else:
            print(f"  {bmp}")

    sig = -proc.returncode if proc.returncode < 0 else None
    crashed = sig is not None and sig not in (signal.SIGKILL, signal.SIGTERM)
    status = "timeout" if timed_out else f"exit={proc.returncode}"
    print(f"\n[sim_run] {status} signal={sig or '-'} elapsed={elapsed:.1f}s {'!! CRASH' if crashed else ''}")
    sys.exit(1 if crashed or (proc.returncode not in (0, None) and sig is None) else 0)


if __name__ == "__main__":
    main()
