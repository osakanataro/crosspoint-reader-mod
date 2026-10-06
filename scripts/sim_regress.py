#!/usr/bin/env python3
"""Screenshot regression check on the desktop simulator.

Reads test/sim-regress/scenarios.yaml, runs every scenario through scripts/sim_run.py's
functions on a fresh simulated card, and compares each screenshot with the stored
baseline (test/sim-regress/baseline/<env>/<scenario>/<shot>.png). Differences are
reported as a pixel count, a bounding box and a diff image (changed pixels in red),
plus an index.html for side-by-side viewing.

  python3 scripts/sim_regress.py --fonts-dir ../fonts                 # compare
  python3 scripts/sim_regress.py --fonts-dir ../fonts --update-baseline
  python3 scripts/sim_regress.py --fonts-dir ../fonts --only vertical-ja --env simulator_x4_pro

Exit status: 0 when every shot matches (within --threshold pixels), 1 when a shot differs,
is missing, has no baseline, or a scenario crashed or hung. The fonts are not in the
repository; point --fonts-dir (or $OST_SIM_FONTS) at a tree holding <Family>/*.cpfont.
"""

import argparse
import html
import os
import shutil
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_run  # noqa: E402

REPO_ROOT = sim_run.REPO_ROOT
DEFAULT_SCENARIOS = REPO_ROOT / "test" / "sim-regress" / "scenarios.yaml"
DEFAULT_BASELINE = REPO_ROOT / "test" / "sim-regress" / "baseline"
BOOKS_DIR = REPO_ROOT / "test" / "sim-regress" / "books"


def load_scenarios(path):
    try:
        import yaml  # noqa: WPS433
    except ImportError:
        sys.exit("error: PyYAML is required (pip install pyyaml)")
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    defaults = doc.get("defaults", {})
    scenarios = doc.get("scenarios", [])
    if not scenarios:
        sys.exit(f"error: no scenarios in {path}")
    return defaults, scenarios


def schedule(scenario, defaults):
    """Turn the step list into timed events and shots."""
    settle = int(scenario.get("settle_ms", defaults.get("settle_ms", 9000)))
    press_ms = int(scenario.get("press_ms", defaults.get("press_ms", 2500)))
    shot_ms = int(scenario.get("shot_ms", defaults.get("shot_ms", 800)))
    t = settle
    events, shots = [], []
    for step in scenario.get("steps", []):
        if not isinstance(step, dict) or len(step) != 1:
            raise sim_run.SimError(f"{scenario['name']}: a step is one of shot:/press:/wait:, got {step!r}")
        (kind, value), = step.items()
        if kind == "shot":
            shots.append((t, str(value)))
            t += shot_ms
        elif kind == "press":
            events.append((t, str(value)))
            t += press_ms
        elif kind == "wait":
            t += int(value)
        else:
            raise sim_run.SimError(f"{scenario['name']}: unknown step {kind}")
    events.append((t + 1000, "QUIT"))
    return events, shots, t + 1000


def compare(baseline, current, diff_out):
    """Return (changed pixel count, bbox) and write a diff image; None count when sizes differ."""
    from PIL import Image, ImageChops  # noqa: WPS433

    with Image.open(baseline) as a, Image.open(current) as b:
        a = a.convert("L")
        b = b.convert("L")
        if a.size != b.size:
            return None, f"size {a.size} vs {b.size}"
        diff = ImageChops.difference(a, b)
        bbox = diff.getbbox()
        if not bbox:
            return 0, None
        mask = diff.point(lambda v: 255 if v else 0)
        count = sum(mask.histogram()[255:])
        # Baseline faded to light gray, changed pixels in red.
        faded = Image.eval(a, lambda v: 160 + v * 95 // 255).convert("RGB")
        red = Image.new("RGB", a.size, (220, 0, 0))
        faded.paste(red, mask=mask)
        faded.save(diff_out)
        return count, bbox


def write_report(out, rows, env, threshold):
    md = [f"# Simulator screenshot regression ({env})", "", f"threshold: {threshold} px", "",
          "| scenario | shot | result | changed px | bbox |", "|---|---|---|---|---|"]
    parts = ["<!doctype html><meta charset='utf-8'><title>sim-regress</title>",
             "<style>body{font-family:sans-serif} img{max-height:400px;border:1px solid #ccc;margin:2px}"
             " .fail{color:#b00} .pass{color:#080} td{vertical-align:top}</style>",
             f"<h1>Simulator screenshot regression ({html.escape(env)})</h1>"]
    for r in rows:
        md.append(f"| {r['scenario']} | {r['shot']} | {r['result']} | {r['count'] if r['count'] is not None else ''} | {r['bbox'] or ''} |")
        cls = "pass" if r["result"] == "same" else "fail"
        parts.append(f"<h2 class='{cls}'>{html.escape(r['scenario'])} / {html.escape(r['shot'])}: {html.escape(r['result'])}"
                     f"{' (' + str(r['count']) + ' px)' if r['count'] else ''}</h2><table><tr>")
        for label, key in (("baseline", "baseline"), ("current", "current"), ("diff", "diff")):
            p = r.get(key)
            if p and Path(p).exists():
                rel = os.path.relpath(p, out)
                parts.append(f"<td>{label}<br><img src='{html.escape(rel)}'></td>")
        parts.append("</tr></table>")
    (out / "report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    (out / "index.html").write_text("\n".join(parts) + "\n", encoding="utf-8")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scenarios", default=str(DEFAULT_SCENARIOS))
    p.add_argument("--env", help="override the env of every scenario (simulator_x3, simulator_x4_pro, ...)")
    p.add_argument("--only", action="append", default=[], help="run only these scenario names (repeatable)")
    p.add_argument("--fonts-dir", default=os.environ.get("OST_SIM_FONTS"), help="tree with <Family>/*.cpfont ($OST_SIM_FONTS)")
    p.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    p.add_argument("--update-baseline", action="store_true", help="store this run's screenshots as the baseline")
    p.add_argument("--threshold", type=int, default=0, help="changed pixels tolerated per shot (default 0)")
    p.add_argument("--out", help="output directory (default .pio/sim-regress/<timestamp>)")
    p.add_argument("--sd", help="simulated card directory (default .pio/sim-regress/sd)")
    p.add_argument("--build", action="store_true", help="pio run -e <env> before running")
    p.add_argument("--pio", default=os.environ.get("PIO", "pio"))
    p.add_argument("--display", choices=["auto", "window", "headless"], default="auto")
    args = p.parse_args()

    defaults, scenarios = load_scenarios(args.scenarios)
    if args.only:
        scenarios = [s for s in scenarios if s["name"] in args.only]
        missing = set(args.only) - {s["name"] for s in scenarios}
        if missing:
            sys.exit(f"error: unknown scenario(s): {', '.join(sorted(missing))}")
    out = Path(args.out).resolve() if args.out else REPO_ROOT / ".pio" / "sim-regress" / datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    sd = Path(args.sd).resolve() if args.sd else REPO_ROOT / ".pio" / "sim-regress" / "sd"
    baseline_root = Path(args.baseline).resolve()
    font = defaults.get("font")
    if font and not args.fonts_dir and not any((sd / d / font).is_dir() for d in ("fonts", ".fonts")):
        sys.exit("error: --fonts-dir (or $OST_SIM_FONTS) is needed to put the SD fonts on the card")

    rows = []
    failed = False
    built = set()
    for sc in scenarios:
        name = sc["name"]
        env = args.env or sc.get("env") or defaults.get("env", "simulator_x3")
        try:
            if env not in built:
                sim_run.build(env, args.pio, force=args.build)
                built.add(env)
            settings = dict(defaults.get("settings", {}))
            settings.update(sc.get("settings", {}))
            book = sc.get("book")
            open_path = None
            books = []
            if book:
                src = Path(book)
                if not src.is_absolute() and not src.exists():
                    src = BOOKS_DIR / book
                books = [src]
                if sc.get("open", True):
                    open_path = f"/Books/{src.name}"
            sim_run.assemble_sd(sd, books=books, fonts_dir=args.fonts_dir,
                                sd_font=sc.get("font", font), settings=settings, open_path=open_path, fresh=True)
            events, shots, end_ms = schedule(sc, defaults)
            sc_out = out / env / name
            print(f"[{name}] env={env} book={book or '-'} shots={len(shots)} ...", end="", flush=True)
            result = sim_run.run(env, sd, events, shots, sc_out, timeout=end_ms / 1000 + 20, display=args.display)
            status = "ok" if result["ok"] else ("timeout" if result["timed_out"] else "CRASH")
            print(f" {status} {result['elapsed']:.1f}s outside={result['outside']} err={len(result['errs'])}")
        except sim_run.SimError as e:
            print(f"\n[{name}] error: {e}")
            failed = True
            rows.append({"scenario": name, "shot": "-", "result": f"error: {e}", "count": None, "bbox": None})
            continue
        if not result["ok"]:
            # A crashed or hung run may have captured half-drawn screens: neither compare
            # them nor let --update-baseline store them.
            failed = True
            rows.append({"scenario": name, "shot": "-", "result": status, "count": None, "bbox": None})
            continue
        for ms, shot in shots:
            base = baseline_root / env / name / f"{shot}.png"
            cur = result["shots"].get(shot)
            row = {"scenario": name, "shot": shot, "baseline": base, "current": cur, "diff": None, "count": None, "bbox": None}
            if cur is None:
                row["result"] = "not captured"
                failed = True
            elif args.update_baseline:
                base.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(cur, base)
                row["result"] = "baseline updated"
            elif not base.exists():
                row["result"] = "no baseline"
                failed = True
            else:
                diff_path = sc_out / f"{shot}.diff.png"
                count, bbox = compare(base, cur, diff_path)
                if count is None:
                    row["result"] = f"size differs: {bbox}"
                    failed = True
                elif count > args.threshold:
                    row.update(result="DIFFERENT", count=count, bbox=bbox, diff=diff_path)
                    failed = True
                else:
                    row.update(result="same", count=count)
            rows.append(row)

    env_label = args.env or defaults.get("env", "simulator_x3")
    write_report(out, rows, env_label, args.threshold)
    print()
    for r in rows:
        mark = "  " if r["result"] in ("same", "baseline updated") else "!!"
        extra = f" {r['count']} px {r['bbox']}" if r["count"] else ""
        print(f"{mark} {r['scenario']:<22} {r['shot']:<10} {r['result']}{extra}")
    print(f"\nreport: {out / 'index.html'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
